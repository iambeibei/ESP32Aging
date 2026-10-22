/**
 * @file mesh_ota.c
 * @brief Mesh-Lite BINARY 分发验证：旧固件运行期间逐层提供已校验的新镜像。
 *
 * mqtt_rx 只通知 cmd_task；根由 dl_task 下载一次，子由 BINARY LAN 回调写入。
 * done 回调完成镜像/版本校验及 set_boot，cmd_task 计算文件 SHA-256、登记文件名、
 * 恢复下级 OTA 后输出 OTA_BINARY_READY，并经本机 event/OTACP 上报完成。不自动重启。
 *
 * 库回调/事件任务与 cmd_task 共享接收会话和供数记录，均经 s_ota_mutex 保护；
 * 文件供数期间持锁读取，避免失败清理/重试擦除与读取同一分区并发。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
#include "mbedtls/sha256.h"

#include "mesh_ota.h"
#include "ConfigData.h"
#include "mqtt_app.h"
#include "task_config.h"

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
#define LG_MESH_OTA_HASH_BUFFER_BYTES 2048
#define LG_MESH_OTA_SERVICE_POLL_MS 1000
#define LG_MESH_OTA_OTACP_RETRY_MS 5000
#define LG_MESH_OTA_OTACP_TOPIC_BYTES 64
#define LG_MESH_OTA_OTACP_PAYLOAD_BYTES 256

#define CMD_EVT_REQUEST BIT0
#define CMD_EVT_FINISH BIT1
#define CMD_EVT_DOWNLOAD_DONE BIT2
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
static bool s_initialized;

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

/* 完成通知仅由 cmd_task 访问；入队失败保留同一事件，成功后交给 SDK QoS1 重传。 */
static bool s_otacp_pending, s_otacp_payload_ready;
static time_t s_otacp_seq;
static TickType_t s_otacp_retry_tick;
static char s_otacp_topic[LG_MESH_OTA_OTACP_TOPIC_BYTES];
static char s_otacp_payload[LG_MESH_OTA_OTACP_PAYLOAD_BYTES];

static void schedule_retry(void);

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

/* 库任务调用；仅提供本次完成校验且已登记的文件，不扫描 app 分区头猜测完整性。 */
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
    err = esp_ota_write_with_offset(s_ota_handle, param->data, param->data_size, param->offset);
    if (err != ESP_OK) goto failed;
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

/* 库需要此返回值决定传输结果，故镜像校验/版本核验/set_boot 在回调中串行完成。
 * SHA-256、文件登记和恢复下级均延后至 cmd_task，绝不在这里重启。 */
static esp_err_t get_file_done_cb(void)
{
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_session_open || !s_rx_accepting || s_rx_failed || s_ota_handle == 0) goto done;
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
    err = esp_ota_set_boot_partition(s_update_partition);
    if (err == ESP_OK) s_rx_validated = true;
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
    if (event_id == ESP_MESH_LITE_EVENT_OTA_PROGRESS) {
        const esp_mesh_lite_event_ota_progress_t *progress = event_data;
        if (progress != NULL && progress->percentage % LG_MESH_OTA_PROGRESS_LOG_STEP_PCT == 0) {
            ESP_LOGI(TAG, "BINARY progress=%u%%", progress->percentage);
        }
    } else if (event_id == ESP_MESH_LITE_EVENT_OTA_FINISH) {
        const esp_mesh_lite_event_ota_finish_t *finish = event_data;
        if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return;
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
    if (s_session_open) {
        s_finish_received = true;
        s_finish_reason = ESP_MESH_LITE_EVENT_OTA_FAIL;
        s_finish_tick = xTaskGetTickCount();
    }
    ota_unlock();
    xEventGroupSetBits(s_cmd_events, CMD_EVT_FINISH);
    return ESP_FAIL;
}

/* cmd_task 调用；就绪前无供数读者，可以在锁外分块读 flash，避免长时间占锁。
 * 每块让出 CPU1，SHA 对应原 BIN 的完整字节，排除 Mesh-Lite 的填充段。 */
static esp_err_t hash_file(mesh_ready_file_t *file)
{
    unsigned char digest[32];
    unsigned char buffer[LG_MESH_OTA_HASH_BUFFER_BYTES];
    static const char hex[] = "0123456789abcdef";
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    esp_err_t err = ESP_FAIL;
    if (mbedtls_sha256_starts(&ctx, 0) != 0) goto done;
    for (size_t offset = 0; offset < file->size_bytes;) {
        size_t count = file->size_bytes - offset;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        err = esp_partition_read(file->partition, offset, buffer, count);
        if (err != ESP_OK) goto done;
        if (mbedtls_sha256_update(&ctx, buffer, count) != 0) {
            err = ESP_FAIL;
            goto done;
        }
        offset += count;
        vTaskDelay(1);
    }
    err = ESP_FAIL;
    if (mbedtls_sha256_finish(&ctx, digest) != 0) goto done;
    for (size_t i = 0; i < sizeof(digest); ++i) {
        file->sha256_hex[2 * i] = hex[digest[i] >> 4];
        file->sha256_hex[2 * i + 1] = hex[digest[i] & 0x0F];
    }
    file->sha256_hex[64] = '\0';
    err = ESP_OK;
done:
    mbedtls_sha256_free(&ctx);
    return err;
}

