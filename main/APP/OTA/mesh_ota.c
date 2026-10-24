/**
 * @file mesh_ota.c
 * @brief Mesh-Lite BINARY 分发验证：旧固件运行期间逐层提供已校验的新镜像。
 *
 * mqtt_rx 只通知 cmd_task；根由 dl_task 下载一次，子由 BINARY LAN 回调写入。
 * done 回调完成镜像/版本校验，cmd_task 计算 SHA、提交 NVS、设置启动分区、登记文件名、
 * 恢复下级 OTA 后输出 OTA_BINARY_READY，并经本机 event/OTACP 上报完成。不自动重启。
 *
 * 库回调/事件任务与 cmd_task 共享接收会话和供数记录，均经 s_ota_mutex 保护；
 * 文件供数期间持锁读取，避免失败清理/重试擦除与读取同一分区并发。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#include "cJSON.h"
#include "esp_app_format.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh_lite.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mesh_ota.h"
#include "ota_cache.h"
#include "ConfigData.h"
#include "mqtt_app.h"
#include "task_config.h"
#include "esp_random.h"
#include "system_restart.h"

static const char *TAG = "mesh_ota";

#ifdef CONFIG_ESP_MESH_LITE_OTA_ENABLE
#ifdef CONFIG_OTA_AUTO_RESTART
#error "BINARY staging requires CONFIG_OTA_AUTO_RESTART to be disabled"
#endif

#define LG_MESH_OTA_RETRY_MAX 2
#define LG_MESH_OTA_RETRY_INTERVAL_MS (90u * 1000u)
#define LG_MESH_OTA_ROOT_DL_TIMEOUT_MS (10u * 60u * 1000u)
#define LG_MESH_OTA_DONE_WAIT_MS (10u * 1000u)
#define LG_MESH_OTA_PROGRESS_LOG_STEP_PCT 10
/* Mesh-Lite 文件长度按 64 KiB 对齐；真实长度单独保留。 */
#define LG_MESH_OTA_FILE_ALIGNMENT_BYTES (64u * 1024u)
#define LG_MESH_OTA_SERVICE_POLL_MS 1000
#define LG_MESH_OTA_OTACP_RETRY_MS 5000
#define LG_MESH_OTA_OTACP_TOPIC_BYTES 64
#define LG_MESH_OTA_OTACP_PAYLOAD_BYTES 256
#define LG_MESH_OTA_PARENT_STABLE_MS 10000u
#define LG_MESH_OTA_PARENT_JITTER_MS 3000u
#define LG_MESH_OTA_DRAIN_WARN_MS 30000u
/* 首块等待与恢复控制独立于 SDK 的 Pending 定时器，不开启第二次传输。 */
#define LG_MESH_OTA_FIRST_DATA_WAIT_MS 10000u
#define LG_MESH_OTA_RESUME_INTERVAL_MS 5000u
#define LG_MESH_OTA_RESUME_JITTER_MS 1000u
#define LG_MESH_OTA_RESUME_REQUEST_MAX 3u
#define LG_MESH_OTA_RESUME_MAILBOX_SIZE 8u
#define LG_MESH_OTA_RESUME_REQUEST_TTL_MS 30000u
#define LG_MESH_OTA_RESUME_MSG "lg_ota_resume_request_v1"

#define CMD_EVT_REQUEST BIT0
#define CMD_EVT_FINISH BIT1
#define CMD_EVT_DOWNLOAD_DONE BIT2
#define CMD_EVT_PARENT BIT3
#define CMD_EVT_RESUME BIT4
#define DL_EVT_START BIT0

typedef struct {
    bool valid;
    char file_name[SIMPLE_OTA_BINARY_FILENAME_CAPACITY];
    char version[SIMPLE_OTA_VERSION_MAX_LEN];
    size_t size_bytes;
    const esp_partition_t *partition;
    char sha256_hex[65];
} mesh_ready_file_t;

static StaticEventGroup_t s_cmd_events_buf, s_dl_events_buf;
static EventGroupHandle_t s_cmd_events, s_dl_events;
static StaticSemaphore_t s_ota_mutex_buf;
static SemaphoreHandle_t s_ota_mutex;
static atomic_bool s_initialized;
/* 1=可受理，0=命令已排队或正在执行。MQTT 入口用原子交换避免连续命令重复入队。 */
static atomic_bool s_command_available = true;
/* 仅服务任务访问；区分“入口已置 0、尚未来得及置事件位”和实际消费过的命令。 */
static bool s_command_observed;

/* 网络事件不能等待 Flash/OTA mutex。代次锁仅保护定宽快照；回调写入前也核对它。 */
typedef struct {
    bool online;
    uint32_t generation;
    TickType_t online_since;
} OtaParentState;
static portMUX_TYPE s_parent_lock = portMUX_INITIALIZER_UNLOCKED;
static OtaParentState s_parent;
static uint32_t s_rx_parent_generation;
/* 旧 SDK 终态未排空时，禁止新传输；esp_ota_abort 不能替代库取消接口。 */
static bool s_sdk_inflight;
/* 接收回调只登记首块事实；服务任务据此停止恢复请求。 */
static bool s_rx_first_data;
static TickType_t s_rx_start_tick;

typedef struct {
    char file_name[SIMPLE_OTA_BINARY_FILENAME_CAPACITY];
    char version[SIMPLE_OTA_VERSION_MAX_LEN];
    char device_id[OTA_CACHE_ID_CAPACITY];
    uint32_t size_bytes;
    uint32_t request_id;
    TickType_t received_tick;
} OtaResumeRequest;
/* SDK 消息回调不能等 OTA/Flash 锁；固定邮箱仅在短临界区内复制值。 */
static portMUX_TYPE s_resume_lock = portMUX_INITIALIZER_UNLOCKED;
static OtaResumeRequest s_resume_mailbox[LG_MESH_OTA_RESUME_MAILBOX_SIZE];
static unsigned s_resume_head, s_resume_count;
static uint32_t s_resume_dropped;

/* s_ota_mutex 保护以下接收会话、完成事实与供数记录。 */
static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_update_partition;
static bool s_session_open, s_rx_accepting, s_rx_done, s_rx_validated, s_rx_failed;
static bool s_finish_received;
static esp_mesh_lite_ota_finish_reason_t s_finish_reason;
static TickType_t s_finish_tick;
static mesh_ready_file_t s_ready_file;
/* 库固定复制 32 字节，缓冲必须补零且在库调用结束后继续有效。 */
static char s_registered_file_name[SIMPLE_OTA_BINARY_FILENAME_CAPACITY];
static esp_err_t s_download_error;

/* 参数在会话开始前设置；回调只在 s_session_open 时读取，重试复用。 */
static simple_ota_metadata_t s_metadata;
static size_t s_firmware_size_bytes;
static bool s_metadata_valid;

/* 以下仅由 cmd_task 访问。 */
static uint8_t s_retry_count;
static bool s_retry_waiting, s_finalization_pending, s_root_downloading;
static bool s_root_timeout_logged, s_finish_wait_logged;
static TickType_t s_retry_deadline, s_root_start_tick;
static ota_cache_record_t s_candidate_record;
static bool s_candidate_persisted, s_candidate_recovered;
/* 只由本次启动内的新完成事件设置；启动恢复供数不触发历史 OTACP。 */
static bool s_candidate_report_completion;
static bool s_recovery_pending, s_bootstrap_pending, s_request_deferred;
/* 仅服务任务访问；是本次启动内的升级意图，不是掉电续传记录。 */
static bool s_child_request_active, s_child_wait_parent, s_child_resume_scheduled;
static TickType_t s_child_resume_due, s_drain_warn_tick;
static uint8_t s_bootstrap_attempts;
static TickType_t s_bootstrap_retry_tick;

/* 完成通知仅由 cmd_task 访问；入队失败保留同一事件，成功后交给 SDK QoS1 重传。 */
static bool s_otacp_pending, s_otacp_payload_ready;
static time_t s_otacp_seq;
static TickType_t s_otacp_retry_tick;
static char s_otacp_topic[LG_MESH_OTA_OTACP_TOPIC_BYTES];
static char s_otacp_payload[LG_MESH_OTA_OTACP_PAYLOAD_BYTES];
static char s_report_device_id[OTA_CACHE_ID_CAPACITY];
static char s_report_aging_number[OTA_CACHE_ID_CAPACITY];
/* 同版本命令正在准备供数；普通失败重试复用 s_metadata 中的原目标。 */
static bool s_current_supply_pending;
/* 以下控制调度仅由服务任务访问，首块事实仍经 s_ota_mutex 读取。 */
static unsigned s_resume_request_count;
static uint32_t s_resume_request_id;
static TickType_t s_resume_request_due, s_last_resume_tick;
static bool s_resume_exhausted_logged, s_last_resume_valid;

