#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_bridge.h"
#include "esp_eth.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "dual_net.h"
#include "mesh.h"
#include "mqtt_app.h"
#include "task_config.h"
#include "ConfigData.h"
#include "log.h"

static const char *TAG = "dual_net";
static esp_netif_t *eth_netif;
static esp_netif_t *wifi_netif;

/* H2 诊断：update_route 因 revision 竞争跳过时的限频日志周期（同因 10s 内一条）。 */
#define LG_DUAL_NET_SKIP_LOG_MS 10000

/* 事件只发布状态；路由、DNS、探测和 Mesh-Lite 控制只有 net_monitor 执行。
 * 锁内仅复制状态，不调用 SDK、回调或阻塞 API。netif 生命周期由 bridge 管理。 */
typedef enum {
    WIFI_STATE_IDLE,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISABLED,
    WIFI_STATE_AUTH_FAILED,
} wifi_state_t;

typedef struct {
    bool initialized;
    bool eth_link_raw;
    bool eth_link_pending;
    bool eth_link_up;
    bool eth_up;
    bool eth_ready;
    bool wifi_up;
    bool wifi_connecting;
    bool wifi_auth_failed;
    bool mesh_switching;
    TickType_t eth_link_change_tick;
    esp_netif_ip_info_t eth_ip;
    uint32_t revision;
    uint32_t eth_generation;
    uint32_t wifi_generation;
    uint8_t probe_ok_count;
    uint8_t probe_fail_count;
    wifi_state_t wifi_state;
    dual_net_uplink_t active_uplink;
    dual_net_uplink_t requested_uplink;
} DualNetState;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static DualNetState s_state;
static TaskHandle_t s_monitor_task_handle;
static network_ready_cb_t s_network_ready_callback;
static dual_net_uplink_cb_t s_uplink_change_callback;

