/* Mesh Internal Communication Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <inttypes.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "nvs_flash.h"
#include "mesh_netif.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"
#include "mesh.h"
#include "task_config.h"
#include "stdio.h"
#include "ConfigData.h"
#include "esp_timer.h"
#include <stdio.h>
#include <unistd.h>
#include "esp_netif_sntp.h"
#include "esp_heap_caps.h"

/*******************************************************
 *                Macros
 *******************************************************/

/*******************************************************
 *                Constants
 *******************************************************/
const char *MESH_TAG = "mesh_main";

/*******************************************************
 *                Variable Definitions
 *******************************************************/
static bool is_running = true;
static mesh_addr_t mesh_parent_addr;
static int mesh_layer = -1;
esp_ip4_addr_t s_current_ip;

static bool s_dual_net_inited = false; // 标志位，确保 dual_net_init() 只被调用一次
static uint32_t s_last_root_ap_gw = 0;

static TaskHandle_t s_sntp_task_handle = NULL;
static bool s_sntp_inited = false;
static bool s_time_synced = false;
// static bool s_leaf_applied = false;

// 这些变量需要被其他文件访问，必须定义为全局（不加static）
// 并且要初始化

mesh_addr_t s_route_table[CONFIG_MESH_ROUTE_TABLE_SIZE] = {0}; // 路由表，最多支持 CONFIG_MESH_ROUTE_TABLE_SIZE 条路由
int s_route_table_size = 0;                                    // 当前路由表大小
SemaphoreHandle_t s_route_table_lock = NULL;                   // 路由表锁，保护 s_route_table 和 s_route_table_size 的访问

static uint8_t s_mesh_tx_payload[CONFIG_MESH_ROUTE_TABLE_SIZE * 6 + 1];

mesh_router_t list_router[WiFiNums] = {0};
// 备用WiFi
static int wifinum = 0;

/// @brief 获取列表中的WiFi信息
/// @param
static void list_router_get(void)
{
    list_router[0].ssid_len = strlen(SSID);
    memcpy((uint8_t *)&list_router[0].ssid, SSID, list_router[0].ssid_len);
    memcpy((uint8_t *)&list_router[0].password, WIFI_PS, strlen(WIFI_PS));
    // printf("ssid0:%s,password:%s,ssid0length:%d\n",list_router[0].ssid,list_router[0].password,list_router[0].ssid_len);

    list_router[1].ssid_len = strlen(SSID1);
    memcpy((uint8_t *)&list_router[1].ssid, SSID1, list_router[1].ssid_len);
    memcpy((uint8_t *)&list_router[1].password, WIFI_PS1, strlen(WIFI_PS1));
    // printf("ssid1:%s,password:%s,ssid1length:%d\n",list_router[1].ssid,list_router[1].password,list_router[1].ssid_len);
}

/*******************************************************
 *                Function Declarations
 *******************************************************/

/*******************************************************
 *                Function Definitions
 *******************************************************/

//====================BLE回调函数====================

// 全局变量，用于存储根节点MAC地址（子节点需要知道根节点是谁）
uint8_t s_root_mac[6] = {0};

// 断开WiFi之后恢复组网
static int R_NET = 0;

//====================远程BLE命令====================

bool parse_mac_address(const char *str, uint8_t *mac)
{
    int values[6];
    int count = sscanf(str, "%x:%x:%x:%x:%x:%x",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]);

    if (count != 6)
        return false;

    for (int i = 0; i < 6; i++)
    {
        if (values[i] < 0 || values[i] > 255)
            return false;
        mac[i] = (uint8_t)values[i];
    }
    return true;
}

void send_mesh_message(uint8_t *target_mac, uint8_t *data, int len)
{
    mesh_addr_t target_addr;
    memcpy(target_addr.addr, target_mac, 6);

    mesh_data_t mesh_data;
    mesh_data.size = len;
    mesh_data.proto = MESH_PROTO_BIN;
    mesh_data.tos = MESH_TOS_P2P;
    mesh_data.data = data;

    esp_err_t err = esp_mesh_send(&target_addr, &mesh_data, MESH_DATA_P2P, NULL, 0);

    if (err == ESP_OK)
    {
        printf("Message sent to " MACSTR "\n", MAC2STR(target_mac));
    }
    else
    {
        printf("Failed to send message: %d\n", err);
    }
}

// ==================== Mesh数据接收回调 ====================