static void schedule_retry(void);
static void start_attempt(void);

static OtaParentState parent_state(void)
{
    portENTER_CRITICAL(&s_parent_lock);
    OtaParentState state = s_parent;
    portEXIT_CRITICAL(&s_parent_lock);
    return state;
}

void mesh_ota_parent_link_changed(bool has_ip)
{
    if (IsRoot == 1) return;
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&s_parent_lock);
    if (!has_ip) {
        s_parent.online = false;
        ++s_parent.generation;
    } else if (!s_parent.online) {
        s_parent.online = true;
        s_parent.online_since = now;
    }
    portEXIT_CRITICAL(&s_parent_lock);
    if (s_initialized) xEventGroupSetBits(s_cmd_events, CMD_EVT_PARENT);
}

/* 调用者持有 OTA mutex；SDK 回调不因网络事件延迟而继续写旧会话。 */
static bool receiver_link_current(void)
{
    OtaParentState parent = parent_state();
    return parent.online && parent.generation == s_rx_parent_generation && !system_restart_is_planned();
}

/* EventGroup SetBits 的返回值为当前事件位而非错误码；置位点均为任务上下文。
 * mutex 的 Give 返回值则必须核验，便于发现错误所有权或初始化问题。 */
static void ota_unlock(void)
{
    if (xSemaphoreGive(s_ota_mutex) != pdTRUE) {
        ESP_LOGE(TAG, "BINARY mutex release failed");
    }
}

static bool binary_name_valid(const char *name)
{
    return name != NULL && name[0] != '\0' &&
           strnlen(name, SIMPLE_OTA_BINARY_FILENAME_CAPACITY) < SIMPLE_OTA_BINARY_FILENAME_CAPACITY;
}

static bool resume_u32(const cJSON *item, uint32_t *value)
{
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 1 && item->valuedouble <= UINT32_MAX)) return false;
    *value = (uint32_t)item->valuedouble;
    return item->valuedouble == (double)*value;
}

/* SDK 通信任务调用：不保存 cJSON 指针、不操作 Flash、不等待 OTA mutex。无回复消息；
 * “收到有效固件首块”才是恢复生效的确认，SDK send 返回成功不能替代它。 */
static cJSON *resume_request_cb(cJSON *payload, uint32_t seq)
{
    (void)seq;
    if (!s_initialized || !cJSON_IsObject(payload)) return NULL;
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(payload, "File");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(payload, "Version");
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(payload, "DeviceId");
    OtaResumeRequest request = {0};
    if (!cJSON_IsString(file) || !binary_name_valid(file->valuestring) ||
        !cJSON_IsString(version) || version->valuestring == NULL || version->valuestring[0] == '\0' ||
        strnlen(version->valuestring, sizeof(request.version)) >= sizeof(request.version) ||
        !cJSON_IsString(device) || device->valuestring == NULL || device->valuestring[0] == '\0' ||
        strnlen(device->valuestring, sizeof(request.device_id)) >= sizeof(request.device_id) ||
        !resume_u32(cJSON_GetObjectItemCaseSensitive(payload, "Size"), &request.size_bytes) ||
        !resume_u32(cJSON_GetObjectItemCaseSensitive(payload, "RequestId"), &request.request_id)) return NULL;
    strlcpy(request.file_name, file->valuestring, sizeof(request.file_name));
    strlcpy(request.version, version->valuestring, sizeof(request.version));
    strlcpy(request.device_id, device->valuestring, sizeof(request.device_id));
    request.received_tick = xTaskGetTickCount();
    portENTER_CRITICAL(&s_resume_lock);
    if (s_resume_count < LG_MESH_OTA_RESUME_MAILBOX_SIZE) {
        unsigned tail = (s_resume_head + s_resume_count) % LG_MESH_OTA_RESUME_MAILBOX_SIZE;
        s_resume_mailbox[tail] = request;
        ++s_resume_count;
    } else if (s_resume_dropped < UINT32_MAX) {
        ++s_resume_dropped;
    }
    portEXIT_CRITICAL(&s_resume_lock);
    xEventGroupSetBits(s_cmd_events, CMD_EVT_RESUME);
    return NULL;
}

static const esp_mesh_lite_msg_action_t s_resume_actions[] = {
    {LG_MESH_OTA_RESUME_MSG, NULL, resume_request_cb},
    {NULL, NULL, NULL},
};

/* 服务任务调用；与下级请求合并，限频时间包含初次就绪通知和通知提交失败。 */
static esp_err_t notify_ready_children(const char *source)
{
    if (system_restart_is_planned()) {
        ESP_LOGI(TAG, "OTA_RESUME_NOTIFY: source=%s suppressed by planned restart", source);
        return ESP_ERR_INVALID_STATE;
    }
    s_last_resume_tick = xTaskGetTickCount();
    s_last_resume_valid = true;
    esp_err_t err = esp_mesh_lite_ota_notify_child_node_restart();
    ESP_LOGI(TAG, "OTA_RESUME_NOTIFY: level=%u file=%s source=%s result=%s",
             (unsigned)esp_mesh_lite_get_level(), s_registered_file_name, source, esp_err_to_name(err));
    return err;
}

static void poll_resume_requests(void)
{
    bool matched = false;
    unsigned count;
    uint32_t dropped;
    portENTER_CRITICAL(&s_resume_lock);
    count = s_resume_count; /* 每轮只消费进入时已有的请求，避免回调持续补充造成忙循环。 */
    dropped = s_resume_dropped;
    s_resume_dropped = 0;
    portEXIT_CRITICAL(&s_resume_lock);
    if (dropped != 0) ESP_LOGW(TAG, "OTA_RESUME_DROP: mailbox full count=%u", (unsigned)dropped);
    for (unsigned i = 0; i < count; ++i) {
        OtaResumeRequest request;
        portENTER_CRITICAL(&s_resume_lock);
        request = s_resume_mailbox[s_resume_head];
        s_resume_head = (s_resume_head + 1) % LG_MESH_OTA_RESUME_MAILBOX_SIZE;
        --s_resume_count;
        portEXIT_CRITICAL(&s_resume_lock);
        const char *reason = NULL;
        if (xTaskGetTickCount() - request.received_tick >= pdMS_TO_TICKS(LG_MESH_OTA_RESUME_REQUEST_TTL_MS)) {
            reason = "stale";
        } else if (system_restart_is_planned()) {
            reason = "restart_planned";
        } else if (s_root_downloading || s_finalization_pending || s_child_request_active || s_current_supply_pending) {
            reason = "busy";
        } else if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) == pdTRUE) {
            if (s_sdk_inflight || s_session_open || !s_ready_file.valid) reason = "not_ready";
            else if (strcmp(request.file_name, s_ready_file.file_name) != 0 ||
                     strcmp(request.version, s_ready_file.version) != 0 ||
                     request.size_bytes != s_ready_file.size_bytes) reason = "target_mismatch";
            ota_unlock();
        } else reason = "lock_failed";
        if (reason == NULL) matched = true;
        ESP_LOGI(TAG, "OTA_RESUME_CHECK: device=%s request=%u file=%s result=%s",
                 request.device_id, (unsigned)request.request_id, request.file_name, reason ? reason : "ready");
    }
    if (!matched) return;
    /* 限频的请求本轮消费；子节点有独立的有界补发，不保留可能过期的恢复许可。 */
    if (s_last_resume_valid && xTaskGetTickCount() - s_last_resume_tick < pdMS_TO_TICKS(LG_MESH_OTA_RESUME_INTERVAL_MS)) {
        ESP_LOGI(TAG, "OTA_RESUME_COALESCED: notification rate limited");
        return;
    }
    if (system_restart_is_planned()) return;
    /* helper 已记录结果；提交失败仍依靠子节点下一次有限请求，不在本轮忙重试。 */
    (void)notify_ready_children("child_request");
}

static esp_err_t send_resume_request(void)
{
    if (DEVICE_ID == NULL || DEVICE_ID[0] == '\0' ||
        strnlen(DEVICE_ID, OTA_CACHE_ID_CAPACITY) >= OTA_CACHE_ID_CAPACITY) return ESP_ERR_INVALID_ARG;
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_ERR_NO_MEM;
    if (cJSON_AddStringToObject(payload, "File", s_metadata.file) &&
        cJSON_AddStringToObject(payload, "Version", s_metadata.version) &&
        cJSON_AddStringToObject(payload, "DeviceId", DEVICE_ID) &&
        cJSON_AddNumberToObject(payload, "Size", s_firmware_size_bytes) &&
        cJSON_AddNumberToObject(payload, "RequestId", s_resume_request_id)) {
        esp_mesh_lite_msg_config_t config = {.json_msg = {
            .send_msg = LG_MESH_OTA_RESUME_MSG, .expect_msg = NULL,
            /* 0 表示不追加 SDK 重传；次数由本服务任务控制，首块到达后无后台补发。 */
            .max_retry = 0, .req_payload = payload, .resend = esp_mesh_lite_send_msg_to_parent,
        }};
        err = esp_mesh_lite_send_msg(ESP_MESH_LITE_JSON_MSG, &config);
    }
    /* 当前 SDK send_msg 在返回前复制/序列化 payload，所有权仍归本任务。 */
    cJSON_Delete(payload);
    return err;
}

