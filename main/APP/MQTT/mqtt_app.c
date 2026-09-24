#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "mqtt_client.h"
#include "mqtt_app.h"
#include "ConfigData.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "appTask.h"

static const char *TAG = "mesh_mqtt";

static esp_mqtt_client_handle_t s_client = NULL;

static QueueHandle_t Topic_queue = NULL;
static QueueHandle_t Data_queue = NULL;

#define MQTT_TOPIC_QUEUE_LEN 10
#define MQTT_DATA_QUEUE_LEN 10
#define MQTT_TOPIC_ITEM_CAPACITY 64
#define MQTT_DATA_ITEM_CAPACITY 1024

typedef struct
{
    uint16_t len;
    char data[MQTT_TOPIC_ITEM_CAPACITY];
} mqtt_topic_msg_t;

typedef struct
{
    uint16_t len;
    char data[MQTT_DATA_ITEM_CAPACITY];
} mqtt_data_msg_t;

static bool s_mqtt_connected = false;

/**
 * @brief 生成包含 Seq 和 DevID 的 JSON 字符串
 * @param seq       Seq 字段的整数值
 * @param dev_id    DevID 字符串
 * @return char*    成功返回 JSON 字符串（需调用 free 释放），失败返回 NULL
 */
static char *create_up_line_json(int seq, const char *dev_id)
{
    if (dev_id == NULL)
    {
        return NULL;
    }

    const char *app_version = esp_app_get_description()->version;
    // 1. 创建根对象
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return NULL;
    }

    // 2. 添加 Seq
    cJSON_AddNumberToObject(root, "Seq", seq);

    // 3. 创建 Data 子对象
    cJSON *data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "Data", data);

    // 添加设备类型
    if (IsRoot == 1)
    {
        cJSON_AddStringToObject(data, "type", "2"); // 2表示总控,也就是根节点
    }
    else
    {
        cJSON_AddStringToObject(data, "type", "1"); // 1表示点位
    }

    // 4. 添加 DevID
    cJSON_AddStringToObject(data, "id", dev_id);

    // 5. 添加 Version,固件版本号
    cJSON_AddStringToObject(data, "Version", app_version);
    // 添加老化架号
    cJSON_AddStringToObject(data, "AgingNumber", AgingNumber);

    // 6. 生成 JSON 字符串（紧凑格式，无缩进）
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root); // 释放 cJSON 对象

    return json_str; // 调用者需 free()
}

