#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "app_mem.h"
#include "Json_data.h"
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

static QueueHandle_t Data_queue = NULL;

/*
 * 统一消息封装：topic 只随首片数据一起入队，与 payload 同队同序。
 * 取代旧双 FIFO（Topic_queue + Data_queue），从结构上消除两类缺陷：
 *  - 任一侧溢出留下孤立项（topic 入队成功而数据被丢，或反之）；
 *  - JSON/Seq 非法时消费端不取 topic，导致后续命令错配到旧 topic。
 * QoS 1 订阅下 Broker 会持续重投直到 PUBACK，队列满又静默丢弃会把
 * "可观测的丢"变成"观测不到的丢"，因此深度按批量下发峰值取 32。
 * 内存：单槽约 1.1KB（含 topic 字段），32 槽约 35KB 内部 RAM。
 */
#define MQTT_DATA_QUEUE_LEN 32
#define MQTT_TOPIC_ITEM_CAPACITY 64
#define MQTT_DATA_ITEM_CAPACITY 1024

/*
 * 下行订阅 QoS（v：可靠性改造）。全部存活订阅统一请求 QoS1；
 * SUBACK 校验（MQTT_EVENT_SUBSCRIBED）必须确认实际授予值等于本宏，
 * 只判 <=2 会让 Broker 静默降级为 QoS0——改了等于没改，且日志看起来
 * 一切正常。降级时订阅判失败，由 mqtt_setup_retry 重试（响亮失败）。
 */
#define MQTT_SUBSCRIBE_QOS 1

typedef struct
{
    uint16_t len;
    char topic[MQTT_TOPIC_ITEM_CAPACITY]; /* 仅首片携带；后续分片为空串 */
    char data[MQTT_DATA_ITEM_CAPACITY];
} mqtt_data_msg_t;

static atomic_bool s_mqtt_connected;
static atomic_uint s_deleted_count;
static atomic_uint s_rejected_count;
/* 接收队列满计数：入队失败即丢消息（SDK 仍会回 PUBACK），必须可观测。 */
static atomic_uint s_queue_overflow_count;
static atomic_uint s_last_outbox_bytes;

/* 根/子节点保活均为60秒。多跳抖动由恢复调度处理，保活时间不代替拥塞控制。 */
#define MQTT_ROOT_KEEPALIVE_SECONDS 60
#define MQTT_NODE_KEEPALIVE_SECONDS 60
/* 重连与上线重试的随机区间：[2,4]、[4,8]、[8,16]、[16,32]、[30,60]秒。 */
#define MQTT_RETRY_STABLE_MS 30000
#define MQTT_INITIAL_JITTER_MS 3000
#define MQTT_CONFIG_JITTER_MS 5000
#define MQTT_SUBACK_WARN_MS 10000
#define MQTT_SERVICE_POLL_MS 100
#define MQTT_OUTBOX_LIMIT_BYTES (64U * 1024U)
#define MQTT_OUTBOX_NORMAL_BYTES (56U * 1024U)
#define MQTT_RUNTIME_QUEUE_LEN 3
#define MQTT_RUNTIME_PAYLOAD_BYTES 256
#define MQTT_ACK_MAP_BYTES (65536U / 2U)
#define MQTT_ACK_PUBLISHED 1U
#define MQTT_ACK_SUBSCRIBED 2U
#define MQTT_ACK_REJECTED 4U
#define MQTT_ACK_DELETED 8U
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
static StaticSemaphore_t s_mqtt_admission_storage;
static SemaphoreHandle_t s_mqtt_admission_mutex;
static TaskHandle_t s_mqtt_service_task;
static QueueHandle_t s_runtime_queue;
/* 启动期分配、模块终生持有，优先 PSRAM。半字节/消息 ID 可无损捕捉接口返回前
 * 的确认，不依赖有限事件队列（大量业务 PUBACK 可把上线确认挤出普通队列）。
 * 仅 capture 期间写；清表时先关闭 capture，在临界区外 memset。 */
static uint8_t *s_ack_map;
static struct {
    uint32_t epoch;
    TickType_t connected_tick;
    bool capture;
    int pending_id;
    uint8_t flags;
    uint8_t subscribe_count;
} s_service_ack;
static uint32_t s_connection_epoch;
static uint8_t s_node_retry_step;
static bool s_node_retry_scheduled;
static TickType_t s_node_retry_due;
static bool s_node_first_start;

typedef struct {
    uint32_t epoch;
    char topic[MQTT_TOPIC_ITEM_CAPACITY];
    char payload[MQTT_RUNTIME_PAYLOAD_BYTES];
} MqttRuntimeLog;

static esp_err_t mqtt_init_locked(void);

static void mqtt_service_wake(void)
{
    /* 任务在客户端创建前建立，终生不删除；首次调度前的通知也可保留。 */
    if (s_mqtt_service_task != NULL)
        xTaskNotifyGive(s_mqtt_service_task);
}

