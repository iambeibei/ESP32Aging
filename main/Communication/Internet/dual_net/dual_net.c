#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_bridge.h"
/* esp_eth.h 提供 ETH_EVENT / ETHERNET_EVENT_* 事件声明，只是常量，不产生硬件占用 */
#include "esp_eth.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "dual_net.h"
#include "task_config.h"
// 添加这些头文件来解决 IP4_ADDR 错误
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "esp_mac.h"
#include "ConfigData.h"
#include "log.h"
static const char *TAG = "dual_net";
static const char *UART = "Uart";

static esp_netif_t *eth_netif = NULL;
static esp_netif_t *wifi_netif = NULL;
static esp_netif_t *softap_netif = NULL;

bool wifi_up = false;
bool wifi_auth_failed = false;

// eth_link_up：物理网线是否插上（已去抖）
// eth_up：以太网是否已经拿到 IP
static bool eth_link_up = false;
static bool eth_up = false;
static bool wifi_connecting = false;

/* 网线插拔去抖：事件里只记录原始状态与时刻，由监控任务确认后才生效 */
static bool s_eth_link_raw = false;
static bool s_eth_link_pending = false;
static TickType_t s_eth_link_change_tick = 0;

/*
 * 上行可用性：
 *   s_eth_ready —— 以太网已取得 IP 且连通性探测通过（以太网优先的唯一依据）
 *   wifi_up     —— WiFi STA 已取得 IP（沿用 mesh.c 既有判据）
 * s_uplink_state 由这两者派生，仅用于对外呈现状态。
 */
static bool s_eth_ready = false;
static dual_net_uplink_state_t s_uplink_state = DUAL_NET_UPLINK_STATE_NONE;
static uint8_t s_probe_ok_count = 0;
static uint8_t s_probe_fail_count = 0;

// 防止 dual_net_init() 重复初始化
static bool s_dual_net_initialized = false;

// 网络就绪回调
static network_ready_cb_t s_network_ready_callback = NULL;

// 上行变化回调（预留，供后续接管网卡切换的业务订阅）
static dual_net_uplink_cb_t s_uplink_change_callback = NULL;

typedef enum
{
    WIFI_STATE_IDLE,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISABLED,
    WIFI_STATE_AUTH_FAILED
} wifi_state_t;

static wifi_state_t s_wifi_state = WIFI_STATE_IDLE; // 当前 WiFi 状态

/// @brief 打印以太网 IP 地址
/// @param prefix
static void dump_eth_ip(const char *prefix)
{
    if (!eth_netif)
    {
        ESP_LOGW(TAG, "%s: eth_netif is NULL", prefix);
        return;
    }

    esp_netif_ip_info_t ip = {0};
    esp_err_t err = esp_netif_get_ip_info(eth_netif, &ip);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "%s: ETH ip=" IPSTR ", gw=" IPSTR ", mask=" IPSTR,
                 prefix,
                 IP2STR(&ip.ip), IP2STR(&ip.gw), IP2STR(&ip.netmask));
    }
    else
    {
        ESP_LOGW(TAG, "%s: esp_netif_get_ip_info failed: %s",
                 prefix, esp_err_to_name(err));
    }
}

/*
 * 以太网硬件由 iot_bridge 初始化，本模块不再持有 esp_eth_handle_t，
 * 因此无法在此覆写硬件 MAC。iot_bridge 在 bridge_eth.c 中为 SPI 以太网
 * 写死了 02:00:00:12:34:56，详见 DUAL_NET_DESIGN.md「已知限制」。
 */

/// @brief 检查网络是否就绪
/// @param
static void check_network_ready(void)
{
    if ((eth_up || wifi_up) && s_network_ready_callback != NULL)
    {
        ESP_LOGI(TAG, "Network is ready! Triggering callback...");
        s_network_ready_callback();
        s_network_ready_callback = NULL;
    }
}

/*
 * 以太网成为出口后，把它的 DNS 重新下发给 SoftAP。
 *
 * iot_bridge 在 bridge_wifi.c 中按固定 #if 顺序解析 external_netif，
 * WIFI_STA_DEF 的赋值在 ETH_WAN 之后会把它覆盖掉，导致出口已是以太网、
 * 子节点拿到的 DNS 却仍来自 STA。此处在以太网生效后显式纠正一次。
 */
esp_err_t dual_net_refresh_dns_to_softap(void)
{
    if (eth_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* esp_bridge_update_dns_info 对 NULL 安全；第二参传 NULL 表示更新所有数据转发 netif */
    esp_err_t err = esp_bridge_update_dns_info(eth_netif, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "向子节点重下发 DNS 失败：%s", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "已将以太网 DNS 下发给数据转发 netif（含 SoftAP）");
    }
    return err;
}