// MQTT事件处理函数
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
    {
        s_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

        char *json = create_up_line_json(time(NULL), DEVICE_ID);
        if (json != NULL)
        {
            app_mqtt_publish("device/public/up_line", json, NULL);
            free(json);
        }

        char topic[64] = {0};

        // 订阅全员OTA升级主题
        memset(topic, 0, sizeof(topic));
        snprintf(topic, sizeof(topic), "server/public/cmd/ota");
        if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
        {
            ESP_LOGE(TAG, "Failed to subscribe to server/public/ota");
        }

        // 订阅定点OTA升级主题
        memset(topic, 0, sizeof(topic));
        snprintf(topic, sizeof(topic), "server/%s/cmd/ota", DEVICE_ID);
        if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
        {
            ESP_LOGE(TAG, "Failed to subscribe to ota");
        }

        // 订阅修改配置主题
        memset(topic, 0, sizeof(topic));
        snprintf(topic, sizeof(topic), "server/%s/cmd/set_config", DEVICE_ID);
        if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
        {
            ESP_LOGE(TAG, "Failed to subscribe to set_config");
        }

        // 订阅读取配置主题
        memset(topic, 0, sizeof(topic));
        snprintf(topic, sizeof(topic), "server/%s/cmd/get_config", DEVICE_ID);
        if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
        {
            ESP_LOGE(TAG, "Failed to subscribe to get_config");
        }

        // 订阅控制电源主题(只有根节点需要订阅)
        if (IsRoot == 1)
        {
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/CPower", DEVICE_ID);
            if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to CPower");
            }
        }
        else if (IsRoot == 0)
        {
            // 订阅老化开始主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/start_aging", DEVICE_ID);
            if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to start_aging");
            }

            // 订阅老化停止主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/stop_aging", DEVICE_ID);
            if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to stop_aging");
            }

            // 订阅设置目标PN主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/set_pn", DEVICE_ID);
            if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to set_pn");
            }

            // 订阅获取设备状态主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/get_state", DEVICE_ID);
            if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to get_state");
            }

            // 订阅获取缺失数据主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/get_lost_aging", DEVICE_ID);
            if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to get_lost_aging");
            }

            // 订阅远程日志上报主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/remote_log", DEVICE_ID);
            if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to remote_log");
            }

            // 订阅本地日志上报主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/local_log", DEVICE_ID);
            if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to local_log");
            }

            // 订阅服务器下发的下一步操作主题，群控模式下才会有这个主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/rack/%s/CanNextstep",AgingNumber);
            if (esp_mqtt_client_subscribe(s_client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to Cannextstep");
            }
        }

        Config_Report(100);

        /*
         * 断网期间可能有"已完成任务"(TaskCP)响应没发出去，而上位机正是据此判定步骤
         * 状态、决定是否下发下一步放行指令。连接建立后补发一次，保证设备与上位机
         * 状态同步、放行结果可追溯（内部幂等，无待补发内容时直接返回）。
         */
        aging_resend_pending_replies();
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
    {
        s_mqtt_connected = false;
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");

        break;
    }

    case MQTT_EVENT_SUBSCRIBED:
    {
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    }

    case MQTT_EVENT_UNSUBSCRIBED:
    {
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    }

    case MQTT_EVENT_PUBLISHED:
    {
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    }

    case MQTT_EVENT_DATA:
    {
        ESP_LOGI(TAG,
                 "MQTT_EVENT_DATA, topic_len=%d, data_len=%d, offset=%d, total=%d",
                 event->topic_len,
                 event->data_len,
                 event->current_data_offset,
                 event->total_data_len);

        if (Topic_queue == NULL || Data_queue == NULL)
        {
            ESP_LOGE(TAG, "MQTT receive queue is not initialized");
            break;
        }

        /*
         * MQTT payload does not need whitespace removal. cJSON can parse formatted
         * JSON directly, and removing spaces here would also alter spaces inside
         * JSON string values.
         *
         * ESP-MQTT may deliver one publish in multiple MQTT_EVENT_DATA callbacks.
         * Only enqueue the topic for the first fragment; data fragments are
         * accumulated by app_MQTT_Rdata_handle().
         */
        if (event->current_data_offset == 0 && event->topic != NULL && event->topic_len > 0)
        {
            mqtt_topic_msg_t topic_msg = {0};
            size_t topic_len = (size_t)event->topic_len;

            if (topic_len >= MQTT_TOPIC_ITEM_CAPACITY)
            {
                ESP_LOGE(TAG,
                         "MQTT topic is too long: %u, capacity=%u",
                         (unsigned)topic_len,
                         (unsigned)(MQTT_TOPIC_ITEM_CAPACITY - 1));
                break;
            }

            memcpy(topic_msg.data, event->topic, topic_len);
            topic_msg.data[topic_len] = '\0';
            topic_msg.len = (uint16_t)topic_len;

            if (xQueueSend(Topic_queue, &topic_msg, pdMS_TO_TICKS(100)) != pdTRUE)
            {
                ESP_LOGE(TAG, "Failed to enqueue MQTT topic");
                break;
            }
        }

        int remaining = event->data_len;
        int offset = 0;
        int send_count = 0;

        while (remaining > 0)
        {
            mqtt_data_msg_t data_msg = {0};
            int chunk_size = remaining;
            if (chunk_size > MQTT_DATA_ITEM_CAPACITY)
            {
                chunk_size = MQTT_DATA_ITEM_CAPACITY;
            }

            memcpy(data_msg.data, event->data + offset, (size_t)chunk_size);
            data_msg.len = (uint16_t)chunk_size;

            if (xQueueSend(Data_queue, &data_msg, pdMS_TO_TICKS(100)) != pdTRUE)
            {
                ESP_LOGE(TAG,
                         "Failed to enqueue MQTT data fragment %d, len=%d",
                         send_count + 1,
                         chunk_size);
                break;
            }

            remaining -= chunk_size;
            offset += chunk_size;
            send_count++;
        }

        ESP_LOGI(TAG,
                 "Queued %d MQTT data fragment(s), event_data_len=%d",
                 send_count,
                 event->data_len);
        break;
    }

    case MQTT_EVENT_ERROR:
    {
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGE(TAG, "TCP transport error");
        }
        break;
    }

    default:
    {
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
    }
    return ESP_OK;
}

// MQTT事件分发函数
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRId32 "", base, event_id);
    mqtt_event_handler_cb(event_data);
}

