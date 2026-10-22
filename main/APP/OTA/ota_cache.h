#pragma once

#include <stdint.h>
#include "ota.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_CACHE_ORIGIN_DOWNLOAD 1u
#define OTA_CACHE_ORIGIN_BOOTSTRAP 2u
#define OTA_CACHE_ID_CAPACITY 32
#define OTA_CACHE_SHA256_CAPACITY 65

/** NVS 格式 v1：固定宽度、无指针；校验字段由 store 生成，调用方不得自行拼接 blob。 */
typedef struct {
    uint32_t magic;
    uint32_t format_version;
    uint32_t record_bytes;
    uint32_t origin;
    uint32_t partition_address;
    uint32_t partition_size;
    uint32_t partition_subtype;
    uint32_t image_size_bytes;
    int64_t completed_seq;
    char partition_label[17];
    char file_name[SIMPLE_OTA_BINARY_FILENAME_CAPACITY];
    char version[SIMPLE_OTA_VERSION_MAX_LEN];
    char sha256_hex[OTA_CACHE_SHA256_CAPACITY];
    char device_id[OTA_CACHE_ID_CAPACITY];
    char aging_number[OTA_CACHE_ID_CAPACITY];
    uint32_t crc32;
} ota_cache_record_t;

/** 以下均限任务上下文、非 ISR。NVS/flash 操作可能阻塞；由 OTA 服务串行调度，
 * 调用者须保护目标分区，禁止校验/供数与擦写同一镜像并发。输出结构由调用方拥有。 */
esp_err_t ota_cache_record_init(ota_cache_record_t *record, const esp_partition_t *partition,
                               const simple_ota_metadata_t *metadata, size_t size_bytes);

/** 原子提交单条供数记录；失败不擦除 NVS、不选择启动分区。 */
esp_err_t ota_cache_store(const ota_cache_record_t *record);
/** 无记录返回 ESP_ERR_NOT_FOUND；格式/CRC 错误返回 ESP_ERR_INVALID_RESPONSE。 */
esp_err_t ota_cache_load(ota_cache_record_t *record);
/** 只删除 OTA 自己的记录，绝不擦除整个 NVS。 */
esp_err_t ota_cache_invalidate(void);
/** 擦写前调用：目标与缓存重叠时先提交失效；失败必须阻止后续擦写。 */
esp_err_t ota_cache_before_write(const esp_partition_t *partition);

/** 核验当前分区表、完整镜像、内嵌版本及原 BIN SHA；成功返回 SDK 只读分区指针。 */
esp_err_t ota_cache_verify(const ota_cache_record_t *record, const esp_partition_t **partition);
/** 校验镜像并取得 SDK 解析的真实镜像长度，不使用分区容量充当文件长度。 */
esp_err_t ota_cache_image_size(const esp_partition_t *partition, const char *version, uint32_t *size_bytes);
/** 按真实 BIN 长度计算 SHA-256，每 2048 字节让出任务；不可从 SDK 回调中调用。 */
esp_err_t ota_cache_hash_file(const esp_partition_t *partition, size_t size_bytes,
                             char sha256_hex[OTA_CACHE_SHA256_CAPACITY]);

#ifdef __cplusplus
}
#endif
