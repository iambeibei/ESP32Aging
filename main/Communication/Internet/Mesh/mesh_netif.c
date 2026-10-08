/* Mesh IP Internal Communication Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "esp_mesh.h"
#include "esp_mac.h"
#include "lwip/lwip_napt.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_wifi_netif.h"
#include "mesh_netif.h"
#include "task_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/*******************************************************
 *                Macros
 *******************************************************/
#define RX_SIZE (1560)

/*
 * ===== 50个子节点规模的MTU口径 =====
 * 之前存在两套不一致的阈值(SAFE_WARN=1400 / MAX_FRAME=1456)，而sdkconfig里
 * CONFIG_LWIP_TCP_MSS=1440，加上IP+TCP头(40)后单帧IP包可达1480字节 > 1456，
 * 导致所有"满尺寸"下行TCP报文被mesh_netif静默丢弃(netif MTU仍是默认1500)。
 * 后果：下行大包(start_aging这类1.5KB JSON)无法落地 -> PUBACK上不去 ->
 * broker QoS1重传 -> 下行流量被进一步放大。
 *
 * 解决办法：约定三者一致 ——
 *   MESH_MAX_FRAME_SIZE = 1400
 *   CONFIG_LWIP_TCP_MSS = 1400 - 40 = 1360   (sdkconfig 中已改)
 *   MESH_SAFE_WARN_SIZE = 1400
 * 这样本机发出的 IP 报文最大为 1360+40 = 1400，永远不会产生超长 mesh 帧。
 * 不依赖 netif MTU(1500)，因此也不会用到 esp_netif_set_mtu()(需 IDF v5.1+)。
 * 改动任意一处都要同步另外两处，否则下面的编译期校检会给出 #warning。
 */
#define MESH_SAFE_WARN_SIZE 1400U
#define MESH_MAX_FRAME_SIZE 1400U
#define MESH_NETIF_MTU MESH_MAX_FRAME_SIZE

/*
 * 约束检查：本机产生的 IP 报文最大 = TCP_MSS + IP头/TCP头(40)，
 * 必须 <= mesh 单帧承载上限，否则下行满帧会被直接丢弃、只能靠 TCP 超时重传。
 * 注意：当前 IDF 版本没有 esp_netif_set_mtu()（v5.1 才引入），
 * 因此这里通过约束 CONFIG_LWIP_TCP_MSS 来保证不产生超长帧。
 */
#if (CONFIG_LWIP_TCP_MSS + 40) > MESH_MAX_FRAME_SIZE
#warning "CONFIG_LWIP_TCP_MSS 过大：TCP/IP报文会超过mesh单帧上限，下行将被丢弃。请把 CONFIG_LWIP_TCP_MSS 调到 (MESH_MAX_FRAME_SIZE - 40) 及以下。"
#endif

/*
 * ===== Root下行转发任务 =====
 * Root的netif transmit运行在lwIP(tcpip_thread)上下文，如果在其中直接调用阻塞式
 * esp_mesh_send()，下行拥塞时会把整个lwIP栈卡住数十秒(esp_mesh_send_block_time)，
 * 表现为Root自己的MQTT ping都发不出去、全网MQTT断连。
 * 这里把Root下行改成"入队 + 独立任务发送"，transmit只做一次拷贝后立即返回。
 */
#define MESH_ROOT_TX_QUEUE_LEN 64
#define MESH_ROOT_TX_STACK 3072
#define MESH_ROOT_TX_PRIO (LG_PRIO_MESH_RX - 1)

/*
 * 广播(ARP/DHCP等)在mesh上会被展开成"逐个节点P2P单播"。
 * 50个子节点时一次广播就是50次并发发送，会瞬间打爆mesh的润滑窗口
 * (日志里的 [tx-wifi]src exceed / ESP_ERR_MESH_XMIT / max_wnd:2)。
 * 这里做两件事：
 *   1) 逐个目标之间插入间隔，降低并发；
 *   2) 对发送失败的目标做短期黑名单，跳过后续帧，避免每个包都等一个超时。
 */
#define MESH_BCAST_TX_GAP_MS 5U
#define MESH_TX_BLACKLIST_SIZE 8U
#define MESH_TX_BLACKLIST_TTL_MS 3000U

#if CONFIG_MESH_USE_GLOBAL_DNS_IP
#define DNS_IP_ADDR CONFIG_MESH_GLOBAL_DNS_IP
#endif

