#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_bridge.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh_lite.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "ConfigData.h"
#include "mesh.h"
#include "dual_net.h"
#include "task_config.h"
#include "system_restart.h"
#include "app_mem.h"


#define MESH_MAX_LEVEL 6
/*
 * 每节点 SoftAP 最大接入子节点数：2026-10-16 曾由 5 调到 8（缓解「中间父
 * 断开+根已满」时无替代父的重连循环），同日应现场要求回退为 5。
 * 回退后该场景依靠：重连/重扫（当前 10s×5/15s）+ 无父自愈 watchdog
 * （软复位→重启兜底，见 MESH_PARENT_LOST_SOFT_RESET_MS）兜底。
 * 约束备忘：ESP32-S3 SoftAP 单节点上限 15；CONFIG_LWIP_DHCPS_MAX_STATION_NUM=64；
 * iot_bridge Kconfig 要求 DHCPS ≥ SOFTAP_MAX_CONNECT_NUMBER；NAPT 表 512 条。
 * 注意：esp_bridge_wifi_set_config() 会用 CONFIG_BRIDGE_SOFTAP_MAX_CONNECT_NUMBER
 * 覆盖此处设置，sdkconfig.defaults 与构建生效 sdkconfig 必须同步。
 */
#define MESH_MAX_CHILDREN 5
#define FAILOVER_LOST_IP_MS 30000
#define FAILOVER_SCAN_INTERVAL_MS 60000
#define FAILOVER_RETRY_COOLDOWN_MS 300000
#define FAILOVER_RTC_MAGIC 0x4D4C4657U

/*
 * mesh-lite 父节点重连/重扫节奏（esp_mesh_lite_set_wifi_reconnect_interval 单位：秒）。
 * 库默认「5s×2 次重连当前父，失败后每 10s 重扫」。
 * 历史取舍（CHILD_NODE_ROOT_LOSS_DESIGN.md 7.3）：曾调到 10s×3/30s 压 BLE 老化链路
 * 射频争用与闭源重扫路径暴露；2026-10-16 折中为 5s×2/15s（重连窗口 30s→10s），
 * 加快中间父永久断开时的重扫发现，配合无父 watchdog 兜底。
 *
 * 2026-10-19 沿用的父恢复窗口取舍：为减少父重启恢复时的提前换父，重连窗口加长到
 * 10s×5=50s：父恢复 SoftAP 时子仍在重连原父窗口内，直接命中、OTA 传输中断面最小。
 * 代价：中间父永久故障时换父延迟从约 10s 增至约 50s（15s 重扫兜底节奏不变，
 * 仍有界；无父 watchdog 的 180s 软复位线不受影响）。根节点侧无负面：根 STA 30s 内
 * 恢复则 failover 的 30s 触发线不会误触发（FAILOVER_LOST_IP_MS），>30s 故障照旧切换。
 * 2026-10-22 一键重启在等待阶段主动暂停重连；启动后仍用本组参数，实机核对恢复耗时。
 */
#define MESH_PARENT_RETRY_INTERVAL_SEC 10
#define MESH_PARENT_RETRY_COUNT        5
#define MESH_RESCAN_INTERVAL_SEC       15

/*
 * 父节点连接失败快通道阈值（2026-10-20 新增，2026-10-21 去掉 reason 过滤）：
 * 连续 N 次与父节点的连接失败即经 mesh_heal_connect_fail_fast_path() 重启，
 * 不等 180s 无父 watchdog。按库重连节奏（10s×5/15s 重扫），N=5 约 50~100s
 * 触发。复用 watchdog 的 RTC 重启预算（MESH_RESTART_MAX_PER_CYCLE）防
 * "父持续满载时无限重启"。
 *
 * 为什么去掉 reason 过滤（2026-10-21，现场反馈"一直重连不重启"）：满员父的
 * deauth reason 由 iot_bridge SoftAP + WiFi 栈发出，实测值不可预先枚举
 * （初版集合 {2,4,5,6} 曾漏掉现场实际码导致计数不累加）；且正常换父成功会
 * 经 GOT_IP 持续在线 10s 后清零；只计非主动的 DISCONNECTED，不重复计 LOST_IP。
 */
#define MESH_PARENT_FAIL_RESTART_THRESHOLD 5

/*
 * "稳定在线"判定窗：同一次连续在线满 10s 才清零；离线结束在线窗。
 * 窗口内的"连上秒级又被踢"继续累计，续租不重置起点，旧 GOT_IP 时间不影响后续失败。
 */
#define MESH_PARENT_STABLE_ONLINE_MS 10000

/* 子节点健康监视周期：父丢失态 5s 一条（掉线属异常态），恢复后 60s 心跳。 */
#define MESH_HEALTH_LOST_INTERVAL_MS 5000
#define MESH_HEALTH_OK_INTERVAL_MS   60000

/* 主备路由器切换任务循环态诊断日志限频周期（掉线态密、健康态疏）。 */
#define MESH_FAILOVER_LOG_LOST_MS 5000
#define MESH_FAILOVER_LOG_OK_MS   60000

/*
 * 无父自愈 watchdog（2026-10-16，仅子节点；根节点由 root_failover_task 负责）。
 * 触发条件只看 Network_Flag（父链路/IP 条件），与 BLE/MQTT 事件解耦——
 * BLE 老化掉线、MQTT 断连都不会触发本 watchdog。
 *
 * 分级 escalate（用户 2026-10-16 确认）：
 *   L1 连续无父 MESH_PARENT_LOST_SOFT_RESET_MS → 软复位一次：
 *      esp_mesh_lite_disconnect() + esp_mesh_lite_connect()，
 *      请求重新扫描匹配节点，不承诺清除 RTC 原父偏好；不重启；
 *   L2 软复位 MESH_SOFT_RESET_MAX 次仍无父 → 清 Mesh RTC 记录再 esp_restart()；
 *   L3 每上电周期重启达 MESH_RESTART_MAX_PER_CYCLE（RTC 计数，
 *      ESP_RST_SW 延续、上电/Brownout 清零，镜像 failover 范式）→
 *      永久放弃重启，只限频打日志并继续扫描，回到「无限期存活于
 *      无上行状态」（CHILD_NODE_ROOT_LOSS_DESIGN.md 7.2/7.7）。
 *
 * 背景：现场实测重启可恢复「中间父断开 + 根已满」的卡死；
 * 软恢复作为第一级手段；连续失败快通道可能早于 watchdog 触发，两者共享 3 次预算。
 */
#define MESH_PARENT_LOST_SOFT_RESET_MS 180000 /* 连续无父 3 分钟后软复位 */
#define MESH_SOFT_RESET_MAX             3      /* 本次无父期间软复位次数上限 */
#define MESH_RESTART_MAX_PER_CYCLE      3      /* 每上电周期 watchdog 重启次数上限 */
#define MESH_HEAL_RTC_MAGIC             0x4D484C57U /* "MHLW" */
#define MESH_HEAL_GIVEUP_LOG_INTERVAL_MS 60000     /* 放弃重启后的限频日志周期 */
#define MESH_HEAL_SOFT_RESET_GAP_MS     100        /* disconnect→connect 间隔与重启前日志落盘等待 */
#define MESH_CONTROL_WAIT_MS           50

static const char *TAG = "mesh_lite_app";
static char s_softap_prefix[16];
static char s_softap_ssid[33];
/* Returned by lg_mesh_get_ssid_by_mac(); has to outlive the callback. */
static uint8_t s_parent_ssid[33];
static uint8_t s_uplink_index;
static atomic_bool s_scan_requested;
static atomic_bool s_scan_active;
static atomic_bool s_restart_for_failover;
static atomic_bool s_trial_success;
static bool s_trial_active;
static bool s_sntp_started;
static RTC_NOINIT_ATTR uint32_t s_failover_magic;
static RTC_NOINIT_ATTR atomic_uint_least32_t s_failover_attempts;
static RTC_NOINIT_ATTR uint32_t s_trial_uplink_index;
/* 无父 watchdog 重启预算：仅软复位无效后的重启计数，镜像 failover 范式。
 * ESP_RST_SW（本 watchdog 或 OTA 的软重启）延续计数；上电/Brownout 清零。 */
