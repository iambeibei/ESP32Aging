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
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "simple_ota";

typedef struct {
    int major;
    int minor;
    int patch;
} ota_semver_t;

typedef struct {
    char version[SIMPLE_OTA_VERSION_MAX_LEN];
    char file[SIMPLE_OTA_FILENAME_MAX_LEN];
} ota_metadata_t;

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

    return ESP_OK;
}

static esp_err_t build_url(const char *base_url,
                           const char *name,
                           char *out,
                           size_t out_size)
{
    if (base_url == NULL || name == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t base_len = strlen(base_url);
    bool has_slash = (base_len > 0 && base_url[base_len - 1] == '/');

    int written = snprintf(out,
                           out_size,
                           has_slash ? "%s%s" : "%s/%s",
                           base_url,
                           name);

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

    /* file 字段只允许同目录文件名，不允许 URL、子目录和 .. 跳转。 */
    if (strstr(file, "..") != NULL ||
        strstr(file, "://") != NULL ||
        strchr(file, '/') != NULL ||
        strchr(file, '\\') != NULL) {
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

    ESP_LOGE(TAG, "HTTPS URL requires cert_pem or certificate bundle");
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t download_metadata_json(const simple_ota_config_t *config,
                                        char *json,
                                        size_t json_size)
{
    char metadata_url[SIMPLE_OTA_URL_MAX_LEN];
    esp_err_t err = build_url(config->base_url,
                              config->metadata_file,
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

static esp_err_t perform_http_ota(const char *firmware_url)
{
    ESP_LOGI(TAG, "Starting HTTP OTA: %s", firmware_url);

    esp_http_client_config_t config = {
        .url = firmware_url,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
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

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA successful. Restarting...");
    esp_restart();
    return ESP_OK;

ota_fail:
    esp_ota_abort(ota_handle);
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return err;
}

static esp_err_t perform_https_ota(const char *firmware_url,
                                   const simple_ota_config_t *config)
{
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

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    ESP_LOGI(TAG, "Starting HTTPS OTA: %s", firmware_url);

    err = esp_https_ota(&ota_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS OTA failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA successful. Restarting...");
    esp_restart();
    return ESP_OK;
}

static esp_err_t perform_firmware_ota(const char *firmware_url,
                                      const simple_ota_config_t *config)
{
    if (is_https_url(firmware_url)) {
        return perform_https_ota(firmware_url, config);
    }

    if (strncmp(firmware_url, "http://", 7) == 0) {
        return perform_http_ota(firmware_url);
    }

    ESP_LOGE(TAG, "Unsupported firmware URL: %s", firmware_url);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t simple_ota_check_and_update(const simple_ota_config_t *config)
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

    return perform_firmware_ota(firmware_url, config);
}

esp_err_t simple_ota_check_and_update_default(void)
{
    simple_ota_config_t config = {
        .base_url = SIMPLE_OTA_DEFAULT_BASE_URL,
        .metadata_file = SIMPLE_OTA_DEFAULT_METADATA_FILE,
        .cert_pem = NULL,
        .skip_cert_common_name_check = false,
        .use_crt_bundle = false,
    };

    return simple_ota_check_and_update(&config);
}