/*
 * 连通性探测：向业务服务器发起一次 TCP 连接。
 *
 * 只拿到 IP 不能认定上行可用（网线可能插在一台不通的设备上），
 * 必须验证业务路径真的可达，否则会锁死在「有 IP 却上不了网、又不回落 WiFi」的僵局。
 * 这里刻意探测 SERVER_IP:SERVER_UDP_Port（MQTT 服务器）而非网关，
 * 因为它才是业务真正依赖的目标。
 *
 * 注意：本函数会阻塞最多 LG_DUAL_NET_UPLINK_PROBE_TIMEOUT_MS，
 * 只能在任务上下文调用，禁止在事件回调中调用。
 */
static bool dual_net_probe_server_reachable(void)
{
    if (SERVER_IP == NULL || SERVER_UDP_Port <= 0 || SERVER_UDP_Port > 65535)
    {
        ESP_LOGW(TAG, "SERVER_IP/SERVER_UDP_Port 无效，跳过连通性探测");
        return false;
    }

    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons((uint16_t)SERVER_UDP_Port);
    if (inet_pton(AF_INET, SERVER_IP, &dest_addr.sin_addr) != 1)
    {
        ESP_LOGW(TAG, "SERVER_IP 不是合法 IPv4 地址，跳过连通性探测");
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0)
    {
        ESP_LOGW(TAG, "探测用 socket 创建失败");
        return false;
    }

    struct timeval timeout = {0};
    timeout.tv_sec = LG_DUAL_NET_UPLINK_PROBE_TIMEOUT_MS / 1000;
    timeout.tv_usec = (LG_DUAL_NET_UPLINK_PROBE_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    int ret = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    bool reachable = false;

    if (ret == 0)
    {
        reachable = true;
    }
    else if (errno == ECONNREFUSED || errno == EISCONN)
    {
        /* 被对端显式拒绝也说明链路是通的，只是该端口没服务（例如 MQTT 未启动） */
        reachable = true;
    }

    if (!reachable)
    {
        ESP_LOGD(TAG, "连通性探测失败：%s:%d errno=%d", SERVER_IP, SERVER_UDP_Port, errno);
    }

    shutdown(sock, SHUT_RDWR);
    close(sock);
    return reachable;
}

/*
 * 以太网是否已确认为可用上行（取得 IP + 连通性探测通过）。
 * 这是「以太网优先」的唯一依据，也是抑制 root_failover 重启的判据。
 */
bool dual_net_is_eth_uplink_ready(void)
{
    return s_eth_ready;
}

/*
 * 本机是否有可用上行：以太网已确认，或 WiFi STA 已取得 IP。
 * 供 Network_Flag 裁决使用。
 */
bool dual_net_is_uplink_ready(void)
{
    return s_eth_ready || wifi_up;
}

dual_net_uplink_state_t dual_net_get_uplink_state(void)
{
    if (s_eth_ready)
    {
        return DUAL_NET_UPLINK_STATE_READY;
    }
    if (eth_up)
    {
        return (s_probe_fail_count > 0) ? DUAL_NET_UPLINK_STATE_STALE
                                        : DUAL_NET_UPLINK_STATE_PROBING;
    }
    if (wifi_up)
    {
        return DUAL_NET_UPLINK_STATE_READY;
    }
    return DUAL_NET_UPLINK_STATE_NONE;
}

/// @brief 上行发生变化时通知订阅者（预留回调，可能在事件上下文调用）
/// @param uplink 变化后的上行类型
static void dual_net_notify_uplink_change(dual_net_uplink_t uplink)
{
    if (s_uplink_change_callback != NULL)
    {
        s_uplink_change_callback(uplink);
    }
}

/// @brief 更新默认路由
/// @param
static void dual_net_update_route(void)
{
    static dual_net_uplink_t s_last_uplink = DUAL_NET_UPLINK_NONE;
    dual_net_uplink_t new_uplink = DUAL_NET_UPLINK_NONE;
    const char *new_active = "NONE";

    /*
     * 以太网优先，但避免「坏网线」打断正常 WiFi：
     *   - WiFi 已断时，以太网是唯一出路，取得 IP 立即启用；
     *   - WiFi 仍在时，必须等连通性探测确认后才切换，防止把可用上行换成不可用上行。
     */
    if (eth_up && eth_netif && (s_eth_ready || !wifi_up))
    {
#if DUAL_NET_ENABLE_ROUTE_CONTROL
        esp_netif_set_default_netif(eth_netif); // 设置默认网卡
#endif
        new_uplink = DUAL_NET_UPLINK_ETH;
        new_active = "ETHERNET";
        if (s_last_uplink != new_uplink)
        {
            ESP_LOGI(TAG, "✓ Default route -> Ethernet (Priority 1), active=%s", new_active);
            s_last_uplink = new_uplink;
            dual_net_notify_uplink_change(new_uplink);
        }
        check_network_ready();
        return;
    }

    if (wifi_up && wifi_netif)
    {
#if DUAL_NET_ENABLE_ROUTE_CONTROL
        esp_netif_set_default_netif(wifi_netif); // 设置默认网卡
#endif
        new_uplink = DUAL_NET_UPLINK_WIFI;
        new_active = "WIFI";
        if (s_last_uplink != new_uplink)
        {
            ESP_LOGI(TAG, "⚠ Default route -> WiFi (Priority 2 - Fallback), active=%s", new_active);
            s_last_uplink = new_uplink;
            dual_net_notify_uplink_change(new_uplink);
        }
        check_network_ready();
        return;
    }

    if (eth_up && !eth_netif)
    {
        ESP_LOGW(TAG, "Ethernet is up but eth_netif is NULL");
    }
    if (wifi_up && !wifi_netif)
    {
        ESP_LOGW(TAG, "WiFi is up but wifi_netif is NULL");
    }

    if (s_last_uplink != DUAL_NET_UPLINK_NONE)
    {
        ESP_LOGW(TAG, "✗ No available uplink");
        s_last_uplink = DUAL_NET_UPLINK_NONE;
        dual_net_notify_uplink_change(DUAL_NET_UPLINK_NONE);
    }
}

static volatile bool s_mesh_switching = false;

/// @brief 设置 Mesh 路由切换状态
/// @param switching   true表示mesh自己控制，false表示使用deal_net控制WiFi
void dual_net_set_mesh_switching(bool switching)
{
    s_mesh_switching = switching;

    if (switching)
    {
        wifi_connecting = false;
        wifi_up = false;
        s_wifi_state = WIFI_STATE_DISABLED;
        ESP_LOGW(TAG, "Mesh router switching started, suppress normal esp_wifi_connect()");
    }
    else
    {
        if (!eth_up && !wifi_auth_failed && !wifi_up)
        {
            s_wifi_state = WIFI_STATE_IDLE;
        }
        ESP_LOGI(TAG, "Mesh router switching finished");
    }
}

bool dual_net_is_mesh_switching(void)
{
    return s_mesh_switching;
}

static void dual_net_start_wifi(void)
{

    if (s_mesh_switching)
    {
        ESP_LOGW(TAG, "Skip esp_wifi_connect(): mesh router switching in progress");
        return;
    }
    if (eth_up)
    {
        return;
    }
    if (wifi_up || wifi_connecting)
    {
        return;
    }
    if (wifi_auth_failed)
    {
        return;
    }

#if !DUAL_NET_ENABLE_WIFI_FALLBACK
    /*
     * 默认不主动 esp_wifi_connect()：Mesh-Lite 自行管理 STA 与父节点选择，
     * 外部强连会打断自组织入网流程。打开 DUAL_NET_ENABLE_WIFI_FALLBACK 后才允许。
     */
    ESP_LOGD(TAG, "WiFi 兜底连接未开启，跳过 esp_wifi_connect()");
    return;
#else
    ESP_LOGI(TAG, "Starting WiFi uplink connection...");
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK)
    {
        wifi_connecting = true;
        s_wifi_state = WIFI_STATE_CONNECTING;
    }
    else
    {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
#endif
}

static void dual_net_refresh_wifi_ip_state(void)
{
    if (!wifi_netif)
    {
        return;
    }
    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(wifi_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
    {
        wifi_up = true;
        wifi_connecting = false;
        wifi_auth_failed = false;
        s_wifi_state = WIFI_STATE_CONNECTED;
        ESP_LOGI(TAG, "Detected existing WiFi IP: " IPSTR, IP2STR(&ip_info.ip));
    }
}

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Up（原始事件，等待去抖确认）");
        /*
         * 网线插拔会产生抖动，这里只记录原始状态与时刻，
         * 由 dual_net_monitor_task 在去抖时间到后统一生效，
         * 避免频繁切换默认路由与刷屏。
         */
        s_eth_link_raw = true;
        s_eth_link_pending = true;
        s_eth_link_change_tick = xTaskGetTickCount();
        // 这里只表示物理链路已连通，不能在这里立刻踢掉 WiFi。
        // 必须等 IP_EVENT_ETH_GOT_IP 之后，才能真正切换默认路由到以太网。
        // 这里只代表物理链路通了，不代表已经有 IP
        // 手动重启一次 DHCP client，确保插线后真的发起 DHCP
        if (eth_netif)
        {
            esp_err_t err1 = esp_netif_dhcpc_stop(eth_netif); // 先停止 DHCP 客户端，可能会返回 ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED，忽略这个错误
            if (err1 != ESP_OK && err1 != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
            {
                ESP_LOGW(TAG, "dhcpc_stop: %s", esp_err_to_name(err1));
            }

            esp_err_t err2 = esp_netif_dhcpc_start(eth_netif);
            if (err2 != ESP_OK && err2 != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
            {
                ESP_LOGE(TAG, "dhcpc_start failed: %s", esp_err_to_name(err2));
            }
            else
            {
                ESP_LOGI(TAG, "DHCP client started on Ethernet");
            }
        }

        dump_eth_ip("after LINK_UP");

        break;

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down（原始事件，等待去抖确认）");
        s_eth_link_raw = false;
        s_eth_link_pending = true;
        s_eth_link_change_tick = xTaskGetTickCount();
        eth_up = false;
        s_eth_ready = false;
        s_uplink_state = DUAL_NET_UPLINK_STATE_NONE;
        s_probe_ok_count = 0;
        s_probe_fail_count = 0;
        /* Network_Flag 的统一裁决入口是 dual_net_arbitrate_network_flag()，此处不再直接改写它 */

        dual_net_update_route();
        break;

    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        s_eth_link_raw = false;
        s_eth_link_pending = true;
        s_eth_link_change_tick = xTaskGetTickCount();
        eth_up = false;
        s_eth_ready = false;
        s_uplink_state = DUAL_NET_UPLINK_STATE_NONE;
        s_probe_ok_count = 0;
        s_probe_fail_count = 0;
        dual_net_update_route();
        dual_net_arbitrate_network_flag();
        break;

    default:
        break;
    }
}

/*
 * Network_Flag 的唯一裁决入口。
 *
 * 语义从「STA 取得 IP」升级为「以太网或 WiFi STA 任一真正可用」，
 * 这样在「WiFi 全断、仅剩网线」时根节点不会被判为断网，
 * 进而也不会触发 root_failover_task 的扫描与 esp_restart()。
 *
 * 为避免两个写者竞争同一全局量，mesh.c 中的两处直接写点已改为调用本函数。
 * 只有打开 DUAL_NET_ENABLE_ROUTE_CONTROL 后，dual_net 才真正写入该标志；
 * 关闭时仍由 mesh.c 的原有逻辑维护（保持 L0 行为）。
 */
void dual_net_arbitrate_network_flag(void)
{
#if DUAL_NET_ENABLE_ROUTE_CONTROL
    /*
     * 兜底：若 dual_net 尚未初始化或初始化失败（例如取不到 WIFI_STA_DEF 句柄），
     * 不能让根节点失去 Network_Flag——否则 MQTT 等业务会永远等不到网络就绪。
     * 此时退化回原有的「STA 取得 IP」判据。
     */
    if (!s_dual_net_initialized)
    {
        esp_netif_ip_info_t ip_info = {0};
        bool sta_ok = (wifi_netif != NULL) &&
                      (esp_netif_get_ip_info(wifi_netif, &ip_info) == ESP_OK) &&
                      (ip_info.ip.addr != 0);
        Network_Flag = sta_ok ? 1U : 0U;
        return;
    }
#endif

    bool any_up = dual_net_is_uplink_ready() || wifi_up;

#if DUAL_NET_ENABLE_ROUTE_CONTROL
    if (Network_Flag != (any_up ? 1U : 0U))
    {
        Network_Flag = any_up ? 1U : 0U;
        ESP_LOGI(TAG, "Network_Flag <- %u（dual_net 裁决：eth_up=%d wifi_up=%d uplink=%d）",
                 (unsigned)Network_Flag, (int)eth_up, (int)wifi_up, (int)s_uplink_state);
    }
#else
    ESP_LOGD(TAG, "上行状态变化 any_up=%d，路由控制未开启，不改写 Network_Flag", (int)any_up);
#endif
}

static void dual_net_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case IP_EVENT_ETH_GOT_IP:
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(UART, "✓ Ethernet Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(UART, "✓ Ethernet GW    : " IPSTR, IP2STR(&event->ip_info.gw));
        ESP_LOGI(UART, "✓ Ethernet MASK  : " IPSTR, IP2STR(&event->ip_info.netmask));

        eth_up = true;
        dump_eth_ip("after GOT_IP");
        s_uplink_state = DUAL_NET_UPLINK_STATE_PROBING;
        s_eth_ready = false;
        s_probe_ok_count = 0;
        s_probe_fail_count = 0;
        dual_net_arbitrate_network_flag();
        dual_net_update_route();

        ESP_LOGI(UART, "以太网已取得 IP，等待连通性探测确认后才认定为可用上行");

        break;
    }

    case IP_EVENT_STA_GOT_IP:
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        wifi_up = true;
        s_mesh_switching = false;
        wifi_connecting = false;
        wifi_auth_failed = false;
        s_wifi_state = WIFI_STATE_CONNECTED;

        if (!eth_up)
        {
            ESP_LOGI(UART, "⚠ WiFi Got IP (Fallback): " IPSTR, IP2STR(&event->ip_info.ip));
            dual_net_arbitrate_network_flag();
            dual_net_update_route();
        }
        else
        {
            dual_net_arbitrate_network_flag();
            ESP_LOGI(UART, "WiFi got IP, but Ethernet already has priority; keep WiFi for mesh only");
        }
        break;
    }
    case IP_EVENT_ETH_LOST_IP:
        eth_up = false;
        s_eth_ready = false;
        s_uplink_state = DUAL_NET_UPLINK_STATE_NONE;
        s_probe_ok_count = 0;
        s_probe_fail_count = 0;
        dual_net_arbitrate_network_flag();
        dual_net_update_route();
        break;
    case IP_EVENT_STA_LOST_IP:
        wifi_up = false;
        wifi_connecting = false;

        if (!eth_up)
        {
            ESP_LOGW(UART, "WiFi lost IP");
            if (!wifi_auth_failed)
            {
                s_wifi_state = WIFI_STATE_IDLE;
            }
            dual_net_arbitrate_network_flag();
            dual_net_update_route();
        }
        else
        {
            s_wifi_state = WIFI_STATE_DISABLED;
        }
        break;

    default:
        break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    static uint32_t s_no_ap_suppressed = 0;
    static TickType_t s_last_no_ap_log = 0;

    if (event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;

        wifi_connecting = false;
        wifi_up = false;
        /* Network_Flag 的唯一写入者是 mesh.c:network_event_handler()，此处不再改写它 */

        // Ethernet 已经工作时，WiFi 对 mesh 的扫描失败不应该刷屏
        if (eth_up && disconn->reason == WIFI_REASON_NO_AP_FOUND)
        {
            s_wifi_state = WIFI_STATE_DISABLED;
            s_no_ap_suppressed++;

            TickType_t now = xTaskGetTickCount();
            if ((now - s_last_no_ap_log) > pdMS_TO_TICKS(30000))
            {
                ESP_LOGW(TAG,
                         "WiFi mesh uplink unavailable (reason=201/NO_AP_FOUND), "
                         "Ethernet active, suppressed=%" PRIu32,
                         s_no_ap_suppressed);
                s_last_no_ap_log = now;
                s_no_ap_suppressed = 0;
            }
            return;
        }

        ESP_LOGI(TAG, "WiFi disconnected, reason: %d", disconn->reason);

        if (disconn->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || disconn->reason == WIFI_REASON_AUTH_FAIL) // 认证失败
        {
            ESP_LOGE(TAG, "✗ WiFi Authentication failed! Wrong password?");
            wifi_auth_failed = true;
            s_wifi_state = WIFI_STATE_AUTH_FAILED;
        }
        else if (!eth_up)
        {
            s_wifi_state = WIFI_STATE_IDLE;
        }
        else
        {
            s_wifi_state = WIFI_STATE_DISABLED;
        }

        dual_net_update_route();
    }
    else if (event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "WiFi STA started");
        wifi_connecting = false;
        if (!eth_up && !wifi_auth_failed)
        {
            s_wifi_state = WIFI_STATE_IDLE;
        }
    }
    else if (event_id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI(TAG, "WiFi connected to AP");
        s_wifi_state = WIFI_STATE_CONNECTING;
    }
}