static void poll_first_data(void)
{
    if (IsRoot == 1 || !s_child_request_active || s_child_wait_parent ||
        s_retry_waiting || s_finalization_pending || system_restart_is_planned()) return;
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return;
    bool waiting = s_session_open && s_rx_accepting && !s_rx_first_data &&
                   !s_rx_done && !s_rx_failed && !s_finish_received && receiver_link_current();
    TickType_t started = s_rx_start_tick;
    ota_unlock();
    if (!waiting || (int32_t)(xTaskGetTickCount() - s_resume_request_due) < 0) return;
    if (s_resume_request_count >= LG_MESH_OTA_RESUME_REQUEST_MAX) {
        if (!s_resume_exhausted_logged) {
            ESP_LOGW(TAG, "OTA_FIRST_DATA_WAIT: resume requests exhausted; waiting for SDK terminal, no new transfer");
            s_resume_exhausted_logged = true;
        }
        return;
    }
    ++s_resume_request_count; /* 内存/发送失败也消耗次数，避免异常环境无限补发。 */
    esp_err_t err = send_resume_request();
    s_resume_request_due = xTaskGetTickCount() + pdMS_TO_TICKS(LG_MESH_OTA_RESUME_INTERVAL_MS +
        esp_random() % (LG_MESH_OTA_RESUME_JITTER_MS + 1u));
    ESP_LOGW(TAG, "OTA_FIRST_DATA_WAIT: elapsed_ms=%u request=%u attempt=%u/%u file=%s send=%s",
             (unsigned)((xTaskGetTickCount() - started) * portTICK_PERIOD_MS),
             (unsigned)s_resume_request_id, s_resume_request_count, LG_MESH_OTA_RESUME_REQUEST_MAX,
             s_metadata.file, esp_err_to_name(err));
}

static size_t aligned_file_size(size_t size_bytes)
{
    if (size_bytes == 0 || size_bytes > SIZE_MAX - (LG_MESH_OTA_FILE_ALIGNMENT_BYTES - 1)) {
        return 0;
    }
    return (size_bytes + LG_MESH_OTA_FILE_ALIGNMENT_BYTES - 1) &
           ~(size_t)(LG_MESH_OTA_FILE_ALIGNMENT_BYTES - 1);
}

static bool transfer_range_valid(const esp_mesh_lite_lan_ota_file_transfer_param_t *param,
                                 size_t size_bytes, const esp_partition_t *partition)
{
    size_t padded_bytes = aligned_file_size(size_bytes);
    return param != NULL && partition != NULL && padded_bytes != 0 &&
           padded_bytes <= partition->size && param->filesize > 0 &&
           (size_t)param->filesize == padded_bytes && param->offset <= padded_bytes &&
           param->data_size <= padded_bytes - param->offset;
}

/* 调用者持有 s_ota_mutex；end 已消耗的 handle 必须先清零，禁止再次 abort。 */
static void reset_receiver_locked(void)
{
    s_rx_accepting = false;
    s_session_open = false;
    s_rx_first_data = false;
    if (s_ota_handle != 0) {
        esp_err_t err = esp_ota_abort(s_ota_handle);
        if (err != ESP_OK) ESP_LOGW(TAG, "OTA receive abort failed: %s", esp_err_to_name(err));
        s_ota_handle = 0;
    }
    s_update_partition = NULL;
    s_rx_done = false;
    s_rx_validated = false;
    s_rx_failed = false;
    s_finish_received = false;
}

/* 服务任务调用。取消的是未完成的本轮写入；SDK 仍可能在退出，故不伪造其终态。 */
static void cancel_child_for_parent(bool keep_request)
{
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return;
    /* 完整接收且双重确认均已到达，后续断链不丢弃有效候选。 */
    if (keep_request && s_rx_validated && s_finish_received &&
        s_finish_reason == ESP_MESH_LITE_EVENT_OTA_SUCCESS) {
        ota_unlock();
        return;
    }
    reset_receiver_locked();
    OtaParentState parent = parent_state();
    s_rx_parent_generation = parent.generation;
    ota_unlock();
    s_retry_waiting = false;
    s_child_request_active = keep_request;
    s_child_wait_parent = keep_request;
    s_child_resume_scheduled = false;
    s_drain_warn_tick = xTaskGetTickCount();
    simple_ota_set_state(keep_request ? SIMPLE_OTA_STATE_WAIT_PARENT : SIMPLE_OTA_STATE_FAILED);
    ESP_LOGW(TAG, "OTA_PARENT_CANCEL: keep_request=%d target=%s; receiver closed, waiting for SDK terminal",
             keep_request, s_metadata_valid ? s_metadata.file : "metadata pending");
}

/* 返回 true 时本轮服务处理不得再启动常规重试。普通 GOT_IP 不产生升级任务。 */
static bool poll_child_parent(void)
{
    if (IsRoot == 1 || !s_child_request_active || s_finalization_pending) return false;
    if (system_restart_is_planned()) {
        cancel_child_for_parent(false);
        return true;
    }
    OtaParentState parent = parent_state();
    if ((!parent.online || parent.generation != s_rx_parent_generation) &&
        (!s_child_wait_parent || parent.generation != s_rx_parent_generation)) {
        cancel_child_for_parent(true);
    }
    if (!s_child_wait_parent) return false;
    parent = parent_state(); /* 取消期间也可能再次断开/重连，不能沿用旧在线窗口。 */
    if (!parent.online || xTaskGetTickCount() - parent.online_since < pdMS_TO_TICKS(LG_MESH_OTA_PARENT_STABLE_MS))
        return true;
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return true;
    bool draining = s_sdk_inflight;
    ota_unlock();
    TickType_t now = xTaskGetTickCount();
    if (draining) {
        if (now - s_drain_warn_tick >= pdMS_TO_TICKS(LG_MESH_OTA_DRAIN_WARN_MS)) {
            s_drain_warn_tick = now;
            ESP_LOGW(TAG, "OTA_PARENT_WAIT_SDK: old transfer has no terminal event; new request held safely");
        }
        return true;
    }
    if (!s_child_resume_scheduled) {
        uint32_t jitter_ms = esp_random() % (LG_MESH_OTA_PARENT_JITTER_MS + 1u);
        s_child_resume_due = now + pdMS_TO_TICKS(jitter_ms);
        s_child_resume_scheduled = true;
        ESP_LOGI(TAG, "OTA_PARENT_RESUME_PENDING: stable_ms=%u jitter_ms=%u", LG_MESH_OTA_PARENT_STABLE_MS, (unsigned)jitter_ms);
    }
    if ((int32_t)(now - s_child_resume_due) >= 0) {
        s_child_resume_scheduled = false;
        ESP_LOGI(TAG, "OTA_PARENT_RESTART: file=%s; downloading from beginning", s_metadata_valid ? s_metadata.file : "metadata pending");
        start_attempt();
    }
    return true;
}