static void recv_cb(mesh_addr_t *from, mesh_data_t *data)
{
    // 取出数据中的命令
    uint8_t cmd = data->data[0];

    // 如果是根节点，记录子节点的MAC（可用于后续通信）
    if (esp_mesh_is_root() && s_root_mac[0] == 0)
    {
        // 这里可以记录第一个通信的节点作为参考
    }

    // 如果是子节点，记录根节点的MAC
    if (!esp_mesh_is_root() && s_root_mac[0] == 0)
    {
        memcpy(s_root_mac, from->addr, 6);
        ESP_LOGI(MESH_TAG, "Recorded root MAC: " MACSTR, MAC2STR(s_root_mac));
    }

    switch (cmd)
    {
    case CMD_ROUTE_TABLE:
    {
        // 原有路由表处理
        int size = data->size - 1;
        if (s_route_table_lock == NULL || size % 6 != 0)
        {
            ESP_LOGE(MESH_TAG, "Error in receiving raw mesh data: Unexpected size");
            return;
        }
        xSemaphoreTake(s_route_table_lock, portMAX_DELAY);
        s_route_table_size = size / 6;
        // 获取当前路由表
        memcpy(&s_route_table, data->data + 1, size); //
        xSemaphoreGive(s_route_table_lock);
        break;
    }

    case CMD_KEYPRESSED:
    {
        // 原有按键处理
        if (data->size != 7)
        {
            ESP_LOGE(MESH_TAG, "Error in receiving raw mesh data: Unexpected size");
            return;
        }
        ESP_LOGW(MESH_TAG, "Keypressed detected on node: " MACSTR,
                 MAC2STR(data->data + 1));
        break;
    }

    // ===== BLE命令处理 =====
    case CMD_BLE_SCAN_START:
    {
        uint8_t scan_time = data->data[1];
        ESP_LOGI(MESH_TAG, "Received BLE scan command from " MACSTR,
                 MAC2STR(from->addr));
        printf("\n[Remote Command] Start BLE scan for %d seconds\n", scan_time);
        // ble_start_scan(scan_time);
        break;
    }

    case CMD_BLE_CONNECT:
    {
        uint8_t name_len = data->data[1];
        char device_name[64];
        memcpy(device_name, data->data + 2, name_len);
        device_name[name_len] = '\0';

        ESP_LOGI(MESH_TAG, "Received BLE connect command for '%s' from " MACSTR,
                 device_name, MAC2STR(from->addr));
        printf("\n[Remote Command] Connect to BLE device: %s\n", device_name);
        // ble_connect_by_name(device_name);
        break;
    }

    case CMD_BLE_SEND_DATA:
    {
        uint8_t data_len = data->data[1];
        ESP_LOGI(MESH_TAG, "Received BLE send command from " MACSTR,
                 MAC2STR(from->addr));
        printf("\n[Remote Command] Send data to BLE: %.*s\n",
               data_len, data->data + 2);
        // ble_send_data(data->data + 2, data_len);
        break;
    }

    case CMD_BLE_DISCONNECT:
    {
        ESP_LOGI(MESH_TAG, "Received BLE disconnect command from " MACSTR,
                 MAC2STR(from->addr));
        printf("\n[Remote Command] Disconnect BLE device\n");
        // ble_disconnect();
        break;
    }

    case CMD_BLE_SCAN_RESULT:
    {
        // 根节点接收子节点的扫描结果
        ESP_LOGI(MESH_TAG, "Received BLE scan results from " MACSTR,
                 MAC2STR(from->addr));

        int device_count = data->data[1];
        int pos = 2;

        printf("\n=== BLE Scan Results from Node " MACSTR " ===\n",
               MAC2STR(from->addr));

        for (int i = 0; i < device_count && pos < data->size; i++)
        {
            int name_len = data->data[pos++];
            char name[64];
            memcpy(name, data->data + pos, name_len);
            name[name_len] = '\0';
            pos += name_len;

            uint8_t *bda = data->data + pos;
            pos += 6;

            int8_t rssi = (int8_t)data->data[pos++];

            printf("[%d] %s\n", i + 1, name);
            printf("     Address: " ESP_BD_ADDR_STR "\n", ESP_BD_ADDR_HEX1(bda));
            printf("     RSSI: %d dBm\n\n", rssi);
        }
        break;
    }

    case CMD_BLE_CONNECT_RESULT:
    {
        uint8_t success = data->data[1];
        uint8_t name_len = data->data[2];
        char device_name[64];
        memcpy(device_name, data->data + 3, name_len);
        device_name[name_len] = '\0';

        printf("\n[Node " MACSTR "] BLE connect %s: %s\n",
               MAC2STR(from->addr),
               success ? "success" : "failed",
               device_name);
        break;
    }

    case CMD_BLE_DATA_FORWARD:
    {
        // 根节点接收子节点转发的BLE数据
        uint8_t data_len = data->data[1];
        ESP_LOGI(MESH_TAG, "Received forwarded BLE data from " MACSTR ", len=%d",
                 MAC2STR(from->addr), data_len);

        printf("\n[BLE Data from Node " MACSTR "] ", MAC2STR(from->addr));
        for (int i = 0; i < data_len && i < 20; i++)
        {
            printf("%02x ", data->data[2 + i]);
        }
        if (data_len > 20)
            printf("...");
        printf("\n");
        break;
    }

    case CMD_USER_MSG:
    {
        // 解析收到的消息
        uint8_t *sender_mac = data->data + 1;     // 发送者MAC
        char *message = (char *)(data->data + 7); // 消息内容
        int msg_len = data->size - 7;

        // 打印收到的消息
        ESP_LOGI(MESH_TAG, "收到来自 " MACSTR " 的消息: %.*s",
                 MAC2STR(sender_mac), msg_len, message);

        // 控制台显示
        printf("\n[收到消息] 来自 " MACSTR ": %.*s\n> ",
               MAC2STR(sender_mac), msg_len, message);
        fflush(stdout);

        // ===== 发送回执（确认消息）=====
        mesh_data_t ack_data;
        uint8_t *my_mac = mesh_netif_get_station_mac();

// 构造回执消息: [CMD_MSG_ACK] [本机MAC] [收到的消息前20字节]
#define MAX_ECHO_LEN 20
        int echo_len = (msg_len < MAX_ECHO_LEN) ? msg_len : MAX_ECHO_LEN;
        uint8_t ack_buffer[1 + 6 + echo_len + 1];

        ack_buffer[0] = CMD_MSG_ACK;               // 命令：消息确认
        memcpy(ack_buffer + 1, my_mac, 6);         // 本机MAC
        memcpy(ack_buffer + 7, message, echo_len); // 回显部分消息内容
        ack_buffer[7 + echo_len] = '\0';           // 字符串结束符

        ack_data.size = 1 + 6 + echo_len;
        ack_data.proto = MESH_PROTO_BIN;
        ack_data.tos = MESH_TOS_P2P;
        ack_data.data = ack_buffer;

        // 发送回执给原始发送者
        mesh_addr_t target;
        memcpy(target.addr, sender_mac, 6);

        esp_err_t err = esp_mesh_send(&target, &ack_data, MESH_DATA_P2P, NULL, 0);
        if (err == ESP_OK)
        {
            ESP_LOGI(MESH_TAG, "已发送回执给 " MACSTR, MAC2STR(sender_mac));
        }
        else
        {
            ESP_LOGE(MESH_TAG, "发送回执失败: %d", err);
        }
        break;
    }
    case CMD_MSG_ACK:
    {
        // 处理收到的回执消息
        uint8_t *ack_sender = data->data + 1;      // 发送回执的节点MAC
        char *echo_msg = (char *)(data->data + 7); // 回显的消息内容
        int echo_len = data->size - 7;

        ESP_LOGI(MESH_TAG, "收到来自 " MACSTR " 的回执: %.*s",
                 MAC2STR(ack_sender), echo_len, echo_msg);

        // 控制台显示
        printf("\n[回执] 节点 " MACSTR " 已收到消息: %.*s\n> ",
               MAC2STR(ack_sender), echo_len, echo_msg);
        break;
    }
    default:
        ESP_LOGD(MESH_TAG, "Unknown command: 0x%02x", cmd);
        break;
    }
}
// 根节点发送路由表给其子节点，这里不使用
void esp_mesh__task(void *arg)
{
    is_running = true;
    char *print;
    mesh_data_t data;
    esp_err_t err;

    while (is_running)
    {
        asprintf(&print, "layer:%d IP:" IPSTR, esp_mesh_get_layer(), IP2STR(&s_current_ip));
        ESP_LOGI(MESH_TAG, "Tried to publish %s", print);

        free(print);
        if (esp_mesh_is_root())
        { // 只有是根节点，就发送路由表
            // 获取当前路由表
            esp_mesh_get_routing_table((mesh_addr_t *)&s_route_table,
                                       CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &s_route_table_size);

            // 构造路由表数据
            data.size = s_route_table_size * 6 + 1;
            data.proto = MESH_PROTO_BIN;
            data.tos = MESH_TOS_P2P;
            s_mesh_tx_payload[0] = CMD_ROUTE_TABLE; // 帧头命令
            memcpy(s_mesh_tx_payload + 1, s_route_table, s_route_table_size * 6);
            data.data = s_mesh_tx_payload;

            // 发送路由表
            for (int i = 0; i < s_route_table_size; i++)
            {
                err = esp_mesh_send(&s_route_table[i], &data, MESH_DATA_P2P, NULL, 0);
                ESP_LOGI(MESH_TAG, "Sending routing table to [%d] " MACSTR ": sent with err code: %d", i, MAC2STR(s_route_table[i].addr), err);
            }
        }
        vTaskDelay(2 * 1000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

// 显示路由表
void Mesh_cmd_list(void)
{
    if (s_route_table_lock == NULL)
    {
        printf("错误: 路由表未初始化\n");
        return;
    }

    xSemaphoreTake(s_route_table_lock, portMAX_DELAY);
    printf("\n路由表 (%d 个节点):\n", s_route_table_size);
    printf("  本机MAC: " MACSTR "\n", MAC2STR(mesh_netif_get_station_mac()));
    printf("  索引 | MAC地址            | 是否本机\n");
    printf("  ------|-------------------|--------\n");

    uint8_t *my_mac = mesh_netif_get_station_mac();
    for (int i = 0; i < s_route_table_size; i++)
    {
        int is_self = MAC_ADDR_EQUAL(s_route_table[i].addr, my_mac); // 判断是否为本机
        printf("  [%d]   | " MACSTR " | %s\n", i, MAC2STR(s_route_table[i].addr), is_self ? "本机" : "");
    }
    fflush(stdout);
    xSemaphoreGive(s_route_table_lock);
}

// 显示本机信息
void Mesh_cmd_info(void)
{
    printf("\n本机信息:\n");
    printf("  MAC地址: " MACSTR "\n", MAC2STR(mesh_netif_get_station_mac()));
    printf("  IP地址: " IPSTR "\n", IP2STR(&s_current_ip));
    printf("  Mesh层级: %d\n", mesh_layer);
    printf("  角色: %s\n", esp_mesh_is_root() ? "根节点" : "子节点");
    if (mesh_layer > 0)
    {
        mesh_type_t type = esp_mesh_get_type();
        printf("  Mesh类型: %s\n",
               type == MESH_ROOT ? "MESH_ROOT" : type == MESH_NODE ? "MESH_NODE"
                                             : type == MESH_LEAF   ? "MESH_LEAF"
                                                                   : "UNKNOWN");
    }

    printf("  路由表大小: %d\n", s_route_table_size);
    printf("  路由表大小: %d\n", s_route_table_size);

    fflush(stdout);
}
// 解析MAC地址字符串为字节数组
bool Mesh_parse_mac_address(const char *str, uint8_t *mac)
{
    int values[6];
    int count = sscanf(str, "%x:%x:%x:%x:%x:%x",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]);

    if (count != 6)
    {
        return false;
    }

    for (int i = 0; i < 6; i++)
    {
        if (values[i] < 0 || values[i] > 255)
        {
            return false;
        }
        mac[i] = (uint8_t)values[i];
    }
    return true;
}
// 向指定MAC地址发送消息
void cmd_send(uint8_t *target_mac, const char *message)
{
    mesh_data_t data;
    uint8_t *my_mac = mesh_netif_get_station_mac();
    int msg_len = strlen(message);

    // 构造数据包: [CMD] [发送者MAC] [消息内容]
    uint8_t *data_to_send = malloc(1 + 6 + msg_len + 1);
    if (data_to_send == NULL)
    {
        printf("错误: 内存分配失败\n");
        return;
    }

    data_to_send[0] = CMD_USER_MSG;             // 命令
    memcpy(data_to_send + 1, my_mac, 6);        // 发送者MAC
    memcpy(data_to_send + 7, message, msg_len); // 消息内容
    data_to_send[7 + msg_len] = '\0';           // 字符串结束符

    data.size = 1 + 6 + msg_len;
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;
    data.data = data_to_send;

    // 检查目标是否在路由表中
    bool found = false;
    mesh_addr_t target_addr;
    memcpy(target_addr.addr, target_mac, 6);

    xSemaphoreTake(s_route_table_lock, portMAX_DELAY);
    for (int i = 0; i < s_route_table_size; i++)
    {
        if (MAC_ADDR_EQUAL(s_route_table[i].addr, target_mac))
        {
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_route_table_lock);

    if (!found)
    {
        printf("错误: 目标MAC不在路由表中\n");
        free(data_to_send);
        return;
    }

    // 发送消息
    esp_err_t err = esp_mesh_send(&target_addr, &data, MESH_DATA_P2P, NULL, 0);

    if (err == ESP_OK)
    {
        printf("消息已发送到 " MACSTR ": %s\n", MAC2STR(target_mac), message);
    }
    else
    {
        printf("发送失败, 错误码: %d\n", err);
    }

    free(data_to_send);
}

// 广播消息到所有节点
void cmd_broadcast(const char *message)
{
    mesh_data_t data;
    uint8_t *my_mac = mesh_netif_get_station_mac();
    int msg_len = strlen(message);

    // 构造数据包
    uint8_t *data_to_send = malloc(1 + 6 + msg_len + 1);
    if (data_to_send == NULL)
    {
        printf("错误: 内存分配失败\n");
        return;
    }

    data_to_send[0] = CMD_USER_MSG;
    memcpy(data_to_send + 1, my_mac, 6);
    memcpy(data_to_send + 7, message, msg_len);
    data_to_send[7 + msg_len] = '\0';

    data.size = 1 + 6 + msg_len;
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;
    data.data = data_to_send;

    int sent_count = 0;
    xSemaphoreTake(s_route_table_lock, portMAX_DELAY);

    for (int i = 0; i < s_route_table_size; i++)
    {
        // 不给自己发送
        if (MAC_ADDR_EQUAL(s_route_table[i].addr, my_mac))
        {
            continue;
        }

        esp_err_t err = esp_mesh_send(&s_route_table[i], &data, MESH_DATA_P2P, NULL, 0);
        if (err == ESP_OK)
        {
            sent_count++;
            ESP_LOGI("CMD", "广播到 [%d] " MACSTR " 成功",
                     i, MAC2STR(s_route_table[i].addr));
        }
    }

    xSemaphoreGive(s_route_table_lock);

    printf("广播完成: 已发送给 %d 个节点\n", sent_count);
    free(data_to_send);
}

/// @brief 网络就绪回调函数，在网络连接成功后被调用，用于启动相关服务
static void on_network_ready(void)
{
    ESP_LOGI(MESH_TAG, "=== Network is ready, starting services ===");

    if (dual_net_is_ethernet_active()) // 如果以太网已连接，切换默认路由到以太网
    {
        ESP_LOGI(MESH_TAG, "Ethernet active, keep WiFi STA for mesh but use Ethernet as default route");
    }
}

/// @brief 验证mesh路由器配置是否有效
/// @param ssid
/// @param password
/// @return
static bool mesh_router_cfg_valid(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL)
    {
        return false;
    }

    if (strlen(ssid) == 0 || strcmp(ssid, "default") == 0)
    {
        return false;
    }

    if (strcmp(password, "default") == 0)
    {
        return false;
    }

    return true;
}

static void sntp_sync_task(void *arg)
{
    ESP_LOGI(MESH_TAG, "SNTP sync task started");

    if (!s_sntp_inited)
    {
        esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_err_t err = esp_netif_sntp_init(&sntp_config);

        if (err == ESP_OK)
        {
            s_sntp_inited = true;
            ESP_LOGI(MESH_TAG, "SNTP initialized");
        }
        else if (err == ESP_ERR_INVALID_STATE)
        {
            s_sntp_inited = true;
            ESP_LOGW(MESH_TAG, "SNTP already initialized");
        }
        else
        {
            ESP_LOGE(MESH_TAG, "SNTP init failed: %s", esp_err_to_name(err));
            s_sntp_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
    }

    setenv("TZ", "CST-8", 1);
    tzset();

    ESP_LOGI(MESH_TAG, "Waiting for SNTP time sync...");

    esp_err_t ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));

    if (ret == ESP_OK)
    {
        s_time_synced = true;
        ESP_LOGI(MESH_TAG, "SNTP time sync completed");
    }
    else
    {
        s_time_synced = false;
        ESP_LOGW(MESH_TAG, "SNTP time sync timeout, continue without blocking network");
    }

    s_sntp_task_handle = NULL;
    vTaskDelete(NULL);
}

static void start_sntp_sync_task(void)
{
    if (s_time_synced)
    {
        return;
    }

    if (s_sntp_task_handle != NULL)
    {
        return;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        sntp_sync_task,
        "sntp_sync",
        LG_STACK_SNTP,
        NULL,
        LG_PRIO_SNTP,
        &s_sntp_task_handle,
        LG_APP_CPU_CORE);

    if (ret != pdPASS)
    {
        ESP_LOGE(MESH_TAG, "Failed to create SNTP sync task");
        s_sntp_task_handle = NULL;
    }
}

// 刷新根节点AP网关地址
static void refresh_root_ap_gateway(uint32_t gw_addr)
{
    if (!esp_mesh_is_root())
    {
        return;
    }

    if (gw_addr == 0)
    {
        return;
    }

    // if (s_last_root_ap_gw == gw_addr)// 网关地址没有变化，不更新
    // {
    //     ESP_LOGI(MESH_TAG, "Root AP gateway unchanged, skip mesh_netif_start_root_ap");
    //     return;
    // }

    s_last_root_ap_gw = gw_addr;
    esp_err_t ret = mesh_netif_start_root_ap(true, gw_addr);
    if (ret == ESP_OK)
    {
        start_sntp_sync_task();
    }
}

// mesh事件处理

static bool s_child_services_started = false;
int compare_ssid(mesh_router_t *router, wifi_ap_record_t *ap)
{
    // 使用 mesh_router 的 ssid_len 进行比较
    if (router->ssid_len != strlen((char *)ap->ssid))
    {
        return 0; // 长度不同，不相等
    }

    return memcmp(router->ssid, ap->ssid, router->ssid_len) == 0;
}

static int router_switch_attempted = 1;
static int noparentfountnum = 0;
// 设备已经连接过标志
static int RouterIsConnected = 0;

static void mesh_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    mesh_addr_t id = {
        0,
    };
    static uint8_t last_layer = 0;

    switch (event_id)
    {
    case MESH_EVENT_STARTED:
    {
        esp_mesh_get_id(&id);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_MESH_STARTED>ID:" MACSTR "", MAC2STR(id.addr));
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_STOPPED:
    {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOPPED>");
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_CHILD_CONNECTED:
    {
        mesh_event_child_connected_t *child_connected = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, " MACSTR "",
                 child_connected->aid,
                 MAC2STR(child_connected->mac));
    }
    break;
    case MESH_EVENT_CHILD_DISCONNECTED:
    {
        mesh_event_child_disconnected_t *child_disconnected = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, " MACSTR "",
                 child_disconnected->aid,
                 MAC2STR(child_disconnected->mac));
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_ADD:
    {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new);
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_REMOVE:
    {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new);
    }
    break;
    // 遇到触发再来修改
    case MESH_EVENT_NO_PARENT_FOUND:
    {
        mesh_event_no_parent_found_t *no_parent = (mesh_event_no_parent_found_t *)event_data;

        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d", no_parent->scan_times);
        if (IsRoot == 1)
        {

            if (RouterIsConnected == 0)
            {
                if (router_switch_attempted == 0)
                {

                    if (noparentfountnum > 2)
                    {
                        router_switch_attempted = 1;
                        noparentfountnum = 0;
                    }
                    noparentfountnum++;
                    printf("已经使用备用WiFi次数:%d\n", noparentfountnum);
                    printf("ssid:%s,password:%s,ssidlength:%d\n", list_router[wifinum].ssid, list_router[wifinum].password, list_router[wifinum].ssid_len);
                    break;
                }
                wifinum = wifinum + 1;
                if (wifinum > WiFiNums - 1)
                {
                    wifinum = 0;
                }
                printf("ssid:%s,password:%s,ssidlength:%d\n", list_router[wifinum].ssid, list_router[wifinum].password, list_router[wifinum].ssid_len);

                esp_mesh_set_self_organized(false, false);
                mesh_router_t router = {0};
                router.ssid_len = list_router[wifinum].ssid_len;
                memcpy((uint8_t *)&router.ssid, &list_router[wifinum].ssid, router.ssid_len);
                memcpy((uint8_t *)&router.password, &list_router[wifinum].password, 8);
                esp_mesh_set_router(&router);
                esp_mesh_set_self_organized(true, true);
            }
            else if (RouterIsConnected == 1)
            {
                if (router_switch_attempted == 0)
                {

                    if (noparentfountnum > 2)
                    {
                        wifinum = wifinum + 1;
                        if (wifinum > WiFiNums - 1)
                        {
                            wifinum = 0;
                        }
                        router_switch_attempted = 1;
                        noparentfountnum = 0;
                    }
                    noparentfountnum++;
                    printf("已经使用备用WiFi次数:%d\n", noparentfountnum);
                    printf("ssid:%s,password:%s,ssidlength:%d\n", list_router[wifinum].ssid, list_router[wifinum].password, list_router[wifinum].ssid_len);
                    break;
                }
                esp_mesh_set_self_organized(false, false);
                mesh_router_t router = {0};
                router.ssid_len = list_router[wifinum].ssid_len;
                memcpy((uint8_t *)&router.ssid, &list_router[wifinum].ssid, router.ssid_len);
                memcpy((uint8_t *)&router.password, &list_router[wifinum].password, 8);
                esp_mesh_set_router(&router);
                esp_mesh_set_self_organized(true, true);
            }

            router_switch_attempted = 0;
        }
    }
    /* TODO handler for the failure */
    break;
    case MESH_EVENT_PARENT_CONNECTED:
    {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        esp_mesh_get_id(&id);
        mesh_layer = connected->self_layer;

        memcpy(&mesh_parent_addr.addr, connected->connected.bssid, 6);

        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:" MACSTR "%s, ID:" MACSTR,
                 last_layer, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                 esp_mesh_is_root() ? "<ROOT>" : (mesh_layer == 2) ? "<layer2>"
                                                                   : "",
                 MAC2STR(id.addr));

        last_layer = mesh_layer;

        // 根节点：保持你原来的逻辑
        if (esp_mesh_is_root())
        {
            RouterIsConnected = 1;
            dual_net_set_mesh_switching(true); // dual网卡WiFi关闭

            // 获取当前连接的WiFi
            wifi_ap_record_t ap_info = {0};
            // 获取当前连接的 AP 信息
            esp_wifi_sta_get_ap_info(&ap_info);

            // 遍历WiFi路由表，找到当前连接的 WiFi SSID
            for (int i = 0; i < WiFiNums; i++)
            {

                if (compare_ssid(&list_router[i], &ap_info))
                {
                    wifinum = i + 1;            // 记录当前连接的WiFi序号的下一个，便于切换到下一个WiFi
                    if (wifinum > WiFiNums - 1) // 防止超出WiFi列表范围
                    {
                        wifinum = 0;
                    }
                    ESP_LOGI(MESH_TAG, "当前连接的WiFi SSID: %s, 切换到列表中的WiFi #%d", ap_info.ssid, i);
                }
            }

            if (mesh_layer == 1 && !s_dual_net_inited)
            {
                // ESP_LOGI(MESH_TAG, "根节点初始化双网卡");
                //  dual_net_init();
                dual_net_register_ready_callback(on_network_ready);
                s_dual_net_inited = true;
            }
        }
        // 子节点：每次连上 parent，都先切换到 mesh_link_sta
        if (!esp_mesh_is_root())
        {
            esp_err_t err = mesh_netifs_start(false); //
            if (err != ESP_OK)
            {
                ESP_LOGE(MESH_TAG, "mesh_netifs_start(false) failed: %s", esp_err_to_name(err));
            }
            else
            {
                ESP_LOGI(MESH_TAG, "mesh_link_sta started for child node");
            }

            memcpy(s_root_mac, connected->connected.bssid, 6);
            ESP_LOGI(MESH_TAG, "Set root/parent MAC on parent connected: " MACSTR, MAC2STR(s_root_mac));

            if (!s_child_services_started)
            {
                ESP_LOGI(MESH_TAG, "子节点已接入mesh，等待IP后启动本地网络任务");
                s_child_services_started = true;
            }
        }
    }
    break;
    case MESH_EVENT_PARENT_DISCONNECTED:
    {

        if (!esp_mesh_is_root())
        {
            mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d", disconnected->reason);

            mesh_layer = esp_mesh_get_layer();
            s_last_root_ap_gw = 0;
            // 只有非 root 节点才停 mesh netif
            // mesh_netifs_stop();
        }
        else if (esp_mesh_is_root())
        {

            mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_PARENT_DISCONNECTED> reason:%d", disconnected->reason);

            // ===== 防抖：3秒内不重复处理 =====
            static int s_last_disconnect_time = 0;
            int now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now - s_last_disconnect_time < 3000)
            {
                ESP_LOGW(MESH_TAG, "Ignore duplicate PARENT_DISCONNECTED within 3s");
                break;
            }
            s_last_disconnect_time = now;

            mesh_layer = esp_mesh_get_layer();
            s_last_root_ap_gw = 0;

            ESP_LOGW(MESH_TAG, "Root parent disconnected, switch to backup router but keep root mesh AP alive");

            dual_net_set_mesh_switching(false);

            esp_mesh_set_self_organized(false, false);

            esp_wifi_disconnect(); // 断开当前WiFi连接
            R_NET = 1;

            noparentfountnum = 0;
            router_switch_attempted = 1;

            wifi_config_t parent_config = {
                .sta = {
                    .channel = 0, // 自动选择信道
                },
            };
            memcpy(parent_config.sta.ssid, list_router[wifinum].ssid, list_router[wifinum].ssid_len);
            memcpy(parent_config.sta.password, list_router[wifinum].password, 8);

            printf("Switching to backup WiFi #%d: %s\n", wifinum, list_router[wifinum].ssid);

            esp_wifi_set_config(WIFI_IF_STA, &parent_config);

            wifinum = wifinum + 1;      // 记录当前连接的WiFi序号的下一个，便于切换到下一个WiFi
            if (wifinum > WiFiNums - 1) // 防止超出WiFi列表范围
            {
                wifinum = 0;
            }
        }
    }
    break;
    case MESH_EVENT_LAYER_CHANGE:
    {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
        mesh_layer = layer_change->new_layer;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s",
                 last_layer, mesh_layer,
                 esp_mesh_is_root() ? "<ROOT>" : (mesh_layer == 2) ? "<layer2>"
                                                                   : "");
        last_layer = mesh_layer;
    }
    break;
    case MESH_EVENT_ROOT_ADDRESS:
    {
        mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_ADDRESS>root address:" MACSTR "",
                 MAC2STR(root_addr->addr));
    }
    break;
    case MESH_EVENT_VOTE_STARTED:
    {
        mesh_event_vote_started_t *vote_started = (mesh_event_vote_started_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, rc_addr:" MACSTR "",
                 vote_started->attempts,
                 vote_started->reason,
                 MAC2STR(vote_started->rc_addr.addr));
    }
    break;
    case MESH_EVENT_VOTE_STOPPED:
    {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STOPPED>");
        break;
    }
    case MESH_EVENT_ROOT_SWITCH_REQ:
    {
        mesh_event_root_switch_req_t *switch_req = (mesh_event_root_switch_req_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:" MACSTR "",
                 switch_req->reason,
                 MAC2STR(switch_req->rc_addr.addr));
    }
    break;
    case MESH_EVENT_ROOT_SWITCH_ACK:
    {
        /* new root */
        mesh_layer = esp_mesh_get_layer();
        esp_mesh_get_parent_bssid(&mesh_parent_addr);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:" MACSTR "", mesh_layer, MAC2STR(mesh_parent_addr.addr));
    }
    break;
    case MESH_EVENT_TODS_STATE:
    {
        mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
        // ESP_LOGI(MESH_TAG, "<MESH_EVENT_TODS_REACHABLE>state:%d", *toDs_state);
        if (*toDs_state == MESH_TODS_REACHABLE) // 根节点可达外网
        {
            Network_Flag = 1;
            printf("有网络使用\n");
            start_sntp_sync_task();
        }
        else if (*toDs_state == MESH_TODS_UNREACHABLE) // 根节点不可达外网
        {
            Network_Flag = 0;
            printf("无网络使用\n");
        }
    }
    break;
    case MESH_EVENT_ROOT_FIXED:
    {
        mesh_event_root_fixed_t *root_fixed = (mesh_event_root_fixed_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_FIXED>%s",
                 root_fixed->is_fixed ? "fixed" : "not fixed");
    }
    break;
    case MESH_EVENT_ROOT_ASKED_YIELD:
    {
        mesh_event_root_conflict_t *root_conflict = (mesh_event_root_conflict_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_ASKED_YIELD>" MACSTR ", rssi:%d, capacity:%d",
                 MAC2STR(root_conflict->addr),
                 root_conflict->rssi,
                 root_conflict->capacity);
    }
    break;
    case MESH_EVENT_CHANNEL_SWITCH:
    {
        mesh_event_channel_switch_t *channel_switch = (mesh_event_channel_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d", channel_switch->channel);
    }
    break;
    case MESH_EVENT_SCAN_DONE:
    {
        mesh_event_scan_done_t *scan_done = (mesh_event_scan_done_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_SCAN_DONE>number:%d",
                 scan_done->number);
    }
    break;
    case MESH_EVENT_NETWORK_STATE:
    {
        mesh_event_network_state_t *network_state = (mesh_event_network_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d",
                 network_state->is_rootless);
    }
    break;
    case MESH_EVENT_STOP_RECONNECTION:
    {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOP_RECONNECTION>");
    }
    break;
    case MESH_EVENT_FIND_NETWORK:
    {
        mesh_event_find_network_t *find_network = (mesh_event_find_network_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:" MACSTR "",
                 find_network->channel, MAC2STR(find_network->router_bssid));
    }
    break;
    case MESH_EVENT_ROUTER_SWITCH:
    {
        mesh_event_router_switch_t *router_switch = (mesh_event_router_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, " MACSTR "",router_switch->ssid, router_switch->channel, MAC2STR(router_switch->bssid));
    }
    break;
    default:
        ESP_LOGI(MESH_TAG, "unknown id:%" PRId32 "", event_id);
        break;
    }
}