/*
 * 处理网线插拔去抖：事件里只记录原始状态，此处确认稳定后才生效。
 * 抖动期间不改变 eth_link_up，避免频繁切路由与日志刷屏。
 */
static void dual_net_process_link_debounce(void)
{
    if (!s_eth_link_pending)
    {
        return;
    }

    TickType_t need_ms = s_eth_link_raw ? (TickType_t)LG_DUAL_NET_ETH_LINK_UP_DEBOUNCE_MS
                                        : (TickType_t)LG_DUAL_NET_ETH_LINK_DOWN_DEBOUNCE_MS;
    if ((xTaskGetTickCount() - s_eth_link_change_tick) < pdMS_TO_TICKS(need_ms))
    {
        return;
    }

    s_eth_link_pending = false;
    eth_link_up = s_eth_link_raw;
    ESP_LOGI(TAG, "网线状态已确认：%s", eth_link_up ? "已插入" : "已拔出");
}

/*
 * 以太网取得 IP 后，周期性探测业务服务器是否可达，
 * 连续成功 LG_DUAL_NET_UPLINK_PROBE_OK_COUNT 次才认定上行可用（READY）；
 * 连续失败 LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT 次则置为 STALE 并回落 WiFi。
 *
 * 注意：探测会阻塞最多 LG_DUAL_NET_UPLINK_PROBE_TIMEOUT_MS，只能在任务上下文调用。
 */