static RTC_NOINIT_ATTR uint32_t s_heal_magic;
static RTC_NOINIT_ATTR atomic_uint_least32_t s_heal_restart_count;

/* 最近一次 STA 断开原因：事件任务写、健康监视任务读。
 * atomic_uint 的 4 字节对齐访问在 Xtensa 上原子；仅用于诊断，不作为控制依据。 */
static atomic_uint s_last_sta_disconnect_reason;

/*
 * 连续父节点连接失败计数：事件任务在非主动 DISCONNECTED 时累加，
 * 事件/健康任务在连续在线 10s 后清零。更新经 s_parent_state_lock 同步，诊断原子读。
 * 仅子节点累加，LOST_IP 不重复计数。
 */
static atomic_uint s_parent_fail_count;

/*
 * 连续在线起点、状态与最近父 BSSID：默认事件任务和健康任务共用短临界区。
 */
static portMUX_TYPE s_parent_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_parent_ip_online;
static TickType_t s_parent_online_since;
static uint8_t s_last_parent_bssid[6];
static atomic_uint s_last_valid_level;
/* 主动 disconnect 的事件可能异步到达；只排除对应的本地 ASSOC_LEAVE 事件。 */
static atomic_bool s_local_disconnect_pending;
static StaticSemaphore_t s_mesh_control_mutex_buf;
static SemaphoreHandle_t s_mesh_control_mutex;

/* 应用连接/断开共用此锁；不在 Wi-Fi 事件中等待，SDK 调用不得置于 spinlock。 */
static bool mesh_control_take(void)
{
    return s_mesh_control_mutex != NULL &&
           xSemaphoreTake(s_mesh_control_mutex, pdMS_TO_TICKS(MESH_CONTROL_WAIT_MS)) == pdTRUE;
}

static void mesh_control_give(void)
{
    if (xSemaphoreGive(s_mesh_control_mutex) != pdTRUE) ESP_LOGE(TAG, "Mesh control mutex release failed");
}

static esp_err_t mesh_disconnect_locked(void)
{
    __atomic_store_n(&s_local_disconnect_pending, true, __ATOMIC_RELEASE);
    esp_err_t err = esp_mesh_lite_disconnect();
    if (err != ESP_OK) __atomic_store_n(&s_local_disconnect_pending, false, __ATOMIC_RELEASE);
    return err;
}

/* 只有 net_monitor 访问此标志；SDK 调用可产生新的断开事件，先标记再调用。
 * disconnect 返回失败时内部可能已暂停重连，仍须在有线失效时执行一次 connect。
 * 这是每次状态转换的一次控制尝试，不在断开事件中反复调用。 */
static bool s_sta_reconnect_paused_by_eth;

void mesh_root_manage_sta_reconnect(bool wired_preferred, bool sta_has_ip)
{
    if (IsRoot != 1) return;
    if (!mesh_control_take()) return; /* 下一轮 net_monitor 会重试最新状态。 */
    if (system_restart_mesh_paused()) {
        mesh_control_give();
        return;
    }
    bool pause = wired_preferred && (s_sta_reconnect_paused_by_eth || !sta_has_ip);
    if (pause == s_sta_reconnect_paused_by_eth) {
        mesh_control_give();
        return;
    }
    s_sta_reconnect_paused_by_eth = pause;
    if (pause) {
        esp_err_t err = mesh_disconnect_locked();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "有线接管：暂停 STA 重连请求返回 %s（STA 可能已断开）", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "有线接管：已暂停 STA 内部重连，保留根身份与 SoftAP");
        }
    } else {
        ESP_LOGI(TAG, "有线上行失效：恢复根节点 STA 重连");
        esp_mesh_lite_connect();
    }
    mesh_control_give();
}

uint8_t mesh_get_current_level(void)
{
    uint8_t level = esp_mesh_lite_get_level();
    if (level >= 1 && level <= MESH_MAX_LEVEL && (IsRoot == 1 || level >= 2)) {
        __atomic_store_n(&s_last_valid_level, level, __ATOMIC_RELEASE);
    }
    return level;
}

uint8_t mesh_get_restart_level(void)
{
    if (IsRoot == 1) return 1;
    uint8_t level = mesh_get_current_level();
    if (level >= 2 && level <= MESH_MAX_LEVEL) return level;
    level = __atomic_load_n(&s_last_valid_level, __ATOMIC_ACQUIRE);
    return level >= 2 && level <= MESH_MAX_LEVEL ? level : MESH_MAX_LEVEL;
}

esp_err_t mesh_pause_for_restart(void)
{
    if (!system_restart_mesh_paused()) return ESP_ERR_INVALID_STATE;
    if (!mesh_control_take()) return ESP_ERR_TIMEOUT;
    esp_err_t err = mesh_disconnect_locked();
    mesh_control_give();
    return err;
}

/* dual_net 初始化成功后由它发布根节点标志；初始化前/失败时保留 STA 兜底。
 * 子节点依旧按父节点 STA 的 IP 事件更新，不参与有线控制。 */
static void mesh_update_network_flag(bool sta_has_ip)
{
#if DUAL_NET_ENABLE && DUAL_NET_ENABLE_ROUTE_CONTROL
    if (IsRoot == 1 && dual_net_is_initialized()) return;
#endif
    __atomic_store_n(&Network_Flag, sta_has_ip ? 1U : 0U, __ATOMIC_RELEASE);
}

/* 事件与健康任务共用状态锁。续租 GOT_IP 不重置在线起点；离线即结束本次在线窗。 */
static void mesh_parent_online_changed(bool online, TickType_t now)
{
    portENTER_CRITICAL(&s_parent_state_lock);
    if (online && !s_parent_ip_online) s_parent_online_since = now;
    if (!online && s_parent_ip_online &&
        now - s_parent_online_since >= pdMS_TO_TICKS(MESH_PARENT_STABLE_ONLINE_MS)) {
        __atomic_store_n(&s_parent_fail_count, 0, __ATOMIC_RELAXED);
    }
    s_parent_ip_online = online;
    portEXIT_CRITICAL(&s_parent_state_lock);
}

static void mesh_parent_stability_poll(TickType_t now)
{
    portENTER_CRITICAL(&s_parent_state_lock);
    if (s_parent_ip_online && now - s_parent_online_since >= pdMS_TO_TICKS(MESH_PARENT_STABLE_ONLINE_MS)) {
        __atomic_store_n(&s_parent_fail_count, 0, __ATOMIC_RELAXED);
    }
    portEXIT_CRITICAL(&s_parent_state_lock);
}

/* 两条子节点自愈路径统一入口。仲裁与计划提交互斥，不清本项目 RTC 预算或任何 NVS。
 * 成功取得重启权后不再返回；网络已恢复/排程已接受时放弃本次自愈。 */
