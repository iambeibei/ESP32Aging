#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
/* esp_eth.h / esp_eth_netif_glue.h 只提供类型与 glue 声明，不产生硬件占用，常驻包含 */
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#if DUAL_NET_ETH_HW_ENABLE
/* 以下头文件只有真正初始化 KSZ8851SNL 硬件时才需要 */
#include "esp_eth_mac_spi.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#endif
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

#if DUAL_NET_ETH_HW_ENABLE
static esp_eth_handle_t eth_handle = NULL;
#endif
static esp_netif_t *eth_netif = NULL;
static esp_netif_t *wifi_netif = NULL;

bool wifi_up = false;
bool wifi_auth_failed = false;

// eth_link_up：物理网线是否插上
// eth_up：以太网是否已经拿到 IP
static bool eth_link_up = false;
static bool eth_up = false;
static bool wifi_connecting = false;

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

#if DUAL_NET_ETH_HW_ENABLE
static esp_eth_netif_glue_handle_t s_eth_glue = NULL; // 以太网网桥句柄
#endif

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

#if DUAL_NET_ETH_HW_ENABLE
static void dual_net_set_valid_eth_mac(void)
{
    uint8_t wifi_sta_mac[6];

    uint8_t eth_mac[6];

    ESP_ERROR_CHECK(esp_read_mac(wifi_sta_mac, ESP_MAC_WIFI_STA));

    memcpy(eth_mac, wifi_sta_mac, 6);

    // 派生一个本地单播 MAC：
    // bit0 必须为0（单播）
    // bit1 设为1（locally administered）
    eth_mac[0] &= 0xFE;
    eth_mac[0] |= 0x02;

    // 避免和 WiFi STA 完全相同
    eth_mac[5] += 3;

    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac));

    uint8_t readback[6] = {0};
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, readback));

    ESP_LOGI(TAG, "Forced ETH MAC: " MACSTR, MAC2STR(readback));
}
#endif /* DUAL_NET_ETH_HW_ENABLE */

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

    if (eth_up && eth_netif)
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
        ESP_LOGI(TAG, "Ethernet Link Up");
        eth_link_up = true;
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
        ESP_LOGI(TAG, "Ethernet Link Down");
        eth_link_up = false;
        eth_up = false;
        /* Network_Flag 的唯一写入者是 mesh.c:network_event_handler()，此处不再改写它 */

        dual_net_update_route();
        if (!wifi_up && !wifi_connecting && !wifi_auth_failed)
        {
            dual_net_start_wifi();
        }
        break;

    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        eth_link_up = false;
        eth_up = false;
        dual_net_update_route();
        if (!wifi_up && !wifi_connecting && !wifi_auth_failed)
        {
            dual_net_start_wifi();
        }
        break;

    default:
        break;
    }
}

/*
 * 通知全局网络状态。
 *
 * Network_Flag 的唯一权威写入者是 mesh.c:network_event_handler()（STA 取得/丢失 IP 时置位）。
 * 为杜绝两个写者竞争同一全局量，这里默认不再写 Network_Flag；只有显式打开
 * DUAL_NET_ENABLE_ROUTE_CONTROL 后，dual_net 才接管该标志。
 */
static void dual_net_notify_network_state(bool any_up)
{
#if DUAL_NET_ENABLE_ROUTE_CONTROL
    Network_Flag = any_up ? 1U : 0U;
    ESP_LOGI(TAG, "Network_Flag <- %u (dual_net 接管路由控制)", (unsigned)Network_Flag);
#else
    ESP_LOGD(TAG, "网络状态变化 any_up=%d，路由控制未开启，不改写 Network_Flag", (int)any_up);
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
        dual_net_notify_network_state(true); // 直接置位
        dual_net_update_route();

        ESP_LOGI(UART, "Restart MQTT on Ethernet");

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
            dual_net_notify_network_state(true); // WiFi 作为备用时置位
            dual_net_update_route();
        }
        else
        {
            dual_net_notify_network_state(true);
            ESP_LOGI(UART, "WiFi got IP, but Ethernet already has priority; keep WiFi for mesh only");
        }
        break;
    }
    case IP_EVENT_ETH_LOST_IP:
        eth_up = false;
        if (!wifi_up)
            dual_net_notify_network_state(false);
        dual_net_update_route();
        if (!wifi_up && !wifi_connecting && !wifi_auth_failed)
        {
            dual_net_start_wifi();
        }
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
            dual_net_notify_network_state(false);
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