static void dual_net_process_uplink_probe(void)
{
    if (!eth_up)
    {
        return;
    }

    bool reachable = dual_net_probe_server_reachable();

    if (reachable)
    {
        s_probe_fail_count = 0;
        if (s_probe_ok_count < LG_DUAL_NET_UPLINK_PROBE_OK_COUNT)
        {
            s_probe_ok_count++;
        }
        if (s_probe_ok_count >= LG_DUAL_NET_UPLINK_PROBE_OK_COUNT && !s_eth_ready)
        {
            ESP_LOGI(TAG, "以太网连通性已确认，上行切换为以太网");
            s_eth_ready = true;
            dual_net_update_route();
            dual_net_refresh_dns_to_softap();
            dual_net_arbitrate_network_flag();
        }
    }
    else
    {
        s_probe_ok_count = 0;
        if (s_probe_fail_count < LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT)
        {
            s_probe_fail_count++;
        }
        if (s_probe_fail_count >= LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT && s_eth_ready)
        {
            ESP_LOGW(TAG, "以太网连通性连续失败 %u 次，取消以太网优先并回落 WiFi",
                     (unsigned)s_probe_fail_count);
            s_eth_ready = false;
            dual_net_update_route();
            /* 回落后把 DNS 重新指向 WiFi 侧，避免子节点仍拿到以太网（已不可用）的 DNS */
            if (wifi_netif != NULL)
            {
                esp_err_t dns_err = esp_bridge_update_dns_info(wifi_netif, NULL);
                if (dns_err != ESP_OK)
                {
                    ESP_LOGW(TAG, "回落时重下发 DNS 失败：%s", esp_err_to_name(dns_err));
                }
            }
            dual_net_arbitrate_network_flag();
        }
    }
}

