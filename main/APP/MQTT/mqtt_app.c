#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
#include "dual_net.h"
#include "mesh.h"

static const char *TAG = "mesh_mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
/* 启动计数由 net_init/配置任务更新；连接计数仅由 SDK MQTT 任务更新。 */
static unsigned s_client_start_count;
static unsigned s_mqtt_connect_count;

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

static atomic_bool s_mqtt_connected;

/*
 * MQTT keepalive（2026-10-16 加长）：
 * 子节点报文要穿 1~N 跳 mesh + 每跳 NAPT，拥塞时（尤其 OTA 下载窗口全网
 * 并发、mesh 重扫期）PINGREQ 到达 broker 的延迟可能超过 1.5×keepalive，
 * 被 broker 判死断开引发无谓重连风暴。根节点上行虽只一跳，但 OTA 期间
 * 其单条 STA 是全网漏斗，同样需要余量。
 * 权衡：keepalive 越长，对端真正死亡时 LWT/下线判定越慢（根 90s→180s、
 * 子 22.5s→45s），对以太网/内网 broker 场景可接受。
 */
#define MQTT_ROOT_KEEPALIVE_SECONDS 60
#define MQTT_NODE_KEEPALIVE_SECONDS 60
#define MQTT_NODE_RECONNECT_WAIT_MS 2000
#define MQTT_UPLINK_DISCONNECT_RETRY_MAX 3
#define MQTT_PUBLISH_DRAIN_TIMEOUT_MS 11000 /* 现有网络写超时10秒 + 调度余量。 */

typedef enum
{
    MQTT_APP_STOPPED,
    MQTT_APP_CONNECTING,
    MQTT_APP_CONNECTED,
    MQTT_APP_WAIT_RECONNECT,
    MQTT_APP_STOPPING,
} MqttAppPhase;

typedef struct
{
    MqttAppPhase phase;
    dual_net_uplink_t desired_uplink;
    uint32_t route_generation;
    uint32_t attempt_generation;
    bool switch_pending;
    bool expedite_pending;
    bool disconnect_requested;
    uint8_t disconnect_failures;
    uint32_t publish_refs;
} MqttUplinkControl;

static portMUX_TYPE s_mqtt_state_lock = portMUX_INITIALIZER_UNLOCKED;
static MqttUplinkControl s_mqtt_control;
static StaticSemaphore_t s_mqtt_lifecycle_storage;
static SemaphoreHandle_t s_mqtt_lifecycle_mutex;
static bool s_mqtt_mutex_initializing;
/* 仅生命周期锁持有者访问；回调使用 event->client，不获取此锁。 */
static bool s_client_started;

static SemaphoreHandle_t mqtt_lifecycle_mutex(void)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    SemaphoreHandle_t mutex = s_mqtt_lifecycle_mutex;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    return mutex;
}

static esp_err_t mqtt_lifecycle_init(void)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    if (s_mqtt_lifecycle_mutex != NULL)
    {
        portEXIT_CRITICAL(&s_mqtt_state_lock);
        return ESP_OK;
    }
    if (s_mqtt_mutex_initializing)
    {
        portEXIT_CRITICAL(&s_mqtt_state_lock);
        return ESP_ERR_INVALID_STATE; // 并发首次初始化者由现有启动重试流程重试。
    }
    s_mqtt_mutex_initializing = true;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&s_mqtt_lifecycle_storage);
    portENTER_CRITICAL(&s_mqtt_state_lock);
    s_mqtt_lifecycle_mutex = mutex;
    s_mqtt_mutex_initializing = false;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    return mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void mqtt_lifecycle_unlock(SemaphoreHandle_t mutex)
{
    if (xSemaphoreGive(mutex) != pdTRUE)
        ESP_LOGE(TAG, "MQTT lifecycle unlock failed");
}

/* 调用者已持生命周期锁，或正在该客户端的 SDK 回调中。SDK API 一律在状态锁外。
 * 设置请求标志早于调用 SDK，合并快速插拔/事件回调产生的重复请求。 */