/*******************************************************
 *                Type Definitions
 *******************************************************/
typedef struct mesh_netif_driver *mesh_netif_driver_t;

typedef struct mesh_netif_driver
{
    esp_netif_driver_base_t base;
    uint8_t sta_mac_addr[MAC_ADDR_LEN];
} *mesh_netif_driver_t;

/*******************************************************
 *                Constants
 *******************************************************/
static const char *TAG = "mesh_netif";
static const char *UART_TAG = "Uart";
const esp_netif_ip_info_t g_mesh_netif_subnet_ip = {
    // mesh subnet IP info
    .ip = {.addr = ESP_IP4TOADDR(10, 0, 0, 1)},
    .gw = {.addr = ESP_IP4TOADDR(10, 0, 0, 1)},
    .netmask = {.addr = ESP_IP4TOADDR(255, 255, 0, 0)},
};

/*******************************************************
 *                Type Definitions
 *******************************************************/
typedef struct
{
    uint8_t dst_mac[MAC_ADDR_LEN];
    size_t len;
    bool tods;      /* true: 子节点->Root的上行(MESH_DATA_TODS) */
    uint8_t data[]; /* 柔性数组：紧随其后的帧内容(以太头+载荷) */
} mesh_tx_item_t;

typedef struct
{
    uint8_t mac[MAC_ADDR_LEN];
    TickType_t invalid_until;
} mesh_tx_blacklist_entry_t;

/*******************************************************
 *                Variable Definitions
 *******************************************************/
static esp_netif_t *netif_sta = NULL;
static esp_netif_t *netif_ap = NULL;
static bool receive_task_is_running = false;
static mesh_addr_t s_route_table[CONFIG_MESH_ROUTE_TABLE_SIZE] = {0};
static mesh_raw_recv_cb_t *s_mesh_raw_recv_cb = NULL;

/* Root下行转发：队列 + 发送任务 */
static QueueHandle_t s_mesh_tx_queue = NULL;
static TaskHandle_t s_mesh_tx_task_handle = NULL;
static volatile bool s_mesh_tx_running = false;

/* 下行失败目标的短期黑名单 */
static mesh_tx_blacklist_entry_t s_tx_blacklist[MESH_TX_BLACKLIST_SIZE] = {0};
static uint32_t s_mesh_tx_dropped = 0;

/*******************************************************
 *                Function Definitions
 *******************************************************/
//  setup DHCP server's DNS OFFER
//
static esp_err_t set_dhcps_dns(esp_netif_t *netif, uint32_t addr)
{
    esp_netif_dns_info_t dns;
    dns.ip.u_addr.ip4.addr = addr;
    dns.ip.type = IPADDR_TYPE_V4;
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(netif));
    return ESP_OK;
}

// Receive task
//
static void receive_task(void *arg)
{
    esp_err_t err;
    mesh_addr_t from;
    int flag = 0;
    mesh_data_t data;
    static uint8_t rx_buf[RX_SIZE] = {
        0,
    };

    ESP_LOGD(TAG, "Receiving task started");
    while (receive_task_is_running)
    {
        data.data = rx_buf;
        data.size = RX_SIZE;
        err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Received with err code %d %s", err, esp_err_to_name(err));
            continue;
        }
        if (data.proto == MESH_PROTO_BIN && s_mesh_raw_recv_cb)
        {
            s_mesh_raw_recv_cb(&from, &data);
        }
        if (esp_mesh_is_root())
        {
            if (data.proto == MESH_PROTO_AP)
            {
                ESP_LOGD(TAG, "Root received: from: " MACSTR " to " MACSTR " size: %d",
                         MAC2STR((uint8_t *)data.data), MAC2STR((uint8_t *)(data.data + 6)), data.size);
                if (netif_ap)
                {
                    // actual receive to TCP/IP stack
                    esp_netif_receive(netif_ap, data.data, data.size, NULL);
                }
            }
            else if (data.proto == MESH_PROTO_STA)
            {
                ESP_LOGE(TAG, "Root station Should never receive data from mesh!");
            }
        }
        else
        {
            if (data.proto == MESH_PROTO_AP)
            {
                ESP_LOGD(TAG, "Node AP should never receive data from mesh");
            }
            else if (data.proto == MESH_PROTO_STA)
            {
                ESP_LOGD(TAG, "Node received: from: " MACSTR " to " MACSTR " size: %d",
                         MAC2STR((uint8_t *)data.data), MAC2STR((uint8_t *)(data.data + 6)), data.size);
                if (netif_sta)
                {
                    // actual receive to TCP/IP stack
                    esp_netif_receive(netif_sta, data.data, data.size, NULL);
                }
            }
        }
    }
    vTaskDelete(NULL);
}

