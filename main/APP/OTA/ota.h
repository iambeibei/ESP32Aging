#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* OTA 根目录地址统一取自 main/Task/service_urls.h */
#include "service_urls.h"

#ifdef __cplusplus
extern "C" {
#endif


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
 * @brief URL 拼接风格
 *
 * PATH  : 目录拼接，base + '/' + name（旧静态目录形态，如 http://ip:port/OTABin/）。
 * QUERY : 查询串拼接，base + name（base 已含 "?fileName="，HTTPS REST 下载接口形态）。
 * 默认值 0（PATH）保证既有调用方行为不变；新服务器默认配置使用 QUERY。
 */
typedef enum {
    SIMPLE_OTA_URL_STYLE_PATH = 0,
    SIMPLE_OTA_URL_STYLE_QUERY
} simple_ota_url_style_t;

/**
 * @brief OTA 配置
 */
typedef struct {
    const char *base_url;                  /**< OTA 根地址：PATH 风格为目录，QUERY 风格为含 "?fileName=" 的下载接口前缀（默认由 lg_url_build(LG_URL_OTA_BASE_URL_FMT, ...) 运行时构建，IP 取自 SERVER_IP，见 service_urls.h） */
    const char *metadata_file;             /**< 版本描述文件，通常固定为 update.json */
    const char *cert_pem;                  /**< HTTPS 根证书；HTTP 或（INSECURE 放行的）无证书 HTTPS 可为 NULL */
    bool skip_cert_common_name_check;      /**< HTTPS 是否跳过证书 CN 检查（IP 地址访问必须为 true） */
    bool use_crt_bundle;                   /**< HTTPS 是否使用 ESP 证书 bundle */
    simple_ota_url_style_t url_style;      /**< URL 拼接风格，见 simple_ota_url_style_t */
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