static void mesh_restart_without_parent(const char *reason, unsigned fails)
{
    if (!mesh_control_take()) return;
    if (system_restart_is_planned() || __atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE)) {
        mesh_control_give();
        return;
    }
    unsigned long restarts = __atomic_load_n(&s_heal_restart_count, __ATOMIC_RELAXED);
    if (restarts >= MESH_RESTART_MAX_PER_CYCLE) {
        mesh_control_give();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(MESH_HEAL_SOFT_RESET_GAP_MS));
    if (__atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE) || !system_restart_claim_recovery()) {
        mesh_control_give();
        return;
    }
    __atomic_store_n(&s_heal_restart_count, restarts + 1, __ATOMIC_RELAXED);
    uint8_t bssid[6];
    portENTER_CRITICAL(&s_parent_state_lock);
    memcpy(bssid, s_last_parent_bssid, sizeof(bssid));
    portEXIT_CRITICAL(&s_parent_state_lock);
    ESP_LOGW(TAG, "MESH_RECOVERY_RESTART reason=%s failures=%u last_parent=" MACSTR
             " reason_code=%u budget=%lu/%u erase_mesh_rtc=1",
             reason, fails, MAC2STR(bssid),
             (unsigned)__atomic_load_n(&s_last_sta_disconnect_reason, __ATOMIC_RELAXED),
             restarts + 1, (unsigned)MESH_RESTART_MAX_PER_CYCLE);
    esp_mesh_lite_erase_rtc_store();
    esp_restart();
}

/*
 * 无父自愈 watchdog：仅子节点、仅由 mesh_health_task 串行调用（单写者）。
 * 分级 escalate 见 MESH_PARENT_LOST_SOFT_RESET_MS 处注释；
 * 触发条件只看 Network_Flag，与 BLE/MQTT 事件解耦。
 * 恢复挂接时清零本次无父周期状态；重启预算跨重启保留（RTC）。
 */
static void mesh_heal_watchdog(bool online, TickType_t now)
{
    static TickType_t s_parent_lost_since = 0;
    static uint32_t s_soft_reset_count = 0;
    static TickType_t s_giveup_log_tick = 0;

    if (system_restart_is_planned()) return;

    if (online) {
        /* 恢复挂接：丢弃本次无父周期的累计状态（含软复位计数）。 */
        s_parent_lost_since = 0;
        s_soft_reset_count = 0;
        return;
    }

    if (s_parent_lost_since == 0) {
        s_parent_lost_since = now;
        s_soft_reset_count = 0;
        ESP_LOGW(TAG, "Parent lost; heal watchdog armed (soft reset after %u ms)",
                 (unsigned)MESH_PARENT_LOST_SOFT_RESET_MS);
        return;
    }

    uint32_t lost_ms = (uint32_t)((now - s_parent_lost_since) * portTICK_PERIOD_MS);
    if (lost_ms < MESH_PARENT_LOST_SOFT_RESET_MS) {
        return;
    }

    if (s_soft_reset_count < MESH_SOFT_RESET_MAX) {
        if (!mesh_control_take()) return;
        if (system_restart_is_planned() || __atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE)) {
            mesh_control_give();
            return;
        }
        /* L1：软复位。重置计时给本次软复位一个完整观察窗。 */
        s_soft_reset_count++;
        s_parent_lost_since = now;
        ESP_LOGW(TAG, "Parent lost %u ms; mesh soft reset %u/%u (disconnect+connect)",
                 (unsigned)lost_ms, (unsigned)s_soft_reset_count, (unsigned)MESH_SOFT_RESET_MAX);
        esp_err_t err = mesh_disconnect_locked();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_mesh_lite_disconnect: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(MESH_HEAL_SOFT_RESET_GAP_MS));
        /* esp_mesh_lite_connect() 为 void 返回（esp_mesh_lite_core.h）：扫描匹配
         * 节点并连接，结果由后续 mesh_health 心跳与 STA IP 日志体现。 */
        if (!system_restart_mesh_paused()) esp_mesh_lite_connect();
        mesh_control_give();
        return;
    }

    /* L2：软复位额度用尽 → 重启（受每上电周期预算约束）。 */
    unsigned long restarts = __atomic_load_n(&s_heal_restart_count, __ATOMIC_RELAXED);
    if (restarts < MESH_RESTART_MAX_PER_CYCLE) {
        mesh_restart_without_parent("parent_lost_timeout", __atomic_load_n(&s_parent_fail_count, __ATOMIC_RELAXED));
        return;
    }

    /* L3：重启预算耗尽 → 永久放弃重启，限频记录后继续扫描（无上行存活）。 */
    if (s_giveup_log_tick == 0 ||
        (now - s_giveup_log_tick) >= pdMS_TO_TICKS(MESH_HEAL_GIVEUP_LOG_INTERVAL_MS)) {
        s_giveup_log_tick = now;
        ESP_LOGW(TAG, "Parent lost %u ms; heal restart budget exhausted (%lu/%u); keep scanning, no more restarts this power cycle",
                 (unsigned)lost_ms, (unsigned long)restarts, (unsigned)MESH_RESTART_MAX_PER_CYCLE);
    }
}

/*
 * 父节点连接失败快通道（2026-10-20 新增，2026-10-21 重构去掉 reason 过滤）：
 * 与 mesh_heal_watchdog 并列的独立分支，不改 watchdog 既有 L1/L2/L3 逻辑。
 * 连续 MESH_PARENT_FAIL_RESTART_THRESHOLD 次连接失败即重启——比 watchdog
 * 的 180s 无父软复位更快恢复「多节点同时上电撞满员父」现场（现场反馈
 * "一直重连不重启"：初版 reason 过滤漏掉实际断开码，已改为任何断开都计数，
 * 并以稳定在线窗防"连上秒级被踢"的计数清零循环）。
 *
 * 自愈重启显式清除 Mesh RTC 后重新扫描；若没有其他合格候选仍可能选中原父。
 *
 * 复用 watchdog 的 RTC 重启预算（s_heal_restart_count /
 * MESH_RESTART_MAX_PER_CYCLE）：父持续满载时重启也无济于事，预算耗尽后
 * 清零计数并交回 watchdog L3（继续扫描、无上行存活），防疯循环。
 *
 * 调用上下文：仅子节点、仅由 mesh_health_task 串行调用（掉线态 5s 周期，
 * 故重启最多延迟 5s 执行）；计数器 s_parent_fail_count 由事件任务累加、
 * 本函数消费（__atomic_* RELAXED 同步）。
 */
static void mesh_heal_connect_fail_fast_path(void)
{
    if (system_restart_is_planned() || __atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE)) return;
    unsigned fails = __atomic_load_n(&s_parent_fail_count, __ATOMIC_RELAXED);
    if (fails < MESH_PARENT_FAIL_RESTART_THRESHOLD) {
        return;
    }

    /* 与 watchdog L2 同一 RTC 预算约束：跨重启延续，上电/Brownout 清零。 */
    unsigned long restarts = __atomic_load_n(&s_heal_restart_count, __ATOMIC_RELAXED);
    if (restarts >= MESH_RESTART_MAX_PER_CYCLE) {
        /*
         * 预算耗尽：清零计数避免本分支反复进入，交回 watchdog L3 的
         * 限频日志 + 继续扫描策略。清零后需再累计满阈值才会再来，
         * 不会刷屏。
         */
        portENTER_CRITICAL(&s_parent_state_lock);
        __atomic_store_n(&s_parent_fail_count, 0, __ATOMIC_RELAXED);
        portEXIT_CRITICAL(&s_parent_state_lock);
        ESP_LOGW(TAG,
                 "Parent connection failed %u times; restart budget exhausted (%lu/%u); "
                 "keep scanning, no more restarts this power cycle",
                 fails, (unsigned long)restarts, (unsigned)MESH_RESTART_MAX_PER_CYCLE);
        return;
    }

    mesh_restart_without_parent("parent_connection_failures", fails);
}