/// @brief 双网卡监控任务，负责定期检查网络状态并在必要时启动 WiFi 连接
static void dual_net_monitor_task(void *arg)
{
    int check_counter = 0;
    int auth_fail_counter = 0;

    while (1)
    {
        check_counter++;

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

#if DUAL_NET_ETH_HW_ENABLE
/*
 * 初始化 Ethernet（KSZ8851SNL over SPI2）。
 *
 * 注意：此处用 esp_netif_new(ESP_NETIF_DEFAULT_ETH()) 自建 netif，绕过了 IoT-Bridge 的
 * NAPT/DHCP 注册，仅适用于「本机当终端」场景。若要让以太网承担根节点上行并转发子节点流量，
 * 需改为 esp_bridge_create_eth_netif() + esp_bridge_netif_list_add()，
 * 详见 DUAL_NET_DESIGN.md 的「有线接入改造清单」。
 */
/// @param
/// @return
static esp_err_t dual_net_eth_init(void)
{
    esp_err_t ret;

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = SPI_MOSIPIN,
        .miso_io_num = SPI_MISOPIN,
        .sclk_io_num = SPI_SCLKPIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = LG_DUAL_NET_ETH_SPI_CLK_HZ,
        .spics_io_num = SPI_CSPIN,
        .queue_size = LG_DUAL_NET_ETH_SPI_QUEUE_SIZE,
    };

    eth_ksz8851snl_config_t ksz_config =
        ETH_KSZ8851SNL_DEFAULT_CONFIG(SPI2_HOST, &devcfg);
    ksz_config.int_gpio_num = ETH_INT;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    esp_eth_mac_t *mac = esp_eth_mac_new_ksz8851snl(&ksz_config, &mac_config);
    if (!mac)
    {
        ESP_LOGE(TAG, "MAC creation failed");
        return ESP_FAIL;
    }

    esp_eth_phy_t *phy = esp_eth_phy_new_ksz8851snl(&phy_config);
    if (!phy)
    {
        ESP_LOGE(TAG, "PHY creation failed");
        mac->del(mac);
        return ESP_FAIL;
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&config, &eth_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(ret));
        mac->del(mac);
        phy->del(phy);
        return ret;
    }
    dual_net_set_valid_eth_mac();

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&cfg);
    if (!eth_netif)
    {
        ESP_LOGE(TAG, "esp_netif_new(ETH) failed");
        return ESP_FAIL;
    }

    s_eth_glue = esp_eth_new_netif_glue(eth_handle);
    if (!s_eth_glue)
    {
        ESP_LOGE(TAG, "esp_eth_new_netif_glue failed");
        return ESP_FAIL;
    }

    ret = esp_netif_attach(eth_netif, s_eth_glue);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Ethernet prepared successfully");
    return ESP_OK;
}
#endif /* DUAL_NET_ETH_HW_ENABLE */

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
    esp_err_t ret = dual_net_eth_init();
    if (ret != ESP_OK)
    {
        storage_write_record_cyclic("", "Ethernet init failed, fallback to WiFi only");
        eth_up = false;
        eth_link_up = false;
    }
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

#if DUAL_NET_ETH_HW_ENABLE
    if (eth_handle != NULL)
    {
        esp_err_t start_err = esp_eth_start(eth_handle);
        if (start_err != ESP_OK)
        {
            storage_write_record_cyclic("", "esp_eth_start failed");
        }
        else
        {
            ESP_LOGI(TAG, "Ethernet started");
        }
    }
#else
    ESP_LOGI(TAG, "以太网硬件未启用（DUAL_NET_ETH_HW_ENABLE=0），仅观察 WiFi 上行");
#endif

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