/* 库任务调用；仅提供本次校验或启动恢复校验后已登记的文件，不扫描分区头猜测完整性。 */
static esp_err_t provide_file_cb(esp_mesh_lite_lan_ota_file_transfer_param_t *param)
{
    if (param == NULL || param->data == NULL || !binary_name_valid(param->fw_version)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (!s_ready_file.valid || strcmp(param->fw_version, s_ready_file.file_name) != 0) {
        ESP_LOGW(TAG, "BINARY provide rejected: request=%.*s ready=%d",
                 SIMPLE_OTA_BINARY_FILENAME_CAPACITY - 1, param->fw_version, s_ready_file.valid);
        goto done;
    }
    if (!transfer_range_valid(param, s_ready_file.size_bytes, s_ready_file.partition)) {
        err = ESP_ERR_INVALID_SIZE;
        ESP_LOGE(TAG, "BINARY provide range rejected: offset=%u bytes=%u filesize=%d",
                 (unsigned)param->offset, (unsigned)param->data_size, param->filesize);
        goto done;
    }
    /* 每个文件首块记录供数节点，实机据此区别 L2 供数与穿透到根。 */
    if (param->offset == 0) {
        ESP_LOGI(TAG, "OTA_BINARY_PROVIDE level=%u request=%.*s partition=%s bytes=%u",
                 (unsigned)esp_mesh_lite_get_level(), SIMPLE_OTA_BINARY_FILENAME_CAPACITY - 1,
                 param->fw_version, s_ready_file.partition->label, (unsigned)s_ready_file.size_bytes);
    }
    size_t read_bytes = 0;
    if (param->offset < s_ready_file.size_bytes) {
        read_bytes = s_ready_file.size_bytes - param->offset;
        if (read_bytes > param->data_size) read_bytes = param->data_size;
    }
    err = ESP_OK;
    if (read_bytes != 0) {
        err = esp_partition_read(s_ready_file.partition, param->offset, param->data, read_bytes);
    }
    if (err == ESP_OK && param->data_size > read_bytes) {
        memset(param->data + read_bytes, 0xFF, param->data_size - read_bytes);
    }
done:
    ota_unlock();
    return err;
}

/* 库任务调用：惰性 begin，按 offset 接收乱序块；失败或已收尾的会话拒绝迟到写入。 */
static esp_err_t get_file_cb(esp_mesh_lite_lan_ota_file_transfer_param_t *param)
{
    if (param == NULL || param->data == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_session_open || !s_rx_accepting || s_rx_failed) goto done;
    if (!receiver_link_current()) goto done;
    if (!binary_name_valid(param->fw_version) || strcmp(param->fw_version, s_metadata.file) != 0) {
        ESP_LOGE(TAG, "BINARY receive identifier mismatch: request=%.*s expected=%s",
                 SIMPLE_OTA_BINARY_FILENAME_CAPACITY - 1,
                 param->fw_version != NULL ? param->fw_version : "(null)", s_metadata.file);
        err = ESP_ERR_INVALID_ARG;
        goto failed;
    }
    const esp_partition_t *update = s_update_partition;
    if (update == NULL) update = esp_ota_get_next_update_partition(NULL);
    if (!transfer_range_valid(param, s_firmware_size_bytes, update)) {
        err = ESP_ERR_INVALID_SIZE;
        goto failed;
    }
    if (param->data_size == 0) {
        err = ESP_OK;
        goto done;
    }
    if (s_ota_handle == 0) {
        err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &s_ota_handle);
        if (err != ESP_OK) goto failed;
        s_update_partition = update;
        ESP_LOGI(TAG, "BINARY receive begin: level=%u request=%.*s partition=%s bytes=%u",
                 (unsigned)esp_mesh_lite_get_level(), SIMPLE_OTA_BINARY_FILENAME_CAPACITY - 1,
                 param->fw_version, update->label, (unsigned)s_firmware_size_bytes);
    }
    /* 对齐尾部也会落入备用槽，且已验证其范围不超过分区。 */
    if (!receiver_link_current()) {
        err = ESP_ERR_INVALID_STATE;
        goto failed;
    }
    err = esp_ota_write_with_offset(s_ota_handle, param->data, param->data_size, param->offset);
    if (err != ESP_OK) goto failed;
    if (!s_rx_first_data) {
        s_rx_first_data = true;
        ESP_LOGI(TAG, "OTA_FIRST_DATA: elapsed_ms=%u file=%s offset=%u bytes=%u",
                 (unsigned)((xTaskGetTickCount() - s_rx_start_tick) * portTICK_PERIOD_MS),
                 s_metadata.file, (unsigned)param->offset, (unsigned)param->data_size);
    }
    goto done;
failed:
    s_rx_failed = true;
    s_rx_accepting = false;
    ESP_LOGE(TAG, "BINARY receive failed: offset=%u bytes=%u error=%s",
             (unsigned)param->offset, (unsigned)param->data_size, esp_err_to_name(err));
done:
    ota_unlock();
    return err;
}

/* 库需要此返回值决定传输结果；此处只完成镜像/版本核验。
 * SHA、NVS、set_boot 与登记均在 cmd_task，保证掉电前记录先于启动选择落盘。 */
static esp_err_t get_file_done_cb(void)
{
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_session_open || !s_rx_accepting || s_rx_failed || s_ota_handle == 0) goto done;
    if (!receiver_link_current()) goto done;
    s_rx_accepting = false;
    esp_ota_handle_t handle = s_ota_handle;
    s_ota_handle = 0; /* esp_ota_end 无论成功失败都消耗 handle。 */
    err = esp_ota_end(handle);
    if (err != ESP_OK) goto done;
    esp_app_desc_t desc;
    err = esp_ota_get_partition_description(s_update_partition, &desc);
    if (err != ESP_OK) goto done;
    if (strncmp(desc.version, s_metadata.version, sizeof(desc.version)) != 0) {
        err = ESP_ERR_INVALID_VERSION;
        goto done;
    }
    if (!receiver_link_current()) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    s_rx_validated = true;
done:
    if (s_session_open) {
        s_rx_done = true;
        if (err != ESP_OK) s_rx_failed = true;
    }
    ota_unlock();
    if (err != ESP_OK) ESP_LOGE(TAG, "BINARY local validation failed: %s", esp_err_to_name(err));
    xEventGroupSetBits(s_cmd_events, CMD_EVT_FINISH);
    return err;
}

/* esp_event 任务只复制终态、唤醒服务任务，不能凭 SDK success 宣告本地固件就绪。 */
static void mesh_ota_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (base != ESP_MESH_LITE_EVENT) return;
    if (event_id == ESP_MESH_LITE_EVENT_OTA_START) {
        /* 库可能在 GOT_IP 后自行恢复旧请求；即使应用已关闭接收，也必须等它退出。 */
        if (IsRoot == 0 && xSemaphoreTake(s_ota_mutex, portMAX_DELAY) == pdTRUE) {
            s_sdk_inflight = true;
            ota_unlock();
        }
    } else if (event_id == ESP_MESH_LITE_EVENT_OTA_PROGRESS) {
        const esp_mesh_lite_event_ota_progress_t *progress = event_data;
        if (progress != NULL && progress->percentage % LG_MESH_OTA_PROGRESS_LOG_STEP_PCT == 0) {
            ESP_LOGI(TAG, "BINARY progress=%u%%", progress->percentage);
        }
    } else if (event_id == ESP_MESH_LITE_EVENT_OTA_FINISH) {
        const esp_mesh_lite_event_ota_finish_t *finish = event_data;
        ESP_LOGI(TAG, "OTA_SDK_FINISH: reason=%d pending_timeout=%d",
                 finish ? (int)finish->reason : (int)ESP_MESH_LITE_EVENT_OTA_FAIL,
                 finish != NULL && finish->reason == ESP_MESH_LITE_EVENT_OTA_PENDING_TIMEOUT);
        if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return;
        s_sdk_inflight = false; /* 先排空旧库传输，WAIT_PARENT 下终态不推进业务。 */
        if (s_session_open && !s_finish_received) {
            s_finish_reason = finish != NULL ? finish->reason : ESP_MESH_LITE_EVENT_OTA_FAIL;
            s_finish_tick = xTaskGetTickCount();
            s_finish_received = true;
        }
        ota_unlock();
        xEventGroupSetBits(s_cmd_events, CMD_EVT_FINISH);
    }
}

static esp_err_t mesh_ota_extern_disabled(void)
{
    ESP_LOGW(TAG, "BINARY file unavailable in mesh; external body download disabled");
    /* transmit_file_start 可能忽略 extern 回调的返回值，显式交给服务任务收尾。 */
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    /* 当前库把 LAN 失败交给 extern 前已结束 LAN；禁用外网桩在此归还传输所有权。
     * 仍唤醒正常失败处理，不把这一回调当成下载成功。 */
    s_sdk_inflight = false;
    if (s_session_open) {
        s_finish_received = true;
        s_finish_reason = ESP_MESH_LITE_EVENT_OTA_FAIL;
        s_finish_tick = xTaskGetTickCount();
    }
    ota_unlock();
    xEventGroupSetBits(s_cmd_events, CMD_EVT_FINISH);
    return ESP_FAIL;
}