// Free RX buffer (not used as the buffer is static)
//
static void mesh_free(void *h, void *buffer)
{
    free(buffer);
}

// ==================== Root 下行 TX 辅助 ====================
// 注意：以下函数只在 mesh_netif_tx_task 线程上下文中运行，
// 绝不能在 lwIP(tcpip_thread)上下文里调用阻塞式 esp_mesh_send()。

static bool mesh_tx_is_blacklisted(const uint8_t *mac)
{
    TickType_t now = xTaskGetTickCount();
    for (int i = 0; i < MESH_TX_BLACKLIST_SIZE; i++)
    {
        if (s_tx_blacklist[i].invalid_until == 0)
        {
            continue;
        }
        if (memcmp(s_tx_blacklist[i].mac, mac, MAC_ADDR_LEN) == 0)
        {
            /*
             * 用无符号回绕比较判断截止时间：
             * now < invalid_until 时，(now - invalid_until) 回绕成一个很大的数。
             */
            if ((TickType_t)(now - s_tx_blacklist[i].invalid_until) > (TickType_t)0x7FFFFFFF)
            {
                return true; // 还没到截止时间，仍在惩罚期内
            }
            s_tx_blacklist[i].invalid_until = 0; // 已过期，腾出槽位
            return false;
        }
    }
    return false;
}

static void mesh_tx_blacklist_add(const uint8_t *mac)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t deadline = now + pdMS_TO_TICKS(MESH_TX_BLACKLIST_TTL_MS);
    int oldest = -1;

    for (int i = 0; i < MESH_TX_BLACKLIST_SIZE; i++)
    {
        if (s_tx_blacklist[i].invalid_until == 0 ||
            memcmp(s_tx_blacklist[i].mac, mac, MAC_ADDR_LEN) == 0)
        {
            oldest = i;
            break;
        }
    }
    if (oldest < 0)
    {
        oldest = 0;
    }

    if (s_tx_blacklist[oldest].invalid_until == 0)
    {
        ESP_LOGW(TAG, "Downlink to " MACSTR " failed, skip it for %u ms",
                 MAC2STR(mac), (unsigned)MESH_TX_BLACKLIST_TTL_MS);
    }
    memcpy(s_tx_blacklist[oldest].mac, mac, MAC_ADDR_LEN);
    s_tx_blacklist[oldest].invalid_until = deadline;
}

static void mesh_tx_blacklist_clear(const uint8_t *mac)
{
    for (int i = 0; i < MESH_TX_BLACKLIST_SIZE; i++)
    {
        if (s_tx_blacklist[i].invalid_until != 0 &&
            memcmp(s_tx_blacklist[i].mac, mac, MAC_ADDR_LEN) == 0)
        {
            s_tx_blacklist[i].invalid_until = 0;
            return;
        }
    }
}

/// @brief 子节点->Root 上行(TODS)
static esp_err_t mesh_tx_tods(uint8_t *frame, size_t len)
{
    mesh_data_t data = {
        .data = frame,
        .size = len,
        .proto = MESH_PROTO_AP,
        .tos = MESH_TOS_P2P,
    };

    esp_err_t err = esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Node mesh send failed: len=%u, err=0x%x, name=%s",
                 (unsigned int)len,
                 (unsigned int)err,
                 esp_err_to_name(err));
    }
    return err;
}

static esp_err_t mesh_root_tx_unicast(const uint8_t *dst_mac, uint8_t *frame, size_t len)
{
    mesh_addr_t dest_addr;
    mesh_data_t data;

    memcpy(dest_addr.addr, dst_mac, MAC_ADDR_LEN);
    data.data = frame;
    data.size = len;
    data.proto = MESH_PROTO_STA; // sending from root AP -> Node's STA
    data.tos = MESH_TOS_P2P;

    esp_err_t err = esp_mesh_send(&dest_addr, &data, MESH_DATA_P2P, NULL, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Root->" MACSTR " send failed: 0x%x %s",
                 MAC2STR(dst_mac), (unsigned)err, esp_err_to_name(err));
        mesh_tx_blacklist_add(dst_mac);
        return err;
    }
    mesh_tx_blacklist_clear(dst_mac);
    return ESP_OK;
}