static uint32_t mqtt_retry_delay_ms(uint8_t *step)
{
    static const uint32_t minimum_ms[] = {2000, 4000, 8000, 16000, 30000};
    unsigned index = *step < 4 ? *step : 4;
    if (*step < 4) (*step)++;
    return minimum_ms[index] + esp_random() % (minimum_ms[index] + 1U);
}

static bool mqtt_due(TickType_t now, TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static uint8_t mqtt_ack_flags_locked(int msg_id)
{
    if (msg_id <= 0 || msg_id > UINT16_MAX) return 0;
    return (s_ack_map[(unsigned)msg_id / 2U] >> (((unsigned)msg_id & 1U) * 4U)) & 15U;
}

/* 只复制标量，SDK 回调不取生命周期锁或准入锁，不持有 event/data 指针。 */
static void mqtt_service_ack_event(esp_mqtt_event_handle_t event, uint8_t flags)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    if (event->client == s_client && s_mqtt_control.phase == MQTT_APP_CONNECTED &&
        s_service_ack.epoch == s_connection_epoch) {
        if (s_service_ack.capture && event->msg_id > 0 && event->msg_id <= UINT16_MAX) {
            unsigned shift = ((unsigned)event->msg_id & 1U) * 4U;
            s_ack_map[(unsigned)event->msg_id / 2U] |= flags << shift;
        }
        if (event->msg_id == s_service_ack.pending_id)
            s_service_ack.flags |= flags;
    }
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    mqtt_service_wake();
}

static bool mqtt_session_current(uint32_t epoch)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    bool current = s_client != NULL && atomic_load(&s_mqtt_connected) &&
                   s_mqtt_control.phase == MQTT_APP_CONNECTED && !s_mqtt_control.switch_pending &&
                   epoch == s_connection_epoch;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    return current;
}

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
    SemaphoreHandle_t admission = xSemaphoreCreateMutexStatic(&s_mqtt_admission_storage);
    portENTER_CRITICAL(&s_mqtt_state_lock);
    s_mqtt_lifecycle_mutex = mutex;
    s_mqtt_admission_mutex = admission;
    s_mqtt_mutex_initializing = false;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    return mutex != NULL && admission != NULL ? ESP_OK : ESP_ERR_NO_MEM;
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
    /* 每次上行变化只加速一次；失败后沿用 SDK 自动重连等待，避免拒绝连接时忙循环。 */
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
    TickType_t now = xTaskGetTickCount();
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
            s_connection_epoch++;
            s_service_ack.epoch = s_connection_epoch;
            s_service_ack.connected_tick = now;
            s_service_ack.capture = false;
            s_service_ack.pending_id = 0;
            s_service_ack.flags = 0;
        }
    }
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    mqtt_service_wake();
    if (stale)
    {
        ESP_LOGI(TAG, "连接期间出口已变化，重新建立 MQTT 连接");
        mqtt_request_disconnect(client);
    }
    return usable && !stale;
}

