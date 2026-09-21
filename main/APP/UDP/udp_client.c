/*
 * UDP客户端模块
 *
 * 基于ESP-IDF的BSD Socket API实现
 */

#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "udp_client.h"
#include "ConfigData.h"
#include "Json_data.h"

#include <time.h>
#include "SqLite.h"
#include "log.h"

static const char *TAG = "udp_client";
static uint16_t udp_client_local_port = 0;
static bool localport_is_saved = false;

static udp_client_handle_t client = NULL; // 全局UDP客户端句柄
uint8_t udp_start_state = 0;              // UDP任务状态
// UDP接收数据队列句柄
static QueueHandle_t s_udprdata_queue = NULL;
/**
 * UDP客户端内部结构体
 */
struct udp_client
{
    int sock;                          // socket描述符
    struct sockaddr_storage dest_addr; // 目标地址
    int addr_family;                   // 地址族
    int ip_protocol;                   // IP协议
    uint16_t local_port;               // 本地端口
    char server_ip[64];                // 服务器IP
    uint16_t server_port;              // 服务器端口
    int timeout_sec;                   // 超时时间(秒)
    bool is_connected;                 // 是否已创建socket
};

/**
 * 创建socket并绑定本地端口
 */
static int create_socket(udp_client_handle_t client)
{
    if (!client)
        return -1;

    if (client->sock >= 0)
    {
        close(client->sock);
        client->sock = -1;
    }

    client->sock = socket(client->addr_family, SOCK_DGRAM, client->ip_protocol);
    if (client->sock < 0)
    {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return -1;
    }

    if (client->local_port > 0)
    {
        if (client->addr_family == AF_INET)
        {
            struct sockaddr_in local_addr = {0};
            local_addr.sin_family = AF_INET;
            local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            local_addr.sin_port = htons(client->local_port);

            int ret = bind(client->sock, (struct sockaddr *)&local_addr, sizeof(local_addr));
            if (ret < 0)
            {
                ESP_LOGE(TAG, "Bind to port %d failed: errno %d", client->local_port, errno);
                close(client->sock);
                client->sock = -1;
                return -1;
            }
            ESP_LOGI(TAG, "Bound to local port %d", client->local_port);
        }
    }

    struct timeval timeout = {
        .tv_sec = client->timeout_sec,
        .tv_usec = 0};
    setsockopt(client->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    client->is_connected = true;
    return 0;
}

udp_client_handle_t udp_client_init(const udp_client_config_t *config)
{
    if (!config || !config->server_ip[0] || config->server_port == 0)
    {
        ESP_LOGE(TAG, "Invalid configuration");
        return NULL;
    }

    // 分配内存
    udp_client_handle_t client = calloc(1, sizeof(struct udp_client));
    if (!client)
    {
        ESP_LOGE(TAG, "Memory allocation failed");
        return NULL;
    }

    // 保存配置
    strncpy(client->server_ip, config->server_ip, sizeof(client->server_ip) - 1);
    client->server_port = config->server_port;
    client->local_port = config->local_port;
    client->timeout_sec = config->timeout_sec;
    client->sock = -1;

    // 配置地址族
    if (config->use_ipv6)
    {
        client->addr_family = AF_INET6;
        client->ip_protocol = IPPROTO_IPV6;

        // 设置IPv6目标地址
        struct sockaddr_in6 *dest_addr = (struct sockaddr_in6 *)&client->dest_addr;
        inet6_aton(config->server_ip, &dest_addr->sin6_addr);
        dest_addr->sin6_family = AF_INET6;
        dest_addr->sin6_port = htons(config->server_port);
    }
    else
    {
        client->addr_family = AF_INET;
        client->ip_protocol = IPPROTO_UDP; //

        // 设置IPv4目标地址
        struct sockaddr_in *dest_addr = (struct sockaddr_in *)&client->dest_addr;
        dest_addr->sin_addr.s_addr = inet_addr(config->server_ip);
        dest_addr->sin_family = AF_INET;
        dest_addr->sin_port = htons(config->server_port);
    }
    // 保存本地端口
    if (localport_is_saved == false)
    {
        udp_client_local_port = UDP_Port;
        client->local_port = udp_client_local_port;
        localport_is_saved = true;
    }
    else
    {
        client->local_port = udp_client_local_port;
    }

    // 创建socket
    if (create_socket(client) < 0)
    {
        free(client);
        return NULL;
    }

    return client;
}

int udp_client_send(const uint8_t *data, size_t len)
{
    if (!client || !client->is_connected || !data)
    {
        return -1;
    }

    int err = sendto(client->sock, data, len, 0,
                     (struct sockaddr *)&client->dest_addr, sizeof(client->dest_addr));
    if (err < 0)
    {
        ESP_LOGE(TAG, "Send failed: errno %d", errno);
        return -1;
    }

    return 0;
}

int udp_client_receive(uint8_t *buffer, size_t buffer_size, int timeout_ms)
{
    if (!client || !client->is_connected || !buffer)
    {
        return -1;
    }

    // 设置临时超时
    if (timeout_ms >= 0)
    {
        struct timeval timeout;
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(client->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    struct sockaddr_storage source_addr;
    socklen_t socklen = sizeof(source_addr);
    int len = recvfrom(client->sock, buffer, buffer_size - 1, 0,
                       (struct sockaddr *)&source_addr, &socklen);

    // 恢复默认超时
    if (timeout_ms >= 0)
    {
        struct timeval timeout;
        timeout.tv_sec = client->timeout_sec;
        timeout.tv_usec = 0;
        setsockopt(client->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    if (len > 0)
    {
        buffer[len] = 0;
    }

    return len;
}

uint16_t udp_client_get_local_port(void)
{
    return client ? client->local_port : 0;
}

bool udp_client_get_server_ip(char *buffer, size_t buffer_size)
{
    if (!client || !buffer || buffer_size == 0)
    {
        return false;
    }

    strncpy(buffer, client->server_ip, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    return true;
}

int udp_client_reconnect(void)
{
    if (!client)
        return -1;
    return create_socket(client);
}

void udp_client_deinit(void)
{
    if (!client)
        return;

    if (client->sock >= 0)
    {
        shutdown(client->sock, 0);
        close(client->sock);
    }

    free(client);
    ESP_LOGI(TAG, "UDP client deinitialized");
}

int get_total_records(void)
{
    FILE *f = fopen("/log/records.dat", "rb");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }

    // 获取文件大小
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // 计算实际记录数
    int total_records = file_size / RECORD_SIZE;
    if (total_records > MAX_RECORDS)
    {
        total_records = MAX_RECORDS;
    }

    fclose(f);

    printf("\n========== All Records (%d/%d) ==========\n", total_records, MAX_RECORDS);
    return total_records;
}

esp_err_t storage_UDP_Send_record(int index)
{
    FILE *f = fopen("/log/records.dat", "rb");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }
    char buffer[RECORD_SIZE];

    fseek(f, log_get_offset(index), SEEK_SET);
    size_t read = fread(buffer, 1, RECORD_SIZE, f);

    if (read == RECORD_SIZE)
    {
        // 检查记录是否为空（全0或首字节为0）
        if (buffer[0] != '\0')
        {
            printf("[%d] %s\n", index, buffer);
            // udp发送
            udp_client_send((uint8_t *)buffer, strlen(buffer));
        }
        else
        {
            printf("[%d] <empty>\n", index);
        }
    }
    fclose(f);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// 辅助函数声明与实现
// -----------------------------------------------------------------------------

/**
 * 构建 DataReport 的 JSON 字符串
 * @param idnum         ID编号
 * @param timestamp     时间戳
 * @param voltage       电压值
 * @param current       电流值
 * @return              需要调用 cJSON_free 释放的字符串，失败返回 NULL
 */
char *build_data_report_json(int idnum, int timestamp, float voltage, float current)
{
    cJSON *pRoot = cJSON_CreateObject();
    if (!pRoot)
        return NULL;

    cJSON *pValue = cJSON_CreateObject();
    if (!pValue)
    {
        cJSON_Delete(pRoot);
        return NULL;
    }

    cJSON_AddStringToObject(pRoot, "Cmd", "DataReport");
    cJSON_AddItemToObject(pRoot, "Value", pValue);

    cJSON_AddStringToObject(pValue, "SN", sn_buffer);
    cJSON_AddNumberToObject(pValue, "IDNUM", idnum);
    cJSON_AddNumberToObject(pValue, "TIMESTAMP", timestamp);

    char voltage_str[32], current_str[32];
    snprintf(voltage_str, sizeof(voltage_str), "%.2f", voltage);
    snprintf(current_str, sizeof(current_str), "%.2f", current);

    cJSON_AddRawToObject(pValue, "Equipmentvoltage", voltage_str);
    cJSON_AddRawToObject(pValue, "EquipmentCurrent", current_str);
    cJSON_AddRawToObject(pValue, "Testvoltage", voltage_str);
    cJSON_AddRawToObject(pValue, "TestCurrent", current_str);

    char *json_str = cJSON_PrintUnformatted(pRoot);
    cJSON_Delete(pRoot);// 释放整个JSON对象树
    return json_str;
}

/**
 * 发送一条数据上报，失败时可选择存储到本地
 * @param client        UDP客户端句柄
 * @param idnum         ID编号
 * @param timestamp     时间戳
 * @param voltage       电压
 * @param current       电流
 * @param store_on_fail 发送失败时是否存储到本地数据库
 * @return              成功发送返回 0，失败返回 -1（若 store_on_fail=1 且存储成功也返回 -1）
 */
int send_data_report(int idnum, int timestamp, float voltage, float current, bool store_on_fail)
{
    char *json_str = build_data_report_json(idnum, timestamp, voltage, current);
    if (!json_str)
    {
        ESP_LOGE(TAG, "build_data_report_json failed");
        return -1;
    }

    int ret = udp_client_send((uint8_t *)json_str, strlen(json_str));
    if (ret < 0)
    {
        ESP_LOGE(TAG, "发送失败，错误码: %d, errno: %d", ret, errno);
        if (store_on_fail)
        {
            //InsertSingleRecord(sn_buffer, idnum, timestamp, voltage, current, voltage, current);
        }
        cJSON_free(json_str);
        return -1;
    }

    // 发送成功时也存储（按照原逻辑：无论成功失败都存一次）
    InsertSingleRecord(sn_buffer, idnum, timestamp, voltage, current, voltage, current);
    ESP_LOGI(TAG, "发送成功 (ID=%d)", idnum);
    cJSON_free(json_str);
    return 0;
}

/**
 * 处理服务器补发请求（解析 "ID:数字" 并重发对应缓存数据）
 * @param client    UDP客户端句柄
 * @param recv_buf  接收到的缓冲区（包含 "ID:xxx" 格式）
 */
void handle_retransmission(const char *recv_buf)
{
    int num;
    if (sscanf(recv_buf, "%*[^0-9]%d", &num) == 1)
    {
        printf("提取的补发ID: %d\n", num);
        query_db1_to_global(num); // 将查询结果存入 g_db1_result

        char *json_str = build_data_report_json(g_db1_result.IDNUM, g_db1_result.timestamp,g_db1_result.Equipmentvoltage,g_db1_result.EquipmentCurrent);

        if (json_str)
        {
            int ret = udp_client_send((uint8_t *)json_str, strlen(json_str));
            if (ret < 0)
            {
                ESP_LOGE(TAG, "补发数据发送失败，错误码: %d, errno: %d", ret, errno);
            }
            cJSON_free(json_str);
        }
    }
    else
    {
        printf("补发解析失败\n");
    }
}

/**
 * 发送简单的控制字符串
 */
int send_control_string(const char *str)
{
    int ret = udp_client_send((uint8_t *)str, strlen(str));
    if (ret < 0)
    {
        ESP_LOGE(TAG, "发送 '%s' 失败: errno %d", str, errno);
    }
    return ret;
}

// -----------------------------------------------------------------------------
// 重构后的 udp_test_task
// -----------------------------------------------------------------------------

void udp_test_task(void *pvParameters)
{
    udp_client_config_t config = UDP_CLIENT_CONFIG_DEFAULT();
    strcpy(config.server_ip, SERVER_IP);
    config.server_port = SERVER_UDP_Port;
    config.local_port = UDP_Port;
    config.timeout_sec = 5;
    config.use_ipv6 = false;

    udp_client_handle_t client = udp_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize UDP client");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP client initialized, local port: %d", udp_client_get_local_port());

    char recv_buf[400];
    int state = 0; // 任务状态机
    int IDNUM = 1;
    int reconnectflag = 0;
    int Bufa = 0; // 补发标志
    int total_records = 0;
    int currentlog_indext = 0;
    bool is_log_upload = true;

    storage_write_record_cyclic(sn_buffer, "UDP_Client Start");
    if (!esp_mesh_is_root())
    {
        storage_write_record_cyclic(sn_buffer, "Mesh is not root");
    }
    else
    {
        storage_write_record_cyclic(sn_buffer, "Mesh is root");
    }

    vTaskDelay(1000); // 等待时间同步

    while (1)
    {
        if (!esp_mesh_is_root() && Network_Flag == 1)
        {
            // ----- 网络正常，非根节点 -----
            if (reconnectflag == 1)
            {
                udp_client_reconnect();
                reconnectflag = 0;
                storage_write_record_cyclic(sn_buffer, "Reconnect UDP");
            }
            // 状态机处理
            switch (state)
            {
            case 0: // 握手
            {
                if (send_control_string(DEVICE_ID) != 0)
                {
                    storage_write_record_cyclic(sn_buffer, "UDP Task Send ID failed");
                }
                break;
            }
            case 1: // 握手成功，等待命令
            {
                send_control_string("Waiting for command");
                break;
            }
            case 2: // 正常发送数据
            {
                time_t now = time(NULL);

                float voltage = 3.30f;
                float current = 0.15f;

                int ret = send_data_report(IDNUM, (int)now, voltage, current, true);
                if (ret < 0)
                {
                    reconnectflag = 1;
                }

                if (IDNUM > 30) // 后期换为soc值来判定
                {
                    printf("数据发送完毕\n");
                    state = 3;
                }
                IDNUM++;
                vTaskDelay(10);

                // 处理补发请求（如果之前收到了补发命令）
                if (Bufa == 1)
                {
                    handle_retransmission(recv_buf);
                    Bufa = 0;
                    memset(recv_buf, 0, sizeof(recv_buf));
                }
                break;
            }
            case 3: // 发送数据结束标志
            {
                send_control_string("DataReportEnd");
                IDNUM = 1;
                break;
            }
            case 4: // 补发模式
            {
                if (Bufa == 1)
                {
                    handle_retransmission(recv_buf);
                    Bufa = 0;
                    memset(recv_buf, 0, sizeof(recv_buf));
                }
                break;
            }
            case 5: // 测试结束
            {
                send_control_string("TaskEnd");
                state = 8;
                break;
            }
            case 6: // 进入日志上报模式
            {
                total_records = get_total_records();
                printf("日志上报模式，总记录数：%d\n", total_records);
                state = 7;
                break;
            }
            case 7: // 上报日志
            {
                if (is_log_upload)
                {
                    if (currentlog_indext < total_records)
                    {
                        storage_UDP_Send_record(currentlog_indext);
                        is_log_upload = false; // 等待服务器确认后递增索引
                    }
                    else
                    {
                        printf("日志上报完毕\n");
                        currentlog_indext = 0;
                        state = 8;
                    }
                }
                break;
            }
            case 8: // 发送任务完成，等待下一个命令
            {
                send_control_string("TaskEnd,Waiting for next command");
                break;
            }
            default:
            {
                break;
            }
            }

            // 接收服务器响应（超时5秒）
            int len = udp_client_receive((uint8_t *)recv_buf, sizeof(recv_buf), 5000);
            if (len > 0)
            {
                ESP_LOGI(TAG, "Received %d bytes: %s", len, recv_buf);
                if (strncmp(recv_buf, "Connected", 9) == 0)
                {
                    state = 1;
                    storage_write_record_cyclic(sn_buffer, "The handshake with the server was successful");
                    memset(recv_buf, 0, sizeof(recv_buf));
                }
                else if (strncmp(recv_buf, "StartTest", 9) == 0)
                {
                    state = 2;
                    memset(recv_buf, 0, sizeof(recv_buf));
                }
                else if (len >= 12 && recv_buf[8] == 'S' && recv_buf[9] == 'c' && recv_buf[10] == 'r' && recv_buf[11] == 'i')
                {
                    if (is_complete_packet(recv_buf, len))
                    {
                        // parse_jsonCommand(recv_buf, len);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Incomplete packet, discarding");
                    }
                    memset(recv_buf, 0, sizeof(recv_buf));
                }
                else if (strncmp(recv_buf, "DataRetransmission", 18) == 0)
                {
                    state = 4;
                    memset(recv_buf, 0, sizeof(recv_buf));
                }
                else if (recv_buf[0] == 'I' && recv_buf[1] == 'D' && recv_buf[2] == ':')
                {
                    Bufa = 1;
                }
                else if (strncmp(recv_buf, "TestEnd", 7) == 0)
                {
                    state = 5;
                    memset(recv_buf, 0, sizeof(recv_buf));
                }
                else if (strncmp(recv_buf, "LogReport", 9) == 0)
                {
                    state = 6;
                    memset(recv_buf, 0, sizeof(recv_buf));
                }
                else if (strncmp(recv_buf, "Received", 8) == 0)
                {
                    currentlog_indext++;
                    is_log_upload = true;
                    memset(recv_buf, 0, sizeof(recv_buf));
                }
            }
        }
        else if (!esp_mesh_is_root() && Network_Flag == 0)
        {
            // ----- 网络断开，数据暂存本地 -----
            time_t now = time(NULL);
            float voltage = 3.30f;
            float current = 0.15f;

            int ret = InsertSingleRecord(sn_buffer, IDNUM, (int)now, voltage, current, voltage, current);
            if (ret == 0)
            {
                ESP_LOGI(TAG, "本地数据存入成功");
                IDNUM++;
            } // 写入本地缓存
            printf("第%d条数据已存本地\n", IDNUM);
            reconnectflag = 1;
            state = 0;
            vTaskDelay(5000); // 延时5秒
        }
        else if (esp_mesh_is_root())
        {
            // 根节点：仅打印状态，不做发送
            if (Network_Flag == 1)
            {
                printf("我是根节点，我有网\n");
            }
            else
            {
                printf("我是根节点，我没网\n");
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    udp_client_deinit();
    vTaskDelete(NULL);
}

static void udp_rec_data(void *pvParameters)
{
    storage_write_record_cyclic(sn_buffer, "UDP_Client Start");
    if (!esp_mesh_is_root())
    {
        storage_write_record_cyclic(sn_buffer, "Mesh is not root");
    }
    else
    {
        storage_write_record_cyclic(sn_buffer, "Mesh is root");
    }
    vTaskDelay(1000); // 等待时间同步
    while(1)
    {
        char recv_buf[400];
        int len = udp_client_receive((uint8_t *)recv_buf, sizeof(recv_buf), 5000);
        if (len > 0)
        {
            //ESP_LOGI(TAG, "Received %d bytes: %s", len, recv_buf);
            if(s_udprdata_queue!=NULL)
            {
                if (xQueueSend(s_udprdata_queue, recv_buf, pdMS_TO_TICKS(100)) != pdTRUE)
                {
                    ESP_LOGW(TAG, "UDP data queue full, failed to enqueue received data");
                }
            }
        }
    }
}

int app_read_UDP_data(char *buffer, size_t buffer_size, int timeout_ms)
{
    if (s_udprdata_queue == NULL || buffer == NULL || buffer_size == 0)
    {
        return -1;
    }

    if (xQueueReceive(s_udprdata_queue, buffer, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        return strlen(buffer);
    }
    else
    {
        return -1; // 超时或队列错误
    }
}

int UDP_Init(void)
{
    udp_client_config_t config = UDP_CLIENT_CONFIG_DEFAULT();
    strcpy(config.server_ip, SERVER_IP);
    config.server_port = SERVER_UDP_Port;
    config.local_port = UDP_Port;
    config.timeout_sec = 5;
    config.use_ipv6 = false;

    client = udp_client_init(&config);

    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize UDP client");
        return -1;
    }

    ESP_LOGI(TAG, "UDP client initialized, local port: %d", udp_client_get_local_port());

    s_udprdata_queue = xQueueCreate(10, 1024); // 创建UDP数据队列，长度10，每项1024字节
    if (s_udprdata_queue == NULL)
    {
        ESP_LOGE(TAG, "data queue create failed");
        return -1;
    }
    
    xTaskCreate(udp_rec_data, "udp_rec_data", 4096, NULL, 5, NULL);

    vTaskDelay(pdMS_TO_TICKS(10)); // 等待UDP初始化完成

    return 0;
}