/*
 * 子节点 mesh 健康监视（仅 IsRoot==0 创建）。
 * 常规职责：只读状态 + 打心跳日志（父丢失态 5s 一条、恢复后 60s 一条），
 * 为「根掉线时子节点必须存活」提供现场证据（CHILD_NODE_ROOT_LOSS_DESIGN.md
 * 第 6.4/7.2 节）。
 * 2026-10-16 增补：无父自愈 watchdog（mesh_heal_watchdog）——
 * 由「只观测」升级为「观测 + 有界自愈」：连续无父先软复位、无效再重启，
 * 每上电周期重启上限 3 次后永久放弃。设计文档对应 7.2/7.7 节。
 */
static void mesh_health_task(void *arg)
{
    TickType_t last_report_tick = 0;
    bool was_online = __atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE) != 0;

    while (1)
    {
        bool online = __atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE) != 0;
        TickType_t now = xTaskGetTickCount();
        mesh_parent_stability_poll(now);
        uint8_t level = mesh_get_current_level();
        uint32_t interval_ms = online ? MESH_HEALTH_OK_INTERVAL_MS : MESH_HEALTH_LOST_INTERVAL_MS;

        if (online != was_online || last_report_tick == 0 ||
            (now - last_report_tick) >= pdMS_TO_TICKS(interval_ms))
        {
            ESP_LOGI(TAG,
                     "mesh_health: parent=%s, level=%u, network_flag=%u, last_sta_reason=%u, internal_free_bytes=%u",
                     online ? "online" : "LOST",
                     (unsigned)level,
                     online ? 1U : 0U,
                     (unsigned)__atomic_load_n(&s_last_sta_disconnect_reason, __ATOMIC_RELAXED),
                     (unsigned)esp_get_free_internal_heap_size());
            last_report_tick = now;
            was_online = online;
        }

        /* 无父自愈：分级 escalate（软复位 → 重启 → 放弃），只看父链路。 */
        mesh_heal_watchdog(online, now);

        /*
         * 父连接失败快通道：连续连接失败达阈值即重启（复用 RTC 预算，
         * 清记录后重新扫描），与 watchdog 共享预算及最终重启仲裁。
         */
        mesh_heal_connect_fail_fast_path();

        /* 始终 5s 检查恢复/稳定窗；健康态仅日志每 60s 输出，不再因长睡眠延误掉线处理。 */
        vTaskDelay(pdMS_TO_TICKS(MESH_HEALTH_LOST_INTERVAL_MS));
    }
}

static bool parse_mesh_id(const char *value, uint8_t *id)
{
    if (!value || strlen(value) != 2 || !isxdigit((unsigned char)value[0]) ||
        !isxdigit((unsigned char)value[1])) {
        return false;
    }
    unsigned int parsed;
    if (sscanf(value, "%2x", &parsed) != 1 || parsed == 0 || parsed > 0xff) {
        return false;
    }
    *id = (uint8_t)parsed;
    return true;
}

static bool valid_router(const char *ssid, const char *password)
{
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    size_t pass_len = password ? strlen(password) : 0;
    /* IoT-Bridge logs this buffer as a C string; keep room for a terminator. */
    return ssid_len > 0 && ssid_len <= 31 && strcmp(ssid, "default") != 0 &&
           pass_len >= 8 && pass_len <= 63 && strcmp(password, "default") != 0;
}

static uint8_t load_uplink_index(void)
{
    nvs_handle_t handle;
    uint8_t index = 0;
    if (nvs_open("mesh_net", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, "uplink", &index) != ESP_OK || index > 1) index = 0;
        nvs_close(handle);
    }
    return index;
}

static esp_err_t save_uplink_index(uint8_t index)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("mesh_net", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "uplink", index);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void sntp_sync_task(void *arg)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        setenv("TZ", "CST-8", 1);
        tzset();
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "SNTP sync: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "SNTP init: %s", esp_err_to_name(err));
        s_sntp_started = false;
    }
    vTaskDelete(NULL);
}

/*
 * 记录一次父节点连接失败（2026-10-20 新增，2026-10-21 重构）：
 * 只由默认事件任务在非主动 DISCONNECTED 时调用；LOST_IP 不计数。
 * 连续在线满 10s 的清零由 online_changed/stability_poll 负责，短连后继续累计。
 */
static void note_parent_connect_failure(void)
{
    if (IsRoot == 1 || system_restart_is_planned()) return;
    portENTER_CRITICAL(&s_parent_state_lock);
    unsigned prev_fails = __atomic_load_n(&s_parent_fail_count, __ATOMIC_RELAXED);
    unsigned fails = prev_fails == UINT_MAX ? UINT_MAX : prev_fails + 1u;
    __atomic_store_n(&s_parent_fail_count, fails, __ATOMIC_RELAXED);
    portEXIT_CRITICAL(&s_parent_state_lock);
    if (fails < MESH_PARENT_FAIL_RESTART_THRESHOLD) {
        ESP_LOGW(TAG,
                 "Parent connection failed (reason=%u), consecutive=%u/%u",
                 (unsigned)__atomic_load_n(&s_last_sta_disconnect_reason, __ATOMIC_RELAXED),
                 fails,
                 (unsigned)MESH_PARENT_FAIL_RESTART_THRESHOLD);
    }
}

static void network_event_handler(void *arg, esp_event_base_t base,
                                  int32_t event_id, void *event_data)
{
    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        if (IsRoot && s_trial_active) {
            ESP_LOGI(TAG, "Alternate uplink obtained IP " IPSTR, IP2STR(&event->ip_info.ip));
            s_trial_success = true;
            return;
        }
        /*
         * Network_Flag 的写入权按角色划分，避免两个写者竞争同一全局量：
         * 根节点由 dual_net 综合「以太网 + WiFi」裁决（以太网可用时不会被判为断网）；
         * 子节点上行由 mesh 父节点提供，dual_net 不运行，沿用原判据。
         */
        mesh_update_network_flag(true);
        s_failover_attempts = 0;
        mesh_parent_online_changed(true, xTaskGetTickCount());
        wifi_ap_record_t parent;
        if (esp_wifi_sta_get_ap_info(&parent) == ESP_OK) {
            portENTER_CRITICAL(&s_parent_state_lock);
            memcpy(s_last_parent_bssid, parent.bssid, sizeof(s_last_parent_bssid));
            portEXIT_CRITICAL(&s_parent_state_lock);
            ESP_LOGI(TAG, "Parent connected: bssid=" MACSTR " rssi=%d", MAC2STR(parent.bssid), parent.rssi);
        }
        ESP_LOGI(TAG, "STA IP " IPSTR ", level=%u, role=%s",
                 IP2STR(&event->ip_info.ip), mesh_get_current_level(),
                 IsRoot ? "root" : "node");
        if (!s_sntp_started) {
            if (xTaskCreatePinnedToCore(sntp_sync_task, "sntp_sync", LG_STACK_SNTP,
                                        NULL, LG_PRIO_SNTP, NULL, LG_APP_CPU_CORE) == pdPASS) {
                s_sntp_started = true;
            }
        }
    } else if ((base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) ||
               (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)) {
        /* 与上面 STA_GOT_IP 对称：根节点交给 dual_net 裁决，子节点沿用原判据 */
        mesh_update_network_flag(false);
        mesh_parent_online_changed(false, xTaskGetTickCount());
        if (s_trial_active) s_trial_success = false;
        /*
         * 断开原因落诊断变量，供子节点健康监视与「根掉线不重启」排查使用。
         * WIFI_EVENT_STA_DISCONNECTED 的 event_data 为 wifi_event_sta_disconnected_t；
         * IP_EVENT_STA_LOST_IP 无该结构，reason 保持上次已知值。
         */
        if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED && event_data != NULL) {
            const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)event_data;
            __atomic_store_n(&s_last_sta_disconnect_reason, disc->reason, __ATOMIC_RELAXED);
            portENTER_CRITICAL(&s_parent_state_lock);
            memcpy(s_last_parent_bssid, disc->bssid, sizeof(s_last_parent_bssid));
            portEXIT_CRITICAL(&s_parent_state_lock);
            /* ssid 非保证 NUL 终止：用精度限定打印，并对 ssid_len 做防御性截断。 */
            unsigned print_len = disc->ssid_len;
            if (print_len > sizeof(disc->ssid)) {
                print_len = sizeof(disc->ssid);
            }
            bool local = __atomic_exchange_n(&s_local_disconnect_pending, false, __ATOMIC_ACQ_REL) &&
                         disc->reason == WIFI_REASON_ASSOC_LEAVE;
            ESP_LOGW(TAG, "STA disconnected, reason=%u, ssid=%.*s, bssid=" MACSTR
                     ", local=%d planned=%d; local Mesh-Lite remains active",
                     (unsigned)disc->reason, (int)print_len, (const char *)disc->ssid,
                     MAC2STR(disc->bssid), local, system_restart_is_planned());

            /*
             * 父节点连接失败计数（仅子节点；根连路由器无父概念）。任何断开
             * 都计数（去掉 reason 过滤的依据见 MESH_PARENT_FAIL_RESTART_THRESHOLD
             * 注释）；稳定在线判定与消费方见 note_parent_connect_failure()。
             */
            if (!local) note_parent_connect_failure();
        } else {
            ESP_LOGW(TAG, "STA uplink lost; local Mesh-Lite remains active");
            /* LOST_IP 结束在线窗但不重复计数；真正的连接失败由 DISCONNECTED 计一次。
             * 单独丢 IP 的恢复由无父 watchdog 处理。缺失事件负载不能猜测失败次数。 */
        }
    }
}