static void mqtt_request_disconnect(esp_mqtt_client_handle_t client)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    bool request = client == s_client && s_mqtt_control.phase == MQTT_APP_CONNECTED &&
                   s_mqtt_control.switch_pending && !s_mqtt_control.disconnect_requested &&
                   s_mqtt_control.desired_uplink != DUAL_NET_UPLINK_NONE &&
                   s_mqtt_control.disconnect_failures < MQTT_UPLINK_DISCONNECT_RETRY_MAX;
    if (request)
    {
        s_mqtt_control.disconnect_requested = true;
        atomic_store(&s_mqtt_connected, false);
    }
    uint32_t generation = s_mqtt_control.route_generation;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    if (!request)
        return;
    esp_err_t err = esp_mqtt_client_disconnect(client);
    if (err != ESP_OK)
    {
        portENTER_CRITICAL(&s_mqtt_state_lock);
        if (client == s_client && s_mqtt_control.phase == MQTT_APP_CONNECTED)
        {
            s_mqtt_control.disconnect_requested = false;
            if (s_mqtt_control.route_generation == generation &&
                ++s_mqtt_control.disconnect_failures >= MQTT_UPLINK_DISCONNECT_RETRY_MAX)
            {
                s_mqtt_control.switch_pending = false;
                s_mqtt_control.expedite_pending = false;
                /* 断开请求从未被SDK受理，旧连接仍在；允许业务继续并由SDK处理后续故障。 */
                atomic_store(&s_mqtt_connected, true);
            }
        }
        portEXIT_CRITICAL(&s_mqtt_state_lock);
        ESP_LOGW(TAG, "上行切换请求 MQTT 断开失败：%s", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "上行切换请求 MQTT 断开：client=%p, generation=%lu",
                 (void *)client, (unsigned long)generation);
    }
}

static void mqtt_request_immediate_reconnect(esp_mqtt_client_handle_t client)
{
    /* 即使路由事件尚未处理，物理链路失效时也不能加速无出口的连接尝试。 */
    if (IsRoot != 1 || dual_net_get_active_uplink() == DUAL_NET_UPLINK_NONE)
        return;
    portENTER_CRITICAL(&s_mqtt_state_lock);
    bool request = client == s_client && s_mqtt_control.phase == MQTT_APP_WAIT_RECONNECT &&
                   s_mqtt_control.expedite_pending &&
                   s_mqtt_control.desired_uplink != DUAL_NET_UPLINK_NONE;
    if (request)
        s_mqtt_control.expedite_pending = false;
    uint32_t generation = s_mqtt_control.route_generation;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    if (!request)
        return;
    /* 每次上行变化只加速一次；失败后沿用 SDK 自动重连退避，避免拒绝连接时忙循环。 */
    esp_err_t err = esp_mqtt_client_reconnect(client);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "立即 MQTT 重连未受理：%s，保留自动重连", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "上行切换立即重连 MQTT：client=%p, generation=%lu",
                 (void *)client, (unsigned long)generation);
    }
}

void mqtt_app_process_uplink_reconnect(void)
{
    if (IsRoot != 1)
        return;
    SemaphoreHandle_t mutex = mqtt_lifecycle_mutex();
    /* 不阻塞 net_monitor；停止/启动持锁时保留请求，下一次监控检查再处理。 */
    if (mutex == NULL || xSemaphoreTake(mutex, 0) != pdTRUE)
        return;
    if (s_client != NULL && s_client_started)
    {
        mqtt_request_disconnect(s_client);
        mqtt_request_immediate_reconnect(s_client);
    }
    mqtt_lifecycle_unlock(mutex);
}

void mqtt_app_on_uplink_changed(dual_net_uplink_t uplink)
{
    if (IsRoot != 1)
        return;
    portENTER_CRITICAL(&s_mqtt_state_lock);
    if (s_mqtt_control.desired_uplink == uplink)
    {
        portEXIT_CRITICAL(&s_mqtt_state_lock);
        return;
    }
    s_mqtt_control.desired_uplink = uplink;
    s_mqtt_control.route_generation++;
    bool running = s_mqtt_control.phase != MQTT_APP_STOPPED &&
                   s_mqtt_control.phase != MQTT_APP_STOPPING;
    s_mqtt_control.switch_pending = running && uplink != DUAL_NET_UPLINK_NONE;
    s_mqtt_control.expedite_pending = s_mqtt_control.switch_pending;
    s_mqtt_control.disconnect_failures = 0;
    if (uplink == DUAL_NET_UPLINK_NONE)
        atomic_store(&s_mqtt_connected, false);
    uint32_t generation = s_mqtt_control.route_generation;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    ESP_LOGI(TAG, "MQTT 上行变化：target=%d, generation=%lu", uplink, (unsigned long)generation);
    mqtt_app_process_uplink_reconnect();
}