static DualNetState dual_net_snapshot(void)
{
    portENTER_CRITICAL(&s_state_lock);
    DualNetState state = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

static void dual_net_wake_monitor(void)
{
    portENTER_CRITICAL(&s_state_lock);
    TaskHandle_t task = s_monitor_task_handle;
    portEXIT_CRITICAL(&s_state_lock);
    if (task != NULL) {
        /* 仅任务/事件上下文调用；监控任务成功创建后不会被删除。 */
        xTaskNotifyGive(task);
    }
}

static bool dual_net_eth_online(const DualNetState *state)
{
    return state->eth_link_raw && state->eth_up && state->eth_ip.ip.addr != 0;
}

/* WiFi 正常时必须先探测通过才抢占；WiFi 断开时立即尝试唯一有线出口。 */
static dual_net_uplink_t dual_net_choose_uplink(const DualNetState *state)
{
    bool eth_online = dual_net_eth_online(state);
    if (state->requested_uplink == DUAL_NET_UPLINK_ETH && eth_online) {
        return DUAL_NET_UPLINK_ETH;
    }
    if (state->requested_uplink == DUAL_NET_UPLINK_WIFI && state->wifi_up) {
        return DUAL_NET_UPLINK_WIFI;
    }
    if (eth_online && (state->eth_ready || !state->wifi_up)) {
        return DUAL_NET_UPLINK_ETH;
    }
    return state->wifi_up ? DUAL_NET_UPLINK_WIFI : DUAL_NET_UPLINK_NONE;
}

static bool dual_net_read_ip(esp_netif_t *netif, esp_netif_ip_info_t *ip)
{
    return netif != NULL && esp_netif_is_netif_up(netif) &&
           esp_netif_get_ip_info(netif, ip) == ESP_OK && ip->ip.addr != 0;
}

/* 取得/丢失 IP 或物理链路改变时递增代次，丢弃旧链路的在途探测结果。锁内调用。 */
static void dual_net_clear_eth_probe(void)
{
    s_state.eth_generation++;
    s_state.eth_ready = false;
    s_state.probe_ok_count = 0;
    s_state.probe_fail_count = 0;
}

/*
 * 探测配置中的 MQTT IPv4 地址 SERVER_IP:SERVER_UDP_Port（历史键名，实际为 TCP）。
 * 同时绑定指定网卡与其源 IP，避免尚未切换默认路由时误探测到 WiFi。
 * 只验证 TCP 建连，不发送 MQTT 报文；端口拒绝连接也算失败。
 * 使用非阻塞 connect + select，将建连等待限制在探测超时内。
 * 只在监控任务调用，不可在 ISR/事件回调调用；每轮独占并关闭自己的 socket。
 */
static bool dual_net_probe_uplink_reachable(esp_netif_t *netif)
{
    if (netif == NULL)
    {
        return false;
    }

    if (SERVER_IP == NULL || SERVER_UDP_Port <= 0 || SERVER_UDP_Port > UINT16_MAX)
    {
        ESP_LOGW(TAG, "MQTT 探测地址或端口无效，检查 SERVER_IP/SERVER_UDP_Port");
        return false;
    }

    struct sockaddr_in target_addr = {0};
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons((uint16_t)SERVER_UDP_Port);
    if (inet_pton(AF_INET, SERVER_IP, &target_addr.sin_addr) != 1)
    {
        ESP_LOGW(TAG, "MQTT 探测需要 IPv4 地址，SERVER_IP=%s", SERVER_IP);
        return false;
    }

    esp_netif_ip_info_t ip_info = {0};
    struct ifreq iface = {0};
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0 ||
        esp_netif_get_netif_impl_name(netif, iface.ifr_name) != ESP_OK)
    {
        ESP_LOGW(TAG, "MQTT 探测网卡未就绪：%s", esp_netif_get_ifkey(netif));
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        ESP_LOGW(TAG, "MQTT 探测 socket 创建失败：errno=%d", errno);
        return false;
    }

    bool reachable = false;
    int probe_errno = 0;
    const char *stage = "绑定网卡";
    if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, &iface, sizeof(iface)) != 0)
    {
        probe_errno = errno;
        goto cleanup;
    }

    struct sockaddr_in source_addr = {0};
    source_addr.sin_family = AF_INET;
    source_addr.sin_addr.s_addr = ip_info.ip.addr;
    stage = "绑定源地址";
    if (bind(sock, (struct sockaddr *)&source_addr, sizeof(source_addr)) != 0)
    {
        probe_errno = errno;
        goto cleanup;
    }

    stage = "设置非阻塞";
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) != 0)
    {
        probe_errno = errno;
        goto cleanup;
    }

    stage = "TCP 建连";
    int64_t deadline_us = esp_timer_get_time() + (int64_t)LG_DUAL_NET_UPLINK_PROBE_TIMEOUT_MS * 1000;
    if (connect(sock, (struct sockaddr *)&target_addr, sizeof(target_addr)) == 0)
    {
        reachable = true;
        goto cleanup;
    }
    probe_errno = errno;
    if (probe_errno != EINPROGRESS)
    {
        goto cleanup;
    }

    for (;;)
    {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        stage = "等待 TCP 建连";
        if (remaining_us <= 0)
        {
            probe_errno = ETIMEDOUT;
            break;
        }
        struct timeval timeout = {
            .tv_sec = remaining_us / 1000000,
            .tv_usec = remaining_us % 1000000,
        };
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sock, &write_fds);
        int selected = select(sock + 1, NULL, &write_fds, NULL, &timeout);
        if (selected < 0)
        {
            probe_errno = errno;
            if (probe_errno == EINTR)
            {
                continue; // 使用原截止时间重试，不延长总等待时间。
            }
            break;
        }
        if (selected == 0)
        {
            probe_errno = ETIMEDOUT;
            break;
        }
        stage = "TCP 建连结果";
        socklen_t error_len = sizeof(probe_errno);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &probe_errno, &error_len) != 0)
        {
            probe_errno = errno;
            break;
        }
        reachable = (probe_errno == 0);
        break;
    }

cleanup:
    if (close(sock) != 0)
    {
        ESP_LOGW(TAG, "关闭 MQTT 探测 socket 失败：errno=%d", errno);
    }
    ESP_LOGD(TAG, "探测 %s(%s) -> MQTT %s:%d（源 IP=" IPSTR "）结果：%s，阶段=%s errno=%d",
             esp_netif_get_ifkey(netif), iface.ifr_name, SERVER_IP, SERVER_UDP_Port,
             IP2STR(&ip_info.ip), reachable ? "通" : "不通", stage, probe_errno);
    return reachable;
}

bool dual_net_is_initialized(void)
{
    return dual_net_snapshot().initialized;
}