/* 服务任务调用；源分区在整个候选收尾期间禁止再次擦写。 */
static esp_err_t stage_record(const ota_cache_record_t *record, const esp_partition_t *partition,
                              bool persisted, bool recovered, bool report_completion)
{
    if (record == NULL || partition == NULL || !binary_name_valid(record->file_name) ||
        aligned_file_size(record->image_size_bytes) == 0 ||
        aligned_file_size(record->image_size_bytes) > partition->size) return ESP_ERR_INVALID_SIZE;
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    s_ready_file = (mesh_ready_file_t){.size_bytes = record->image_size_bytes, .partition = partition};
    strlcpy(s_ready_file.file_name, record->file_name, sizeof(s_ready_file.file_name));
    strlcpy(s_ready_file.version, record->version, sizeof(s_ready_file.version));
    strlcpy(s_ready_file.sha256_hex, record->sha256_hex, sizeof(s_ready_file.sha256_hex));
    ota_unlock();
    s_candidate_record = *record;
    s_candidate_persisted = persisted;
    s_candidate_recovered = recovered;
    s_candidate_report_completion = report_completion;
    s_finalization_pending = true;
    s_current_supply_pending = false;
    s_child_request_active = false;
    s_child_wait_parent = false;
    s_child_resume_scheduled = false;
    /* 收尾尚未完成，启动恢复时也先进入 RUNNING，再依据当前运行版本确定状态。 */
    simple_ota_set_state(SIMPLE_OTA_STATE_RUNNING);
    return ESP_OK;
}

/* 固化已校验候选的完成事件身份；供数收尾成功后才允许上报。 */
static esp_err_t snapshot_completion(ota_cache_record_t *record, uint32_t origin)
{
    if (DEVICE_ID == NULL || DEVICE_ID[0] == '\0' || AgingNumber == NULL || AgingNumber[0] == '\0' ||
        strlen(DEVICE_ID) >= sizeof(record->device_id) || strlen(AgingNumber) >= sizeof(record->aging_number)) {
        ESP_LOGE(TAG, "Cannot snapshot OTA completion identity");
        return ESP_ERR_INVALID_ARG;
    }
    record->origin = origin;
    record->completed_seq = (int64_t)time(NULL);
    strlcpy(record->device_id, DEVICE_ID, sizeof(record->device_id));
    strlcpy(record->aging_number, AgingNumber, sizeof(record->aging_number));
    return ESP_OK;
}

static esp_err_t stage_candidate(const simple_ota_metadata_t *metadata, size_t size_bytes,
                                  const esp_partition_t *partition)
{
    ota_cache_record_t record;
    esp_err_t err = ota_cache_record_init(&record, partition, metadata, size_bytes);
    if (err == ESP_OK) err = snapshot_completion(&record, OTA_CACHE_ORIGIN_DOWNLOAD);
    if (err != ESP_OK) return err;
    s_bootstrap_pending = false;
    return stage_record(&record, partition, false, false, true);
}

/* 仅 cmd_task 调用。入队成功前保持同一 Seq 与身份，沿用现有 QoS1/断线重试。 */
static void queue_completion_report(int64_t completed_seq, const char *device_id, const char *aging_number)
{
    s_otacp_seq = (time_t)completed_seq;
    strlcpy(s_report_device_id, device_id, sizeof(s_report_device_id));
    strlcpy(s_report_aging_number, aging_number, sizeof(s_report_aging_number));
    s_otacp_pending = true;
    s_otacp_payload_ready = false;
    s_otacp_retry_tick = xTaskGetTickCount();
}

/* cJSON 临时节点全部在本函数释放；最终负载静态保存，不跨任务传递堆指针。
 * 使用 JSON 转义保留 AgingNumber 的字符串类型，重试复用同一 Seq/负载。 */
static bool prepare_otacp_payload(void)
{
    if (s_report_device_id[0] == '\0' || s_report_aging_number[0] == '\0') {
        ESP_LOGE(TAG, "OTACP requires configured DEVICE_ID and AgingNumber");
        return false;
    }
    int len = snprintf(s_otacp_topic, sizeof(s_otacp_topic), "device/%s/event/OTACP", s_report_device_id);
    if (len < 0 || (size_t)len >= sizeof(s_otacp_topic)) {
        ESP_LOGE(TAG, "OTACP topic exceeds buffer; refusing truncation");
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return false;
    bool ok = cJSON_AddNumberToObject(root, "Seq", (double)s_otacp_seq) != NULL;
    cJSON *data = ok ? cJSON_AddObjectToObject(root, "Data") : NULL;
    ok = data != NULL && cJSON_AddNumberToObject(data, "IsComplete", 1) != NULL &&
              cJSON_AddStringToObject(data, "AgingNumber", s_report_aging_number) != NULL &&
              cJSON_PrintPreallocated(root, s_otacp_payload, sizeof(s_otacp_payload), false);
    cJSON_Delete(root);
    return ok;
}

/* 服务任务每秒轮询；本次下载/同版本供数就绪后置 pending，启动恢复不补报。 */
static void poll_otacp_report(void)
{
    if (!s_otacp_pending || (int32_t)(xTaskGetTickCount() - s_otacp_retry_tick) < 0) return;
    s_otacp_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(LG_MESH_OTA_OTACP_RETRY_MS);
    if (!s_otacp_payload_ready) {
        s_otacp_payload_ready = prepare_otacp_payload();
        if (!s_otacp_payload_ready) {
            ESP_LOGW(TAG, "OTACP payload preparation failed; retry later");
            return;
        }
    }
    if (!app_mqtt_is_connected()) return;
    int msg_id = app_mqtt_publish(s_otacp_topic, s_otacp_payload, NULL);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "OTACP enqueue rejected: %d; retry later", msg_id);
        return;
    }
    s_otacp_pending = false;
    ESP_LOGI(TAG, "OTACP enqueued: topic=%s seq=%lld msg_id=%d", s_otacp_topic,
             (long long)s_otacp_seq, msg_id);
}

/* cmd_task 调用；登记/恢复失败仅重试收尾，不重新下载或擦除已经有效的固件。 */
static esp_err_t finalize_candidate(void)
{
    mesh_ready_file_t file;
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    file = s_ready_file;
    ota_unlock();
    if (!s_finalization_pending || file.partition == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t err;
    if (file.sha256_hex[0] == '\0') {
        err = ota_cache_hash_file(file.partition, file.size_bytes, file.sha256_hex);
        if (err != ESP_OK) return err;
        if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
        s_ready_file = file; /* 登记/恢复失败时复用 SHA，仍保持 valid=false。 */
        ota_unlock();
    }
    char running_version[SIMPLE_OTA_VERSION_MAX_LEN];
    err = simple_ota_get_running_version(running_version, sizeof(running_version));
    if (err != ESP_OK) return err;
    int comparison;
    err = simple_ota_compare_versions(file.version, running_version, &comparison);
    if (err != ESP_OK || comparison < 0) return ESP_ERR_INVALID_VERSION;
    if (!s_candidate_persisted) {
        strlcpy(s_candidate_record.sha256_hex, file.sha256_hex, sizeof(s_candidate_record.sha256_hex));
        err = ota_cache_store(&s_candidate_record);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA cache commit failed; boot selection withheld: %s", esp_err_to_name(err));
            return err;
        }
        s_candidate_persisted = true;
    }
    bool pending_restart = s_candidate_record.origin == OTA_CACHE_ORIGIN_DOWNLOAD && comparison > 0;
    if (pending_restart) {
        const esp_partition_t *boot = esp_ota_get_boot_partition();
        if (boot == NULL || boot->address != file.partition->address) {
            err = esp_ota_set_boot_partition(file.partition);
            if (err != ESP_OK) return err;
        }
    }
    simple_ota_set_state(pending_restart ? SIMPLE_OTA_STATE_PENDING_RESTART : SIMPLE_OTA_STATE_IDLE);
    memset(s_registered_file_name, 0, sizeof(s_registered_file_name));
    memcpy(s_registered_file_name, file.file_name, strlen(file.file_name));
    err = esp_mesh_lite_lan_ota_set_file_name(s_registered_file_name);
    if (err != ESP_OK) return err;
    /* 先允许读取完整镜像，再通知下级。通知失败仍保留供数，不提前输出 READY/OTACP。 */
    file.valid = true;
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    s_ready_file = file;
    ota_unlock();
    /* 此 restart 仅恢复下级 OTA 请求，绝不是 esp_restart。 */
    err = notify_ready_children("supply_ready");
    if (err != ESP_OK) return err;
    s_finalization_pending = false;
    s_retry_waiting = false;
    ESP_LOGI(TAG, "OTA_BINARY_READY level=%u running=%s target=%s file=%s partition=%s bytes=%u sha256=%s",
             (unsigned)esp_mesh_lite_get_level(), running_version, file.version, file.file_name,
             file.partition->label, (unsigned)file.size_bytes, file.sha256_hex);
    if (s_candidate_recovered) {
        ESP_LOGI(TAG, "OTA_CACHE_RESTORED source=%s file=%s",
                 file.partition->address == esp_ota_get_running_partition()->address ? "running" : "staged",
                 file.file_name);
    }
    if (s_candidate_report_completion && s_candidate_record.origin != OTA_CACHE_ORIGIN_BOOTSTRAP) {
        queue_completion_report(s_candidate_record.completed_seq, s_candidate_record.device_id,
                                s_candidate_record.aging_number);
    }
    s_candidate_report_completion = false;
    return ESP_OK;
}