static void mqtt_before_connect(esp_mqtt_client_handle_t client)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    if (client == s_client && s_mqtt_control.phase != MQTT_APP_STOPPING)
    {
        s_mqtt_control.phase = MQTT_APP_CONNECTING;
        s_mqtt_control.attempt_generation = s_mqtt_control.route_generation;
        /* 本轮已使用当前默认出口；失败后不继续零等待重试。 */
        s_mqtt_control.expedite_pending = false;
        s_mqtt_control.disconnect_requested = false;
    }
    portEXIT_CRITICAL(&s_mqtt_state_lock);
}

/* 返回 true 才发布上线/恢复订阅。过期连接只请求断开，不让它被误报为切换成功。 */
static bool mqtt_connected(esp_mqtt_client_handle_t client)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    bool usable = client == s_client && s_mqtt_control.phase != MQTT_APP_STOPPING &&
                  s_mqtt_control.phase != MQTT_APP_STOPPED;
    bool stale = usable && IsRoot == 1 && s_mqtt_control.switch_pending &&
                 s_mqtt_control.attempt_generation != s_mqtt_control.route_generation;
    if (usable)
    {
        s_mqtt_control.phase = MQTT_APP_CONNECTED;
        if (!stale)
        {
            s_mqtt_control.switch_pending = false;
            s_mqtt_control.expedite_pending = false;
            s_mqtt_control.disconnect_requested = false;
            s_mqtt_control.disconnect_failures = 0;
            atomic_store(&s_mqtt_connected, true);
        }
    }
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    if (stale)
    {
        ESP_LOGI(TAG, "连接期间出口已变化，重新建立 MQTT 连接");
        mqtt_request_disconnect(client);
    }
    return usable && !stale;
}

static void mqtt_disconnected(esp_mqtt_client_handle_t client)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    bool running = client == s_client && s_mqtt_control.phase != MQTT_APP_STOPPING &&
                   s_mqtt_control.phase != MQTT_APP_STOPPED;
    if (running)
    {
        atomic_store(&s_mqtt_connected, false);
        s_mqtt_control.phase = MQTT_APP_WAIT_RECONNECT;
        s_mqtt_control.disconnect_requested = false;
    }
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    if (running)
        mqtt_request_immediate_reconnect(client);
}

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
    // 添加 Mesh 层级：根节点=1，子节点≥2（库未就绪时可能为 0）。
    // 服务器据此区分节点层级与 mesh 拓扑形态。
    cJSON_AddNumberToObject(data, "MeshLevel", mesh_get_current_level());

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
    case MQTT_EVENT_BEFORE_CONNECT:
        mqtt_before_connect(event->client);
        ESP_LOGI(TAG, "MQTT_EVENT_BEFORE_CONNECT client=%p, task=%p, uplink=%s",
                 (void *)event->client, (void *)xTaskGetCurrentTaskHandle(),
                 IsRoot == 1 ? dual_net_get_active_interface() : "MESH_STA");
        break;
    case MQTT_EVENT_CONNECTED:
    {
        if (!mqtt_connected(event->client))
            break;
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED client=%p, connection_count=%u, uplink=%s",
                 (void *)event->client, ++s_mqtt_connect_count,
                 IsRoot == 1 ? dual_net_get_active_interface() : "MESH_STA");

        char *json = create_up_line_json(time(NULL), DEVICE_ID);
        if (json != NULL)
        {
            app_mqtt_publish("device/public/up_line", json, NULL);
            free(json);
        }

        char topic[64] = {0};

        // 订阅定点OTA升级主题
        memset(topic, 0, sizeof(topic));
        snprintf(topic, sizeof(topic), "server/%s/cmd/ota", DEVICE_ID);
        if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
        {
            ESP_LOGE(TAG, "Failed to subscribe to ota");
        }

        // 订阅服务，重启主题，多点位ota升级时统一重启
        memset(topic, 0, sizeof(topic));
        snprintf(topic, sizeof(topic), "server/rack/%s/CanRestart", AgingNumber);
        if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
        {
            ESP_LOGE(TAG, "Failed to subscribe to CanRestart");
        }

        // 订阅修改配置主题
        memset(topic, 0, sizeof(topic));
        snprintf(topic, sizeof(topic), "server/%s/cmd/set_config", DEVICE_ID);
        if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
        {
            ESP_LOGE(TAG, "Failed to subscribe to set_config");
        }

        // 订阅读取配置主题
        memset(topic, 0, sizeof(topic));
        snprintf(topic, sizeof(topic), "server/%s/cmd/get_config", DEVICE_ID);
        if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
        {
            ESP_LOGE(TAG, "Failed to subscribe to get_config");
        }

        // 订阅控制电源主题(只有根节点需要订阅)
        if (IsRoot == 1)
        {
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/CPower", DEVICE_ID);
            if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to CPower");
            }
        }
        else if (IsRoot == 0)
        {
            // 订阅老化开始主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/start_aging", DEVICE_ID);
            if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to start_aging");
            }

            // 订阅老化停止主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/stop_aging", DEVICE_ID);
            if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to stop_aging");
            }

            // 订阅设置目标PN主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/set_pn", DEVICE_ID);
            if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to set_pn");
            }

            // 订阅获取设备状态主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/get_state", DEVICE_ID);
            if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to get_state");
            }

            // 订阅获取缺失数据主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/get_lost_aging", DEVICE_ID);
            if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to get_lost_aging");
            }

            // 订阅远程日志上报主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/remote_log", DEVICE_ID);
            if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to remote_log");
            }

            // 订阅本地日志上报主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/%s/cmd/local_log", DEVICE_ID);
            if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to local_log");
            }

            // 订阅服务器下发的下一步操作主题，群控模式下才会有这个主题
            memset(topic, 0, sizeof(topic));
            snprintf(topic, sizeof(topic), "server/rack/%s/CanNextstep", AgingNumber);
            if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
            {
                ESP_LOGE(TAG, "Failed to subscribe to Cannextstep");
            }
        }

        Config_Report(100);
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
    {
        mqtt_disconnected(event->client);
        /* 自动重连复用 SDK MQTT 任务、队列和 mqtt_rx，不在这里重建客户端。 */
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED client=%p, uplink=%s; keep client and queues",
                 (void *)event->client,
                 IsRoot == 1 ? dual_net_get_active_interface() : "MESH_STA");

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
        if (event->error_handle != NULL && event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
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

/* 生命周期锁保护下调用；SDK任务已停止或从未启动时才能销毁。 */
static void mqtt_destroy_client_locked(void)
{
    esp_err_t err = esp_mqtt_client_destroy(s_client);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Destroy MQTT client failed: %s; retain handle", esp_err_to_name(err));
        portENTER_CRITICAL(&s_mqtt_state_lock);
        s_mqtt_control.phase = MQTT_APP_STOPPING;
        portEXIT_CRITICAL(&s_mqtt_state_lock);
        return;
    }
    portENTER_CRITICAL(&s_mqtt_state_lock);
    s_client = NULL;
    s_mqtt_control.phase = MQTT_APP_STOPPED;
    s_mqtt_control.switch_pending = false;
    s_mqtt_control.expedite_pending = false;
    s_mqtt_control.disconnect_requested = false;
    atomic_store(&s_mqtt_connected, false);
    portEXIT_CRITICAL(&s_mqtt_state_lock);
}