// 修改IP事件处理
static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    if (event_id == IP_EVENT_STA_GOT_IP) // STA
    {
        ESP_LOGI(MESH_TAG, "STA Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_current_ip.addr = event->ip_info.ip.addr;

        // ===== 关键：WiFi连接成功 → 恢复自组网 =====
        if (R_NET == 1)
        {
            esp_err_t err = esp_mesh_set_self_organized(true, false);
            if (err == ESP_OK)
            {
                ESP_LOGI(MESH_TAG, "Self-organized restored, children can rejoin");
            }
            else
            {
                ESP_LOGE(MESH_TAG, "Failed to restore self-organized: %s", esp_err_to_name(err));
            }
            R_NET = 0;
        }

        if (esp_mesh_is_root())
        {
            refresh_root_ap_gateway(event->ip_info.gw.addr); // 更新网关
        }
    }
    else if (event_id == IP_EVENT_ETH_GOT_IP) // ETH
    {
        ESP_LOGI("NET", "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_current_ip.addr = event->ip_info.ip.addr;

        if (R_NET == 1)
        {
            esp_err_t err = esp_mesh_set_self_organized(true, false);
            if (err == ESP_OK)
            {
                ESP_LOGI(MESH_TAG, "Self-organized restored, children can rejoin");
            }
            else
            {
                ESP_LOGE(MESH_TAG, "Failed to restore self-organized: %s", esp_err_to_name(err));
            }
            R_NET = 0;
        }

        if (esp_mesh_is_root())
        {
            refresh_root_ap_gateway(event->ip_info.gw.addr); // 更新网关
        }
    }
}

static void show_free_heap(const char *tag)
{
    size_t free_internal_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(tag, "Internal SRAM free: %u bytes (%.2f KB)\n", free_internal_heap, free_internal_heap / 1024.0);

    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(tag, "PSRAM free: %u bytes (%.2f KB)\n", free_spiram, free_spiram / 1024.0);

    ESP_LOGI(tag, "Free heap: %" PRId32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(tag, "Minimum free heap: %" PRId32 " bytes", esp_get_minimum_free_heap_size());
}

void mesh_init_Custom(void)
{
    // 启动时设置日志级别

    // 获取flash配置中的wifi信息

    // show_free_heap("INIT"); // 显示初始化时的内存状态

    list_router_get();

    // 初始化网络接口
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(mesh_netifs_init(recv_cb)); //

    // 提前初始化路由表锁，避免子节点不启动 MQTT 任务时收不到路由表
    if (s_route_table_lock == NULL)
    {
        s_route_table_lock = xSemaphoreCreateMutex();
        assert(s_route_table_lock != NULL);
    }

    // 初始化WiFi
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL)); // 注册IP事件处理函数
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 初始化Mesh
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));

    if (IsRoot)
    {
        ESP_ERROR_CHECK(esp_mesh_fix_root(true));
        ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));
        ESP_LOGW(MESH_TAG, "This node is fixed as MESH_ROOT");
    }
    else
    {
        ESP_ERROR_CHECK(esp_mesh_fix_root(true));
        ESP_LOGW(MESH_TAG, "This node is normal MESH_NODE");
    }

    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10)); // 设置AP关联超时时间
    /* set blocking time of esp_mesh_send() to 30s, to prevent the esp_mesh_send() from permanently for some reason */
    ESP_ERROR_CHECK(esp_mesh_send_block_time(30000)); // 设置mesh_send()阻塞时间
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
#if !MESH_IE_ENCRYPTED
    cfg.crypto_funcs = NULL;