static void schedule_retry(void)
{
    if (s_retry_waiting) return;
    const char *stage = s_finalization_pending ? "register/resume" :
                        s_current_supply_pending ? "prepare_supply" : "download";
    if (s_retry_count >= LG_MESH_OTA_RETRY_MAX) {
        s_child_request_active = false;
        ESP_LOGE(TAG, "BINARY retry exhausted; stage=%s waiting for new command",
                 stage);
        return;
    }
    ++s_retry_count;
    s_retry_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(LG_MESH_OTA_RETRY_INTERVAL_MS);
    s_retry_waiting = true;
    ESP_LOGW(TAG, "BINARY retry %u/%u in %u ms, stage=%s", s_retry_count, LG_MESH_OTA_RETRY_MAX,
             (unsigned)LG_MESH_OTA_RETRY_INTERVAL_MS, stage);
}

static void finish_candidate(void)
{
    esp_err_t err = finalize_candidate();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BINARY finalization failed: %s; no READY", esp_err_to_name(err));
        schedule_retry();
    }
}

/* 启动恢复只供数、不补报；target 非空表示本次同版本命令，需要新的完成通知。 */
static esp_err_t restore_cached_supply(const simple_ota_metadata_t *target)
{
    ota_cache_record_t record;
    esp_err_t err = ota_cache_load(&record);
    if (err == ESP_ERR_NOT_FOUND) return err;
    if (err != ESP_OK && err != ESP_ERR_INVALID_RESPONSE) return err;
    /* 同版本命令只接受本次检查的精确文件；其他有效记录留到新候选提交时替换。 */
    if (err == ESP_OK && target != NULL &&
        (strcmp(record.version, target->version) != 0 || strcmp(record.file_name, target->file) != 0)) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_partition_t *partition = NULL;
    if (err == ESP_OK) {
        err = ota_cache_verify(&record, &partition);
        if (err == ESP_OK) {
            char running[SIMPLE_OTA_VERSION_MAX_LEN];
            int comparison;
            err = simple_ota_get_running_version(running, sizeof(running));
            if (err == ESP_OK) err = simple_ota_compare_versions(record.version, running, &comparison);
            const esp_partition_t *current = esp_ota_get_running_partition();
            if (err == ESP_OK && (comparison < 0 || current == NULL ||
                (record.origin == OTA_CACHE_ORIGIN_CURRENT && comparison != 0) ||
                (record.origin == OTA_CACHE_ORIGIN_BOOTSTRAP && partition->address != current->address))) {
                err = ESP_ERR_INVALID_VERSION;
            }
        }
    }
    if (err == ESP_OK) {
        if (target != NULL) err = snapshot_completion(&record, OTA_CACHE_ORIGIN_CURRENT);
        if (err == ESP_OK) err = stage_record(&record, partition, target == NULL, true, target != NULL);
        if (err == ESP_OK) finish_candidate();
        return err;
    }
    ESP_LOGW(TAG, "OTA cache cannot be restored: %s; supply withheld", esp_err_to_name(err));
    /* 读 flash 等暂态错误保留原记录，下一条命令可重试；不擅自覆盖未知有效缓存。 */
    if (err == ESP_FAIL || err == ESP_ERR_NO_MEM) return err;
    err = ota_cache_invalidate();
    return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
}

/* 首次直烧无记录时：只获取元数据/响应头，核实当前镜像真实长度，不下载 BIN 正文。 */
static esp_err_t bind_running_supply(const simple_ota_metadata_t *metadata, bool report_completion)
{
    char running[SIMPLE_OTA_VERSION_MAX_LEN];
    esp_err_t err = simple_ota_get_running_version(running, sizeof(running));
    if (err != ESP_OK) return err;
    if (!binary_name_valid(metadata->file) || strcmp(metadata->version, running) != 0) {
        ESP_LOGI(TAG, "OTA cache bootstrap skipped: server version/file does not identify running image");
        return ESP_ERR_INVALID_VERSION;
    }
    size_t server_bytes;
    err = simple_ota_probe_firmware_size(metadata->file, &server_bytes);
    if (err != ESP_OK) return err;
    const esp_partition_t *partition = esp_ota_get_running_partition();
    uint32_t image_bytes;
    err = ota_cache_image_size(partition, metadata->version, &image_bytes);
    if (err != ESP_OK) return err;
    if (server_bytes != image_bytes) {
        ESP_LOGE(TAG, "OTA cache bootstrap size mismatch: server=%u image=%u", (unsigned)server_bytes, (unsigned)image_bytes);
        return ESP_ERR_INVALID_SIZE;
    }
    ota_cache_record_t record;
    err = ota_cache_record_init(&record, partition, metadata, server_bytes);
    if (err == ESP_OK) err = ota_cache_hash_file(partition, server_bytes, record.sha256_hex);
    if (err == ESP_OK && report_completion) err = snapshot_completion(&record, OTA_CACHE_ORIGIN_CURRENT);
    if (err == ESP_OK) err = stage_record(&record, partition, false, true, report_completion);
    if (err == ESP_OK) finish_candidate();
    return err;
}

static esp_err_t bootstrap_running_supply(void)
{
    simple_ota_metadata_t metadata;
    esp_err_t err = simple_ota_fetch_metadata(&metadata);
    return err == ESP_OK ? bind_running_supply(&metadata, false) : err;
}

/* 同版本命令复用供数收尾；失败有限重试，始终不下载 BIN、不提前排入 OTACP。 */
static void prepare_current_supply(void)
{
    simple_ota_set_state(SIMPLE_OTA_STATE_RUNNING);
    esp_err_t err = restore_cached_supply(&s_metadata);
    if (err == ESP_ERR_NOT_FOUND) err = bind_running_supply(&s_metadata, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Current-version supply unavailable: %s; completion withheld", esp_err_to_name(err));
        simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
        schedule_retry();
    }
}

static void request_supply_recovery(void)
{
    s_bootstrap_pending = false;
    esp_err_t err = restore_cached_supply(NULL);
    if (err == ESP_ERR_NOT_FOUND) {
        s_bootstrap_pending = true;
        s_bootstrap_attempts = 0;
        s_bootstrap_retry_tick = xTaskGetTickCount();
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA supply recovery unavailable: %s", esp_err_to_name(err));
    }
}

/* done 和 FINISH 可能以不同顺序唤醒任务；二者都成功才进入登记流程。 */
static void process_child_finish(void)
{
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return;
    if (!s_session_open || !s_finish_received) {
        ota_unlock();
        return;
    }
    if (s_finish_reason == ESP_MESH_LITE_EVENT_OTA_SUCCESS && !s_rx_done &&
        xTaskGetTickCount() - s_finish_tick < pdMS_TO_TICKS(LG_MESH_OTA_DONE_WAIT_MS)) {
        ota_unlock();
        if (!s_finish_wait_logged) ESP_LOGW(TAG, "SDK success received; waiting for local validation");
        s_finish_wait_logged = true;
        return;
    }
    bool valid = s_finish_reason == ESP_MESH_LITE_EVENT_OTA_SUCCESS && s_rx_validated && !s_rx_failed;
    const esp_partition_t *partition = s_update_partition;
    int reason = (int)s_finish_reason;
    reset_receiver_locked();
    ota_unlock();
    if (valid) {
        esp_err_t err = stage_candidate(&s_metadata, s_firmware_size_bytes, partition);
        if (err == ESP_OK) {
            finish_candidate();
            return;
        }
        ESP_LOGE(TAG, "BINARY candidate rejected: %s", esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "BINARY transfer not locally validated, SDK reason=%d; no READY", reason);
    }
    simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
    schedule_retry();
}

/* 根完成事件只在 dl_task 真正退出下载后处理，超时不并发启动第二次下载。 */
static void process_root_finish(void)
{
    if (!s_root_downloading) return;
    s_root_downloading = false;
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return;
    esp_err_t err = s_download_error;
    ota_unlock();
    simple_ota_download_result_t result;
    if (err == ESP_OK) {
        err = simple_ota_get_download_result(&result);
        if (err == ESP_ERR_NOT_FOUND && simple_ota_get_state() == SIMPLE_OTA_STATE_IDLE) {
            int comparison;
            err = simple_ota_get_version_comparison(&comparison);
            if (err != ESP_OK) goto failed;
            if (comparison == 0) {
                err = simple_ota_get_checked_metadata(&s_metadata);
                if (err != ESP_OK) goto failed;
                s_bootstrap_pending = false;
                s_current_supply_pending = true;
                prepare_current_supply();
                return;
            }
            esp_err_t resume_err = esp_mesh_lite_ota_notify_child_node_restart();
            if (resume_err != ESP_OK) ESP_LOGW(TAG, "Root skip OTA: resume failed: %s", esp_err_to_name(resume_err));
            ESP_LOGI(TAG, "Root has no newer firmware; checking existing supply cache");
            request_supply_recovery();
            return;
        }
    }
    if (err == ESP_OK) err = stage_candidate(&result.metadata, result.size_bytes, result.partition);
    if (err == ESP_OK) {
        finish_candidate();
        return;
    }
failed:
    ESP_LOGE(TAG, "Root BINARY download/stage failed: %s", esp_err_to_name(err));
    esp_err_t resume_err = esp_mesh_lite_ota_notify_child_node_restart();
    if (resume_err != ESP_OK) ESP_LOGW(TAG, "Root failure: resume failed: %s", esp_err_to_name(resume_err));
    simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
    schedule_retry();
}