static esp_err_t mqtt_app_start_locked(void)
{
    if (s_client != NULL)
    {
        portENTER_CRITICAL(&s_mqtt_state_lock);
        bool stopping = s_mqtt_control.phase == MQTT_APP_STOPPING;
        portEXIT_CRITICAL(&s_mqtt_state_lock);
        return s_client_started && !stopping ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    char *json = create_up_line_json(0, DEVICE_ID);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .hostname = SERVER_IP,
                .port = SERVER_UDP_Port,
                .transport = MQTT_TRANSPORT_OVER_TCP,
            }},
        .credentials = {.username = "dev", .authentication = {
                                               .password = "123456",
                                           }},
        .session = {.keepalive = IsRoot == 1 ? MQTT_ROOT_KEEPALIVE_SECONDS : MQTT_NODE_KEEPALIVE_SECONDS, .disable_clean_session = false, .protocol_ver = MQTT_PROTOCOL_V_3_1_1, .last_will = {
                                                                                                                                                                                     .topic = "device/public/down_line",
                                                                                                                                                                                     .msg = json,
                                                                                                                                                                                     .qos = 1,
                                                                                                                                                                                     .retain = 0,

                                                                                                                                                                                 }},
        .network = {
            .timeout_ms = 10000, // 网络连接超时时间，单位为毫秒
            .disable_auto_reconnect = false,
            .reconnect_timeout_ms = IsRoot == 1 ? 0 : MQTT_NODE_RECONNECT_WAIT_MS, // 根节点保留SDK默认退避。

        },
        .buffer = {
            .size = 1024,
            .out_size = 8192,
        }

    };

    ESP_LOGI(TAG, "Starting MQTT client...");
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    free(json);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_mqtt_state_lock);
    s_client = client;
    s_mqtt_control.phase = MQTT_APP_CONNECTING;
    s_mqtt_control.switch_pending = false;
    s_mqtt_control.expedite_pending = false;
    s_mqtt_control.disconnect_requested = false;
    atomic_store(&s_mqtt_connected, false);
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    esp_err_t ret = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(ret));
        mqtt_destroy_client_locked();
        return ret;
    }

    ret = esp_mqtt_client_start(s_client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        mqtt_destroy_client_locked();
        return ret;
    }

    s_client_started = true;
    ESP_LOGI(TAG, "MQTT client started: client=%p, start_count=%u",
             (void *)s_client, ++s_client_start_count);
    return ESP_OK;
}

