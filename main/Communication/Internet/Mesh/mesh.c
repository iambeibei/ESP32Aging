#include <ctype.h>
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
#include "nvs.h"

#include "ConfigData.h"
#include "mesh.h"
#include "dual_net.h"
#include "task_config.h"


#define MESH_MAX_LEVEL 6
/*
 * 每节点 SoftAP 最大接入子节点数：2026-10-16 曾由 5 调到 8（缓解「中间父
 * 断开+根已满」时无替代父的重连循环），同日应现场要求回退为 5。
 * 回退后该场景依靠：重连节奏 5s×2/15s（加快重扫发现）+ 无父自愈 watchdog
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
 * 2026-10-19 OTA 适配（本次）：父节点重启恢复期约 25~40s（外网下载完成 + 3s 延迟 +
 * 重启入网 + SoftAP 就绪，见 MESH_LITE_OTA_INTEGRATION.md 方案 A），10s 重连窗口
 * 覆盖不到，子节点会在父恢复前提前进入重扫、多绕一轮 OTA 重试（90s）。故加长到
 * 10s×5=50s：父恢复 SoftAP 时子仍在重连原父窗口内，直接命中、OTA 传输中断面最小。
 * 代价：中间父永久故障时换父延迟从约 10s 增至约 50s（15s 重扫兜底节奏不变，
 * 仍有界；无父 watchdog 的 180s 软复位线不受影响）。根节点侧无负面：根 STA 30s 内
 * 恢复则 failover 的 30s 触发线不会误触发（FAILOVER_LOST_IP_MS），>30s 故障照旧切换。
 * 后续「逐层重启」方案落地时需重新评估本组参数。
 */
#define MESH_PARENT_RETRY_INTERVAL_SEC 10
#define MESH_PARENT_RETRY_COUNT        5
#define MESH_RESCAN_INTERVAL_SEC       15

/*
 * 父节点满员拒绝快通道阈值（2026-10-20，用户决策）：
 * 子节点连续 N 次因父满员类原因（见 is_parent_reject_reason）断开即经
 * mesh_heal_reject_fast_path() 重启，不等 180s 无父 watchdog。按库重连
 * 节奏（10s×5/15s 重扫），N=5 约 50~100s 触发。复用 watchdog 的 RTC
 * 重启预算（MESH_RESTART_MAX_PER_CYCLE）防"根持续满员时无限重启"。
 */
#define MESH_PARENT_REJECT_RESTART_THRESHOLD 5

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
 *      强制库走「扫描匹配节点并连接」的 fresh 路径，绕开可能卡住的
 *      内部重试状态与 RTC 原父偏好；不重启、不动老化任务、不丢数据；
 *   L2 软复位 MESH_SOFT_RESET_MAX 次仍无父 → esp_restart()；
 *   L3 每上电周期重启达 MESH_RESTART_MAX_PER_CYCLE（RTC 计数，
 *      ESP_RST_SW 延续、上电/Brownout 清零，镜像 failover 范式）→
 *      永久放弃重启，只限频打日志并继续扫描，回到「无限期存活于
 *      无上行状态」（CHILD_NODE_ROOT_LOSS_DESIGN.md 7.2/7.7）。
 *
 * 背景：现场实测重启可恢复「中间父断开 + 根已满」的卡死；
 * 软复位用官方 API 达成同等 scan-first 效果且不杀老化会话，
 * 故作为第一级手段，重启仅作兜底。根掉线场景最坏时间线：
 * 约 12/24/36 分钟三次重启后稳定存活（每次软复位有 3 分钟观察窗）。
 */