/* cmd_task 调用，只接受来自本次本地校验成功路径的分区，不扫描分区头。 */
static esp_err_t stage_candidate(const simple_ota_metadata_t *metadata, size_t size_bytes,
                                  const esp_partition_t *partition)
{
    if (metadata == NULL || !binary_name_valid(metadata->file) || partition == NULL ||
        aligned_file_size(size_bytes) == 0 || aligned_file_size(size_bytes) > partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    s_ready_file = (mesh_ready_file_t){.size_bytes = size_bytes, .partition = partition};
    strlcpy(s_ready_file.file_name, metadata->file, sizeof(s_ready_file.file_name));
    strlcpy(s_ready_file.version, metadata->version, sizeof(s_ready_file.version));
    ota_unlock();
    s_finalization_pending = true;
    simple_ota_set_state(SIMPLE_OTA_STATE_PENDING_RESTART);
    return ESP_OK;
}

/* cJSON 临时节点全部在本函数释放；最终负载静态保存，不跨任务传递堆指针。
 * 使用 JSON 转义保留 AgingNumber 的字符串类型，重试复用同一 Seq/负载。 */
static bool prepare_otacp_payload(void)
{
    if (DEVICE_ID == NULL || DEVICE_ID[0] == '\0' || AgingNumber == NULL) {
        ESP_LOGE(TAG, "OTACP requires configured DEVICE_ID and AgingNumber");
        return false;
    }
    int len = snprintf(s_otacp_topic, sizeof(s_otacp_topic), "device/%s/event/OTACP", DEVICE_ID);
    if (len < 0 || (size_t)len >= sizeof(s_otacp_topic)) {
        ESP_LOGE(TAG, "OTACP topic exceeds buffer; refusing truncation");
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return false;
    bool ok = cJSON_AddNumberToObject(root, "Seq", (double)s_otacp_seq) != NULL;
    cJSON *data = ok ? cJSON_AddObjectToObject(root, "Data") : NULL;
    ok = data != NULL && cJSON_AddNumberToObject(data, "IsComplete", 1) != NULL &&
              cJSON_AddStringToObject(data, "AgingNumber", AgingNumber) != NULL &&
              cJSON_PrintPreallocated(root, s_otacp_payload, sizeof(s_otacp_payload), false);
    cJSON_Delete(root);
    return ok;
}

/* 服务任务每秒轮询；仅实际下载并完成就绪登记才置 pending，失败/跳过无通知。 */
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
        err = hash_file(&file);
        if (err != ESP_OK) return err;
        if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
        s_ready_file = file; /* 登记/恢复失败时复用 SHA，仍保持 valid=false。 */
        ota_unlock();
    }
    memset(s_registered_file_name, 0, sizeof(s_registered_file_name));
    memcpy(s_registered_file_name, file.file_name, strlen(file.file_name));
    err = esp_mesh_lite_lan_ota_set_file_name(s_registered_file_name);
    if (err != ESP_OK) return err;
    /* 此 restart 仅恢复下级 OTA 请求，绝不是 esp_restart。根不重启也须解除 pause。 */
    err = esp_mesh_lite_ota_notify_child_node_restart();
    if (err != ESP_OK) return err;
    file.valid = true;
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    s_ready_file = file;
    ota_unlock();
    s_finalization_pending = false;
    s_retry_waiting = false;
    char running_version[SIMPLE_OTA_VERSION_MAX_LEN];
    err = simple_ota_get_running_version(running_version, sizeof(running_version));
    if (err != ESP_OK) strlcpy(running_version, "unknown", sizeof(running_version));
    ESP_LOGI(TAG, "OTA_BINARY_READY level=%u running=%s target=%s file=%s partition=%s bytes=%u sha256=%s",
             (unsigned)esp_mesh_lite_get_level(), running_version, file.version, file.file_name,
             file.partition->label, (unsigned)file.size_bytes, file.sha256_hex);
    s_otacp_seq = time(NULL);
    s_otacp_pending = true;
    s_otacp_payload_ready = false;
    s_otacp_retry_tick = xTaskGetTickCount();
    return ESP_OK;
}

static void schedule_retry(void)
{
    if (s_retry_waiting) return;
    if (s_retry_count >= LG_MESH_OTA_RETRY_MAX) {
        ESP_LOGE(TAG, "BINARY retry exhausted; stage=%s waiting for new command",
                 s_finalization_pending ? "register/resume" : "download");
        return;
    }
    ++s_retry_count;
    s_retry_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(LG_MESH_OTA_RETRY_INTERVAL_MS);
    s_retry_waiting = true;
    ESP_LOGW(TAG, "BINARY retry %u/%u in %u ms, stage=%s", s_retry_count, LG_MESH_OTA_RETRY_MAX,
             (unsigned)LG_MESH_OTA_RETRY_INTERVAL_MS, s_finalization_pending ? "register/resume" : "download");
}