bool dual_net_has_eth_ip(void)
{
    DualNetState state = dual_net_snapshot();
    return state.initialized && dual_net_eth_online(&state);
}

bool dual_net_is_eth_uplink_ready(void)
{
    DualNetState state = dual_net_snapshot();
    return dual_net_eth_online(&state) && state.eth_ready;
}

bool dual_net_is_uplink_ready(void)
{
    DualNetState state = dual_net_snapshot();
    return dual_net_eth_online(&state) || state.wifi_up;
}

dual_net_uplink_state_t dual_net_get_uplink_state(void)
{
    DualNetState state = dual_net_snapshot();
    if (dual_net_eth_online(&state)) {
        if (state.eth_ready) return DUAL_NET_UPLINK_STATE_READY;
        return state.probe_fail_count >= LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT
                   ? DUAL_NET_UPLINK_STATE_STALE : DUAL_NET_UPLINK_STATE_PROBING;
    }
    return state.wifi_up ? DUAL_NET_UPLINK_STATE_READY : DUAL_NET_UPLINK_STATE_NONE;
}

/* net_monitor 调用。先确认 SDK 成功且状态未变，再发布实际选择与回调。 */
static bool dual_net_update_route(void)
{
    DualNetState state = dual_net_snapshot();
    dual_net_uplink_t target = dual_net_choose_uplink(&state);
#if DUAL_NET_ENABLE_ROUTE_CONTROL
    if (target != DUAL_NET_UPLINK_NONE) {
        esp_netif_t *netif = target == DUAL_NET_UPLINK_ETH ? eth_netif : wifi_netif;
        if (netif == NULL) return false;
        esp_err_t err = esp_netif_set_default_netif(netif);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "设置默认路由失败：uplink=%d, %s", target, esp_err_to_name(err));
            return false;
        }
    }
#else
    esp_netif_t *current = esp_netif_get_default_netif();
    target = current == eth_netif && dual_net_eth_online(&state) ? DUAL_NET_UPLINK_ETH
             : current == wifi_netif && state.wifi_up ? DUAL_NET_UPLINK_WIFI
             : DUAL_NET_UPLINK_NONE;
#endif
    portENTER_CRITICAL(&s_state_lock);
    if (s_state.revision != state.revision) {
        /*
         * H2 诊断：快照之后 revision 又变化，本轮 Network_Flag 发布被跳过。
         * 若 mesh-lite 重试风暴使该分支持续命中，根节点标志将停在旧值，
         * 主备路由器切换的扫描触发条件永不成立。限频打印便于证实/证伪。
         */
        uint32_t skipped_revision = s_state.revision;
        portEXIT_CRITICAL(&s_state_lock);
        dual_net_wake_monitor();
        static TickType_t last_skip_log_tick = 0;
        TickType_t skip_now = xTaskGetTickCount();
        if (last_skip_log_tick == 0 ||
            (skip_now - last_skip_log_tick) >= pdMS_TO_TICKS(LG_DUAL_NET_SKIP_LOG_MS)) {
            last_skip_log_tick = skip_now;
            ESP_LOGW(TAG, "Route reconcile skipped: revision changed %lu -> %lu during snapshot",
                     (unsigned long)state.revision, (unsigned long)skipped_revision);
        }
        return false;
    }
    bool changed = s_state.active_uplink != target;
    s_state.active_uplink = target;
    dual_net_uplink_cb_t callback = s_uplink_change_callback;
    portEXIT_CRITICAL(&s_state_lock);
    if (changed) {
        ESP_LOGI(TAG, "Default route -> %s, active=%d, eth_ip=%d probe_ready=%d wifi_ip=%d",
                 target == DUAL_NET_UPLINK_ETH ? "Ethernet" :
                 target == DUAL_NET_UPLINK_WIFI ? "WiFi" : "NONE",
                 target, dual_net_eth_online(&state), state.eth_ready, state.wifi_up);
        if (callback != NULL) callback(target);
    }
    return true;
}

/* 保留兼容入口；初始化后只唤醒唯一写者，不在事件回调中用旧状态裁决。 */
void dual_net_arbitrate_network_flag(void)
{
    dual_net_wake_monitor();
}

/* net_monitor 是初始化后根节点 Network_Flag 的唯一写者。
 * 标志描述本机链路/IP条件，MQTT服务器状态由探测和MQTT客户端分别维护。 */