#endif
    /* mesh ID */
    // printf("MESH_ID: %02x:%02x:%02x:%02x:%02x:%02x\n", MESH_ID[0], MESH_ID[1], MESH_ID[2], MESH_ID[3], MESH_ID[4], MESH_ID[5]);
    memcpy((uint8_t *)&cfg.mesh_id, MESH_ID, 6); // 6字节的Mesh ID，所有节点必须相同才能加入同一个网络

    /* router */
    // cfg.channel = Chanel;

    cfg.channel = 0;                    // 0表示自动选择频道
    cfg.router.ssid_len = strlen(SSID); // 路由器的SSID长度

    // Ctrl+Shift+P，点击配置编辑器修改CONFIG_MESH_ROUTER_SSID，CONFIG_MESH_ROUTER_PASSWD，CONFIG_MESH_AP_PASSWD
    memcpy((uint8_t *)&cfg.router.ssid, SSID, cfg.router.ssid_len);
    memcpy((uint8_t *)&cfg.router.password, WIFI_PS, strlen(WIFI_PS));
    /* mesh softAP */
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE));
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    cfg.mesh_ap.nonmesh_max_connection = CONFIG_MESH_NON_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *)&cfg.mesh_ap.password, Mesh_PS, strlen(Mesh_PS));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg)); // 会判断是否是根节点，只能有一个

    /* mesh start */
    ESP_ERROR_CHECK(esp_mesh_start()); // 启动mesh

    ESP_LOGI(MESH_TAG, "mesh starts successfully, heap:%" PRId32 ", %s", esp_get_free_heap_size(), esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed");

    ESP_LOGI(MESH_TAG, "System initialization complete");
    // printf("\n========================================\n");
    // printf("Mesh Gateway Ready\n");
    // printf("Type 'help' for available commands\n");
    // printf("========================================\n");
    // show_free_heap("INIT"); // 显示初始化时的内存状态

    if (IsRoot)//根节点
    {
        ESP_LOGI(MESH_TAG, "根节点初始化双网卡");
        esp_err_t err = dual_net_init();
        if (err != ESP_OK)
        {
            //ESP_LOGE(MESH_TAG, "dual_net_init() failed: %s", esp_err_to_name(err));
            storage_write_record_cyclic("", "dual_net_init failed");
        }
    }
}