static void finish_candidate(void)
{
    esp_err_t err = finalize_candidate();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BINARY finalization failed: %s; no READY", esp_err_to_name(err));
        schedule_retry();
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
            esp_err_t resume_err = esp_mesh_lite_ota_notify_child_node_restart();
            if (resume_err != ESP_OK) ESP_LOGW(TAG, "Root skip OTA: resume failed: %s", esp_err_to_name(resume_err));
            ESP_LOGI(TAG, "Root has no newer firmware; no BINARY file registered, no READY");
            return;
        }
    }
    if (err == ESP_OK) err = stage_candidate(&result.metadata, result.size_bytes, result.partition);
    if (err == ESP_OK) {
        finish_candidate();
        return;
    }
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
    if (!s_metadata_valid) {
        err = simple_ota_fetch_metadata(&s_metadata);
        if (err != ESP_OK) goto failed;
        if (!binary_name_valid(s_metadata.file)) {
            err = ESP_ERR_INVALID_SIZE;
            ESP_LOGE(TAG, "BINARY file name must be 1..31 bytes; refusing truncation");
            goto failed;
        }
        if (!simple_ota_version_is_newer(s_metadata.version)) {
            simple_ota_set_state(SIMPLE_OTA_STATE_IDLE);
            ESP_LOGI(TAG, "Child has no newer firmware; no READY");
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
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) {
        err = ESP_FAIL;
        goto failed;
    }
    reset_receiver_locked();
    s_session_open = true;
    s_rx_accepting = true;
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
        reset_receiver_locked();
        ota_unlock();
    }
failed:
    ESP_LOGE(TAG, "Child BINARY request failed: %s", esp_err_to_name(err));
    simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
    schedule_retry();
}

static void start_attempt(void)
{
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) return;
    s_ready_file = (mesh_ready_file_t){0};
    reset_receiver_locked();
    ota_unlock();
    simple_ota_set_state(SIMPLE_OTA_STATE_RUNNING);
    /* 配置角色决定外网下载权限，失去父节点的子不能因 level=0 被当成根。 */
    if (IsRoot == 1) handle_root_path();
    else handle_child_path();
}

/* 单轮服务处理，所有重试调度均不阻塞，保证迟到完成事件能及时收尾。 */
static void mesh_ota_service_poll(EventBits_t bits)
{
    if (bits & CMD_EVT_DOWNLOAD_DONE) process_root_finish();
    process_child_finish();
    if (s_root_downloading && !s_root_timeout_logged &&
        xTaskGetTickCount() - s_root_start_tick >= pdMS_TO_TICKS(LG_MESH_OTA_ROOT_DL_TIMEOUT_MS)) {
        s_root_timeout_logged = true;
        ESP_LOGE(TAG, "Root download exceeded timeout; waiting for worker exit, concurrent retry prohibited");
    }
    if (bits & CMD_EVT_REQUEST) {
        simple_ota_state_t state = simple_ota_get_state();
        if (s_root_downloading || state == SIMPLE_OTA_STATE_RUNNING || s_retry_waiting) {
            ESP_LOGW(TAG, "BINARY command ignored: current attempt/retry still active");
        } else if (state == SIMPLE_OTA_STATE_PENDING_RESTART) {
            if (s_finalization_pending) {
                s_retry_count = 0;
                finish_candidate();
            } else ESP_LOGI(TAG, "BINARY already staged; duplicate command will not erase firmware");
        } else {
            s_retry_count = 0;
            s_metadata_valid = false;
            start_attempt();
        }
    }
    if (s_retry_waiting && (int32_t)(xTaskGetTickCount() - s_retry_deadline) >= 0) {
        s_retry_waiting = false;
        if (s_finalization_pending) finish_candidate();
        else if (!s_root_downloading) start_attempt();
    }
    poll_otacp_report();
}

static void mesh_ota_cmd_task(void *arg)
{
    (void)arg;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_cmd_events,
            CMD_EVT_REQUEST | CMD_EVT_FINISH | CMD_EVT_DOWNLOAD_DONE,
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
    static esp_mesh_lite_lan_ota_file_transfer_cb_t callbacks = {
        .provide_file_cb = provide_file_cb, .get_file_cb = get_file_cb, .get_file_done = get_file_done_cb,
    };
    esp_mesh_lite_ota_register_file_transfer_cb(&callbacks);
    esp_err_t err = esp_event_handler_register(ESP_MESH_LITE_EVENT, ESP_EVENT_ANY_ID,
                                               mesh_ota_event_handler, NULL);
    if (err != ESP_OK) return err;
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
    xEventGroupSetBits(s_cmd_events, CMD_EVT_REQUEST);
#endif
}

simple_ota_state_t mesh_ota_get_state(void)
{
    return simple_ota_get_state();
}