/// @brief 双网卡监控任务，负责定期检查网络状态并在必要时启动 WiFi 连接
static void dual_net_monitor_task(void *arg)
{
    int check_counter = 0;
    int auth_fail_counter = 0;

    while (1)
    {
        check_counter++;

        dual_net_process_link_debounce();
        dual_net_process_uplink_probe();

        // 只有当 Ethernet 还没有真正拿到 IP 时，WiFi 才作为 fallback 使用。
        // 注意：这里不能用 eth_link_up 判断，因为“插了网线但 DHCP 失败”时仍然需要 WiFi 继续兜底。
        if (!eth_up && !wifi_up && !wifi_connecting && !wifi_auth_failed && s_wifi_state == WIFI_STATE_IDLE)
        {
            if (check_counter % LG_DUAL_NET_WIFI_RETRY_INTERVAL == 0)
            {
                ESP_LOGI(TAG, "No Ethernet IP, attempting WiFi connection...");
                dual_net_start_wifi();
            }
        }

        // 认证失败后按轮询次数重试，达到上限才复位标志（有退出条件，不会无限等待）
        if (wifi_auth_failed)
        {
            auth_fail_counter++;
            if (auth_fail_counter >= LG_DUAL_NET_AUTH_RETRY_MAX_COUNT)
            {
                ESP_LOGI(TAG, "Retrying WiFi connection (previous auth failure)");
                wifi_auth_failed = false;
                if (!eth_up)
                {
                    s_wifi_state = WIFI_STATE_IDLE;
                }
                auth_fail_counter = 0;
            }
        }
        else
        {
            auth_fail_counter = 0;
        }

#if DUAL_NET_ENABLE_WIFI_FALLBACK
        vTaskDelay(LG_DUAL_NET_MONITOR_ACTIVE_PERIOD_MS / portTICK_PERIOD_MS);
#else
        /* 仅观察模式下无实际动作，降频到 60 秒一次，避免日志刷屏 */
        vTaskDelay(LG_DUAL_NET_MONITOR_IDLE_PERIOD_MS / portTICK_PERIOD_MS);
#endif
    }
}