/// @brief 广播帧下行：不再一次性对全路由表并发发送，而是限速逐个送达
static void mesh_root_tx_broadcast(uint8_t *frame, size_t len)
{
    static const uint8_t eth_broadcast[MAC_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
    int route_table_size = 0;
    uint8_t my_mac[MAC_ADDR_LEN];
    uint32_t sent = 0, skipped = 0;

    memset(route_table, 0, sizeof(route_table));
    esp_wifi_get_mac(WIFI_IF_STA, my_mac);

    esp_err_t err = esp_mesh_get_routing_table((mesh_addr_t *)route_table,
                                               CONFIG_MESH_ROUTE_TABLE_SIZE * 6,
                                               &route_table_size);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Get routing table failed: %s", esp_err_to_name(err));
        return;
    }
    if (route_table_size > CONFIG_MESH_ROUTE_TABLE_SIZE)
    {
        ESP_LOGW(TAG, "Routing table truncated: %d > %d (increase CONFIG_MESH_ROUTE_TABLE_SIZE)",
                 route_table_size, CONFIG_MESH_ROUTE_TABLE_SIZE);
        route_table_size = CONFIG_MESH_ROUTE_TABLE_SIZE;
    }

    for (int i = 0; i < route_table_size; i++)
    {
        if (MAC_ADDR_EQUAL(route_table[i].addr, my_mac) ||
            MAC_ADDR_EQUAL(route_table[i].addr, eth_broadcast))
        {
            continue;
        }
        if (mesh_tx_is_blacklisted(route_table[i].addr))
        {
            skipped++;
            continue;
        }
        if (sent > 0)
        {
            // 逐个目标之间留间隔，避免瞬间并发把mesh发送窗口打满
            vTaskDelay(pdMS_TO_TICKS(MESH_BCAST_TX_GAP_MS));
        }
        if (mesh_root_tx_unicast(route_table[i].addr, frame, len) == ESP_OK)
        {
            sent++;
        }
    }

    ESP_LOGD(TAG, "Broadcast delivered to %u node(s), %u skipped", (unsigned)sent, (unsigned)skipped);
}