static void dual_net_publish_network_flag(void)
{
#if DUAL_NET_ENABLE_ROUTE_CONTROL
    DualNetState state = dual_net_snapshot();
    uint8_t ready = dual_net_eth_online(&state) || state.wifi_up;
    uint8_t previous = __atomic_exchange_n(&Network_Flag, ready, __ATOMIC_ACQ_REL);
    if (previous != ready) {
        ESP_LOGI(TAG, "Network_Flag <- %u (eth_ip=%d wifi_ip=%d probe_ready=%d active=%d)",
                 ready, dual_net_eth_online(&state), state.wifi_up,
                 state.eth_ready, state.active_uplink);
    }
#endif
}

esp_err_t dual_net_refresh_dns_to_softap(void)
{
    esp_netif_t *netif = dual_net_get_uplink_netif(dual_net_get_active_uplink());
    if (netif == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_bridge_update_dns_info(netif, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "向子节点下发上行 DNS 失败：%s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "已将 %s DNS 下发给数据转发 netif（含 SoftAP）", esp_netif_get_ifkey(netif));
    }
    return err;
}

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != ETHERNET_EVENT_CONNECTED && id != ETHERNET_EVENT_DISCONNECTED &&
        id != ETHERNET_EVENT_STOP) return;
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&s_state_lock);
    s_state.eth_link_raw = id == ETHERNET_EVENT_CONNECTED;
    s_state.eth_link_pending = true;
    s_state.eth_link_change_tick = now;
    s_state.eth_up = false;
    dual_net_clear_eth_probe();
    s_state.revision++;
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "Ethernet Link %s（原始事件，等待去抖确认）",
             id == ETHERNET_EVENT_CONNECTED ? "Up" : "Down");
    dual_net_wake_monitor();

    /* 延用已有 DHCP 重启策略；不在事件回调中探测、改路由或操作 Mesh-Lite。 */
    if (id == ETHERNET_EVENT_CONNECTED && eth_netif != NULL) {
        esp_err_t err = esp_netif_dhcpc_stop(eth_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_LOGW(TAG, "dhcpc_stop: %s", esp_err_to_name(err));
        }
        err = esp_netif_dhcpc_start(eth_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "dhcpc_start: %s", esp_err_to_name(err));
        }
    }
}

static void dual_net_ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        if (event == NULL || event->esp_netif != eth_netif ||
            !esp_netif_is_netif_up(eth_netif) || event->ip_info.ip.addr == 0) return;
        portENTER_CRITICAL(&s_state_lock);
        if (!s_state.eth_up || memcmp(&s_state.eth_ip, &event->ip_info, sizeof(event->ip_info)) != 0) {
            dual_net_clear_eth_probe();
        }
        /* 有效 GOT_IP 也覆盖初始化时可能错过的 LINK_UP。相同地址续租不清空探测。 */
        s_state.eth_ip = event->ip_info;
        s_state.eth_link_raw = true;
        s_state.eth_up = true;
        s_state.revision++;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI(TAG, "Ethernet Got IP: " IPSTR ", gw=" IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        portENTER_CRITICAL(&s_state_lock);
        s_state.eth_up = false;
        dual_net_clear_eth_probe();
        s_state.revision++;
        portEXIT_CRITICAL(&s_state_lock);
    } else if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        if (event == NULL || event->esp_netif != wifi_netif ||
            !esp_netif_is_netif_up(wifi_netif) || event->ip_info.ip.addr == 0) return;
        portENTER_CRITICAL(&s_state_lock);
        s_state.wifi_up = true;
        s_state.wifi_connecting = false;
        s_state.wifi_auth_failed = false;
        s_state.mesh_switching = false;
        s_state.wifi_state = WIFI_STATE_CONNECTED;
        s_state.wifi_generation++;
        s_state.revision++;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI(TAG, "WiFi Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    } else if (id == IP_EVENT_STA_LOST_IP) {
        portENTER_CRITICAL(&s_state_lock);
        s_state.wifi_up = false;
        s_state.wifi_connecting = false;
        s_state.wifi_state = s_state.wifi_auth_failed ? WIFI_STATE_AUTH_FAILED : WIFI_STATE_IDLE;
        s_state.wifi_generation++;
        s_state.revision++;
        portEXIT_CRITICAL(&s_state_lock);
    } else {
        return;
    }
    dual_net_wake_monitor();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        if (event == NULL) return;
        uint32_t wifi_generation;
        uint32_t revision;
        portENTER_CRITICAL(&s_state_lock);
        s_state.wifi_up = false;
        s_state.wifi_connecting = false;
        s_state.wifi_auth_failed = event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                                  event->reason == WIFI_REASON_AUTH_FAIL;
        s_state.wifi_state = s_state.wifi_auth_failed ? WIFI_STATE_AUTH_FAILED : WIFI_STATE_IDLE;
        s_state.wifi_generation++;
        s_state.revision++;
        wifi_generation = s_state.wifi_generation;
        revision = s_state.revision;
        portEXIT_CRITICAL(&s_state_lock);
        /* 包括 NO_AP_FOUND，不能在完成状态发布前提前返回。
         * 诊断：generation/revision 用于与 net_monitor 的标志发布日志对齐时序。 */
        ESP_LOGI(TAG, "WiFi disconnected, reason=%u, wifi_generation=%lu, revision=%lu",
                 event->reason, (unsigned long)wifi_generation, (unsigned long)revision);
    } else if (id == WIFI_EVENT_STA_CONNECTED || id == WIFI_EVENT_STA_START) {
        portENTER_CRITICAL(&s_state_lock);
        s_state.wifi_connecting = id == WIFI_EVENT_STA_CONNECTED;
        if (id == WIFI_EVENT_STA_START) s_state.wifi_up = false;
        s_state.wifi_state = s_state.wifi_connecting ? WIFI_STATE_CONNECTING : WIFI_STATE_IDLE;
        s_state.wifi_generation++;
        s_state.revision++;
        portEXIT_CRITICAL(&s_state_lock);
    } else {
        return;
    }
    dual_net_wake_monitor();
}

