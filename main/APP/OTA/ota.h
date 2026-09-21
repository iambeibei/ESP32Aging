#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 默认 OTA 目录。目录末尾可以带 /，也可以不带 /。 */
#define SIMPLE_OTA_DEFAULT_BASE_URL      "http://10.16.160.51:8888/OTABin/"

/** 固定版本描述文件名。 */
#define SIMPLE_OTA_DEFAULT_METADATA_FILE "update.json"

/** update.json 最大长度。当前只有两个字段，1024 字节足够。 */
#define SIMPLE_OTA_METADATA_MAX_LEN      1024

/** 最长 URL。 */
#define SIMPLE_OTA_URL_MAX_LEN           512

/** 最长版本字符串，例如 123.123.123。 */
#define SIMPLE_OTA_VERSION_MAX_LEN       32

/** 最长 BIN 文件名。 */
#define SIMPLE_OTA_FILENAME_MAX_LEN      128

/**
 * @brief OTA 配置
 */
typedef struct {
    const char *base_url;                  /**< OTA 根目录，例如 http://10.16.160.51:8888/OTABin/ */
    const char *metadata_file;             /**< 版本描述文件，通常固定为 update.json */
    const char *cert_pem;                  /**< HTTPS 根证书；HTTP 可为 NULL */
    bool skip_cert_common_name_check;      /**< HTTPS 是否跳过证书 CN 检查 */
    bool use_crt_bundle;                   /**< HTTPS 是否使用 ESP 证书 bundle */
} simple_ota_config_t;

/**
 * @brief 获取当前正在运行固件的版本号
 *
 * 版本来自固件镜像自身的 esp_app_desc_t.version，不写入 NVS。
 * 建议在工程中通过 PROJECT_VER 设置，例如 1.0.0。
 *
 * @param version 输出缓冲区
 * @param version_size 输出缓冲区大小
 * @return ESP_OK 成功，否则返回错误码
 */
esp_err_t simple_ota_get_running_version(char *version, size_t version_size);

/**
 * @brief 检查服务器版本并在需要时执行 OTA
 *
 * - 服务器版本 > 本地版本：执行 OTA，成功后设备重启，本函数不会正常返回。
 * - 服务器版本 <= 本地版本：不升级，返回 ESP_OK。
 * - 网络、JSON、版本格式或 OTA 出错：返回对应错误码。
 *
 * @param config OTA 配置
 * @return ESP_OK 无需升级；升级成功会重启；失败返回错误码
 */
esp_err_t simple_ota_check_and_update(const simple_ota_config_t *config);

/**
 * @brief 使用默认服务器地址检查更新
 *
 * 默认地址：SIMPLE_OTA_DEFAULT_BASE_URL + SIMPLE_OTA_DEFAULT_METADATA_FILE
 *
 * @return ESP_OK 无需升级；升级成功会重启；失败返回错误码
 */
esp_err_t simple_ota_check_and_update_default(void);

#ifdef __cplusplus
}
#endif
