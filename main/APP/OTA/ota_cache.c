/** OTA 专用供数记录，使用单个 NVS blob + commit，不写入老化恢复命名空间。 */
#include "ota_cache.h"

#include <stdio.h>
#include <string.h>
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#define OTA_CACHE_MAGIC 0x4f544143u
#define OTA_CACHE_FORMAT_VERSION 1u
#define OTA_CACHE_NAMESPACE "ota_cache"
#define OTA_CACHE_KEY "image_v1"
#define OTA_CACHE_HASH_BUFFER_BYTES 2048
#define OTA_CACHE_ALIGNMENT_BYTES (64u * 1024u)

_Static_assert(sizeof(ota_cache_record_t) == 256, "OTA cache v1 layout changed; bump format version");

static bool terminated(const char *value, size_t capacity)
{
    return memchr(value, '\0', capacity) != NULL;
}

static uint32_t record_crc(const ota_cache_record_t *record)
{
    ota_cache_record_t copy;
    memcpy(&copy, record, sizeof(copy));
    copy.crc32 = 0;
    return esp_rom_crc32_le(0, (const uint8_t *)&copy, sizeof(copy));
}

static bool record_valid(const ota_cache_record_t *record)
{
    if (record == NULL || record->magic != OTA_CACHE_MAGIC ||
        record->format_version != OTA_CACHE_FORMAT_VERSION || record->record_bytes != sizeof(*record) ||
        (record->origin != OTA_CACHE_ORIGIN_DOWNLOAD && record->origin != OTA_CACHE_ORIGIN_BOOTSTRAP) ||
        (record->partition_subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         record->partition_subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
        record->image_size_bytes == 0 || record->image_size_bytes > record->partition_size ||
        record->image_size_bytes > UINT32_MAX - (OTA_CACHE_ALIGNMENT_BYTES - 1) ||
        ((record->image_size_bytes + OTA_CACHE_ALIGNMENT_BYTES - 1) & ~(OTA_CACHE_ALIGNMENT_BYTES - 1)) > record->partition_size ||
        !terminated(record->partition_label, sizeof(record->partition_label)) || record->partition_label[0] == '\0' ||
        !terminated(record->file_name, sizeof(record->file_name)) || record->file_name[0] == '\0' ||
        !terminated(record->version, sizeof(record->version)) ||
        !terminated(record->device_id, sizeof(record->device_id)) ||
        !terminated(record->aging_number, sizeof(record->aging_number)) ||
        !terminated(record->sha256_hex, sizeof(record->sha256_hex)) || strlen(record->sha256_hex) != 64) return false;
    if (record->origin == OTA_CACHE_ORIGIN_DOWNLOAD &&
        (record->completed_seq < 0 || record->device_id[0] == '\0' || record->aging_number[0] == '\0')) return false;
    if (record->origin == OTA_CACHE_ORIGIN_BOOTSTRAP && record->completed_seq != 0) return false;
    for (size_t i = 0; i < 64; ++i) {
        char ch = record->sha256_hex[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    int comparison;
    if (simple_ota_compare_versions(record->version, record->version, &comparison) != ESP_OK) return false;
    if (strpbrk(record->file_name, "/\\?&#:") != NULL || strstr(record->file_name, "..") != NULL) return false;
    char suffix[SIMPLE_OTA_VERSION_MAX_LEN + 8];
    int length = snprintf(suffix, sizeof(suffix), "_v%s.bin", record->version);
    size_t file_bytes = strlen(record->file_name);
    return length > 0 && (size_t)length < sizeof(suffix) && file_bytes >= (size_t)length &&
           strcmp(record->file_name + file_bytes - (size_t)length, suffix) == 0;
}

esp_err_t ota_cache_record_init(ota_cache_record_t *record, const esp_partition_t *partition,
                               const simple_ota_metadata_t *metadata, size_t size_bytes)
{
    if (record == NULL || partition == NULL || metadata == NULL || size_bytes == 0 ||
        size_bytes > UINT32_MAX || size_bytes > partition->size ||
        strnlen(metadata->file, sizeof(metadata->file)) >= sizeof(record->file_name) ||
        strnlen(metadata->version, sizeof(metadata->version)) >= sizeof(record->version)) return ESP_ERR_INVALID_ARG;
    memset(record, 0, sizeof(*record));
    record->magic = OTA_CACHE_MAGIC;
    record->format_version = OTA_CACHE_FORMAT_VERSION;
    record->record_bytes = sizeof(*record);
    record->origin = OTA_CACHE_ORIGIN_BOOTSTRAP;
    record->partition_address = partition->address;
    record->partition_size = partition->size;
    record->partition_subtype = partition->subtype;
    record->image_size_bytes = (uint32_t)size_bytes;
    strlcpy(record->partition_label, partition->label, sizeof(record->partition_label));
    strlcpy(record->file_name, metadata->file, sizeof(record->file_name));
    strlcpy(record->version, metadata->version, sizeof(record->version));
    return ESP_OK;
}

esp_err_t ota_cache_store(const ota_cache_record_t *record)
{
    if (!record_valid(record)) return ESP_ERR_INVALID_ARG;
    ota_cache_record_t copy;
    memcpy(&copy, record, sizeof(copy));
    copy.crc32 = record_crc(&copy);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_CACHE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, OTA_CACHE_KEY, &copy, sizeof(copy));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t ota_cache_load(ota_cache_record_t *record)
{
    if (record == NULL) return ESP_ERR_INVALID_ARG;
    memset(record, 0, sizeof(*record));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_CACHE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    size_t length = 0;
    err = nvs_get_blob(handle, OTA_CACHE_KEY, NULL, &length);
    if (err == ESP_OK && length != sizeof(*record)) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) err = nvs_get_blob(handle, OTA_CACHE_KEY, record, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    if (!record_valid(record) || record->crc32 != record_crc(record)) {
        memset(record, 0, sizeof(*record));
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t ota_cache_invalidate(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_CACHE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, OTA_CACHE_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    else if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t ota_cache_before_write(const esp_partition_t *partition)
{
    if (partition == NULL) return ESP_ERR_INVALID_ARG;
    ota_cache_record_t record;
    esp_err_t err = ota_cache_load(&record);
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (err == ESP_ERR_INVALID_RESPONSE) return ota_cache_invalidate();
    if (err != ESP_OK) return err;
    if (record.partition_address == partition->address ||
        strcmp(record.partition_label, partition->label) == 0) return ota_cache_invalidate();
    return ESP_OK;
}

esp_err_t ota_cache_image_size(const esp_partition_t *partition, const char *version, uint32_t *size_bytes)
{
    if (partition == NULL || version == NULL || size_bytes == NULL) return ESP_ERR_INVALID_ARG;
    esp_app_desc_t desc;
    esp_err_t err = esp_ota_get_partition_description(partition, &desc);
    if (err != ESP_OK) return err;
    if (strncmp(desc.version, version, sizeof(desc.version)) != 0) return ESP_ERR_INVALID_VERSION;
    esp_partition_pos_t position = {.offset = partition->address, .size = partition->size};
    esp_image_metadata_t image;
    err = esp_image_verify(ESP_IMAGE_VERIFY, &position, &image);
    if (err != ESP_OK) return err;
    if (image.image_len == 0 || image.image_len > partition->size) return ESP_ERR_INVALID_SIZE;
    *size_bytes = image.image_len;
    return ESP_OK;
}

esp_err_t ota_cache_hash_file(const esp_partition_t *partition, size_t size_bytes, char sha256_hex[OTA_CACHE_SHA256_CAPACITY])
{
    if (partition == NULL || sha256_hex == NULL || size_bytes == 0 || size_bytes > partition->size) return ESP_ERR_INVALID_ARG;
    unsigned char digest[32], buffer[OTA_CACHE_HASH_BUFFER_BYTES];
    static const char hex[] = "0123456789abcdef";
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    esp_err_t err = ESP_FAIL;
    if (mbedtls_sha256_starts(&ctx, 0) != 0) goto done;
    for (size_t offset = 0; offset < size_bytes;) {
        size_t count = size_bytes - offset;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        err = esp_partition_read(partition, offset, buffer, count);
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
        sha256_hex[2 * i] = hex[digest[i] >> 4];
        sha256_hex[2 * i + 1] = hex[digest[i] & 0x0F];
    }
    sha256_hex[64] = '\0';
    err = ESP_OK;
done:
    mbedtls_sha256_free(&ctx);
    return err;
}

esp_err_t ota_cache_verify(const ota_cache_record_t *record, const esp_partition_t **partition)
{
    if (partition == NULL || !record_valid(record)) return ESP_ERR_INVALID_ARG;
    *partition = NULL;
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        record->partition_subtype, record->partition_label);
    if (part == NULL || part->address != record->partition_address || part->size != record->partition_size) return ESP_ERR_NOT_FOUND;
    uint32_t image_bytes;
    esp_err_t err = ota_cache_image_size(part, record->version, &image_bytes);
    if (err != ESP_OK) return err;
    if (image_bytes > record->image_size_bytes) return ESP_ERR_INVALID_SIZE;
    char digest[OTA_CACHE_SHA256_CAPACITY];
    err = ota_cache_hash_file(part, record->image_size_bytes, digest);
    if (err != ESP_OK) return err;
    if (strcmp(digest, record->sha256_hex) != 0) return ESP_ERR_INVALID_RESPONSE;
    *partition = part;
    return ESP_OK;
}