#define MESH_PARENT_LOST_SOFT_RESET_MS 180000 /* 连续无父 3 分钟后软复位 */
#define MESH_SOFT_RESET_MAX             3      /* 本次无父期间软复位次数上限 */
#define MESH_RESTART_MAX_PER_CYCLE      3      /* 每上电周期 watchdog 重启次数上限 */
#define MESH_HEAL_RTC_MAGIC             0x4D484C57U /* "MHLW" */
#define MESH_HEAL_GIVEUP_LOG_INTERVAL_MS 60000     /* 放弃重启后的限频日志周期 */
#define MESH_HEAL_SOFT_RESET_GAP_MS     100        /* disconnect→connect 间隔与重启前日志落盘等待 */

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
 * 连续父满员拒绝计数：事件任务写（STA_DISCONNECTED 分支）、健康监视任务读
 * （mesh_heal_reject_fast_path），__atomic_* RELAXED 同步。仅子节点累加
 * （根无父概念，由 root_failover_task 管理）；GOT_IP 清零保证"连续"语义。
 */
static atomic_uint s_parent_reject_count;

/* 只有 net_monitor 访问此标志；SDK 调用可产生新的断开事件，先标记再调用。
 * disconnect 返回失败时内部可能已暂停重连，仍须在有线失效时执行一次 connect。
 * 这是每次状态转换的一次控制尝试，不在断开事件中反复调用。 */
static bool s_sta_reconnect_paused_by_eth;

void mesh_root_manage_sta_reconnect(bool wired_preferred, bool sta_has_ip)
{
    if (IsRoot != 1) return;
    bool pause = wired_preferred && (s_sta_reconnect_paused_by_eth || !sta_has_ip);
    if (pause == s_sta_reconnect_paused_by_eth) return;
    s_sta_reconnect_paused_by_eth = pause;
    if (pause) {
        esp_err_t err = esp_mesh_lite_disconnect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "有线接管：暂停 STA 重连请求返回 %s（STA 可能已断开）", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "有线接管：已暂停 STA 内部重连，保留根身份与 SoftAP");
        }
    } else {
        ESP_LOGI(TAG, "有线上行失效：恢复根节点 STA 重连");
        esp_mesh_lite_connect();
    }
}