static void dual_net_process_link_debounce(void)
{
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&s_state_lock);
    uint32_t debounce_ms = s_state.eth_link_raw ? LG_DUAL_NET_ETH_LINK_UP_DEBOUNCE_MS
                                               : LG_DUAL_NET_ETH_LINK_DOWN_DEBOUNCE_MS;
    bool confirmed = s_state.eth_link_pending &&
                     now - s_state.eth_link_change_tick >= pdMS_TO_TICKS(debounce_ms);
    bool online = s_state.eth_link_raw;
    if (confirmed) {
        s_state.eth_link_pending = false;
        s_state.eth_link_up = online;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (confirmed) ESP_LOGI(TAG, "网线状态已确认：%s", online ? "已插入" : "已拔出");
}

static void dual_net_process_uplink_probe(void)
{
    DualNetState before = dual_net_snapshot();
    if (!dual_net_eth_online(&before)) return;
    bool reachable = dual_net_probe_uplink_reachable(eth_netif);
    portENTER_CRITICAL(&s_state_lock);
    if (s_state.eth_generation != before.eth_generation || !dual_net_eth_online(&s_state)) {
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI(TAG, "丢弃链路/IP变化前的探测结果");
        return;
    }
    bool was_ready = s_state.eth_ready;
    uint8_t previous_fail_count = s_state.probe_fail_count;
    if (reachable) {
        s_state.probe_fail_count = 0;
        if (s_state.probe_ok_count < LG_DUAL_NET_UPLINK_PROBE_OK_COUNT) s_state.probe_ok_count++;
        if (s_state.probe_ok_count >= LG_DUAL_NET_UPLINK_PROBE_OK_COUNT) s_state.eth_ready = true;
    } else {
        s_state.probe_ok_count = 0;
        if (s_state.probe_fail_count < LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT) s_state.probe_fail_count++;
        if (s_state.probe_fail_count >= LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT) s_state.eth_ready = false;
    }
    bool ready = s_state.eth_ready;
    bool failed = previous_fail_count < LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT &&
                  s_state.probe_fail_count >= LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT;
    s_state.revision++;
    portEXIT_CRITICAL(&s_state_lock);
    if (!was_ready && ready) ESP_LOGI(TAG, "以太网连通性已确认，允许以太网优先");
    if (failed) ESP_LOGW(TAG, "以太网探测连续失败，恢复无线重连；有线 IP 状态单独保留");
}