/// @brief Root下行发送任务：真正执行 esp_mesh_send()
static void mesh_netif_tx_task(void *arg)
{
    mesh_tx_item_t *item = NULL;
    static const uint8_t eth_broadcast[MAC_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    ESP_LOGI(TAG, "Root downlink TX task started");

    while (s_mesh_tx_running)
    {
        if (xQueueReceive(s_mesh_tx_queue, &item, portMAX_DELAY) != pdTRUE || item == NULL)
        {
            continue;
        }

        if (item->tods)
        {
            mesh_tx_tods(item->data, item->len);
        }
        else if (MAC_ADDR_EQUAL(item->dst_mac, eth_broadcast))
        {
            mesh_root_tx_broadcast(item->data, item->len);
        }
        else
        {
            mesh_root_tx_unicast(item->dst_mac, item->data, item->len);
        }

        free(item);
    }

    s_mesh_tx_task_handle = NULL;
    ESP_LOGI(TAG, "Root downlink TX task stopped");
    vTaskDelete(NULL);
}

static bool mesh_netif_tx_task_start(void)
{
    if (s_mesh_tx_task_handle != NULL)
    {
        return true;
    }

    if (s_mesh_tx_queue == NULL)
    {
        s_mesh_tx_queue = xQueueCreate(MESH_ROOT_TX_QUEUE_LEN, sizeof(mesh_tx_item_t *));
        if (s_mesh_tx_queue == NULL)
        {
            ESP_LOGE(TAG, "Failed to create root TX queue");
            return false;
        }
    }

    s_mesh_tx_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(mesh_netif_tx_task,
                                             "mesh_root_tx",
                                             MESH_ROOT_TX_STACK,
                                             NULL,
                                             MESH_ROOT_TX_PRIO,
                                             &s_mesh_tx_task_handle,
                                             LG_APP_CPU_CORE);
    if (ret != pdPASS)
    {
        s_mesh_tx_running = false;
        s_mesh_tx_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create root TX task");
        return false;
    }
    return true;
}

// Transmit function variants
//
static esp_err_t mesh_netif_transmit_from_root_ap(void *h, void *buffer, size_t len)
{
    (void)h;
    if (buffer == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (len > MESH_MAX_FRAME_SIZE)
    {
        ESP_LOGE(TAG,
                 "Root->node frame exceeds Mesh limit: len=%u, max=%u",
                 (unsigned int)len,
                 (unsigned int)MESH_MAX_FRAME_SIZE);

        return ESP_ERR_MESH_EXCEED_MTU;
    }

    /*
     * 该函数运行在 lwIP(tcpip_thread)上下文，绝不能在这里做阻塞式 mesh 发送，
     * 否则一次下行拥塞就会把整个Root的TCP/IP栈卡死。
     * 这里只做一次帧拷贝并入队，交由 mesh_netif_tx_task 发送。
     */
    if (s_mesh_tx_queue == NULL)
    {
        ESP_LOGW(TAG, "Root TX task not ready, drop frame len=%u", (unsigned int)len);
        return ESP_ERR_INVALID_STATE;
    }

    mesh_tx_item_t *item = (mesh_tx_item_t *)malloc(sizeof(mesh_tx_item_t) + len);
    if (item == NULL)
    {
        ESP_LOGE(TAG, "No memory for root TX item, len=%u", (unsigned int)len);
        return ESP_ERR_NO_MEM;
    }

    memcpy(item->dst_mac, buffer, MAC_ADDR_LEN); // 以太头里的目的MAC
    item->len = len;
    memcpy(item->data, buffer, len);

    if (xQueueSend(s_mesh_tx_queue, &item, 0) != pdTRUE)
    {
        free(item);
        s_mesh_tx_dropped++;
        ESP_LOGW(TAG, "Root TX queue full, drop frame len=%u (total dropped=%u)",
                 (unsigned int)len, (unsigned int)s_mesh_tx_dropped);
        // 返回失败让lwIP走标准丢包处理，由TCP负责重传
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t mesh_netif_transmit_from_root_ap_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    return mesh_netif_transmit_from_root_ap(h, buffer, len);
}



static esp_err_t mesh_netif_transmit_from_node_sta(void *h,
                                                   void *buffer,
                                                   size_t len)
{
    (void)h;
    if (buffer == NULL || len == 0)
    {
        ESP_LOGE(TAG, "Invalid node mesh TX frame");
        return ESP_ERR_INVALID_ARG;
    }

    if (len > MESH_SAFE_WARN_SIZE)
    {
        ESP_LOGW(TAG,
                 "Large node->root frame: len=%u, TCP_MSS=%d",
                 (unsigned int)len,
                 CONFIG_LWIP_TCP_MSS);
    }

    if (len > MESH_MAX_FRAME_SIZE)
    {
        ESP_LOGE(TAG,
                 "Mesh frame exceeds limit: len=%u, max=%u",
                 (unsigned int)len,
                 (unsigned int)MESH_MAX_FRAME_SIZE);

        return ESP_ERR_MESH_EXCEED_MTU;
    }

    /*
     * 与Root下行同理：本函数运行在 lwIP 上下文，不能直接做阻塞式 mesh 发送。
     * 上行拥塞(Root在重连、父节点在切换)时把帧排队交给独立任务，
     * 避免子节点自己的 TCP/IP 栈被卡死导致 MQTT ping 发不出去。
     */
    if (s_mesh_tx_queue == NULL)
    {
        ESP_LOGW(TAG, "Mesh TX task not ready, drop uplink frame len=%u", (unsigned int)len);
        return ESP_ERR_INVALID_STATE;
    }

    mesh_tx_item_t *item = (mesh_tx_item_t *)malloc(sizeof(mesh_tx_item_t) + len);
    if (item == NULL)
    {
        ESP_LOGE(TAG, "No memory for TX item, len=%u", (unsigned int)len);
        return ESP_ERR_NO_MEM;
    }

    memset(item->dst_mac, 0, sizeof(item->dst_mac)); // TODS 不需要目的MAC
    item->len = len;
    item->tods = true;
    memcpy(item->data, buffer, len);

    if (xQueueSend(s_mesh_tx_queue, &item, 0) != pdTRUE)
    {
        free(item);
        s_mesh_tx_dropped++;
        ESP_LOGW(TAG, "Mesh TX queue full, drop uplink frame len=%u (total dropped=%u)",
                 (unsigned int)len, (unsigned int)s_mesh_tx_dropped);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t mesh_netif_transmit_from_node_sta_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    return mesh_netif_transmit_from_node_sta(h, buffer, len);
}

// Construct and Destruct functions
//
static esp_err_t mesh_driver_start_root_ap(esp_netif_t *esp_netif, void *args)
{
    mesh_netif_driver_t driver = args;
    driver->base.netif = esp_netif;
    esp_netif_driver_ifconfig_t driver_ifconfig = {
        .handle = driver,
        .transmit = mesh_netif_transmit_from_root_ap,
        .transmit_wrap = mesh_netif_transmit_from_root_ap_wrap,
        .driver_free_rx_buffer = mesh_free};

    return esp_netif_set_driver_config(esp_netif, &driver_ifconfig);
}

static esp_err_t mesh_driver_start_node_sta(esp_netif_t *esp_netif, void *args)
{
    mesh_netif_driver_t driver = args;
    driver->base.netif = esp_netif;
    esp_netif_driver_ifconfig_t driver_ifconfig = {
        .handle = driver,
        .transmit = mesh_netif_transmit_from_node_sta,
        .transmit_wrap = mesh_netif_transmit_from_node_sta_wrap,
        .driver_free_rx_buffer = mesh_free};

    return esp_netif_set_driver_config(esp_netif, &driver_ifconfig);
}

void mesh_delete_if_driver(mesh_netif_driver_t driver)
{
    // Stop the task once both drivers are removed
    //    receive_task_is_running = true;
    free(driver);
}

mesh_netif_driver_t mesh_create_if_driver(bool is_ap, bool is_root)
{
    mesh_netif_driver_t driver = calloc(1, sizeof(struct mesh_netif_driver));
    if (driver == NULL)
    {
        ESP_LOGE(TAG, "No memory to create a wifi interface handle");
        return NULL;
    }
    if (is_ap && is_root)
    {
        driver->base.post_attach = mesh_driver_start_root_ap;
    }
    else if (!is_ap && !is_root)
    {
        driver->base.post_attach = mesh_driver_start_node_sta;
    }
    else
    {
        free(driver); // 修复：原来的 return NULL 泄漏了刚申请的 driver
        return NULL;
    }

    if (!receive_task_is_running)
    {
        receive_task_is_running = true;
        if (xTaskCreatePinnedToCore(receive_task,
                                    "mesh_rx",
                                    LG_STACK_MESH_RX,
                                    NULL,
                                    LG_PRIO_MESH_RX,
                                    NULL,
                                    LG_APP_CPU_CORE) != pdPASS)
        {
            receive_task_is_running = false;
            free(driver);
            ESP_LOGE(TAG, "Failed to create mesh receive task");
            return NULL;
        }
    }

    /*
     * 所有经由 mesh 的 netif 发送都不再走 lwIP 线程直发，
     * 统一交给独立的 TX 任务排队发送。
     */
    if (!mesh_netif_tx_task_start())
    {
        free(driver);
        return NULL;
    }

    // save station mac address to exclude it from routing-table on broadcast
    esp_wifi_get_mac(WIFI_IF_STA, driver->sta_mac_addr);

    return driver;
}

esp_err_t mesh_netifs_destroy(void)
{
    receive_task_is_running = false;
    s_mesh_tx_running = false;
    return ESP_OK;
}

static void mesh_netif_init_station(void)
{
    // By default create a station that would connect to AP (expecting root to connect to external network)
    esp_netif_config_t cfg_sta = ESP_NETIF_DEFAULT_WIFI_STA();
    netif_sta = esp_netif_new(&cfg_sta);
    assert(netif_sta);
    ESP_ERROR_CHECK(esp_netif_attach_wifi_station(netif_sta));
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_sta_handlers());
}

// Init by default for both potential root and node
//
esp_err_t mesh_netifs_init(mesh_raw_recv_cb_t *cb)
{
    mesh_netif_init_station();
    s_mesh_raw_recv_cb = cb;
    return ESP_OK;
}

/**
 * @brief Starts AP esp-netif link over mesh (root's AP on mesh)
 */
static esp_err_t start_mesh_link_ap(void)
{
    uint8_t mac[MAC_ADDR_LEN];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    esp_netif_set_mac(netif_ap, mac);
    esp_netif_action_start(netif_ap, NULL, 0, NULL);
    return ESP_OK;
}

/**
 * @brief Starts station link over wifi (root node to the router)
 */
static esp_err_t start_wifi_link_sta(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_err_t ret;
    void *driver = esp_netif_get_io_driver(netif_sta);
    if ((ret = esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif_sta)) != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_register_if_rxcb for if=%p failed with %d", driver, ret);
        return ESP_FAIL;
    }
    esp_netif_set_mac(netif_sta, mac);
    esp_netif_action_start(netif_sta, NULL, 0, NULL);
    return ESP_OK;
}

/**
 * @brief Starts station link over mesh (node to root over mesh)
 */
static esp_err_t start_mesh_link_sta(void)
{
    uint8_t mac[MAC_ADDR_LEN];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_netif_set_mac(netif_sta, mac);
    esp_netif_action_start(netif_sta, NULL, 0, NULL);
    esp_netif_action_connected(netif_sta, NULL, 0, NULL);
    return ESP_OK;
}

/**
 * @brief Creates esp-netif for AP interface over mesh
 *
 * @return Pointer to esp-netif instance
 */
static esp_netif_t *create_mesh_link_ap(void)
{
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    base_cfg.if_desc = "mesh_link_ap";
    base_cfg.ip_info = &g_mesh_netif_subnet_ip;

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_AP};
    esp_netif_t *netif = esp_netif_new(&cfg);
    assert(netif);
    /*
     * 这里不调用 esp_netif_set_mtu()（当前 IDF 版本 v5.0 及以下没有该 API）。
     * 超长帧由 CONFIG_LWIP_TCP_MSS 约束：1360 + IP/TCP头(40) = 1400 <= MESH_MAX_FRAME_SIZE，
     * 详见文件头的编译期校检。
     */
    return netif;
}

/**
 * @brief Destroy esp-netif for AP interface over mesh
 */
static void destory_mesh_link_ap(void)
{
    if (netif_ap)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(netif_ap));
        esp_netif_action_disconnected(netif_ap, NULL, 0, NULL);
        mesh_delete_if_driver(esp_netif_get_io_driver(netif_ap));
        esp_netif_destroy(netif_ap);
        netif_ap = NULL;
    }
}