uint8_t mesh_get_current_level(void)
{
    return esp_mesh_lite_get_level();
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
        /* L1：软复位。重置计时给本次软复位一个完整观察窗。 */
        s_soft_reset_count++;
        s_parent_lost_since = now;
        ESP_LOGW(TAG, "Parent lost %u ms; mesh soft reset %u/%u (disconnect+connect)",
                 (unsigned)lost_ms, (unsigned)s_soft_reset_count, (unsigned)MESH_SOFT_RESET_MAX);
        esp_err_t err = esp_mesh_lite_disconnect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_mesh_lite_disconnect: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(MESH_HEAL_SOFT_RESET_GAP_MS));
        /* esp_mesh_lite_connect() 为 void 返回（esp_mesh_lite_core.h）：扫描匹配
         * 节点并连接，结果由后续 mesh_health 心跳与 STA IP 日志体现。 */
        esp_mesh_lite_connect();
        return;
    }

    /* L2：软复位额度用尽 → 重启（受每上电周期预算约束）。 */
    unsigned long restarts = __atomic_load_n(&s_heal_restart_count, __ATOMIC_RELAXED);
    if (restarts < MESH_RESTART_MAX_PER_CYCLE) {
        __atomic_store_n(&s_heal_restart_count, (atomic_uint_least32_t)(restarts + 1), __ATOMIC_RELAXED);
        ESP_LOGW(TAG, "Parent lost %u ms; soft reset x%u ineffective; restarting node (%lu/%u this power cycle)",
                 (unsigned)lost_ms, (unsigned)MESH_SOFT_RESET_MAX,
                 (unsigned long)(restarts + 1), (unsigned)MESH_RESTART_MAX_PER_CYCLE);
        vTaskDelay(pdMS_TO_TICKS(MESH_HEAL_SOFT_RESET_GAP_MS)); /* 给日志落盘留时间 */
        esp_restart();
        return; /* 不会到达 */
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
 * 父满员拒绝快通道（2026-10-20）：与 mesh_heal_watchdog 并列的独立分支，
 * 不改 watchdog 既有 L1/L2/L3 逻辑。连续 MESH_PARENT_REJECT_RESTART_THRESHOLD
 * 次因父满员类原因（is_parent_reject_reason）断开即重启——比 watchdog 的
 * 180s 无父软复位更快恢复「多节点同时上电撞满员父」现场。
 *
 * 复用 watchdog 的 RTC 重启预算（s_heal_restart_count /
 * MESH_RESTART_MAX_PER_CYCLE）：根持续满员时重启也无济于事，预算耗尽后
 * 清零计数并交回 watchdog L3（继续扫描、无上行存活），防疯循环。
 *
 * 调用上下文：仅子节点、仅由 mesh_health_task 串行调用（掉线态 5s 周期，
 * 故重启最多延迟 5s 执行）；计数器 s_parent_reject_count 由事件任务累加、
 * 本函数消费（__atomic_* RELAXED 同步）。
 */
static void mesh_heal_reject_fast_path(void)
{
    unsigned rejects = __atomic_load_n(&s_parent_reject_count, __ATOMIC_RELAXED);
    if (rejects < MESH_PARENT_REJECT_RESTART_THRESHOLD) {
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
        __atomic_store_n(&s_parent_reject_count, 0, __ATOMIC_RELAXED);
        ESP_LOGW(TAG,
                 "Parent rejected %u times; restart budget exhausted (%lu/%u); "
                 "keep scanning, no more restarts this power cycle",
                 rejects, (unsigned long)restarts, (unsigned)MESH_RESTART_MAX_PER_CYCLE);
        return;
    }

    __atomic_store_n(&s_heal_restart_count, (atomic_uint_least32_t)(restarts + 1), __ATOMIC_RELAXED);
    ESP_LOGW(TAG,
             "Parent rejected %u consecutive times (full/reject); restarting node (%lu/%u this power cycle)",
             rejects, (unsigned long)(restarts + 1), (unsigned)MESH_RESTART_MAX_PER_CYCLE);
    vTaskDelay(pdMS_TO_TICKS(MESH_HEAL_SOFT_RESET_GAP_MS)); /* 给日志落盘留时间 */
    esp_restart();
    /* 不会到达 */
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
        uint32_t interval_ms = online ? MESH_HEALTH_OK_INTERVAL_MS : MESH_HEALTH_LOST_INTERVAL_MS;

        if (online != was_online || last_report_tick == 0 ||
            (now - last_report_tick) >= pdMS_TO_TICKS(interval_ms))
        {
            ESP_LOGI(TAG,
                     "mesh_health: parent=%s, level=%u, network_flag=%u, last_sta_reason=%u, internal_free_bytes=%u",
                     online ? "online" : "LOST",
                     (unsigned)esp_mesh_lite_get_level(),
                     online ? 1U : 0U,
                     (unsigned)__atomic_load_n(&s_last_sta_disconnect_reason, __ATOMIC_RELAXED),
                     (unsigned)esp_get_free_internal_heap_size());
            last_report_tick = now;
            was_online = online;
        }

        /* 无父自愈：分级 escalate（软复位 → 重启 → 放弃），只看父链路。 */
        mesh_heal_watchdog(online, now);

        /*
         * 父满员拒绝快通道：连续被父满员拒绝达阈值即重启（复用 RTC 预算），
         * 与 watchdog 并列——两者都只看父链路，触发条件互不重叠
         * （watchdog 管"无父超时"，快通道管"连续被拒"）。
         */
        mesh_heal_reject_fast_path();

        /* 按当前态周期休眠：在线时 60s 一轮，无需每 5s 空转唤醒。 */
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
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
 * 父节点满员拒绝类断开原因（ESP Wi-Fi reason code，esp_wifi_types.h）：
 *   2  AUTH_EXPIRE   认证阶段 AP 无响应——满员 SoftAP 忽略 auth 的典型表现
 *   4  ASSOC_EXPIRE  关联阶段超时
 *   5  ASSOC_TOOMANY AP 关联数已满（iot_bridge "max connection" 现场特征）
 *   6  NOT_AUTHED    未认证即尝试关联被拒
 * 仅子节点使用（根无父概念）。现场实测若命中其他 reason，按串口
 * "STA disconnected, reason=N" 日志扩展本集合。
 */
static bool is_parent_reject_reason(uint8_t reason)
{
    switch (reason) {
    case 2:
    case 4:
    case 5:
    case 6:
        return true;
    default:
        return false;
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
        /*
         * 成功拿到 IP：清零连续满员拒绝计数，保证"连续被拒"语义
         * （中间成功过即重新累计，见 mesh_heal_reject_fast_path）。
         */
        __atomic_store_n(&s_parent_reject_count, 0, __ATOMIC_RELAXED);
        ESP_LOGI(TAG, "STA IP " IPSTR ", level=%u, role=%s",
                 IP2STR(&event->ip_info.ip), esp_mesh_lite_get_level(),
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
        if (s_trial_active) s_trial_success = false;
        /*
         * 断开原因落诊断变量，供子节点健康监视与「根掉线不重启」排查使用。
         * WIFI_EVENT_STA_DISCONNECTED 的 event_data 为 wifi_event_sta_disconnected_t；
         * IP_EVENT_STA_LOST_IP 无该结构，reason 保持上次已知值。
         */
        if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED && event_data != NULL) {
            const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)event_data;
            __atomic_store_n(&s_last_sta_disconnect_reason, disc->reason, __ATOMIC_RELAXED);
            /* ssid 非保证 NUL 终止：用精度限定打印，并对 ssid_len 做防御性截断。 */
            unsigned print_len = disc->ssid_len;
            if (print_len > sizeof(disc->ssid) - 1) {
                print_len = sizeof(disc->ssid) - 1;
            }
            ESP_LOGW(TAG, "STA disconnected, reason=%u, ssid=%.*s; local Mesh-Lite remains active",
                     (unsigned)disc->reason, (int)print_len, (const char *)disc->ssid);

            /*
             * 父满员拒绝计数（仅子节点；根连路由器无父概念）。累计值由
             * mesh_health_task 的 mesh_heal_reject_fast_path 消费（达阈值
             * MESH_PARENT_REJECT_RESTART_THRESHOLD 重启）。非拒绝类断开
             * （库正常换父/重扫等）不计数，避免误触。
             * 事件上下文仅一次原子加与比较，无阻塞。
             */
            if (IsRoot != 1 && is_parent_reject_reason((uint8_t)disc->reason)) {
                unsigned rejects = __atomic_fetch_add(&s_parent_reject_count, 1, __ATOMIC_RELAXED) + 1;
                if (rejects < MESH_PARENT_REJECT_RESTART_THRESHOLD) {
                    ESP_LOGW(TAG, "Parent rejected (reason=%u), consecutive=%u/%u; waiting",
                             (unsigned)disc->reason, rejects,
                             (unsigned)MESH_PARENT_REJECT_RESTART_THRESHOLD);
                }
            }
        } else {
            ESP_LOGW(TAG, "STA uplink lost; local Mesh-Lite remains active");
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
    wifi_ap_record_t *records = calloc(count, sizeof(*records));
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
 * 存储」，且仅在根节点故障转移重启路径调用。RTC 内存为直接写访问，无需等待。
 */
static void mesh_failover_restart(const char *reason)
{
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
     * 当前为 5s×2/15s 折中值，加快中间父断开后的重扫发现，
     * 同时不回到库默认的激进节奏；持续无父场景由无父
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
        /* 根掉线期间子节点必须存活：只观测不动作的健康监视，见设计文档 7.2 节。 */
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