esp_err_t mqtt_app_start(void)
{
    if (s_client != NULL)
    {
        ESP_LOGI(TAG, "MQTT client already started");
        return ESP_OK;
    }

    char *json = create_up_line_json(0, DEVICE_ID);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .hostname = SERVER_IP,
                .port = SERVER_UDP_Port,
                .transport = MQTT_TRANSPORT_OVER_TCP,
            }},
        .credentials = {.username = "admin", .authentication = {
                                                 .password = "123456",
                                             }},
        .session = {.keepalive = 60, // 心跳间隔，单位为秒
                    .disable_clean_session = false,
                    .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
                    .last_will = {
                        .topic = "device/public/down_line",
                        .msg = json,
                        .qos = 1,
                        .retain = 0,

                    }},
        .network = {
            .timeout_ms = 10000, // 网络连接超时时间，单位为毫秒
            .disable_auto_reconnect = false,

        },
        .buffer = {
            .size = 1024,
            .out_size = 8192,
        }

    };

    ESP_LOGI(TAG, "Starting MQTT client...");
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_err_t ret = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, s_client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_mqtt_client_start(s_client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

void mqtt_app_stop(void)
{
    if (s_client)
    {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
}

void mqtt_app_restart(void)
{
    mqtt_app_stop();
    vTaskDelay(200 / portTICK_PERIOD_MS);
    mqtt_app_start();
}

static int mqtt_app_publish(char *topic, char *publish_string)
{
    int msg_id = -1;
    if (s_client)
    {
        msg_id = esp_mqtt_client_publish(s_client, topic, publish_string, 0, 1, 0);
        ESP_LOGI(TAG, "sent publish returned msg_id=%d", msg_id);
    }
    return msg_id;
}

int app_mqtt_publish(char *topic_name, char *publish_string, char *DEVICE_ID)
{
    if (s_mqtt_connected == false)
    {
        ESP_LOGW(TAG, "MQTT is not connected. Cannot publish message.");
        return -1;
    }
    char topic[64]; // 增大一点，确保空间足够

    // DEVICE_ID 是整数，要用 %d 格式符
    if (DEVICE_ID != NULL)
    {
        snprintf(topic, sizeof(topic), topic_name, DEVICE_ID);
    }
    else
    {
        snprintf(topic, sizeof(topic), topic_name);
    }

    return mqtt_app_publish(topic, publish_string);
}

int app_read_MQTT_topic(char *topic_buffer, int buffer_len, uint32_t timeout)
{
    if (topic_buffer == NULL || buffer_len <= 0)
    {
        ESP_LOGE(TAG, "invalid topic buffer");
        return -1;
    }

    if (Topic_queue == NULL)
    {
        ESP_LOGE(TAG, "Topic queue not initialized");
        return -1;
    }

    mqtt_topic_msg_t msg = {0};
    if (xQueueReceive(Topic_queue, &msg, timeout) != pdTRUE)
    {
        return -1;
    }

    size_t copy_len = msg.len;
    if (copy_len > (size_t)(buffer_len - 1))
    {
        copy_len = (size_t)(buffer_len - 1);
        ESP_LOGW(TAG, "MQTT topic truncated from %u to %u bytes",
                 (unsigned)msg.len, (unsigned)copy_len);
    }

    memcpy(topic_buffer, msg.data, copy_len);
    topic_buffer[copy_len] = '\0';
    return (int)copy_len;
}

int app_read_MQTT_data(char *data_buffer, int buffer_len, uint32_t timeout)
{
    if (data_buffer == NULL || buffer_len <= 0)
    {
        ESP_LOGE(TAG, "invalid data buffer");
        return -1;
    }

    if (Data_queue == NULL)
    {
        ESP_LOGE(TAG, "Data queue not initialized");
        return -1;
    }

    mqtt_data_msg_t msg = {0};
    if (xQueueReceive(Data_queue, &msg, timeout) != pdTRUE)
    {
        return -1;
    }

    size_t copy_len = msg.len;
    if (copy_len > (size_t)(buffer_len - 1))
    {
        copy_len = (size_t)(buffer_len - 1);
        ESP_LOGW(TAG, "MQTT data truncated from %u to %u bytes",
                 (unsigned)msg.len, (unsigned)copy_len);
    }

    memcpy(data_buffer, msg.data, copy_len);
    data_buffer[copy_len] = '\0';
    return (int)copy_len;
}

esp_err_t mqtt_init(void)
{
    /* Allow repeated initialization calls without leaking or replacing queues. */
    if (Topic_queue == NULL)
    {
        Topic_queue = xQueueCreate(MQTT_TOPIC_QUEUE_LEN, sizeof(mqtt_topic_msg_t));
        if (Topic_queue == NULL)
        {
            ESP_LOGE(TAG, "Failed to create Topic_queue");
            return ESP_ERR_NO_MEM;
        }
    }

    if (Data_queue == NULL)
    {
        Data_queue = xQueueCreate(MQTT_DATA_QUEUE_LEN, sizeof(mqtt_data_msg_t));
        if (Data_queue == NULL)
        {
            ESP_LOGE(TAG, "Failed to create Data_queue");
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}