/*
 * 接管 iot_bridge 创建的以太网上行 netif。
 *
 * 以太网硬件（KSZ8851SNL over SPI）由 esp_bridge_create_all_netif() 内部的
 * esp_bridge_create_eth_netif() 负责初始化，本模块只按 ifkey 取回句柄并观察其状态。
 * 这样既能接入 NAPT/DHCP 转发链（ETH_WAN 的 route_prio=50），又不会与 bridge_eth.c 的
 * ESP_ERROR_CHECK(spi_bus_initialize(...)) 争用 SPI2_HOST 而 abort。
 *
 * @return ESP_OK 已接管，或以太网不可用（降级为纯 WiFi 上行）
 */
static esp_err_t dual_net_eth_takeover(void)
{
    eth_netif = esp_netif_get_handle_from_ifkey("ETH_WAN");
    if (eth_netif == NULL)
    {
        /*
         * 以太网未启用或硬件缺失：芯片未焊接时 iot_bridge 只会留下一个拿不到 IP 的 netif，
         * 不会 abort。此处按「无以太网」处理，降级为纯 WiFi 上行。
         */
        ESP_LOGW(TAG, "未找到 ETH_WAN netif，以太网不可用，降级为纯 WiFi 上行");
        return ESP_OK;
    }

    softap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (softap_netif == NULL)
    {
        ESP_LOGW(TAG, "未找到 WIFI_AP_DEF netif，以太网生效时无法向子节点重下发 DNS");
    }

    ESP_LOGI(TAG, "已接管以太网上行 netif: %s", esp_netif_get_ifkey(eth_netif));
    return ESP_OK;
}