static void scan_start_cb(void)
{
    /* 诊断：mesh-lite 回调进入顺序（与 WIFI_EVENT_SCAN_DONE 的先后是 H3 关键证据）。 */
    ESP_LOGI(TAG, "mesh scan start cb: requested=%d",
             __atomic_load_n(&s_scan_requested, __ATOMIC_RELAXED) ? 1 : 0);
    if (s_scan_requested) s_scan_active = true;
}

/*
 * SoftAP 站点进出观测（2026-10-16 增补，所有节点生效）：
 * 每个节点的 SoftAP 都开放接入，此前对「谁占满了槽位、谁在反复撞满员的 AP」
 * 完全不可观测，现场只能看到 WiFi 栈的 max connection, deauth! 原始告警，
 * 无法区分是 mesh 子节点重连还是外来设备占槽。
 * is_mesh_child 为 mesh-lite 补丁字段：可区分 mesh 子节点与手机/调试机等外来站点。
 * 只打印事件与当前占用，不动作、不重连、不删站点；回调上下文仅打日志，
 * 无阻塞 API。sta_list 用文件级静态而非局部：默认事件循环为单任务，
 * 所有 AP 事件串行到达，单访问者无需同步，同时避免事件任务栈（2304B）压力。
 */
static wifi_sta_list_t s_ap_sta_list;

static void mesh_ap_station_event_handler(void *arg, esp_event_base_t base,
                                          int32_t event_id, void *event_data)
{
    if (event_data == NULL) {
        return;
    }

    uint8_t mac[6] = {0};
    uint8_t aid = 0;
    bool is_mesh_child = false;
    const char *what = "unknown";

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *ev = (const wifi_event_ap_staconnected_t *)event_data;
        memcpy(mac, ev->mac, sizeof(mac));
        aid = ev->aid;
        is_mesh_child = ev->is_mesh_child;
        what = "connected";
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *ev = (const wifi_event_ap_stadisconnected_t *)event_data;
        memcpy(mac, ev->mac, sizeof(mac));
        aid = ev->aid;
        is_mesh_child = ev->is_mesh_child;
        what = "disconnected";
    } else {
        return;
    }

    unsigned occupied = 0;
    if (esp_wifi_ap_get_sta_list(&s_ap_sta_list) == ESP_OK) {
        occupied = (unsigned)s_ap_sta_list.num;
    }

    ESP_LOGI(TAG, "SoftAP station %s: aid=%u, mac=" MACSTR ", mesh_child=%d, occupied=%u/%u",
             what, (unsigned)aid, MAC2STR(mac), is_mesh_child ? 1 : 0,
             occupied, (unsigned)MESH_MAX_CHILDREN);
}

static void scan_end_cb(void)
{
    ESP_LOGI(TAG, "mesh scan end cb");
    s_scan_requested = false;
    s_scan_active = false;
}

static void scan_done_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    /* 诊断：无论后续是否处理，先记录 SCAN_DONE 到达时刻的各门禁值。 */
    ESP_LOGI(TAG, "WIFI_EVENT_SCAN_DONE: scan_active=%d, network_flag=%u, eth_ip=%d, is_root=%d",
             __atomic_load_n(&s_scan_active, __ATOMIC_RELAXED) ? 1 : 0,
             (unsigned)__atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE),
             dual_net_has_eth_ip() ? 1 : 0,
             IsRoot ? 1 : 0);
    if (system_restart_is_planned()) {
        scan_end_cb();
        return;
    }
    if (!s_scan_active) return;
    if (__atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE) || !IsRoot || dual_net_has_eth_ip()) {
        scan_end_cb();
        return;
    }
    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || count == 0) {
        ESP_LOGW(TAG, "Failover scan returned no AP (err or count=0)");
        scan_end_cb();
        return;
    }
    wifi_ap_record_t *records = app_calloc_prefer_psram(count, sizeof(*records));
    if (!records) {
        scan_end_cb();
        return;
    }
    const char *other_ssid = s_uplink_index == 0 ? SSID1 : SSID;
    bool matched = false;
    if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
        for (uint16_t i = 0; i < count; ++i) {
            bool hit = strcmp((const char *)records[i].ssid, other_ssid) == 0;
            /* 诊断：仅打印前 10 条防刷屏；匹配判定必须覆盖全部 AP（旧实现只统计前 10 条，
             * 目标位于第 11 条以后时会误报 "not found"）。ssid 非保证 NUL 终止，限宽打印。 */
            if (i < 10) {
                ESP_LOGI(TAG, "scan ap[%u/%u]=%.32s rssi=%d %s",
                         (unsigned)(i + 1), (unsigned)count,
                         (const char *)records[i].ssid, records[i].rssi,
                         hit ? "<== TARGET" : "");
            }
            if (hit && !dual_net_has_eth_ip()) {
                ESP_LOGW(TAG, "Alternate SSID found at ap[%u/%u] rssi=%d; testing it after restart",
                         (unsigned)(i + 1), (unsigned)count, records[i].rssi);
                s_restart_for_failover = true;
                matched = true;
                break;
            }
        }
    } else {
        ESP_LOGW(TAG, "Failover scan: get ap records failed");
    }
    if (!matched) {
        ESP_LOGW(TAG, "Failover scan done: target SSID \"%s\" not found in %u APs",
                 other_ssid ? other_ssid : "(null)", (unsigned)count);
    }
    free(records);
    scan_end_cb();
}

/*
 * 故障转移任务循环态诊断日志（限频，不改变任何行为）：
 * 仅由 root_failover_task 调用；状态翻转立即打印，否则按掉线 5s / 健康 60s 节流。
 */