static void dual_net_start_wifi(void)
{
#if DUAL_NET_ENABLE_WIFI_FALLBACK
    DualNetState state = dual_net_snapshot();
    if (state.mesh_switching || dual_net_eth_online(&state) || state.wifi_up ||
        state.wifi_connecting || state.wifi_auth_failed) return;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_state.wifi_connecting = true;
    s_state.wifi_state = WIFI_STATE_CONNECTING;
    s_state.revision++;
    portEXIT_CRITICAL(&s_state_lock);
#endif
}

static void dual_net_check_ready_callback(void)
{
    portENTER_CRITICAL(&s_state_lock);
    network_ready_cb_t callback = NULL;
    if (dual_net_eth_online(&s_state) || s_state.wifi_up) {
        callback = s_network_ready_callback;
        s_network_ready_callback = NULL;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (callback != NULL) callback();
}

/* 事件唤醒负责立即裁决，周期截止时间负责探测/重试；事件频率不改变阈值计数。 */
static void dual_net_monitor_task(void *arg)
{
    /* 创建者发布句柄和初始化状态后才启动，不依赖任务调度顺序。 */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#if DUAL_NET_ENABLE_WIFI_FALLBACK
    const int64_t period_us = (int64_t)LG_DUAL_NET_MONITOR_ACTIVE_PERIOD_MS * 1000;
#else
    const int64_t period_us = (int64_t)LG_DUAL_NET_MONITOR_IDLE_PERIOD_MS * 1000;
#endif
    int64_t next_probe_us = 0;
    int64_t next_retry_us = 0;
    uint32_t probe_generation = UINT32_MAX;
    uint32_t applied_revision = UINT32_MAX;
    unsigned check_counter = 0;
    unsigned auth_fail_counter = 0;
    for (;;) {
        dual_net_process_link_debounce();
        DualNetState state = dual_net_snapshot();
        if (state.revision != applied_revision) {
            uint32_t reconcile_revision = state.revision;
            if (dual_net_update_route()) {
                bool dns_updated = true;
                dual_net_publish_network_flag();
#if DUAL_NET_ENABLE_ROUTE_CONTROL
                /* 首先保障路由/根身份，再暂停内部重连；所有 SDK 调用均在锁外。 */
                state = dual_net_snapshot();
                bool wired_preferred = dual_net_eth_online(&state) &&
                                       state.active_uplink == DUAL_NET_UPLINK_ETH &&
                                       state.probe_fail_count < LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT;
                mesh_root_manage_sta_reconnect(wired_preferred, state.wifi_up);
                if (state.active_uplink != DUAL_NET_UPLINK_NONE) {
                    esp_err_t err = dual_net_refresh_dns_to_softap();
                    dns_updated = err == ESP_OK;
                }
#endif
                /* DNS 失败时下一检查周期重试，避免自行通知造成忙循环。 */
                /* SDK/回调期间的新事件必须留到下一轮，不能标成已应用。 */
                if (dns_updated) applied_revision = reconcile_revision;
            }
        }
        /* 非阻塞服务延后的MQTT请求；同一出口的周期探测不产生新重连请求。 */
        mqtt_app_process_uplink_reconnect();
        dual_net_check_ready_callback();
        state = dual_net_snapshot();
        int64_t now_us = esp_timer_get_time();
        if (state.eth_generation != probe_generation) {
            probe_generation = state.eth_generation;
            next_probe_us = 0; // 新 IP 首次立即探测，随后保留原周期。
        }
        if (dual_net_eth_online(&state) && now_us >= next_probe_us) {
            dual_net_process_uplink_probe();
            next_probe_us = esp_timer_get_time() + period_us;
            continue; // 根据新探测结果立即重新裁决。
        }
        if (now_us >= next_retry_us) {
            next_retry_us = now_us + period_us;
            check_counter++;
            if (state.wifi_auth_failed) {
                if (++auth_fail_counter >= LG_DUAL_NET_AUTH_RETRY_MAX_COUNT) {
                    portENTER_CRITICAL(&s_state_lock);
                    s_state.wifi_auth_failed = false;
                    s_state.wifi_state = WIFI_STATE_IDLE;
                    s_state.revision++;
                    portEXIT_CRITICAL(&s_state_lock);
                    auth_fail_counter = 0;
                }
            } else {
                auth_fail_counter = 0;
            }
            if (check_counter % LG_DUAL_NET_WIFI_RETRY_INTERVAL == 0) dual_net_start_wifi();
        }
        /* 一秒内完成去抖/路由失败重试；通知可提前唤醒，无新任务或队列。 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LG_DUAL_NET_EVENT_CHECK_PERIOD_MS));
    }
}

esp_err_t dual_net_init(void)
{
#if !DUAL_NET_ENABLE
    return ESP_OK;
#endif
#if DUAL_NET_ROOT_ONLY
    if (IsRoot != 1) return ESP_OK;
#endif
    if (dual_net_is_initialized()) return ESP_OK;
    wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (wifi_netif == NULL) return ESP_ERR_INVALID_STATE;
#if DUAL_NET_ETH_HW_ENABLE
    eth_netif = esp_netif_get_handle_from_ifkey("ETH_WAN");
    if (eth_netif == NULL) ESP_LOGW(TAG, "未找到 ETH_WAN，使用无线接口");
#endif
    esp_event_handler_instance_t eth_handler = NULL, ip_handler = NULL, wifi_handler = NULL;
    esp_err_t err = esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                       eth_event_handler, NULL, &eth_handler);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              dual_net_ip_event_handler, NULL, &ip_handler);
    if (err != ESP_OK) goto cleanup;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, &wifi_handler);
    if (err != ESP_OK) goto cleanup;

    /* 注册后读取现状，覆盖开机已插网线/已有IP的情况；不覆盖查询期间收到的新事件。 */
    DualNetState before = dual_net_snapshot();
    esp_netif_ip_info_t eth_ip = {0}, wifi_ip = {0};
    bool eth_online = dual_net_read_ip(eth_netif, &eth_ip);
    bool wifi_online = dual_net_read_ip(wifi_netif, &wifi_ip);
    portENTER_CRITICAL(&s_state_lock);
    if (s_state.eth_generation == before.eth_generation) {
        s_state.eth_link_raw = eth_online;
        s_state.eth_link_up = eth_online;
        s_state.eth_up = eth_online;
        s_state.eth_ip = eth_ip;
        dual_net_clear_eth_probe();
    }
    if (s_state.wifi_generation == before.wifi_generation) {
        s_state.wifi_up = wifi_online;
        s_state.wifi_state = wifi_online ? WIFI_STATE_CONNECTED : WIFI_STATE_IDLE;
    }
    s_state.revision++;
    portEXIT_CRITICAL(&s_state_lock);
    TaskHandle_t monitor = NULL;
    if (xTaskCreatePinnedToCore(dual_net_monitor_task, "net_monitor", LG_STACK_NET_MONITOR,
                               NULL, LG_PRIO_NET_MONITOR, &monitor, LG_APP_CPU_CORE) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_monitor_task_handle = monitor;
    s_state.initialized = true;
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "Dual Network Init: root, eth=%s, monitor=%p",
             eth_netif != NULL ? esp_netif_get_ifkey(eth_netif) : "NONE", (void *)monitor);
    dual_net_wake_monitor();
    return ESP_OK;

cleanup:
    /* 失败不留下半初始化的处理器；mesh 的 STA 判据继续提供兜底。 */
    if (wifi_handler != NULL) {
        esp_err_t cleanup_err = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
        if (cleanup_err != ESP_OK) ESP_LOGE(TAG, "注销 WIFI handler: %s", esp_err_to_name(cleanup_err));
    }
    if (ip_handler != NULL) {
        esp_err_t cleanup_err = esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_handler);
        if (cleanup_err != ESP_OK) ESP_LOGE(TAG, "注销 IP handler: %s", esp_err_to_name(cleanup_err));
    }
    esp_err_t cleanup_err = esp_event_handler_instance_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_handler);
    if (cleanup_err != ESP_OK) ESP_LOGE(TAG, "注销 ETH handler: %s", esp_err_to_name(cleanup_err));
    storage_write_record_cyclic("", "dual_net_init failed; keep WiFi fallback");
    return err;
}