static void handle_root_path(void)
{
    esp_err_t err = esp_mesh_lite_ota_notify_child_node_pause();
    if (err != ESP_OK) ESP_LOGW(TAG, "Root pause failed: %s; unready callbacks still reject data", esp_err_to_name(err));
    s_root_downloading = true;
    s_root_timeout_logged = false;
    s_root_start_tick = xTaskGetTickCount();
    xEventGroupSetBits(s_dl_events, DL_EVT_START);
}

static void handle_child_path(void)
{
    esp_err_t err;
    OtaParentState parent = parent_state();
    /* 从稳定窗口排程到实际入口之间仍可换父；保留旧代次直到此处复核。 */
    if (s_child_wait_parent && (parent.generation != s_rx_parent_generation || !parent.online ||
        xTaskGetTickCount() - parent.online_since < pdMS_TO_TICKS(LG_MESH_OTA_PARENT_STABLE_MS))) {
        cancel_child_for_parent(!system_restart_is_planned());
        return;
    }
    s_child_wait_parent = false;
    s_rx_parent_generation = parent.generation;
    if (!parent.online || system_restart_is_planned()) {
        cancel_child_for_parent(!system_restart_is_planned());
        return;
    }
    if (!s_metadata_valid) {
        err = simple_ota_fetch_metadata(&s_metadata);
        if (err != ESP_OK) goto failed;
        if (!receiver_link_current()) {
            cancel_child_for_parent(!system_restart_is_planned());
            return;
        }
        if (!binary_name_valid(s_metadata.file)) {
            err = ESP_ERR_INVALID_SIZE;
            ESP_LOGE(TAG, "BINARY file name must be 1..31 bytes; refusing truncation");
            goto failed;
        }
        char running[SIMPLE_OTA_VERSION_MAX_LEN];
        int comparison;
        err = simple_ota_get_running_version(running, sizeof(running));
        if (err == ESP_OK) err = simple_ota_compare_versions(s_metadata.version, running, &comparison);
        if (err != ESP_OK) goto failed;
        if (comparison <= 0) {
            if (comparison == 0) {
                s_child_request_active = false;
                s_bootstrap_pending = false;
                s_current_supply_pending = true;
                prepare_current_supply();
                return;
            }
            s_child_request_active = false;
            simple_ota_set_state(SIMPLE_OTA_STATE_IDLE);
            ESP_LOGI(TAG, "Child has no newer firmware; checking existing supply cache");
            request_supply_recovery();
            return;
        }
        err = simple_ota_probe_firmware_size(s_metadata.file, &s_firmware_size_bytes);
        if (err != ESP_OK) goto failed;
        const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
        size_t padded_bytes = aligned_file_size(s_firmware_size_bytes);
        if (partition == NULL || padded_bytes == 0 || padded_bytes > partition->size) {
            err = ESP_ERR_INVALID_SIZE;
            goto failed;
        }
        s_metadata_valid = true;
    }
    if (!receiver_link_current()) {
        cancel_child_for_parent(!system_restart_is_planned());
        return;
    }
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) {
        err = ESP_FAIL;
        goto failed;
    }
    reset_receiver_locked();
    if (s_sdk_inflight) {
        ota_unlock();
        cancel_child_for_parent(true);
        return;
    }
    /* 同一互斥保护在途供数与擦写准入；持久记录失效失败时不启动库传输。 */
    s_ready_file.valid = false;
    err = ota_cache_before_write(esp_ota_get_next_update_partition(NULL));
    if (err != ESP_OK) {
        ota_unlock();
        goto failed;
    }
    s_session_open = true;
    s_rx_accepting = true;
    s_sdk_inflight = true;
    s_rx_start_tick = xTaskGetTickCount();
    s_resume_request_due = s_rx_start_tick + pdMS_TO_TICKS(LG_MESH_OTA_FIRST_DATA_WAIT_MS);
    s_resume_request_count = 0;
    s_resume_exhausted_logged = false;
    if (++s_resume_request_id == 0) ++s_resume_request_id;
    s_finish_wait_logged = false;
    ota_unlock();
    esp_mesh_lite_file_transmit_config_t cfg = {
        .type = ESP_MESH_LITE_OTA_TRANSMIT_BINARY,
        .size = s_firmware_size_bytes,
        .extern_url_ota_cb = mesh_ota_extern_disabled,
    };
    strlcpy(cfg.file_name, s_metadata.file, sizeof(cfg.file_name));
    ESP_LOGI(TAG, "OTA_BINARY_REQUEST level=%u file=%s target=%s bytes=%u",
             (unsigned)esp_mesh_lite_get_level(), cfg.file_name, s_metadata.version, (unsigned)cfg.size);
    err = esp_mesh_lite_transmit_file_start(&cfg);
    if (err == ESP_OK) return;
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) == pdTRUE) {
        s_sdk_inflight = false; /* API 拒绝启动，未拥有新的库传输。 */
        reset_receiver_locked();
        ota_unlock();
    }
failed:
    if (!receiver_link_current()) {
        cancel_child_for_parent(!system_restart_is_planned());
        return;
    }
    ESP_LOGE(TAG, "Child BINARY request failed: %s", esp_err_to_name(err));
    simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
    schedule_retry();
}

static void start_attempt(void)
{
    s_current_supply_pending = false;
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return;
    s_ready_file = (mesh_ready_file_t){0};
    reset_receiver_locked();
    ota_unlock();
    simple_ota_set_state(SIMPLE_OTA_STATE_RUNNING);
    /* 配置角色决定外网下载权限，失去父节点的子不能因 level=0 被当成根。 */
    if (IsRoot == 1) handle_root_path();
    else handle_child_path();
}

/* 已暂存但未重启时仅确认目标身份，不进入 begin/write 或替换供数记录。
 * MQTT 命令本身不含版本/文件名，故须查询 update.json 才能区分原文件与新版本。 */
static void confirm_staged_command(void)
{
    simple_ota_metadata_t target;
    esp_err_t err = simple_ota_fetch_metadata(&target);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA 目标查询失败，保留待重启固件，本次不确认完成: %s", esp_err_to_name(err));
        return;
    }
    if (system_restart_is_planned()) return;
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return;
    bool same = s_ready_file.valid && s_candidate_persisted &&
                strcmp(target.version, s_ready_file.version) == 0 &&
                strcmp(target.file, s_ready_file.file_name) == 0;
    ota_unlock();
    if (!same) {
        ESP_LOGW(TAG, "OTA 目标与暂存固件不同，请先重启再升级: requested=%s file=%s staged=%s",
                 target.version, target.file, s_candidate_record.version);
        return;
    }
    ESP_LOGI(TAG, "OTA 同一固件已完成，重新确认 OTACP，保留待重启镜像");
    /* 明确命令触发的再次确认，复用原完成事实；启动恢复本身仍不自动补报。 */
    queue_completion_report(s_candidate_record.completed_seq, s_candidate_record.device_id,
                            s_candidate_record.aging_number);
}

/* 服务任务调用。完成/无需升级/失败耗尽都释放接收标志；重试、WAIT_PARENT 和旧 SDK
 * 会话未结束时保持 0。不以 MQTT 是否在线作为本地下载是否完成的条件。 */
static void finish_command_if_idle(void)
{
    if (!s_command_observed || s_root_downloading || s_retry_waiting ||
        s_child_request_active || s_child_wait_parent || s_request_deferred) return;
    bool exhausted = s_finalization_pending && s_retry_count >= LG_MESH_OTA_RETRY_MAX;
    if (s_finalization_pending && !exhausted) return;
    simple_ota_state_t state = simple_ota_get_state();
    if (!exhausted && state != SIMPLE_OTA_STATE_IDLE && state != SIMPLE_OTA_STATE_PENDING_RESTART &&
        state != SIMPLE_OTA_STATE_FAILED) return;
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return;
    bool active = s_sdk_inflight || s_session_open;
    ota_unlock();
    if (active) return;
    s_command_observed = false;
    atomic_store(&s_command_available, true);
    ESP_LOGI(TAG, "OTA 命令处理结束，接收标志恢复为 1");
}

