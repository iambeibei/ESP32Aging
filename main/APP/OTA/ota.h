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
 * @brief OTA 状态机（2026-10-16 起：下载完成后不再自动重启）
 *
 * 状态转换：
 *   IDLE --OTA命令--> RUNNING --下载成功--> PENDING_RESTART（等待外部重启）
 *                        |                （无需升级/版本不新）--> IDLE
 *                        +--任何失败--> FAILED（可再次收到命令重试）
 *
 * PENDING_RESTART 表示新固件已写入非运行槽并设为启动分区，
 * 重启后即运行新固件；重启由外部触发（MQTT CanRestart 命令等），
 * 本模块不自动执行 esp_restart()。
 */
typedef enum {
    SIMPLE_OTA_STATE_IDLE = 0,           /**< 空闲（初始态/无需升级后回到此态） */
    SIMPLE_OTA_STATE_RUNNING,            /**< 检查/下载进行中 */
    SIMPLE_OTA_STATE_PENDING_RESTART,    /**< 下载完成，等待外部触发重启 */
    SIMPLE_OTA_STATE_FAILED,             /**< 本次检查/下载失败 */
} simple_ota_state_t;

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
 * @brief 查询 OTA 状态机当前状态
 *
 * 供外部（MQTT 命令处理、状态上报等）判断：
 * 是否已下载完成等待重启（SIMPLE_OTA_STATE_PENDING_RESTART）、
 * 是否正在进行（RUNNING）等。任何任务上下文可调用，无阻塞。
 *
 * @return 当前 simple_ota_state_t
 */
simple_ota_state_t simple_ota_get_state(void);

/**
 * @brief 是否已下载完成并等待外部触发重启
 *
 * 便捷查询：返回 true 表示新固件已写入非运行槽并设为启动分区，
 * 重启（由外部执行）后即运行新固件。
 *
 * @return true: 等待重启; false: 其他状态
 */
bool simple_ota_is_pending_restart(void);

/**
 * @brief 检查服务器版本并在需要时执行 OTA
 *
 * - 服务器版本 > 本地版本：下载并写入非运行槽、设为启动分区，完成后进入
 *   PENDING_RESTART 状态并正常返回 ESP_OK（**不再自动重启**，重启由外部
 *   通过 CanRestart 等机制触发）。
 * - 服务器版本 <= 本地版本：不升级，返回 ESP_OK，状态回 IDLE。
 * - 状态为 PENDING_RESTART 时收到新的 OTA 命令：忽略（防止二次下载覆盖
 *   正在运行的槽位），返回 ESP_OK。
 * - 网络、JSON、版本格式或 OTA 出错：返回对应错误码，状态置 FAILED。
 *
 * @param config OTA 配置
 * @return ESP_OK 无需升级/已下载完成（等待重启）或重复命令被忽略；失败返回错误码
 */
esp_err_t simple_ota_check_and_update(const simple_ota_config_t *config);

/**
 * @brief 服务器 OTA 元数据（update.json 解析结果）
 *
 * 与 ota.c 内部 ota_metadata_t 同布局；mesh_ota 子节点路径用它在
 * transmit_file_start 之前拿到目标版本与文件名。
 */
typedef struct {
    char version[SIMPLE_OTA_VERSION_MAX_LEN];
    char file[SIMPLE_OTA_FILENAME_MAX_LEN];
} simple_ota_metadata_t;

/**
 * @brief 使用默认服务器地址检查更新
 *
 * 默认地址由 lg_url_build(LG_URL_OTA_BASE_URL_FMT, ...) 运行时构建
 * （IP 取自 baseconfig.json 的 SERVER_IP）。
 *
 * @return 语义同 simple_ota_check_and_update()
 */
esp_err_t simple_ota_check_and_update_default(void);

/**
 * @brief 下载并解析默认服务器的 update.json
 *
 * 复用 download_metadata_json/parse_metadata_json 既有链路（默认 URL、
 * QUERY 风格、无证书策略与 simple_ota_check_and_update_default 完全一致）。
 * 供 mesh_ota 子节点路径在发起 mesh 内分发前获取目标版本。
 *
 * @param out_metadata 输出元数据（version + file）
 * @return ESP_OK 成功；失败返回对应错误码
 *
 * @note 阻塞网络 I/O，必须在任务上下文调用（不得在事件回调/ISR）。
 */
esp_err_t simple_ota_fetch_metadata(simple_ota_metadata_t *out_metadata);

/**
 * @brief 探测固件文件大小（不下载 body）
 *
 * 对固件 URL 做 esp_http_client_open + fetch_headers 后立即关闭，
 * 取 Content-Length。供 mesh_ota 子节点路径作为 transmit_file_start
 * 的 size 参数（update.json 契约不含 filesize 字段）。
 *
 * @param file_name 固件文件名（update.json 的 file 字段）
 * @param out_size_bytes 输出字节数
 * @return ESP_OK 成功；网络错误/非 200/长度未知返回对应错误码
 *
 * @note 阻塞网络 I/O，必须在任务上下文调用（不得在事件回调/ISR）。
 */
esp_err_t simple_ota_probe_firmware_size(const char *file_name, size_t *out_size_bytes);

/**
 * @brief 服务器版本是否新于本地运行版本
 *
 * 复用内部 semver 严格三段式比较（服务器 > 本地才返回 true）。
 * 无阻塞、无网络 I/O。
 *
 * @param server_version 服务器版本字符串
 * @return true: 服务器更新; false: 不更新/参数非法/本地版本不可读
 */
bool simple_ota_version_is_newer(const char *server_version);

/**
 * @brief 状态机置值（mesh OTA 路径驱动同一状态机的唯一写入口）
 *
 * mesh_ota 的 get_file_done / OTA_FINISH 事件路径经此收敛到
 * s_ota_state，保持单一事实源。无阻塞。
 *
 * @param state 目标状态
 */
void simple_ota_set_state(simple_ota_state_t state);

#ifdef __cplusplus
}
#endif
