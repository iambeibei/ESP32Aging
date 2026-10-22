/**
 * @file ota.c
 * @brief ESP32 OTA 检查与升级模块实现
 */

#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"

#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "simple_ota";

/* cmd_task/dl_task/库回调共享；只在短临界区内读写状态和下载结果。 */
static portMUX_TYPE s_ota_state_lock = portMUX_INITIALIZER_UNLOCKED;
static simple_ota_state_t s_ota_state = SIMPLE_OTA_STATE_IDLE;
static simple_ota_download_result_t s_download_result;
static bool s_download_result_valid;

simple_ota_state_t simple_ota_get_state(void)
{
    portENTER_CRITICAL(&s_ota_state_lock);
    simple_ota_state_t state = s_ota_state;
    portEXIT_CRITICAL(&s_ota_state_lock);
    return state;
}

bool simple_ota_is_pending_restart(void)
{
    return simple_ota_get_state() == SIMPLE_OTA_STATE_PENDING_RESTART;
}

esp_err_t simple_ota_get_download_result(simple_ota_download_result_t *out_result)
{
    if (out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_ota_state_lock);
    bool valid = s_download_result_valid;
    *out_result = valid ? s_download_result : (simple_ota_download_result_t){0};
    portEXIT_CRITICAL(&s_ota_state_lock);
    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/*
 * 默认 OTA 配置构造（定义在文件后部，simple_ota_check_and_update_default 先行使用）。
 */
static esp_err_t build_default_ota_config(char *base_url,
                                          size_t base_url_size,
                                          simple_ota_config_t *config);

typedef struct {
    int major;
    int minor;
    int patch;
} ota_semver_t;

/*
 * 内部元数据别名：ota.h 的 simple_ota_metadata_t 是同一布局的公开类型
 * （mesh_ota 子节点路径复用），此处保留别名避免改动既有函数签名。
 */
typedef simple_ota_metadata_t ota_metadata_t;

typedef struct {
    char data[SIMPLE_OTA_METADATA_MAX_LEN];
    size_t len;
    bool overflow;
} ota_http_response_t;

static bool is_https_url(const char *url)
{
    return url != NULL && strncmp(url, "https://", 8) == 0;
}

static esp_err_t validate_config(const simple_ota_config_t *config)
{
    if (config == NULL || config->base_url == NULL || config->base_url[0] == '\0') {
        ESP_LOGE(TAG, "Invalid OTA config: base_url is empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (config->metadata_file == NULL || config->metadata_file[0] == '\0') {
        ESP_LOGE(TAG, "Invalid OTA config: metadata_file is empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (strstr(config->metadata_file, "..") != NULL ||
        strchr(config->metadata_file, '/') != NULL ||
        strchr(config->metadata_file, '\\') != NULL) {
        ESP_LOGE(TAG, "Invalid metadata file name: %s", config->metadata_file);
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * QUERY 风格的 base 必须是完整的下载接口前缀：同时含 scheme 与 '?'。
     * 缺 '?' 说明配置写成了目录形态（或反之），继续拼会产出
     * "…Download?fileName=/update.json" 这类脏 URL，此处防呆。
     */
    if (config->url_style == SIMPLE_OTA_URL_STYLE_QUERY &&
        (strstr(config->base_url, "://") == NULL ||
         strchr(config->base_url, '?') == NULL)) {
        ESP_LOGE(TAG, "QUERY url_style requires base_url with scheme and '?': %s",
                 config->base_url);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t build_url(const char *base_url,
                           const char *name,
                           simple_ota_url_style_t url_style,
                           char *out,
                           size_t out_size)
{
    if (base_url == NULL || name == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written;
    if (url_style == SIMPLE_OTA_URL_STYLE_QUERY) {
        /*
         * QUERY：base 已含 "?fileName=" 等查询前缀，必须原样拼接。
         * 此处若按 PATH 逻辑补 '/'，会生成 "…?fileName=/update.json" 导致 404。
         */
        written = snprintf(out, out_size, "%s%s", base_url, name);
    } else {
        /* PATH：目录拼接，base 不带 '/' 时自动补分隔（旧静态目录形态）。 */
        size_t base_len = strlen(base_url);
        bool has_slash = (base_len > 0 && base_url[base_len - 1] == '/');
        written = snprintf(out,
                           out_size,
                           has_slash ? "%s%s" : "%s/%s",
                           base_url,
                           name);
    }

    if (written < 0 || (size_t)written >= out_size) {
        ESP_LOGE(TAG, "OTA URL is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static bool parse_semver(const char *text, ota_semver_t *version)
{
    if (text == NULL || version == NULL) {
        return false;
    }

    int major = -1;
    int minor = -1;
    int patch = -1;
    char extra = '\0';

    /* 严格要求 MAJOR.MINOR.PATCH，不接受 1.0、v1.0.0 或 1.0.0-beta。 */
    int count = sscanf(text, "%d.%d.%d%c", &major, &minor, &patch, &extra);
    if (count != 3 || major < 0 || minor < 0 || patch < 0) {
        return false;
    }

    version->major = major;
    version->minor = minor;
    version->patch = patch;
    return true;
}

static int compare_semver(const ota_semver_t *a, const ota_semver_t *b)
{
    if (a->major != b->major) {
        return (a->major > b->major) ? 1 : -1;
    }
    if (a->minor != b->minor) {
        return (a->minor > b->minor) ? 1 : -1;
    }
    if (a->patch != b->patch) {
        return (a->patch > b->patch) ? 1 : -1;
    }
    return 0;
}

static bool validate_firmware_filename(const char *file)
{
    if (file == NULL || file[0] == '\0') {
        return false;
    }

    /* file 字段只允许同目录文件名，不允许 URL、子目录和 .. 跳转。
     * QUERY 风格下 file 直接拼进 "?fileName=" 查询串，'?'、'&'、'#' 会破坏
     * 查询参数边界，一并拒绝（PATH 风格下这些字符同样无合法用途）。 */
    if (strstr(file, "..") != NULL ||
        strstr(file, "://") != NULL ||
        strchr(file, '/') != NULL ||
        strchr(file, '\\') != NULL ||
        strchr(file, '?') != NULL ||
        strchr(file, '&') != NULL ||
        strchr(file, '#') != NULL) {
        return false;
    }

    size_t len = strlen(file);
    return len > 4 && strcmp(file + len - 4, ".bin") == 0;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    ota_http_response_t *response = (ota_http_response_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;

    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;

    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;

    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP header: %s: %s",
                 evt->header_key ? evt->header_key : "",
                 evt->header_value ? evt->header_value : "");
        break;

    case HTTP_EVENT_ON_DATA:
        if (response != NULL && evt->data != NULL && evt->data_len > 0) {
            size_t remain = sizeof(response->data) - 1 - response->len;
            size_t copy_len = (size_t)evt->data_len;

            if (copy_len > remain) {
                copy_len = remain;
                response->overflow = true;
            }

            if (copy_len > 0) {
                memcpy(response->data + response->len, evt->data, copy_len);
                response->len += copy_len;
                response->data[response->len] = '\0';
            }
        }
        break;

    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;

    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;

    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }

    return ESP_OK;
}

static esp_err_t apply_https_config(esp_http_client_config_t *http_config,
                                    const simple_ota_config_t *config)
{
    if (!is_https_url(http_config->url)) {
        return ESP_OK;
    }

#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (config->use_crt_bundle) {
        http_config->crt_bundle_attach = esp_crt_bundle_attach;
        return ESP_OK;
    }
#else
    if (config->use_crt_bundle) {
        ESP_LOGE(TAG, "Certificate bundle requested but not enabled in sdkconfig");
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

    if (config->cert_pem != NULL) {
        http_config->cert_pem = config->cert_pem;
        return ESP_OK;
    }

#ifdef CONFIG_ESP_TLS_INSECURE
    /*
     * 无证书 HTTPS 放行：仅当 sdkconfig 启用 CONFIG_ESP_TLS_INSECURE
     * （及 CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY）时才编译进本分支，
     * 与仓库内访问同一内网主机的 test_http_post_record_query() 先例一致。
     * 打 WRN 以便现场识别本次为不安全连接；关闭该配置时维持报错，不静默降级。
     */
    ESP_LOGW(TAG, "HTTPS without certificate (insecure build): %s", http_config->url);
    return ESP_OK;
#else
    ESP_LOGE(TAG, "HTTPS URL requires cert_pem or certificate bundle");
    return ESP_ERR_INVALID_ARG;
#endif
}

static esp_err_t download_metadata_json(const simple_ota_config_t *config,
                                        char *json,
                                        size_t json_size)
{
    char metadata_url[SIMPLE_OTA_URL_MAX_LEN];
    esp_err_t err = build_url(config->base_url,
                              config->metadata_file,
                              config->url_style,
                              metadata_url,
                              sizeof(metadata_url));
    if (err != ESP_OK) {
        return err;
    }

    ota_http_response_t response = {0};

    esp_http_client_config_t http_config = {
        .url = metadata_url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = config->skip_cert_common_name_check,
    };

    err = apply_https_config(&http_config, config);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Checking OTA metadata: %s", metadata_url);

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to download update.json: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "update.json HTTP status: %d", status_code);
        return ESP_FAIL;
    }

    if (response.overflow) {
        ESP_LOGE(TAG, "update.json exceeds %d bytes", SIMPLE_OTA_METADATA_MAX_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    if (response.len == 0) {
        ESP_LOGE(TAG, "update.json is empty");
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (response.len + 1 > json_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(json, response.data, response.len + 1);
    return ESP_OK;
}

static esp_err_t parse_metadata_json(const char *json, ota_metadata_t *metadata)
{
    if (json == NULL || metadata == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Invalid update.json");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *file = cJSON_GetObjectItemCaseSensitive(root, "file");

    if (!cJSON_IsString(version) || version->valuestring == NULL ||
        !cJSON_IsString(file) || file->valuestring == NULL) {
        ESP_LOGE(TAG, "update.json must contain string fields: version and file");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (strlen(version->valuestring) >= sizeof(metadata->version) ||
        strlen(file->valuestring) >= sizeof(metadata->file)) {
        ESP_LOGE(TAG, "update.json field is too long");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }

    ota_semver_t parsed_version;
    if (!parse_semver(version->valuestring, &parsed_version)) {
        ESP_LOGE(TAG, "Invalid server version format: %s", version->valuestring);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!validate_firmware_filename(file->valuestring)) {
        ESP_LOGE(TAG, "Invalid firmware file name: %s", file->valuestring);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 命名规范要求 BIN 文件名以 _v<version>.bin 结尾，避免 JSON 版本和文件名写错。 */
    char expected_suffix[SIMPLE_OTA_VERSION_MAX_LEN + 8];
    int suffix_len = snprintf(expected_suffix,
                              sizeof(expected_suffix),
                              "_v%s.bin",
                              version->valuestring);
    size_t file_len = strlen(file->valuestring);
    if (suffix_len <= 0 ||
        (size_t)suffix_len >= sizeof(expected_suffix) ||
        file_len < (size_t)suffix_len ||
        strcmp(file->valuestring + file_len - (size_t)suffix_len, expected_suffix) != 0) {
        ESP_LOGE(TAG,
                 "Firmware file name must end with %s, got: %s",
                 expected_suffix,
                 file->valuestring);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    strlcpy(metadata->version, version->valuestring, sizeof(metadata->version));
    strlcpy(metadata->file, file->valuestring, sizeof(metadata->file));

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t simple_ota_get_running_version(char *version, size_t version_size)
{
    if (version == NULL || version_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    if (running_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get running partition");
        return ESP_FAIL;
    }

    esp_app_desc_t app_desc;
    esp_err_t err = esp_ota_get_partition_description(running_partition, &app_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read running app description: %s", esp_err_to_name(err));
        return err;
    }

    if (app_desc.version[0] == '\0') {
        ESP_LOGE(TAG, "Running firmware version is empty");
        return ESP_ERR_INVALID_VERSION;
    }

    if (strlen(app_desc.version) + 1 > version_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    strlcpy(version, app_desc.version, version_size);
    return ESP_OK;
}

/*
 * 固件下载：esp_http_client 手动 OTA 通路，对 http:// 与 https:// 均适用。
 * 证书策略与元数据下载共用 apply_https_config()（cert_pem / bundle /
 * 无证书 INSECURE 放行），不使用 esp_https_ota 组件——后者强制要求证书配置，
 * 与本报文的无证书 HTTPS 策略冲突（见 OTA_SERVER_MIGRATION_DESIGN.md 第 8 节）。
 */
static esp_err_t perform_http_ota(const char *firmware_url,
                                  const simple_ota_config_t *config,
                                  const ota_metadata_t *metadata)
{
    ESP_LOGI(TAG, "Starting OTA download: %s", firmware_url);

    esp_http_client_config_t http_config = {
        .url = firmware_url,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = config->skip_cert_common_name_check,
    };

    esp_err_t err = apply_https_config(&http_config, config);
    if (err != ESP_OK) {
        return err;
    }

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open firmware URL: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "Firmware HTTP status: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (content_length > 0) {
        ESP_LOGI(TAG, "Firmware size: %lld bytes", (long long)content_length);
    } else {
        ESP_LOGI(TAG, "Firmware size unknown or chunked");
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA update partition available");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_FOUND;
    }
    if (content_length > update_partition->size) {
        ESP_LOGE(TAG, "Firmware exceeds OTA partition");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG,
             "Writing OTA partition: type=%d subtype=%d offset=0x%lx",
             update_partition->type,
             update_partition->subtype,
             (unsigned long)update_partition->address);

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    uint8_t buffer[2048];
    int64_t total_read = 0;
    int last_progress = -1;

    while (true) {
        int read_len = esp_http_client_read(client, (char *)buffer, sizeof(buffer));

        if (read_len < 0) {
            ESP_LOGE(TAG, "Firmware download read failed");
            err = ESP_FAIL;
            goto ota_fail;
        }

        if (read_len == 0) {
            break;
        }

        if ((size_t)read_len > update_partition->size - (size_t)total_read) {
            err = ESP_ERR_INVALID_SIZE;
            goto ota_fail;
        }
        err = esp_ota_write(ota_handle, buffer, (size_t)read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            goto ota_fail;
        }

        total_read += read_len;

        if (content_length > 0) {
            int progress = (int)((total_read * 100) / content_length);
            if (progress >= last_progress + 10 || progress == 100) {
                ESP_LOGI(TAG, "OTA download progress: %d%%", progress);
                last_progress = progress;
            }
        }
    }

    if (!esp_http_client_is_complete_data_received(client)) {
        ESP_LOGE(TAG, "Firmware download is incomplete");
        err = ESP_FAIL;
        goto ota_fail;
    }

    if (content_length > 0 && total_read != content_length) {
        ESP_LOGE(TAG,
                 "Firmware length mismatch: received=%lld expected=%lld",
                 (long long)total_read,
                 (long long)content_length);
        err = ESP_FAIL;
        goto ota_fail;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    client = NULL;

    ESP_LOGI(TAG, "Firmware download completed: %lld bytes", (long long)total_read);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_app_desc_t downloaded_desc;
    err = esp_ota_get_partition_description(update_partition, &downloaded_desc);
    if (err != ESP_OK ||
        strncmp(downloaded_desc.version, metadata->version, sizeof(downloaded_desc.version)) != 0) {
        ESP_LOGE(TAG, "Downloaded image version does not match update.json target=%s", metadata->version);
        return err != ESP_OK ? err : ESP_ERR_INVALID_VERSION;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * 2026-10-16：不再自动重启。新固件已写入非运行槽并设为启动分区，
     * 重启由外部触发（MQTT CanRestart），以便多点位 mesh 场景下各节点
     * 先全部完成下载再统一重启，避免自动重启打断老化/MQTT/mesh 拓扑。
     * 重复 OTA 命令在此状态下会被 simple_ota_check_and_update() 忽略，
     * 防止二次下载覆盖正在运行的槽位。
     */
    portENTER_CRITICAL(&s_ota_state_lock);
    s_download_result = (simple_ota_download_result_t){
        .metadata = *metadata,
        .size_bytes = (size_t)total_read,
        .partition = update_partition,
    };
    s_download_result_valid = true;
    s_ota_state = SIMPLE_OTA_STATE_PENDING_RESTART;
    portEXIT_CRITICAL(&s_ota_state_lock);
    ESP_LOGI(TAG, "OTA download completed and set as boot partition; restart deferred to external trigger (CanRestart)");
    return ESP_OK;

ota_fail:
    {
        esp_err_t abort_err = esp_ota_abort(ota_handle);
        if (abort_err != ESP_OK) ESP_LOGW(TAG, "HTTP OTA abort failed: %s", esp_err_to_name(abort_err));
    }
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return err;
}

static esp_err_t perform_firmware_ota(const char *firmware_url,
                                      const simple_ota_config_t *config,
                                      const ota_metadata_t *metadata)
{
    /*
     * http:// 与 https:// 统一走 esp_http_client 手动 OTA 通路。
     * 不按 scheme 分流到 esp_https_ota 组件：其 is_server_verification_enabled()
     * 只接受 cert_pem/use_global_ca_store/crt_bundle_attach，拒绝本工程的无证书
     * HTTPS 策略；而 esp_http_client 通路经 apply_https_config() 处理后已由
     * 元数据下载实测验证可用（见 OTA_SERVER_MIGRATION_DESIGN.md 第 8 节）。
     */
    if (strncmp(firmware_url, "http://", 7) == 0 ||
        strncmp(firmware_url, "https://", 8) == 0) {
        return perform_http_ota(firmware_url, config, metadata);
    }

    ESP_LOGE(TAG, "Unsupported firmware URL: %s", firmware_url);
    return ESP_ERR_NOT_SUPPORTED;
}

/*
 * OTA 检查与下载实现（状态管理见外层包装 simple_ota_check_and_update）。
 * 保持原有下载链路不变：配置校验 → 本地版本 → update.json → 版本比较 →
 * HTTP 下载 → 双槽写入 → 设置启动分区。
 */
static esp_err_t ota_check_and_update_impl(const simple_ota_config_t *config)
{
    esp_err_t err = validate_config(config);
    if (err != ESP_OK) {
        return err;
    }

    char current_version[SIMPLE_OTA_VERSION_MAX_LEN];
    err = simple_ota_get_running_version(current_version, sizeof(current_version));
    if (err != ESP_OK) {
        return err;
    }

    ota_semver_t current_semver;
    if (!parse_semver(current_version, &current_semver)) {
        ESP_LOGE(TAG,
                 "Current firmware version must use MAJOR.MINOR.PATCH, got: %s",
                 current_version);
        return ESP_ERR_INVALID_VERSION;
    }

    ESP_LOGI(TAG, "Current firmware version: %s", current_version);

    char json[SIMPLE_OTA_METADATA_MAX_LEN];
    err = download_metadata_json(config, json, sizeof(json));
    if (err != ESP_OK) {
        return err;
    }

    ota_metadata_t metadata = {0};
    err = parse_metadata_json(json, &metadata);
    if (err != ESP_OK) {
        return err;
    }

    ota_semver_t server_semver;
    if (!parse_semver(metadata.version, &server_semver)) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (strlen(metadata.file) >= SIMPLE_OTA_BINARY_FILENAME_CAPACITY) {
        ESP_LOGE(TAG, "BINARY file name exceeds 31 bytes: %s", metadata.file);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Server firmware version: %s", metadata.version);
    ESP_LOGI(TAG, "Server firmware file: %s", metadata.file);

    int cmp = compare_semver(&server_semver, &current_semver);
    if (cmp <= 0) {
        if (cmp == 0) {
            ESP_LOGI(TAG, "Firmware is already up to date");
        } else {
            ESP_LOGW(TAG,
                     "Server version %s is older than current version %s; downgrade is blocked",
                     metadata.version,
                     current_version);
        }
        return ESP_OK;
    }

    char firmware_url[SIMPLE_OTA_URL_MAX_LEN];
    err = build_url(config->base_url,
                    metadata.file,
                    config->url_style,
                    firmware_url,
                    sizeof(firmware_url));
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "New firmware available: %s -> %s",
             current_version,
             metadata.version);
    ESP_LOGI(TAG, "Firmware URL: %s", firmware_url);

    return perform_firmware_ota(firmware_url, config, &metadata);
}

/*
 * OTA 状态管理薄包装：在每个错误返回点散落状态赋值容易漏，统一在此收敛。
 * - PENDING_RESTART 期间收到新命令：忽略（防二次下载覆盖运行槽）；
 * - 进入时置 RUNNING；失败置 FAILED；
 * - 无需升级（版本不新/更旧，ESP_OK 且状态仍为 RUNNING）回 IDLE；
 * - 下载成功时 perform_http_ota 已置 PENDING_RESTART，此处不覆盖。
 */
esp_err_t simple_ota_check_and_update(const simple_ota_config_t *config)
{
    if (simple_ota_is_pending_restart()) {
        ESP_LOGW(TAG, "OTA already pending restart; ignoring duplicate OTA command");
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_ota_state_lock);
    s_download_result_valid = false;
    s_ota_state = SIMPLE_OTA_STATE_RUNNING;
    portEXIT_CRITICAL(&s_ota_state_lock);
    esp_err_t err = ota_check_and_update_impl(config);

    if (err != ESP_OK) {
        simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
    } else if (simple_ota_get_state() == SIMPLE_OTA_STATE_RUNNING) {
        /* 无需升级路径：回到 IDLE（下载成功路径已是 PENDING_RESTART） */
        simple_ota_set_state(SIMPLE_OTA_STATE_IDLE);
    }
    return err;
}

esp_err_t simple_ota_check_and_update_default(void)
{
    /*
     * 服务器 IP 运行时取自 baseconfig.json 的 SERVER_IP（与 MQTT broker 同一
     * 机制），端口/路径为编译期模板；换服务器 IP 无需重新编译 app。
     * 配置构造与 simple_ota_fetch_metadata / simple_ota_probe_firmware_size
     * 共用 build_default_ota_config()，保证三条路径同源同策略。
     */
    char base_url[LG_URL_MAX_LEN];
    simple_ota_config_t config;
    esp_err_t err = build_default_ota_config(base_url, sizeof(base_url), &config);
    if (err != ESP_OK) {
        return err;
    }

    return simple_ota_check_and_update(&config);
}

/*
 * 默认 OTA 配置构造：运行时 SERVER_IP + 编译期端口/路径 + QUERY 风格 +
 * 无证书放行策略（与既有默认入口完全一致）。
 * base_url 由调用方提供存储，其生命周期必须覆盖 config 的使用期。
 * 调用上下文：任务上下文（内部走 lg_url_build，无网络 I/O）。
 */
static esp_err_t build_default_ota_config(char *base_url,
                                          size_t base_url_size,
                                          simple_ota_config_t *config)
{
    if (base_url == NULL || config == NULL || base_url_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lg_url_build(LG_URL_OTA_BASE_URL_FMT, base_url, base_url_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Build OTA base URL failed: %s", esp_err_to_name(err));
        return err;
    }

    *config = (simple_ota_config_t){
        .base_url = base_url,
        .metadata_file = SIMPLE_OTA_DEFAULT_METADATA_FILE,
        .cert_pem = NULL,
        /* 内网 IP + 下载接口：CN 校验必然失败，与默认入口同策略无证书放行。 */
        .skip_cert_common_name_check = true,
        .use_crt_bundle = false,
        .url_style = SIMPLE_OTA_URL_STYLE_QUERY,
    };
    return ESP_OK;
}

esp_err_t simple_ota_fetch_metadata(simple_ota_metadata_t *out_metadata)
{
    if (out_metadata == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char base_url[LG_URL_MAX_LEN];
    simple_ota_config_t config;
    esp_err_t err = build_default_ota_config(base_url, sizeof(base_url), &config);
    if (err != ESP_OK) {
        return err;
    }

    char json[SIMPLE_OTA_METADATA_MAX_LEN];
    err = download_metadata_json(&config, json, sizeof(json));
    if (err != ESP_OK) {
        return err;
    }

    return parse_metadata_json(json, out_metadata);
}

esp_err_t simple_ota_probe_firmware_size(const char *file_name, size_t *out_size_bytes)
{
    if (file_name == NULL || out_size_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 与下载路径同一文件名校验，提前拒绝非法输入。 */
    if (!validate_firmware_filename(file_name)) {
        ESP_LOGE(TAG, "Invalid firmware file name: %s", file_name);
        return ESP_ERR_INVALID_ARG;
    }

    char base_url[LG_URL_MAX_LEN];
    simple_ota_config_t config;
    esp_err_t err = build_default_ota_config(base_url, sizeof(base_url), &config);
    if (err != ESP_OK) {
        return err;
    }

    char firmware_url[SIMPLE_OTA_URL_MAX_LEN];
    err = build_url(base_url, file_name, config.url_style, firmware_url, sizeof(firmware_url));
    if (err != ESP_OK) {
        return err;
    }

    esp_http_client_config_t http_config = {
        .url = firmware_url,
        .event_handler = http_event_handler,
        .user_data = NULL, /* fetch_headers 不触发 ON_DATA；无累积缓冲需求。 */
        .timeout_ms = 10000,
        /* 探测完即关，不保持连接。 */
        .keep_alive_enable = false,
        .skip_cert_common_name_check = config.skip_cert_common_name_check,
    };

    err = apply_https_config(&http_config, &config);
    if (err != ESP_OK) {
        return err;
    }

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open firmware URL for size probe: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "Firmware probe HTTP status: %d", status_code);
        return ESP_FAIL;
    }

    if (content_length <= 0) {
        ESP_LOGE(TAG, "Firmware size unknown or chunked");
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Firmware size probe: %lld bytes", (long long)content_length);
    *out_size_bytes = (size_t)content_length;
    return ESP_OK;
}

bool simple_ota_version_is_newer(const char *server_version)
{
    if (server_version == NULL) {
        return false;
    }

    char current_version[SIMPLE_OTA_VERSION_MAX_LEN];
    esp_err_t err = simple_ota_get_running_version(current_version, sizeof(current_version));
    if (err != ESP_OK) {
        return false;
    }

    ota_semver_t current_semver;
    ota_semver_t server_semver;
    if (!parse_semver(current_version, &current_semver) ||
        !parse_semver(server_version, &server_semver)) {
        ESP_LOGE(TAG,
                 "Version compare failed: local=%s server=%s",
                 current_version,
                 server_version);
        return false;
    }

    return compare_semver(&server_semver, &current_semver) > 0;
}

void simple_ota_set_state(simple_ota_state_t state)
{
    portENTER_CRITICAL(&s_ota_state_lock);
    s_ota_state = state;
    portEXIT_CRITICAL(&s_ota_state_lock);
}