/* 单轮服务处理，所有重试调度均不阻塞，保证迟到完成事件能及时收尾。 */
static void mesh_ota_service_poll(EventBits_t bits)
{
    if (s_recovery_pending) {
        s_recovery_pending = false;
        request_supply_recovery();
    }
    if (bits & CMD_EVT_DOWNLOAD_DONE) process_root_finish();
    bool parent_wait = poll_child_parent();
    process_child_finish();
    if (s_root_downloading && !s_root_timeout_logged &&
        xTaskGetTickCount() - s_root_start_tick >= pdMS_TO_TICKS(LG_MESH_OTA_ROOT_DL_TIMEOUT_MS)) {
        s_root_timeout_logged = true;
        ESP_LOGE(TAG, "Root download exceeded timeout; waiting for worker exit, concurrent retry prohibited");
    }
    if (bits & CMD_EVT_REQUEST) {
        s_command_observed = true;
        atomic_store(&s_command_available, false);
        simple_ota_state_t state = simple_ota_get_state();
        if (IsRoot == 0 && (s_child_request_active || system_restart_is_planned())) {
            ESP_LOGI(TAG, "BINARY command coalesced: child task active or restart planned");
        } else if (s_otacp_pending) {
            /* 单条通知持久记录不能被下一次下载覆盖；保留一个合并请求到入队成功。 */
            s_request_deferred = true;
        } else if (s_finalization_pending) {
            /* 启动恢复收尾本身不报完成；新收到的 OTA 命令在恢复后独立处理。 */
            if (!s_candidate_report_completion) s_request_deferred = true;
            if (!s_retry_waiting) {
                s_retry_count = 0;
                finish_candidate();
            } else s_request_deferred = true;
        } else if (s_root_downloading || state == SIMPLE_OTA_STATE_RUNNING || s_retry_waiting) {
            ESP_LOGW(TAG, "BINARY command ignored: current attempt/retry still active");
        } else if (state == SIMPLE_OTA_STATE_PENDING_RESTART) {
            confirm_staged_command();
        } else {
            s_retry_count = 0;
            s_metadata_valid = false;
            s_child_request_active = IsRoot == 0;
            start_attempt();
        }
    }
    if (!parent_wait && s_retry_waiting && (int32_t)(xTaskGetTickCount() - s_retry_deadline) >= 0) {
        s_retry_waiting = false;
        if (s_finalization_pending) finish_candidate();
        else if (s_current_supply_pending) prepare_current_supply();
        else if (!s_root_downloading) start_attempt();
    }
    if (s_bootstrap_pending && !s_otacp_pending && !s_root_downloading && !s_finalization_pending && !s_retry_waiting &&
        simple_ota_get_state() == SIMPLE_OTA_STATE_IDLE && app_mqtt_is_connected() &&
        (int32_t)(xTaskGetTickCount() - s_bootstrap_retry_tick) >= 0) {
        esp_err_t err = bootstrap_running_supply();
        ++s_bootstrap_attempts;
        if (err == ESP_OK || err == ESP_ERR_INVALID_VERSION || s_bootstrap_attempts > LG_MESH_OTA_RETRY_MAX) {
            s_bootstrap_pending = false;
        } else {
            s_bootstrap_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(LG_MESH_OTA_RETRY_INTERVAL_MS);
        }
        if (err != ESP_OK) ESP_LOGW(TAG, "OTA cache bootstrap unavailable: %s", esp_err_to_name(err));
    }
    poll_resume_requests();
    poll_first_data();
    poll_otacp_report();
    if (s_request_deferred && !s_otacp_pending && !s_finalization_pending && !s_root_downloading && !s_retry_waiting) {
        s_request_deferred = false;
        xEventGroupSetBits(s_cmd_events, CMD_EVT_REQUEST);
        return; /* 合并命令仍已排队，消费前不能提前把接收标志恢复为 1。 */
    }
    finish_command_if_idle();
}

static void mesh_ota_cmd_task(void *arg)
{
    (void)arg;
    mesh_ota_service_poll(0); /* 先本地恢复，再消费后续 OTA 命令。 */
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_cmd_events,
            CMD_EVT_REQUEST | CMD_EVT_FINISH | CMD_EVT_DOWNLOAD_DONE | CMD_EVT_PARENT | CMD_EVT_RESUME,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(LG_MESH_OTA_SERVICE_POLL_MS));
        mesh_ota_service_poll(bits);
    }
}

static void mesh_ota_dl_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_dl_events, DL_EVT_START, pdTRUE, pdFALSE, portMAX_DELAY);
        esp_err_t err = simple_ota_check_and_update_default();
        if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) == pdTRUE) {
            s_download_error = err;
            ota_unlock();
        }
        xEventGroupSetBits(s_cmd_events, CMD_EVT_DOWNLOAD_DONE);
    }
}
#endif /* CONFIG_ESP_MESH_LITE_OTA_ENABLE */

#ifndef CONFIG_ESP_MESH_LITE_OTA_ENABLE
void mesh_ota_parent_link_changed(bool has_ip)
{
    (void)has_ip;
}
#endif

esp_err_t mesh_ota_init(void)
{
#ifndef CONFIG_ESP_MESH_LITE_OTA_ENABLE
    ESP_LOGE(TAG, "CONFIG_ESP_MESH_LITE_OTA_ENABLE not set; BINARY unavailable");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_initialized) return ESP_OK;
    s_cmd_events = xEventGroupCreateStatic(&s_cmd_events_buf);
    s_dl_events = xEventGroupCreateStatic(&s_dl_events_buf);
    s_ota_mutex = xSemaphoreCreateMutexStatic(&s_ota_mutex_buf);
    if (s_cmd_events == NULL || s_dl_events == NULL || s_ota_mutex == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_mesh_lite_msg_action_list_register(s_resume_actions);
    if (err != ESP_OK) return err;
    static esp_mesh_lite_lan_ota_file_transfer_cb_t callbacks = {
        .provide_file_cb = provide_file_cb, .get_file_cb = get_file_cb, .get_file_done = get_file_done_cb,
    };
    esp_mesh_lite_ota_register_file_transfer_cb(&callbacks);
    err = esp_event_handler_register(ESP_MESH_LITE_EVENT, ESP_EVENT_ANY_ID,
                                               mesh_ota_event_handler, NULL);
    if (err != ESP_OK) {
        esp_err_t unregister_err = esp_mesh_lite_msg_action_list_unregister(s_resume_actions);
        if (unregister_err != ESP_OK) ESP_LOGW(TAG, "OTA resume action cleanup failed: %s", esp_err_to_name(unregister_err));
        return err;
    }
    s_recovery_pending = true;
    /* 沿用既有任务，固定 CPU1 与 task_config.h 中的栈/优先级。 */
    if (xTaskCreatePinnedToCore(mesh_ota_cmd_task, "mesh_ota_cmd", LG_STACK_MESH_OTA,
            NULL, LG_PRIO_MESH_OTA, NULL, LG_APP_CPU_CORE) != pdPASS) return ESP_ERR_NO_MEM;
    if (IsRoot == 1 && xTaskCreatePinnedToCore(mesh_ota_dl_task, "mesh_ota_dl", LG_STACK_MESH_OTA_DL,
            NULL, LG_PRIO_MESH_OTA_DL, NULL, LG_APP_CPU_CORE) != pdPASS) return ESP_ERR_NO_MEM;
    s_initialized = true;
    ESP_LOGI(TAG, "BINARY staging initialized; no automatic restart, OTACP enabled, retry=%u x 90s", LG_MESH_OTA_RETRY_MAX);
    return ESP_OK;
#endif
}

void mesh_ota_handle_mqtt_command(void)
{
#ifndef CONFIG_ESP_MESH_LITE_OTA_ENABLE
    ESP_LOGE(TAG, "Mesh OTA not compiled in; ignoring command");
#else
    if (!s_initialized) {
        ESP_LOGE(TAG, "BINARY not initialized; ignoring command");
        return;
    }
    if (system_restart_is_planned()) {
        ESP_LOGI(TAG, "设备已进入计划重启，忽略 OTA 命令");
        return;
    }
    if (!atomic_exchange(&s_command_available, false)) {
        ESP_LOGI(TAG, "OTA 命令已触发，当前任务尚未结束，请勿重复发送");
        return;
    }
    xEventGroupSetBits(s_cmd_events, CMD_EVT_REQUEST);
#endif
}

simple_ota_state_t mesh_ota_get_state(void)
{
    return simple_ota_get_state();
}