/**
 * @brief Creates esp-netif for station interface over mesh
 *
 * @note Interface needs to be started (later) using the above APIs
 * based on the actual configuration root/node,
 * since root connects normally over wifi
 *
 * @return Pointer to esp-netif instance
 */
static esp_netif_t *create_mesh_link_sta(void)
{
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    base_cfg.if_desc = "mesh_link_sta";

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA};
    esp_netif_t *netif = esp_netif_new(&cfg);
    assert(netif);
    /* 上行超长帧同样由 CONFIG_LWIP_TCP_MSS 约束，见文件头校检 */
    return netif;
}

bool mesh_netif_root_ap_ready(void)
{
    return (netif_ap != NULL);
}

esp_err_t mesh_netif_start_root_ap(bool is_root, uint32_t addr, bool rebuild)
{
    if (is_root)
    {
        /*
         * Root每次拿到外网IP时都会走到这里。
         * 之前无条件 destroy+create，DHCP租约表和NAPT会话会被清空，
         * 所有子节点已建立的TCP连接瞬间变成黑洞连接，只能等TCP RTO或
         * MQTT keepalive(60s)超时才恢复 —— 这是子节点大段"静默期"的根源。
         *
         * 现在的规则：
         *   - Root AP 还在且网关未变(rebuild=false)：什么都不做，保留全部会话；
         *   - Root AP 还在但网关真变了(rebuild=true)：只能重建，此时丢会话不可避免；
         *   - Root AP 不存在：首次创建。
         */
        if (mesh_netif_root_ap_ready())
        {
            if (!rebuild)
            {
                ESP_LOGD(TAG, "Root AP netif already up, keep DHCP leases and NAPT sessions");
                return ESP_OK;
            }
            ESP_LOGW(TAG, "Root AP gateway really changed, rebuild root AP netif (sessions will drop)");
        }

        destory_mesh_link_ap();
        netif_ap = create_mesh_link_ap();
        mesh_netif_driver_t driver = mesh_create_if_driver(true, true);
        if (driver == NULL)
        {
            ESP_LOGE(TAG, "Failed to create wifi interface handle");
            return ESP_FAIL;
        }
        esp_netif_attach(netif_ap, driver);
        set_dhcps_dns(netif_ap, addr);
        start_mesh_link_ap();
        //ip_napt_enable(g_mesh_netif_subnet_ip.ip.addr, 1);
        esp_netif_napt_enable(netif_ap);
    }
    return ESP_OK;
}