esp_err_t dual_net_init(void)
{
#if !DUAL_NET_ENABLE
    ESP_LOGI(TAG, "dual_net 总开关关闭，不初始化");
    return ESP_OK;
#endif

#if DUAL_NET_ROOT_ONLY
    /*
     * 根节点专属模块：只有根节点才有「外部路由器 / 网线」这类真实上行。
     * 子节点的上行由 mesh 父节点经 NAPT 提供，dual_net 对它无意义，
     * 必须零代码运行——不注册事件处理器、不创建监控任务、不触碰 ETH。
     * 此处为兜底判断，主防线在 appTask.c 的调用点。
     */
    if (IsRoot != 1)
    {
        ESP_LOGI(TAG, "非根节点，dual_net 不启用（上行由 mesh 父节点提供）");
        return ESP_OK;
    }
#endif

    if (s_dual_net_initialized)
    {
        ESP_LOGW(TAG, "dual_net_init already done, skip");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "=== Dual Network Init (Ethernet Priority Mode, root only) ===");

    // 获取 WiFi STA netif 句柄；必须在 esp_bridge_create_all_netif() 之后才能取到
    wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (wifi_netif == NULL)
    {
        storage_write_record_cyclic("", "Failed to get WiFi_STA_DEF netif");
        return ESP_FAIL;
    }

    s_dual_net_initialized = true;
    s_wifi_state = WIFI_STATE_IDLE;
    eth_up = false;
    eth_link_up = false;

#if DUAL_NET_ETH_HW_ENABLE
    esp_err_t ret = dual_net_eth_takeover();
    if (ret != ESP_OK)
    {
        storage_write_record_cyclic("", "Ethernet takeover failed, fallback to WiFi only");
        eth_up = false;
        eth_link_up = false;
    }
#else
    ESP_LOGI(TAG, "以太网未启用（DUAL_NET_ETH_HW_ENABLE=0），仅观察 WiFi 上行");
#endif

    // 注册以太网事件处理器
    esp_err_t reg_err = esp_event_handler_register(ETH_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   &eth_event_handler,
                                                   NULL);
    if (reg_err != ESP_OK)
    {
        storage_write_record_cyclic("", "dual_net register ETH_EVENT handler failed");
        return reg_err;
    }

    // 注册 IP 事件处理器，监听以太网和 WiFi 的 IP 获取和丢失事件
    reg_err = esp_event_handler_register(IP_EVENT,
                                         ESP_EVENT_ANY_ID,
                                         &dual_net_ip_event_handler,
                                         NULL);
    if (reg_err != ESP_OK)
    {
        storage_write_record_cyclic("", "dual_net register IP_EVENT handler failed");
        return reg_err;
    }

    // 注册 WiFi 事件处理器，监控连接状态和认证失败等事件
    reg_err = esp_event_handler_register(WIFI_EVENT,
                                         ESP_EVENT_ANY_ID,
                                         &wifi_event_handler,
                                         NULL);
    if (reg_err != ESP_OK)
    {
        storage_write_record_cyclic("", "dual_net register WIFI_EVENT handler failed");
        return reg_err;
    }

    BaseType_t ret1 = xTaskCreatePinnedToCore(dual_net_monitor_task,
                                               "net_monitor",
                                               LG_STACK_NET_MONITOR,
                                               NULL,
                                               LG_PRIO_NET_MONITOR,
                                               NULL,
                                               LG_APP_CPU_CORE);
    if (ret1 != pdPASS)
    {
        storage_write_record_cyclic("", "Failed to create dual_net_monitor_task");
        return ESP_FAIL;
    }

    dual_net_refresh_wifi_ip_state();
    if (wifi_up)
    {
        dual_net_update_route();
        check_network_ready();
    }

    return ESP_OK;
}