void dual_net_notify_wifi_up(void)
{
    esp_netif_ip_info_t ip = {0};
    bool online = dual_net_read_ip(wifi_netif, &ip);
    portENTER_CRITICAL(&s_state_lock);
    s_state.wifi_up = online;
    s_state.wifi_connecting = false;
    s_state.wifi_state = online ? WIFI_STATE_CONNECTED : WIFI_STATE_IDLE;
    s_state.wifi_generation++;
    s_state.revision++;
    portEXIT_CRITICAL(&s_state_lock);
    dual_net_wake_monitor();
}

void dual_net_notify_wifi_down(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.wifi_up = false;
    s_state.wifi_connecting = false;
    s_state.wifi_state = WIFI_STATE_IDLE;
    s_state.wifi_generation++;
    s_state.revision++;
    portEXIT_CRITICAL(&s_state_lock);
    dual_net_wake_monitor();
}

void dual_net_set_mesh_switching(bool switching)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.mesh_switching = switching;
    if (switching) {
        s_state.wifi_up = false;
        s_state.wifi_connecting = false;
        s_state.wifi_state = WIFI_STATE_DISABLED;
        s_state.wifi_generation++;
    }
    s_state.revision++;
    portEXIT_CRITICAL(&s_state_lock);
    dual_net_wake_monitor();
}