esp_err_t mesh_netifs_start(bool is_root)
{
    if (is_root)
    {
        // ROOT: need both sta should use standard wifi, AP mesh link netif

        // Root: Station
        if (netif_sta && strcmp(esp_netif_get_desc(netif_sta), "sta") == 0)
        {
            ESP_LOGI(TAG, "Already wifi station, no need to do anything");
        }
        else if (netif_sta && strcmp(esp_netif_get_desc(netif_sta), "mesh_link_sta") == 0)
        {
            esp_netif_action_disconnected(netif_sta, NULL, 0, NULL);
            mesh_delete_if_driver(esp_netif_get_io_driver(netif_sta));
            esp_netif_destroy(netif_sta);
            mesh_netif_init_station();
        }
        else if (netif_sta == NULL)
        {
            mesh_netif_init_station();
        }

        // Root: AP is initialized only if GLOBAL DNS configured
        // (otherwise have to wait until the actual DNS record received from the router)
#if CONFIG_MESH_USE_GLOBAL_DNS_IP
        mesh_netif_start_root_ap(true, htonl(DNS_IP_ADDR), false);
#endif
    }
    else
    {
        // NODE: create only STA in form of mesh link
        if (netif_sta && strcmp(esp_netif_get_desc(netif_sta), "mesh_link_sta") == 0)
        {
            ESP_LOGI(TAG, "Already mesh link station, no need to do anything");
            return ESP_OK;
        }
        if (netif_sta)
        {
            esp_netif_action_disconnected(netif_sta, NULL, 0, NULL);
            // should remove the actual driver
            if (strcmp(esp_netif_get_desc(netif_sta), "sta") == 0)
            {
                ESP_LOGI(TAG, "It was a wifi station removing stuff");
                esp_wifi_clear_default_wifi_driver_and_handlers(netif_sta);
            }
            esp_netif_destroy(netif_sta);
        }
        netif_sta = create_mesh_link_sta();
        // now we create a mesh driver and attach it to the existing netif
        mesh_netif_driver_t driver = mesh_create_if_driver(false, false);
        if (driver == NULL)
        {
            ESP_LOGE(TAG, "Failed to create wifi interface handle");
            return ESP_FAIL;
        }
        esp_netif_attach(netif_sta, driver);
        start_mesh_link_sta();
        // If we have a AP on NODE -> stop and remove it!
        destory_mesh_link_ap();
    }
    return ESP_OK;
}