static void mesh_failover_log_state(bool online, TickType_t now)
{
    static bool prev_online = true;
    static TickType_t last_log_tick = 0;

    uint32_t interval_ms = online ? MESH_FAILOVER_LOG_OK_MS : MESH_FAILOVER_LOG_LOST_MS;
    if (online == prev_online && last_log_tick != 0 &&
        (now - last_log_tick) < pdMS_TO_TICKS(interval_ms)) {
        return;
    }
    prev_online = online;
    last_log_tick = now;

    const char *other_ssid = s_uplink_index == 0 ? SSID1 : SSID;
    ESP_LOGI(TAG,
             "failover_state: online=%u, attempts=%lu, uplink_index=%u, other_ssid=%s, "
             "scan_requested=%d, scan_active=%d, restart_pending=%d, trial_active=%d, eth_ip=%d",
             online ? 1U : 0U,
             (unsigned long)__atomic_load_n(&s_failover_attempts, __ATOMIC_RELAXED),
             (unsigned)s_uplink_index,
             other_ssid ? other_ssid : "(null)",
             __atomic_load_n(&s_scan_requested, __ATOMIC_RELAXED) ? 1 : 0,
             __atomic_load_n(&s_scan_active, __ATOMIC_RELAXED) ? 1 : 0,
             __atomic_load_n(&s_restart_for_failover, __ATOMIC_RELAXED) ? 1 : 0,
             s_trial_active ? 1 : 0,
             dual_net_has_eth_ip() ? 1 : 0);
}

/*
 * 故障转移重启辅助：先清除 mesh-lite 库存储的路由器信息，再重启。
 *
 * 根因（2026-10-14 实机日志确认）：mesh-lite 闭源库在启动时若其 RTC 存储非空，
 * 会用存储的路由器配置覆盖 mesh_init_Custom() 设置的 STA 配置并重连旧路由器
 * （串口表现为 "[Unexpected restart] ... Try to connect the original parent node"
 * 之后紧跟第二次 STA 配置应用/authmode 日志）。若不清除，切备用路由器的 trial
 * 重启会被库改回旧路由器（已关闭），30s 后必然退回 saved uplink，切换永远无法生效。
 *
 * 清除后启动时库无存储可恢复，只能使用本次 mesh_init_Custom 设置的目标 uplink
 * 配置（由 s_uplink_index/s_trial_uplink_index 决定）。
 *
 * 注意：esp_mesh_lite_erase_rtc_store() 官方注释为 "Must be called before node
 * factory reset"，此处为有意的偏离使用——语义上正是「切换路由器前丢弃旧路由器
 * 存储」。子节点自愈使用相同清理，正常一键重启保留该记录。RTC 写入无需 NVS 提交。
 */
static void mesh_failover_restart(const char *reason)
{
    if (!mesh_control_take()) return;
    if (!system_restart_claim_recovery()) {
        mesh_control_give();
        return;
    }
    ESP_LOGW(TAG, "Failover restart (%s): erase stored router, then restart", reason);
    esp_mesh_lite_erase_rtc_store();
    esp_restart();
}