bool dual_net_is_mesh_switching(void)
{
    return dual_net_snapshot().mesh_switching;
}

dual_net_uplink_t dual_net_get_active_uplink(void)
{
    DualNetState state = dual_net_snapshot();
    if (state.active_uplink == DUAL_NET_UPLINK_ETH && dual_net_eth_online(&state)) return DUAL_NET_UPLINK_ETH;
    if (state.active_uplink == DUAL_NET_UPLINK_WIFI && state.wifi_up) return DUAL_NET_UPLINK_WIFI;
    return DUAL_NET_UPLINK_NONE;
}

const char *dual_net_get_active_interface(void)
{
    dual_net_uplink_t active = dual_net_get_active_uplink();
    return active == DUAL_NET_UPLINK_ETH ? "ETHERNET" : active == DUAL_NET_UPLINK_WIFI ? "WIFI" : "NONE";
}

bool dual_net_is_ethernet_active(void) { return dual_net_get_active_uplink() == DUAL_NET_UPLINK_ETH; }
bool dual_net_is_wifi_active(void) { return dual_net_get_active_uplink() == DUAL_NET_UPLINK_WIFI; }
bool dual_net_is_any_network_up(void) { return dual_net_is_uplink_ready(); }

const char *dual_net_get_wifi_state(void)
{
    switch (dual_net_snapshot().wifi_state) {
    case WIFI_STATE_IDLE: return "IDLE";
    case WIFI_STATE_CONNECTING: return "CONNECTING";
    case WIFI_STATE_CONNECTED: return "CONNECTED";
    case WIFI_STATE_DISABLED: return "DISABLED";
    case WIFI_STATE_AUTH_FAILED: return "AUTH_FAILED";
    default: return "UNKNOWN";
    }
}

void dual_net_register_ready_callback(network_ready_cb_t callback)
{
    portENTER_CRITICAL(&s_state_lock);
    s_network_ready_callback = callback;
    portEXIT_CRITICAL(&s_state_lock);
    dual_net_wake_monitor();
}

esp_netif_t *dual_net_get_uplink_netif(dual_net_uplink_t uplink)
{
    DualNetState state = dual_net_snapshot();
    if (uplink == DUAL_NET_UPLINK_ETH && dual_net_eth_online(&state)) return eth_netif;
    if (uplink == DUAL_NET_UPLINK_WIFI && state.wifi_up) return wifi_netif;
    return NULL;
}

esp_err_t dual_net_select_uplink(dual_net_uplink_t uplink)
{
#if !DUAL_NET_ENABLE_ROUTE_CONTROL
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (uplink != DUAL_NET_UPLINK_NONE && uplink != DUAL_NET_UPLINK_ETH && uplink != DUAL_NET_UPLINK_WIFI) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!dual_net_is_initialized() ||
        (uplink != DUAL_NET_UPLINK_NONE && dual_net_get_uplink_netif(uplink) == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_state.requested_uplink = uplink;
    s_state.revision++;
    portEXIT_CRITICAL(&s_state_lock);
    dual_net_wake_monitor();
    return ESP_OK;
#endif
}

void dual_net_register_uplink_change_cb(dual_net_uplink_cb_t callback)
{
    portENTER_CRITICAL(&s_state_lock);
    s_uplink_change_callback = callback;
    portEXIT_CRITICAL(&s_state_lock);
}