static void mqtt_disconnected(esp_mqtt_client_handle_t client)
{
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&s_mqtt_state_lock);
    bool running = client == s_client && s_mqtt_control.phase != MQTT_APP_STOPPING &&
                   s_mqtt_control.phase != MQTT_APP_STOPPED;
    if (running)
    {
        if (IsRoot == 0 && s_mqtt_control.phase == MQTT_APP_CONNECTED &&
            now - s_service_ack.connected_tick >= pdMS_TO_TICKS(MQTT_RETRY_STABLE_MS))
            s_node_retry_step = 0;
        s_connection_epoch++;
        s_service_ack.capture = false;
        s_service_ack.pending_id = 0;
        s_node_retry_scheduled = false;
        atomic_store(&s_mqtt_connected, false);
        s_mqtt_control.phase = MQTT_APP_WAIT_RECONNECT;
        s_mqtt_control.disconnect_requested = false;
    }
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    if (running)
        mqtt_request_immediate_reconnect(client);
    mqtt_service_wake();
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

        /* 订阅、上线与配置上报由 CPU1 服务任务按确认事件推进。 */
        mqtt_service_wake();
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
        bool accepted = event->data != NULL && event->data_len > 0;
        portENTER_CRITICAL(&s_mqtt_state_lock);
        unsigned expected = s_service_ack.subscribe_count;
        portEXIT_CRITICAL(&s_mqtt_state_lock);
        accepted = accepted && event->data_len == expected;
        /*
         * 严格校验：请求 QoS1 时必须确认 Broker 真的授予了 1。
         * 旧逻辑 <=2U 把降级授予 0 也算成功——那会让"切 QoS1"看起来
         * 生效实则静默降级。任一条 filter 被降级即整批判失败，走上线
         * 重试（MQTT_SETUP_SUBSCRIBE → mqtt_setup_retry），可观测。
         */
        for (int i = 0; i < event->data_len && accepted; ++i)
            accepted = ((uint8_t)event->data[i] == MQTT_SUBSCRIBE_QOS);
        mqtt_service_ack_event(event, accepted ? MQTT_ACK_SUBSCRIBED : MQTT_ACK_REJECTED);
        break;
    }

    case MQTT_EVENT_UNSUBSCRIBED:
    {
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    }

    case MQTT_EVENT_PUBLISHED:
    {
        mqtt_service_ack_event(event, MQTT_ACK_PUBLISHED);
        /* 业务发布确认（PUBACK）：appTask 按 msg_id 结算 pushed=acked。
         * 钩子只入队，不写 DB——SDK 任务上下文禁止 SQLite 提交。 */
        app_mqtt_publish_settled(event->msg_id, true);
        break;
    }

    case MQTT_EVENT_DELETED:
        mqtt_service_ack_event(event, MQTT_ACK_DELETED);
        /* outbox 淘汰/过期：回滚 inflight → pending，等补传 worker 重试。 */
        app_mqtt_publish_settled(event->msg_id, false);
        /* 汇总诊断在服务任务输出，回调不递归发布日志。 */
        atomic_fetch_add(&s_deleted_count, 1);
        break;

    case MQTT_EVENT_DATA:
    {
        ESP_LOGI(TAG,
                 "MQTT_EVENT_DATA, topic_len=%d, data_len=%d, offset=%d, total=%d",
                 event->topic_len,
                 event->data_len,
                 event->current_data_offset,
                 event->total_data_len);

        if (Data_queue == NULL)
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
         * Unified message: the topic rides on the first fragment (offset==0) of the
         * same message, so topic and payload can never be paired with the wrong
         * message. Fragments are accumulated by app_MQTT_Rdata_handle().
         */
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

            if (send_count == 0 && event->topic != NULL && event->topic_len > 0)
            {
                size_t topic_len = (size_t)event->topic_len;
                if (topic_len >= MQTT_TOPIC_ITEM_CAPACITY)
                {
                    ESP_LOGE(TAG,
                             "MQTT topic is too long: %u, capacity=%u; whole message dropped",
                             (unsigned)topic_len,
                             (unsigned)(MQTT_TOPIC_ITEM_CAPACITY - 1));
                    break;
                }
                memcpy(data_msg.topic, event->topic, topic_len);
                data_msg.topic[topic_len] = '\0';
            }

            if (xQueueSend(Data_queue, &data_msg, pdMS_TO_TICKS(100)) != pdTRUE)
            {
                atomic_fetch_add(&s_queue_overflow_count, 1);
                ESP_LOGE(TAG,
                         "Failed to enqueue MQTT data fragment %d, len=%d, queue_overflow_total=%u",
                         send_count + 1,
                         chunk_size,
                         (unsigned)atomic_load(&s_queue_overflow_count));
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

    esp_err_t init_err = mqtt_init_locked();
    if (init_err != ESP_OK) return init_err;
    char *json = create_up_line_json(0, DEVICE_ID);
    if (json == NULL) return ESP_ERR_NO_MEM;

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address = {
            .hostname = SERVER_IP,
            .port = SERVER_UDP_Port,
            .transport = MQTT_TRANSPORT_OVER_TCP,
        },
        .credentials = {.username = "dev", .authentication.password = "123456"},
        .session = {
            .keepalive = IsRoot == 1 ? MQTT_ROOT_KEEPALIVE_SECONDS : MQTT_NODE_KEEPALIVE_SECONDS,
            .message_retransmit_timeout = 5000,
            .disable_clean_session = false,
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
            .last_will = {
                .topic = "device/public/down_line", .msg = json, .qos = 1, .retain = 0,
            },
        },
        .network = {
            .timeout_ms = 10000,
            .disable_auto_reconnect = IsRoot == 0,
            .reconnect_timeout_ms = 0, // 根节点使用SDK默认10秒；子节点由服务任务调度。
        },
        .outbox.limit = MQTT_OUTBOX_LIMIT_BYTES,
        .buffer = {.size = 1024, .out_size = 8192},
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
    s_connection_epoch++;
    s_node_retry_step = 0;
    s_node_first_start = IsRoot == 0;
    s_node_retry_scheduled = false;
    s_mqtt_control.phase = IsRoot == 0 ? MQTT_APP_WAIT_RECONNECT : MQTT_APP_CONNECTING;
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

    if (IsRoot == 0) {
        /* 首次 start 也由服务任务错峰；句柄在生命周期锁下已完整注册。 */
        mqtt_service_wake();
        return ESP_OK;
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
    s_connection_epoch++;
    s_service_ack.capture = false;
    s_service_ack.pending_id = 0;
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

/* SDK QoS0 enqueue(store=false) 不发送，运行日志另走有界、一次性服务队列。 */
static bool mqtt_runtime_topic(const char *topic)
{
    char expected[MQTT_TOPIC_ITEM_CAPACITY];
    int len = snprintf(expected, sizeof(expected), "device/%s/log/runtime", DEVICE_ID);
    return len > 0 && len < sizeof(expected) && strcmp(topic, expected) == 0;
}

static void mqtt_client_release(void)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    s_mqtt_control.publish_refs--;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
}

/* 准入锁下获取引用，stop 等待引用释放后才销毁；不获取生命周期锁。 */
static esp_mqtt_client_handle_t mqtt_client_acquire(uint32_t epoch)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    esp_mqtt_client_handle_t client = atomic_load(&s_mqtt_connected) &&
        s_mqtt_control.phase == MQTT_APP_CONNECTED && !s_mqtt_control.switch_pending &&
        (epoch == 0 || epoch == s_connection_epoch) ? s_client : NULL;
    if (client != NULL) s_mqtt_control.publish_refs++;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    return client;
}

static int mqtt_enqueue_for_session(const char *topic, const char *payload, uint32_t epoch, bool startup)
{
    if (s_mqtt_admission_mutex == NULL ||
        xSemaphoreTake(s_mqtt_admission_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;
    esp_mqtt_client_handle_t client = mqtt_client_acquire(epoch);
    int result = -1;
    if (client != NULL) {
        size_t bytes = strlen(payload) + strlen(topic) + 16U; // 含 MQTT 可变长头与 packet ID 的保守上界。
        unsigned used = esp_mqtt_client_get_outbox_size(client);
        unsigned limit = startup ? MQTT_OUTBOX_LIMIT_BYTES : MQTT_OUTBOX_NORMAL_BYTES;
        atomic_store(&s_last_outbox_bytes, used);
        if (bytes > limit || used > limit - bytes) {
            result = -2;
            atomic_fetch_add(&s_rejected_count, 1);
        } else {
            result = esp_mqtt_client_enqueue(client, topic, payload, 0, 1, 0, true);
        }
        mqtt_client_release();
    }
    if (xSemaphoreGive(s_mqtt_admission_mutex) != pdTRUE)
        ESP_LOGE(TAG, "MQTT admission unlock failed");
    return result;
}

bool app_mqtt_is_connected(void)
{
    return atomic_load(&s_mqtt_connected);
}

int app_mqtt_publish(char *topic_name, char *publish_string, char *device_id)
{
    if (topic_name == NULL || publish_string == NULL) return -1;
    char topic[MQTT_TOPIC_ITEM_CAPACITY];
    int len = device_id != NULL ? snprintf(topic, sizeof(topic), topic_name, device_id)
                                : snprintf(topic, sizeof(topic), "%s", topic_name);
    if (len < 0 || len >= sizeof(topic)) return -1;
    if (mqtt_runtime_topic(topic)) {
        MqttRuntimeLog item = {0};
        if (strlen(publish_string) >= sizeof(item.payload) || s_runtime_queue == NULL) return -1;
        portENTER_CRITICAL(&s_mqtt_state_lock);
        item.epoch = s_connection_epoch;
        bool connected = atomic_load(&s_mqtt_connected);
        portEXIT_CRITICAL(&s_mqtt_state_lock);
        if (!connected) return -1;
        memcpy(item.topic, topic, (size_t)len + 1U);
        memcpy(item.payload, publish_string, strlen(publish_string) + 1U);
        if (xQueueSend(s_runtime_queue, &item, 0) != pdTRUE) {
            atomic_fetch_add(&s_rejected_count, 1);
            return -2;
        }
        mqtt_service_wake();
        return 0; // 已接受 best effort 日志；不是 broker 确认。
    }
    return mqtt_enqueue_for_session(topic, publish_string, 0, false);
}

typedef enum {
    MQTT_SETUP_SUBSCRIBE, MQTT_SETUP_SUBACK, MQTT_SETUP_ONLINE,
    MQTT_SETUP_ONLINE_ACK, MQTT_SETUP_CONFIG_WAIT, MQTT_SETUP_CONFIG, MQTT_SETUP_DONE
} MqttSetupPhase;

typedef struct {
    uint32_t epoch;
    MqttSetupPhase phase;
    TickType_t due;
    TickType_t suback_started;
    uint8_t retry_step;
    bool suback_warned;
    char *online_json; // 仅服务任务访问、每代次生成一次；下一代次/断开时释放。
} MqttServiceWork;
static MqttServiceWork s_service_work;

static void mqtt_setup_retry(MqttServiceWork *work, TickType_t now)
{
    uint32_t delay_ms = mqtt_retry_delay_ms(&work->retry_step);
    work->due = now + pdMS_TO_TICKS(delay_ms);
    ESP_LOGW(TAG, "MQTT setup retry: epoch=%lu phase=%d wait_ms=%lu",
             (unsigned long)work->epoch, work->phase, (unsigned long)delay_ms);
}

static bool mqtt_ack_begin(uint32_t epoch, unsigned subscribe_count)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    s_service_ack.capture = false;
    s_service_ack.pending_id = 0;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    memset(s_ack_map, 0, MQTT_ACK_MAP_BYTES);
    portENTER_CRITICAL(&s_mqtt_state_lock);
    bool current = epoch == s_connection_epoch && atomic_load(&s_mqtt_connected);
    if (current) {
        s_service_ack.epoch = epoch;
        s_service_ack.flags = 0;
        s_service_ack.subscribe_count = subscribe_count;
        s_service_ack.capture = true;
    }
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    return current;
}

static void mqtt_ack_register(uint32_t epoch, int msg_id)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    if (epoch == s_connection_epoch && s_service_ack.epoch == epoch) {
        s_service_ack.flags = mqtt_ack_flags_locked(msg_id);
        s_service_ack.pending_id = msg_id > 0 ? msg_id : 0;
        s_service_ack.capture = false;
    }
    portEXIT_CRITICAL(&s_mqtt_state_lock);
}

static uint8_t mqtt_ack_result(uint32_t epoch)
{
    portENTER_CRITICAL(&s_mqtt_state_lock);
    uint8_t flags = epoch == s_connection_epoch ? s_service_ack.flags : 0;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    return flags;
}

/* 生命周期锁由调用者持有。SDK subscribe 写入可能阻塞，回调不获取本锁。 */
static int mqtt_restore_subscriptions(uint32_t epoch)
{
    /*
     * 下行命令必须 QoS1：QoS0 下命令可静默丢失，服务端只能等超时。
     * 与 SUBACK 严格校验共用此宏，避免两处不一致。
     * 前置条件（实施顺序硬约束）：接收队列加深与统一消息封装必须已生效，
     * 否则 QoS1 的重投会把"可观测的丢"放大为"静默丢 + 重传风暴"。
     */
    static const struct { const char *format; bool rack; } common[] = {
        {"server/%s/cmd/ota", false}, {"server/rack/%s/CanRestart", true},
        {"server/%s/cmd/set_config", false}, {"server/%s/cmd/get_config", false},
    }, child[] = {
        {"server/%s/cmd/start_aging", false}, {"server/%s/cmd/stop_aging", false},
        {"server/%s/public/cmd/stop_aging", true}, {"server/%s/cmd/set_pn", false},
        {"server/%s/cmd/get_state", false}, {"server/%s/cmd/get_lost_aging", false},
        {"server/%s/cmd/remote_log", false}, {"server/%s/cmd/local_log", false},
        {"server/rack/%s/CanNextstep", true},
    };
    char names[13][MQTT_TOPIC_ITEM_CAPACITY];
    esp_mqtt_topic_t topics[13];
    unsigned count = 0;
    for (unsigned i = 0; i < sizeof(common) / sizeof(common[0]); ++i) {
        int n = snprintf(names[count], sizeof(names[count]), common[i].format,
                         common[i].rack ? AgingNumber : DEVICE_ID);
        if (n < 0 || n >= sizeof(names[count])) return -1;
        topics[count] = (esp_mqtt_topic_t){.filter = names[count], .qos = MQTT_SUBSCRIBE_QOS};
        count++;
    }
    if (IsRoot == 1) {
        int n = snprintf(names[count], sizeof(names[count]), "server/%s/cmd/CPower", DEVICE_ID);
        if (n < 0 || n >= sizeof(names[count])) return -1;
        topics[count] = (esp_mqtt_topic_t){.filter = names[count], .qos = MQTT_SUBSCRIBE_QOS};
        count++;
    } else {
        for (unsigned i = 0; i < sizeof(child) / sizeof(child[0]); ++i) {
            int n = snprintf(names[count], sizeof(names[count]), child[i].format,
                             child[i].rack ? AgingNumber : DEVICE_ID);
            if (n < 0 || n >= sizeof(names[count])) return -1;
            topics[count] = (esp_mqtt_topic_t){.filter = names[count], .qos = MQTT_SUBSCRIBE_QOS};
            count++;
        }
    }
    if (xSemaphoreTake(s_mqtt_admission_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;
    int result = -1;
    if (mqtt_session_current(epoch) && mqtt_ack_begin(epoch, count)) {
        unsigned used = esp_mqtt_client_get_outbox_size(s_client);
        /* SUBSCRIBE: fixed header + packet ID + 每个 topic 的长度字段与 QoS 字节。 */
        size_t bytes = 16U;
        for (unsigned i = 0; i < count; ++i) bytes += strlen(names[i]) + 3U;
        if (bytes > MQTT_OUTBOX_LIMIT_BYTES || used > MQTT_OUTBOX_LIMIT_BYTES - bytes) {
            result = -2;
            atomic_fetch_add(&s_rejected_count, 1);
        } else {
            result = esp_mqtt_client_subscribe_multiple(s_client, topics, count);
            /* SDK 写订阅失败时可能已将未知 msg_id 放入 outbox，不能直接再提交一批。
             * 请求正常断开后由既有恢复路径处理，不停止/重建客户端。 */
            if (result < 0 && esp_mqtt_client_disconnect(s_client) != ESP_OK)
                ESP_LOGW(TAG, "MQTT subscribe failed; disconnect request rejected");
        }
        mqtt_ack_register(epoch, result);
    }
    if (xSemaphoreGive(s_mqtt_admission_mutex) != pdTRUE)
        ESP_LOGE(TAG, "MQTT admission unlock failed");
    return result;
}

/* 只在服务任务访问调度；回调更新的计数/状态在短临界区内快照。 */
static void mqtt_node_reconnect_poll(TickType_t now)
{
    if (IsRoot != 0) return;
    SemaphoreHandle_t mutex = mqtt_lifecycle_mutex();
    if (mutex == NULL || xSemaphoreTake(mutex, 0) != pdTRUE) return;
    portENTER_CRITICAL(&s_mqtt_state_lock);
    bool waiting = s_client != NULL && s_mqtt_control.phase == MQTT_APP_WAIT_RECONNECT;
    bool scheduled = s_node_retry_scheduled;
    bool first = s_node_first_start;
    uint8_t step = s_node_retry_step;
    TickType_t due = s_node_retry_due;
    uint32_t epoch = s_connection_epoch;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    bool parent_online = __atomic_load_n(&Network_Flag, __ATOMIC_ACQUIRE) != 0;
    if (!waiting || !parent_online) {
        portENTER_CRITICAL(&s_mqtt_state_lock);
        s_node_retry_scheduled = false;
        portEXIT_CRITICAL(&s_mqtt_state_lock);
    } else if (!scheduled) {
        unsigned logged_step = step;
        uint32_t delay_ms = first ? esp_random() % (MQTT_INITIAL_JITTER_MS + 1U)
                                  : mqtt_retry_delay_ms(&step);
        portENTER_CRITICAL(&s_mqtt_state_lock);
        if (epoch == s_connection_epoch) {
            s_node_retry_due = now + pdMS_TO_TICKS(delay_ms);
            s_node_retry_scheduled = true;
        }
        portEXIT_CRITICAL(&s_mqtt_state_lock);
        ESP_LOGI(TAG, "Child MQTT connect scheduled: wait_ms=%lu step=%u first=%d",
                 (unsigned long)delay_ms, logged_step, first);
    } else if (mqtt_due(now, due)) {
        portENTER_CRITICAL(&s_mqtt_state_lock);
        bool current = epoch == s_connection_epoch && s_mqtt_control.phase == MQTT_APP_WAIT_RECONNECT;
        if (current) {
            s_mqtt_control.phase = MQTT_APP_CONNECTING;
            s_node_retry_scheduled = false;
            s_node_first_start = false;
            if (!first && s_node_retry_step < 4) s_node_retry_step++;
        }
        portEXIT_CRITICAL(&s_mqtt_state_lock);
        if (current) {
            esp_err_t err = s_client_started ? esp_mqtt_client_reconnect(s_client)
                                             : esp_mqtt_client_start(s_client);
            if (err == ESP_OK) {
                if (!s_client_started) {
                    s_client_started = true;
                    ESP_LOGI(TAG, "MQTT client started: client=%p, start_count=%u",
                             (void *)s_client, ++s_client_start_count);
                }
            } else {
                portENTER_CRITICAL(&s_mqtt_state_lock);
                if (epoch == s_connection_epoch && s_mqtt_control.phase == MQTT_APP_CONNECTING)
                    s_mqtt_control.phase = MQTT_APP_WAIT_RECONNECT;
                portEXIT_CRITICAL(&s_mqtt_state_lock);
                ESP_LOGW(TAG, "Child MQTT connect request failed: %s", esp_err_to_name(err));
            }
        }
    }
    mqtt_lifecycle_unlock(mutex);
}

static void mqtt_service_poll(void)
{
    TickType_t now = xTaskGetTickCount();
    mqtt_node_reconnect_poll(now);
    portENTER_CRITICAL(&s_mqtt_state_lock);
    uint32_t epoch = s_connection_epoch;
    bool connected = atomic_load(&s_mqtt_connected) && s_mqtt_control.phase == MQTT_APP_CONNECTED;
    portEXIT_CRITICAL(&s_mqtt_state_lock);
    MqttServiceWork *work = &s_service_work;
    if (work->epoch != epoch) {
        free(work->online_json);
        *work = (MqttServiceWork){.epoch = epoch, .phase = MQTT_SETUP_SUBSCRIBE, .due = now};
    }
    if (!connected || !mqtt_due(now, work->due)) return;
    uint8_t flags = mqtt_ack_result(epoch);
    if (work->phase == MQTT_SETUP_SUBACK) {
        if (flags & (MQTT_ACK_REJECTED | MQTT_ACK_DELETED)) {
            work->phase = MQTT_SETUP_SUBSCRIBE;
            mqtt_setup_retry(work, now);
        } else if (flags & MQTT_ACK_SUBSCRIBED) {
            work->phase = MQTT_SETUP_ONLINE;
            work->retry_step = 0;
            ESP_LOGI(TAG, "MQTT subscriptions confirmed: epoch=%lu", (unsigned long)epoch);
        } else if (!work->suback_warned &&
                   now - work->suback_started >= pdMS_TO_TICKS(MQTT_SUBACK_WARN_MS)) {
            work->suback_warned = true;
            ESP_LOGW(TAG, "MQTT SUBACK delayed; keep original batch pending, epoch=%lu", (unsigned long)epoch);
        }
        return;
    }
    if (work->phase == MQTT_SETUP_ONLINE_ACK) {
        if (flags & MQTT_ACK_PUBLISHED) {
            work->phase = MQTT_SETUP_CONFIG_WAIT;
            work->due = now + pdMS_TO_TICKS(esp_random() % (MQTT_CONFIG_JITTER_MS + 1U));
            work->retry_step = 0;
            ESP_LOGI(TAG, "MQTT online confirmed: epoch=%lu", (unsigned long)epoch);
        } else if (flags & MQTT_ACK_DELETED) {
            work->phase = MQTT_SETUP_ONLINE;
            mqtt_setup_retry(work, now);
        }
        return;
    }
    if (work->phase == MQTT_SETUP_CONFIG_WAIT) work->phase = MQTT_SETUP_CONFIG;
    if (work->phase == MQTT_SETUP_DONE) return;
    SemaphoreHandle_t mutex = mqtt_lifecycle_mutex();
    if (xSemaphoreTake(mutex, 0) != pdTRUE) return;
    if (!mqtt_session_current(epoch)) {
        mqtt_lifecycle_unlock(mutex);
        return;
    }
    if (work->phase == MQTT_SETUP_SUBSCRIBE) {
        int id = mqtt_restore_subscriptions(epoch);
        if (id > 0) {
            work->phase = MQTT_SETUP_SUBACK;
            work->suback_started = now;
            work->suback_warned = false;
        } else mqtt_setup_retry(work, now);
    } else if (work->phase == MQTT_SETUP_ONLINE) {
        if (work->online_json == NULL) work->online_json = create_up_line_json(time(NULL), DEVICE_ID);
        int id = -1;
        if (work->online_json != NULL && mqtt_ack_begin(epoch, 0)) {
            id = mqtt_enqueue_for_session("device/public/up_line", work->online_json, epoch, true);
            mqtt_ack_register(epoch, id);
        }
        if (id > 0) work->phase = MQTT_SETUP_ONLINE_ACK;
        else mqtt_setup_retry(work, now);
    } else if (work->phase == MQTT_SETUP_CONFIG) {
        char *json = CreateReadConfigJsonFromFiles_MQTT(time(NULL), 100, 1, NULL);
        char topic[MQTT_TOPIC_ITEM_CAPACITY];
        int len = snprintf(topic, sizeof(topic), "device/%s/cmd/reply/config", DEVICE_ID);
        if (json != NULL && len > 0 && len < sizeof(topic)) {
            if (strlen(json) + strlen(topic) + 16U > MQTT_OUTBOX_NORMAL_BYTES) {
                atomic_fetch_add(&s_rejected_count, 1);
                ESP_LOGE(TAG, "MQTT config exceeds budget; full payload rejected, bytes=%u", (unsigned)strlen(json));
                work->phase = MQTT_SETUP_DONE;
                app_mqtt_backfill_wake(); /* 上线完成：触发断线补传扫描 */
            } else if (mqtt_enqueue_for_session(topic, json, epoch, false) > 0) {
                work->phase = MQTT_SETUP_DONE;
                app_mqtt_backfill_wake(); /* 上线完成：触发断线补传扫描 */
            } else mqtt_setup_retry(work, now);
        } else mqtt_setup_retry(work, now);
        free(json);
    }
    mqtt_lifecycle_unlock(mutex);
}

static void mqtt_runtime_poll(void)
{
    MqttRuntimeLog item;
    if (xQueueReceive(s_runtime_queue, &item, 0) != pdTRUE) return;
    if (s_service_work.phase < MQTT_SETUP_CONFIG_WAIT) return; // 上线优先，日志不补发。
    if (xSemaphoreTake(s_mqtt_admission_mutex, 0) != pdTRUE) return;
    esp_mqtt_client_handle_t client = mqtt_client_acquire(item.epoch);
    if (client != NULL) {
        // 仅此 best effort 路径同步写，位于 CPU1 服务任务，绝不在 SDK 回调/业务调用者中写。
        if (esp_mqtt_client_publish(client, item.topic, item.payload, 0, 0, 0) < 0)
            atomic_fetch_add(&s_rejected_count, 1);
        mqtt_client_release();
    }
    if (xSemaphoreGive(s_mqtt_admission_mutex) != pdTRUE)
        ESP_LOGE(TAG, "MQTT admission unlock failed");
}

void mqtt_app_service_task(void *arg)
{
    TickType_t last_diagnostic = xTaskGetTickCount();
    unsigned last_rejected = 0, last_deleted = 0, last_overflow = 0;
    for (;;) {
        mqtt_service_poll();
        mqtt_runtime_poll();
        TickType_t now = xTaskGetTickCount();
        if (now - last_diagnostic >= pdMS_TO_TICKS(10000)) {
            unsigned rejected = atomic_load(&s_rejected_count), deleted = atomic_load(&s_deleted_count);
            unsigned overflow = atomic_load(&s_queue_overflow_count);
            if (rejected != last_rejected || deleted != last_deleted || overflow != last_overflow)
                ESP_LOGW(TAG, "MQTT pressure: rejected=%u expired=%u queue_overflow=%u last_outbox_bytes=%u",
                         rejected, deleted, overflow, atomic_load(&s_last_outbox_bytes));
            last_rejected = rejected;
            last_deleted = deleted;
            last_overflow = overflow;
            last_diagnostic = now;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MQTT_SERVICE_POLL_MS));
    }
}

/*
 * 统一消息封装的消费端暂存区：topic 由数据首片携带，app_read_MQTT_data
 * 取出首片时在此暂存，app_read_MQTT_topic 随后取用。二者被同一任务
 * （mqtt_rx）顺序调用，单写者单读者，无需同步原语。
 * 暂存区永不过期匹配：新消息的首片会覆盖旧暂存值，因此旧 topic 不可能
 * 与后续命令的 payload 错配（旧双 FIFO 的根因）。
 */
static char s_pending_topic[MQTT_TOPIC_ITEM_CAPACITY];
static bool s_pending_topic_valid;

int app_read_MQTT_topic(char *topic_buffer, int buffer_len, uint32_t timeout)
{
    if (topic_buffer == NULL || buffer_len <= 0)
    {
        ESP_LOGE(TAG, "invalid topic buffer");
        return -1;
    }

    (void)timeout; /* topic 与数据同源，无需独立等待 */

    if (!s_pending_topic_valid)
    {
        /*
         * 当前没有与数据配对的主题：对应消息的首片在入队前已被丢弃
         * （队列满/topic 超长）。返回 -1 让调用方按不可路由处理，
         * 绝不去猜一条旧主题——那正是旧双 FIFO 错配的成因。
         */
        return -1;
    }

    size_t copy_len = strlen(s_pending_topic);
    if (copy_len > (size_t)(buffer_len - 1))
    {
        copy_len = (size_t)(buffer_len - 1);
        ESP_LOGW(TAG, "MQTT topic truncated from %u to %u bytes",
                 (unsigned)strlen(s_pending_topic), (unsigned)copy_len);
    }

    memcpy(topic_buffer, s_pending_topic, copy_len);
    topic_buffer[copy_len] = '\0';
    s_pending_topic_valid = false;
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

    /* 首片携带 topic：先暂存供 app_read_MQTT_topic 取用（同队同序，必然配对）。 */
    if (msg.topic[0] != '\0')
    {
        size_t topic_len = strlen(msg.topic);
        if (topic_len >= sizeof(s_pending_topic))
        {
            topic_len = sizeof(s_pending_topic) - 1;
        }
        memcpy(s_pending_topic, msg.topic, topic_len);
        s_pending_topic[topic_len] = '\0';
        s_pending_topic_valid = true;
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
    if (Data_queue == NULL)
    {
        Data_queue = xQueueCreate(MQTT_DATA_QUEUE_LEN, sizeof(mqtt_data_msg_t));
        if (Data_queue == NULL)
        {
            ESP_LOGE(TAG, "Failed to create Data_queue");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_ack_map == NULL) {
        s_ack_map = app_calloc_prefer_psram(1, MQTT_ACK_MAP_BYTES);
        if (s_ack_map == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_runtime_queue == NULL) {
        s_runtime_queue = xQueueCreate(MQTT_RUNTIME_QUEUE_LEN, sizeof(MqttRuntimeLog));
        if (s_runtime_queue == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_mqtt_service_task == NULL &&
        app_create_mqtt_service_task(mqtt_app_service_task, &s_mqtt_service_task) != pdPASS)
        return ESP_ERR_NO_MEM;
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