void dual_net_notify_wifi_up(void)
{
    //兼容旧接口：统一改为按 netif 实际 IP 状态刷新，不再直接写状态机。
    ESP_LOGI(TAG, "Legacy WiFi-up notify received, refreshing state from netif");
    dual_net_refresh_wifi_ip_state();
    dual_net_update_route();
    check_network_ready();
}

void dual_net_notify_wifi_down(void)
{
    // 兼容旧接口：新版本不再建议外部直接驱动这里的状态。
    ESP_LOGI(TAG, "Legacy WiFi-down notify received");
    wifi_up = false;
    wifi_connecting = false;
    if (!eth_up && !wifi_auth_failed)
    {
        s_wifi_state = WIFI_STATE_IDLE;
    }
    dual_net_update_route();
}

const char *dual_net_get_active_interface(void)
{
    if (eth_up)
        return "ETHERNET";
    if (wifi_up)
        return "WIFI";
    return "NONE";
}

bool dual_net_is_ethernet_active(void)
{
    return eth_up;
}

bool dual_net_is_wifi_active(void)
{
    return wifi_up;
}

bool dual_net_is_any_network_up(void)
{
    return eth_up || wifi_up;
}

const char *dual_net_get_wifi_state(void)
{
    switch (s_wifi_state)
    {
    case WIFI_STATE_IDLE:
        return "IDLE";
    case WIFI_STATE_CONNECTING:
        return "CONNECTING";
    case WIFI_STATE_CONNECTED:
        return "CONNECTED";
    case WIFI_STATE_DISABLED:
        return "DISABLED";
    case WIFI_STATE_AUTH_FAILED:
        return "AUTH_FAILED";
    default:
        return "UNKNOWN";
    }
}

void dual_net_register_ready_callback(network_ready_cb_t callback)
{
    s_network_ready_callback = callback;
    ESP_LOGI(TAG, "Network ready callback registered");

    if (dual_net_is_any_network_up() && callback != NULL)
    {
        ESP_LOGI(TAG, "Network already ready, triggering callback immediately");
        callback();
        s_network_ready_callback = NULL;
    }
}

dual_net_uplink_t dual_net_get_active_uplink(void)
{
    /* 以太网优先、WiFi 兜底：同一时刻只承认一条上行 */
    if (eth_up)
    {
        return DUAL_NET_UPLINK_ETH;
    }
    if (wifi_up)
    {
        return DUAL_NET_UPLINK_WIFI;
    }
    return DUAL_NET_UPLINK_NONE;
}

esp_netif_t *dual_net_get_uplink_netif(dual_net_uplink_t uplink)
{
    switch (uplink)
    {
    case DUAL_NET_UPLINK_ETH:
        return eth_up ? eth_netif : NULL;
    case DUAL_NET_UPLINK_WIFI:
        return wifi_up ? wifi_netif : NULL;
    case DUAL_NET_UPLINK_NONE:
    default:
        return NULL;
    }
}

/*
 * 预留：手动选择上行链路。
 * 当前以太网硬件默认不启用，且路由控制默认关闭，因此不具备切换条件；
 * 打开 DUAL_NET_ENABLE_ROUTE_CONTROL 后在此实现真正的网卡切换。
 */
esp_err_t dual_net_select_uplink(dual_net_uplink_t uplink)
{
#if !DUAL_NET_ENABLE_ROUTE_CONTROL
    (void)uplink;
    ESP_LOGW(TAG, "路由控制未开启，不支持手动切换上行");
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_netif_t *target = dual_net_get_uplink_netif(uplink);
    if (target == NULL)
    {
        ESP_LOGW(TAG, "目标上行不可用或尚未就绪：uplink=%d", (int)uplink);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_netif_set_default_netif(target);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "切换默认网卡失败：%s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "上行已切换为 %s", dual_net_get_active_interface());
    return ESP_OK;
#endif
}

void dual_net_register_uplink_change_cb(dual_net_uplink_cb_t callback)
{
    s_uplink_change_callback = callback;
    ESP_LOGI(TAG, "上行变化回调已%s", callback != NULL ? "注册" : "取消");
}