esp_err_t mqtt_app_start(void)
{
    esp_err_t err = mqtt_lifecycle_init();
    if (err != ESP_OK)
        return err;
    SemaphoreHandle_t mutex = mqtt_lifecycle_mutex();
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    err = mqtt_app_start_locked();
    mqtt_lifecycle_unlock(mutex);
    return err;
}

void mqtt_app_stop(void)
{
    SemaphoreHandle_t mutex = mqtt_lifecycle_mutex();
    if (mutex == NULL)
        return;
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "MQTT lifecycle lock failed on stop");
        return;
    }
    portENTER_CRITICAL(&s_mqtt_state_lock);
    s_mqtt_control.phase = MQTT_APP_STOPPING;
    s_mqtt_control.switch_pending = false;
    s_mqtt_control.expedite_pending = false;
    atomic_store(&s_mqtt_connected, false);
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    if (s_client != NULL)
    {
        if (s_client_started)
        {
            esp_err_t err = esp_mqtt_client_stop(s_client);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Stop MQTT client failed: %s; retain handle", esp_err_to_name(err));
                mqtt_lifecycle_unlock(mutex);
                return;
            }
            s_client_started = false;
        }
        /* 回调不获取生命周期锁；在途 publish 用计数保护句柄，释放计数不依赖该锁。 */
        TickType_t started_ticks = xTaskGetTickCount();
        for (;;)
        {
            portENTER_CRITICAL(&s_mqtt_state_lock);
            uint32_t refs = s_mqtt_control.publish_refs;
            portEXIT_CRITICAL(&s_mqtt_state_lock);
            if (refs == 0)
                break;
            if (xTaskGetTickCount() - started_ticks >= pdMS_TO_TICKS(MQTT_PUBLISH_DRAIN_TIMEOUT_MS))
            {
                ESP_LOGE(TAG, "MQTT publish drain timed out; retain stopped client");
                mqtt_lifecycle_unlock(mutex);
                return;
            }
            vTaskDelay(1);
        }
        mqtt_destroy_client_locked();
    }
    else
    {
        portENTER_CRITICAL(&s_mqtt_state_lock);
        s_mqtt_control.phase = MQTT_APP_STOPPED;
        portEXIT_CRITICAL(&s_mqtt_state_lock);
    }
    mqtt_lifecycle_unlock(mutex);
}

void mqtt_app_restart(void)
{
    mqtt_app_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t err = mqtt_app_start();
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Restart MQTT client failed: %s", esp_err_to_name(err));
}

static int mqtt_app_publish(char *topic, char *publish_string)
{
    /* SDK回调中的 Config_Report 也会发布，不能获取可能被 stop 持有的生命周期锁。 */
    portENTER_CRITICAL(&s_mqtt_state_lock);
    esp_mqtt_client_handle_t client = atomic_load(&s_mqtt_connected) &&
                                              s_mqtt_control.phase == MQTT_APP_CONNECTED
                                          ? s_client
                                          : NULL;
    if (client != NULL)
        s_mqtt_control.publish_refs++;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    if (client == NULL)
        return -1;
    int msg_id = esp_mqtt_client_publish(client, topic, publish_string, 0, 1, 0);
    portENTER_CRITICAL(&s_mqtt_state_lock);
    s_mqtt_control.publish_refs--;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    ESP_LOGI(TAG, "sent publish returned msg_id=%d", msg_id);
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
        snprintf(topic, sizeof(topic), "%s", topic_name);
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

static esp_err_t mqtt_init_locked(void)
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

/* 队列仅首次建立，切换出口/重连复用；SDK内部队列分配沿用现有启动期行为。 */
esp_err_t mqtt_init(void)
{
    esp_err_t err = mqtt_lifecycle_init();
    if (err != ESP_OK)
        return err;
    SemaphoreHandle_t mutex = mqtt_lifecycle_mutex();
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    err = mqtt_init_locked();
    mqtt_lifecycle_unlock(mutex);
    return err;
}