static void root_failover_task(void *arg)
{
    TickType_t lost_since = 0;
    TickType_t last_scan = 0;
    TickType_t last_log_cooldown_tick = 0;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (system_restart_is_planned()) {
            s_scan_requested = false;
            s_scan_active = false;
            s_restart_for_failover = false;
            if (!s_trial_active) s_trial_uplink_index = UINT32_MAX;
            lost_since = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        /* 诊断：循环态限频日志（翻转即报），供 H1-H5 假设树判读。 */
        mesh_failover_log_state(__atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE) != 0, now);
        /*
         * 以太网守卫：上行已由有线承担时，WiFi STA 没有 IP 属于正常情况。
         * 此时不应按「上行故障」处理，否则会扫描备用路由器并 esp_restart()，
         * 导致插着网线的根节点被无谓重启（本任务只在根节点创建）。
         */
        if (dual_net_has_eth_ip()) {
            if (lost_since != 0 || s_scan_requested) {
                ESP_LOGI(TAG, "以太网上行可用，暂停主备路由器切换（WiFi 侧无 IP 属正常）");
            }
            lost_since = 0;
            s_scan_requested = false;
            s_scan_active = false;
            s_restart_for_failover = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (s_trial_active) {
            if (s_trial_success) {
                esp_err_t err = save_uplink_index(s_uplink_index);
                if (err == ESP_OK) {
                    s_trial_uplink_index = UINT32_MAX;
                    ESP_LOGI(TAG, "Alternate uplink confirmed; restarting with saved credentials");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    if (dual_net_has_eth_ip()) continue;
                    mesh_failover_restart("alternate uplink confirmed");
                }
                ESP_LOGE(TAG, "Failed to save confirmed uplink: %s", esp_err_to_name(err));
            } else {
                if (lost_since == 0) lost_since = now;
                if ((now - lost_since) >= pdMS_TO_TICKS(FAILOVER_LOST_IP_MS)) {
                    s_trial_uplink_index = UINT32_MAX;
                    ESP_LOGW(TAG, "Alternate uplink did not obtain IP; returning to saved uplink");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    if (dual_net_has_eth_ip()) continue;
                    mesh_failover_restart("alternate uplink failed, return to saved");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (__atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE)) {
            lost_since = 0;
        } else {
            if (lost_since == 0) lost_since = now;
            if (s_failover_attempts >= 2 &&
                (now - lost_since) >= pdMS_TO_TICKS(FAILOVER_RETRY_COOLDOWN_MS)) {
                s_failover_attempts = 0;
            }
            const char *other_ssid = s_uplink_index == 0 ? SSID1 : SSID;
            const char *other_password = s_uplink_index == 0 ? WIFI_PS1 : WIFI_PS;
            if (s_failover_attempts < 2 && valid_router(other_ssid, other_password) &&
                (now - lost_since) >= pdMS_TO_TICKS(FAILOVER_LOST_IP_MS) &&
                (last_scan == 0 || (now - last_scan) >= pdMS_TO_TICKS(FAILOVER_SCAN_INTERVAL_MS)) &&
                !s_scan_requested) {
                s_scan_requested = true;
                last_scan = now;
                ESP_LOGI(TAG,
                         "Failover scan requested: lost_ms=%lu, attempts=%lu, target=%s",
                         (unsigned long)((now - lost_since) * portTICK_PERIOD_MS),
                         (unsigned long)__atomic_load_n(&s_failover_attempts, __ATOMIC_RELAXED),
                         other_ssid ? other_ssid : "(null)");
                esp_err_t err = esp_mesh_lite_wifi_scan_start(NULL, pdMS_TO_TICKS(3000));
                if (err != ESP_OK) {
                    s_scan_requested = false;
                    ESP_LOGW(TAG, "Failover scan: %s", esp_err_to_name(err));
                }
            } else if (s_failover_attempts >= 2 &&
                       (now - lost_since) < pdMS_TO_TICKS(FAILOVER_RETRY_COOLDOWN_MS) &&
                       (now - last_log_cooldown_tick) >= pdMS_TO_TICKS(MESH_FAILOVER_LOG_LOST_MS)) {
                /* H5：attempts 耗尽量冷却期，明示剩余等待，避免误判为“无反应”。 */
                last_log_cooldown_tick = now;
                ESP_LOGW(TAG,
                         "Failover paused in cooldown: attempts=%lu, elapsed_ms=%lu, cooldown_ms=%lu",
                         (unsigned long)__atomic_load_n(&s_failover_attempts, __ATOMIC_RELAXED),
                         (unsigned long)((now - lost_since) * portTICK_PERIOD_MS),
                         (unsigned long)FAILOVER_RETRY_COOLDOWN_MS);
            }
        }
        if (s_restart_for_failover &&
            (__atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE) || dual_net_has_eth_ip())) {
            s_restart_for_failover = false;
            ESP_LOGI(TAG, "Failover restart cancelled: link recovered before restart");
        }
        if (s_restart_for_failover) {
            uint8_t next = s_uplink_index == 0 ? 1 : 0;
            s_trial_uplink_index = next;
            ++s_failover_attempts;
            ESP_LOGW(TAG, "Restarting to verify uplink %u", next);
            vTaskDelay(pdMS_TO_TICKS(200));
            if (dual_net_has_eth_ip() || __atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE)) {
                s_trial_uplink_index = UINT32_MAX;
                s_restart_for_failover = false;
                /* GOT_IP 回调也会清零计数，不能用 -- 回滚而导致无符号下溢。 */
                s_failover_attempts = 0;
                ESP_LOGI(TAG, "Failover restart aborted: link recovered during guard window");
                continue;
            }
            mesh_failover_restart("switch to alternate uplink");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*
 * Fallback used when Mesh-Lite cannot derive the parent SSID from the scan record.
 * Registered through esp_mesh_lite_get_ssid_by_mac_cb_register().
 *
 * Every node publishes the SoftAP name built by mesh_init_Custom(), so the parent
 * name can always be reconstructed from the candidate BSSID. This must mirror that
 * construction exactly, including the MAC suffix mode, otherwise the node would
 * hand an unknown SSID to esp_wifi_connect().
 *
 * whitelist=false: the callback answers for any BSSID that already passed the
 * mesh_id/vendor filter, and is only consulted when the SSID cannot be read from
 * the scan result.
 */
static const uint8_t *lg_mesh_get_ssid_by_mac(const uint8_t *bssid)
{
    if (!bssid) {
        return NULL;
    }

    memset(s_parent_ssid, 0, sizeof(s_parent_ssid));
#ifdef CONFIG_BRIDGE_SOFTAP_SSID_END_WITH_THE_MAC
    snprintf((char *)s_parent_ssid, sizeof(s_parent_ssid), "%s_%02x%02x%02x",
             s_softap_prefix, bssid[3], bssid[4], bssid[5]);
#else
    snprintf((char *)s_parent_ssid, sizeof(s_parent_ssid), "%s", s_softap_prefix);
#endif
    ESP_LOGD(TAG, "ssid by mac " MACSTR ": %s", MAC2STR(bssid), (char *)s_parent_ssid);
    return s_parent_ssid;
}

/*
 * Diagnostic anchor for the join failure: shows what the STA interface actually
 * holds. Once a parent has been selected this must no longer be empty.
 */
static void lg_mesh_dump_sta_config(const char *when)
{
    wifi_config_t cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "[%s] STA config read failed", when);
        return;
    }
    ESP_LOGD(TAG, "[%s] sta ssid=\"%s\" len=%u bssid_set=%d password_set=%d level=%u",
             when, (const char *)cfg.sta.ssid, (unsigned)strlen((const char *)cfg.sta.ssid),
             cfg.sta.bssid_set, cfg.sta.password[0] != '\0',
             (unsigned)esp_mesh_lite_get_level());
}

static void lg_mesh_node_scan_done_handler(void *arg, esp_event_base_t base,
                                           int32_t event_id, void *event_data)
{
    lg_mesh_dump_sta_config("node scan done");
}

/*
 * Diagnostic helper for the mesh join failure.
 *
 * The parent-selection and esp_wifi_connect decision tree (the "Test Log ..."
 * lines) lives in the closed-source esp-mesh-lite library. Those logs do not go
 * through ESP_LOGD directly: they use ESP_MESH_LITE_LOGx(), which is gated by the
 * fixed ESP_MESH_LITE_LOG_LEVEL=ESP_LOG_DEBUG in esp_mesh_lite_log.h and finally
 * reaches esp_log_writev(). Since esp_log_writev() filters by the runtime level of
 * the tag, esp_log_level_set() alone is enough to reveal them - DEBUG is the
 * effective maximum for that library, so VERBOSE buys nothing.
 *
 * Set LG_MESH_DIAG_LOG to 0 to disable, or to 2 to also promote the per-packet
 * communication and espnow tags (very noisy, only for targeted debugging).
 */
#ifndef LG_MESH_DIAG_LOG
#define LG_MESH_DIAG_LOG 1
#endif

#if LG_MESH_DIAG_LOG
static const char *const s_mesh_diag_tags[] = {
    "mesh_lite_app", /* TAG itself; promoting it exposes this file's own ESP_LOGD lines */
    "vendor_ie",
    "Mesh-Lite",
#if LG_MESH_DIAG_LOG > 1
    "ESP_Mesh_Lite_Comm",
    "mesh-lite-espnow",
#endif
};

static void mesh_lite_diag_log_enable(void)
{
    for (size_t i = 0; i < sizeof(s_mesh_diag_tags) / sizeof(s_mesh_diag_tags[0]); ++i) {
        esp_log_level_set(s_mesh_diag_tags[i], ESP_LOG_DEBUG);
    }
    ESP_LOGI(TAG, "Mesh-Lite diagnostic logging enabled (level=%d)", LG_MESH_DIAG_LOG);
}
#else
static void mesh_lite_diag_log_enable(void)
{
}
#endif

esp_err_t mesh_init_Custom(void)
{
    uint8_t mesh_id;
    if (IsRoot > 1) {
        ESP_LOGE(TAG, "IsRoot must be 0 or 1");
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_mesh_id(Mesh_ID, &mesh_id)) {
        ESP_LOGE(TAG, "Mesh_ID must be exactly two hex digits from 01 to FF");
        return ESP_ERR_INVALID_ARG;
    }
    if (!Mesh_PS || strlen(Mesh_PS) < 8 || strlen(Mesh_PS) > 63) {
        ESP_LOGE(TAG, "Mesh_PS must contain 8-63 characters");
        return ESP_ERR_INVALID_ARG;
    }
    if (IsRoot && !valid_router(SSID, WIFI_PS)) {
        ESP_LOGE(TAG, "Root SSID/WIFI_PS is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (IsRoot && valid_router(SSID1, WIFI_PS1) && strcmp(SSID, SSID1) == 0) {
        ESP_LOGE(TAG, "SSID and SSID1 must differ for failover");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mesh_control_mutex == NULL) s_mesh_control_mutex = xSemaphoreCreateMutexStatic(&s_mesh_control_mutex_buf);
    if (s_mesh_control_mutex == NULL) return ESP_ERR_NO_MEM;

    if (esp_reset_reason() != ESP_RST_SW || s_failover_magic != FAILOVER_RTC_MAGIC) {
        s_failover_magic = FAILOVER_RTC_MAGIC;
        s_failover_attempts = 0;
        s_trial_uplink_index = UINT32_MAX;
    }
    /* 无父 watchdog 重启预算：软复位无效后的重启计数。
     * ESP_RST_SW（本 watchdog 主动重启或 OTA）延续计数以强制执行上限；
     * 上电/Brownout 等硬复位清零，开启新的上电周期预算。 */
    if (esp_reset_reason() != ESP_RST_SW || s_heal_magic != MESH_HEAL_RTC_MAGIC) {
        s_heal_magic = MESH_HEAL_RTC_MAGIC;
        s_heal_restart_count = 0;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop init failed");
    esp_bridge_create_all_netif();

    s_uplink_index = IsRoot ? load_uplink_index() : 0;
    if (IsRoot && s_trial_uplink_index <= 1) {
        s_uplink_index = (uint8_t)s_trial_uplink_index;
        s_trial_active = true;
    }
    if (s_uplink_index == 1 && !valid_router(SSID1, WIFI_PS1)) {
        s_uplink_index = 0;
        s_trial_uplink_index = UINT32_MAX;
        s_trial_active = false;
    }
    wifi_config_t sta_config = {0};
    if (IsRoot) {
        const char *ssid = s_uplink_index == 0 ? SSID : SSID1;
        const char *password = s_uplink_index == 0 ? WIFI_PS : WIFI_PS1;
        memcpy(sta_config.sta.ssid, ssid, strlen(ssid));
        memcpy(sta_config.sta.password, password, strlen(password));
    }
    ESP_RETURN_ON_ERROR(esp_bridge_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "STA config failed");

    uint8_t ap_mac[6];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_AP, ap_mac), TAG, "AP MAC read failed");
    snprintf(s_softap_prefix, sizeof(s_softap_prefix), "LGMesh_%02X", mesh_id);
#ifdef CONFIG_BRIDGE_SOFTAP_SSID_END_WITH_THE_MAC
    snprintf(s_softap_ssid, sizeof(s_softap_ssid), "LGMesh_%02X_%02x%02x%02x",
             mesh_id, ap_mac[3], ap_mac[4], ap_mac[5]);
#else
    snprintf(s_softap_ssid, sizeof(s_softap_ssid), "%s", s_softap_prefix);
#endif
    wifi_config_t ap_config = {0};
    /* Match the SSID format used by IoT-Bridge and Mesh-Lite for this build. */
    memcpy(ap_config.ap.ssid, s_softap_prefix, strlen(s_softap_prefix));
    memcpy(ap_config.ap.password, Mesh_PS, strlen(Mesh_PS));
    ap_config.ap.max_connection = MESH_MAX_CHILDREN;
    ESP_RETURN_ON_ERROR(esp_bridge_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "AP config failed");

    /* Run before esp_mesh_lite_init() so the vendor_ie startup logs are captured. */
    mesh_lite_diag_log_enable();

    esp_mesh_lite_config_t config = ESP_MESH_LITE_DEFAULT_INIT();
    config.mesh_id = mesh_id;
    config.max_level = MESH_MAX_LEVEL;
    config.max_connect_number = MESH_MAX_CHILDREN;
    config.join_mesh_ignore_router_status = true;
    config.join_mesh_without_configured_wifi = !IsRoot;
    config.softap_ssid = s_softap_prefix;
    config.softap_password = Mesh_PS;
    esp_mesh_lite_init(&config);
    if (esp_mesh_lite_get_mesh_id() != mesh_id) {
        /* A previous Mesh-Lite run may have persisted another ID in NVS. */
        esp_mesh_lite_set_mesh_id(mesh_id, true);
    }
    ESP_RETURN_ON_ERROR(esp_mesh_lite_set_softap_info(s_softap_ssid, Mesh_PS), TAG, "SoftAP info failed");
    wifi_config_t applied_ap = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_AP, &applied_ap), TAG, "AP config read failed");
    if (strncmp((const char *)applied_ap.ap.ssid, s_softap_ssid,
                sizeof(applied_ap.ap.ssid)) != 0) {
        ESP_LOGE(TAG, "SoftAP SSID mismatch: expected=%s, actual=%.*s",
                 s_softap_ssid, (int)sizeof(applied_ap.ap.ssid), applied_ap.ap.ssid);
        return ESP_ERR_INVALID_STATE;
    }
    if (IsRoot) {
        ESP_RETURN_ON_ERROR(esp_mesh_lite_set_allowed_level(1), TAG, "root level failed");
    } else {
        ESP_RETURN_ON_ERROR(esp_mesh_lite_set_disallowed_level(1), TAG, "node level failed");
    }

    /*
     * 父节点重连/重扫节奏（库默认 5s×2 次后每 10s 重扫），取值见文件头部
     * MESH_PARENT_RETRY 与 MESH_RESCAN_INTERVAL_SEC 系列宏及注释：
     * 当前为 10s×5/15s，保留父重启恢复窗口；持续无父场景由无父
     * watchdog（MESH_PARENT_LOST_SOFT_RESET_MS）兜底。
     */
    esp_mesh_lite_set_wifi_reconnect_interval(MESH_PARENT_RETRY_INTERVAL_SEC,
                                              MESH_PARENT_RETRY_COUNT,
                                              MESH_RESCAN_INTERVAL_SEC);

    /*
     * Give Mesh-Lite a way to resolve the parent SSID from the BSSID even when the
     * scan record cannot supply it. Without this, the STA keeps an empty SSID and
     * esp_wifi_connect() only answers "Haven't to connect to a suitable AP now!".
     */
    ESP_RETURN_ON_ERROR(esp_mesh_lite_get_ssid_by_mac_cb_register(lg_mesh_get_ssid_by_mac, false),
                        TAG, "ssid-by-mac callback failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    network_event_handler, NULL), TAG, "IP handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                    network_event_handler, NULL), TAG, "IP handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                    network_event_handler, NULL), TAG, "WiFi handler failed");
    /*
     * SoftAP 站点进出观测（所有节点）：本节点 SoftAP 的接入/离开事件，
     * 用于诊断「槽位被谁占满、谁在反复撞满员的 AP」。只打印不动作。
     * 注意：不覆盖 iot_bridge 自己注册的 AP 事件处理器（多处理器可共存）。
     */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                                    mesh_ap_station_event_handler, NULL),
                        TAG, "AP sta-connected handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                                    mesh_ap_station_event_handler, NULL),
                        TAG, "AP sta-disconnected handler failed");
    if (IsRoot && valid_router(SSID1, WIFI_PS1)) {
        static esp_mesh_lite_scan_cb_t scan_callbacks = {
            .scan_start_cb = scan_start_cb,
            .scan_end_cb = scan_end_cb,
        };
        ESP_RETURN_ON_ERROR(esp_mesh_lite_scan_cb_register(&scan_callbacks), TAG, "scan callbacks failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                                        scan_done_handler, NULL), TAG, "scan handler failed");
    }
    if (!IsRoot) {
        /* Anchor: shows whether Mesh-Lite ever writes the parent into the STA config. */
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                                        lg_mesh_node_scan_done_handler, NULL),
                            TAG, "node scan handler failed");
    }

    esp_mesh_lite_start();
    lg_mesh_dump_sta_config("after start");
    if (IsRoot && valid_router(SSID1, WIFI_PS1)) {
        if (xTaskCreatePinnedToCore(root_failover_task, "mesh_failover", LG_STACK_MESH_FAILOVER,
                                    NULL, LG_PRIO_MESH_FAILOVER,
                                    NULL, LG_APP_CPU_CORE) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create failover task");
        } else {
            /* 诊断：主备切换任务就绪，明示初始 uplink_index 与扫描目标（H1 证据）。 */
            ESP_LOGI(TAG, "Failover task created: uplink_index=%u, primary=%s, alternate=%s",
                     (unsigned)s_uplink_index,
                     SSID ? SSID : "(null)", SSID1 ? SSID1 : "(null)");
        }
    } else if (IsRoot) {
        /*
         * H1：主备切换任务被条件静默跳过——根节点将永远不会切备用路由器。
         * 此处必须可诊断：打印 SSID1 与密码长度（不打印密码本体）。
         */
        ESP_LOGE(TAG, "Failover task NOT created: alternate router invalid, ssid1=%s, pass1_len=%u",
                 SSID1 ? SSID1 : "(null)",
                 (unsigned)(WIFI_PS1 ? strlen(WIFI_PS1) : 0));
    }
    bool mesh_health_started = true; /* 根节点无此任务，视为不适用 */
    if (!IsRoot) {
        /* 子节点健康监视与有界自愈；一键重启期间抑制自愈动作。 */
        mesh_health_started = xTaskCreatePinnedToCore(mesh_health_task, "mesh_health", LG_STACK_MESH_HEALTH,
                                                      NULL, LG_PRIO_MESH_HEALTH,
                                                      NULL, LG_APP_CPU_CORE) == pdPASS;
        if (!mesh_health_started) {
            ESP_LOGE(TAG, "Failed to create mesh health task");
        }
    }
    ESP_LOGI(TAG, "Mesh-Lite started: ID=%02X, role=%s, AP=%s, uplink=%u, mesh_health=%s",
             mesh_id, IsRoot ? "root" : "node", s_softap_ssid, s_uplink_index,
             mesh_health_started ? "on" : "OFF");
    return ESP_OK;
}