esp_err_t mesh_netifs_stop(void)
{
    if (netif_sta && strcmp(esp_netif_get_desc(netif_sta), "sta") == 0 && netif_ap == NULL)
    {
        return ESP_OK;
    }

    if (netif_sta)
    {
        if (strcmp(esp_netif_get_desc(netif_sta), "sta") == 0)
        {
            esp_netif_action_disconnected(netif_sta, NULL, 0, NULL);
            esp_netif_action_stop(netif_sta, NULL, 0, NULL);
            esp_wifi_clear_default_wifi_driver_and_handlers(netif_sta);
        }
        else
        {
            esp_netif_action_disconnected(netif_sta, NULL, 0, NULL);
            mesh_delete_if_driver(esp_netif_get_io_driver(netif_sta));
        }
        esp_netif_destroy(netif_sta);
        netif_sta = NULL;
    }

    destory_mesh_link_ap();
    // reserve the default (STA gets ready to become root)
    mesh_netif_init_station();
    start_wifi_link_sta();
    return ESP_OK;
}

// uint8_t *mesh_netif_get_station_mac(void)
//{
// mesh_netif_driver_t mesh = esp_netif_get_io_driver(netif_sta);
// return mesh->sta_mac_addr;
//}

uint8_t *mesh_netif_get_station_mac(void)
{
    static uint8_t s_sta_mac[6];
    static bool initialized = false;

    if (!initialized)
    {
        // 直接从WiFi驱动读取MAC地址
        esp_read_mac(s_sta_mac, ESP_MAC_WIFI_STA);
        initialized = true;
        ESP_LOGI(UART_TAG, "Station MAC initialized: " MACSTR, MAC2STR(s_sta_mac));
    }

    return s_sta_mac;
}