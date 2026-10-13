#pragma once

#include <stddef.h>
#include "esp_err.h"

/*
 * 服务端地址集中配置
 *
 * 目的：把散落在 APP/HTTP、APP/OTA 里的 URL 规则收到一处。
 *
 * 两类地址：
 * A) IP 运行时化（_FMT 模板）：URL 中的服务器 IP 在运行时取自 baseconfig.json
 *    的 SERVER_IP（与 MQTT broker 同一机制，见 ConfigData.c / readconfig()）。
 *    换服务器 IP 只需改 baseconfig.json 并完整 flash，无需重新编译 app。
 *    模板中的 %s 为 IP 占位，必须经 lg_url_build() 填充后使用，禁止直接把
 *    模板当 URL 传给 esp_http_client。
 * B) 完整 URL 宏：公网域名等不依赖本机配置的地址（如协议拉取接口）。
 *
 * 使用约定：
 * 1) 这里只放"地址规则"（scheme/端口/路径），不放超时、buffer 等业务参数；
 * 2) _FMT 模板的端口与路径仍是编译期常量，修改后需要重新编译烧录；
 * 3) 本文件位于 main/Task，该目录已在 main/CMakeLists.txt 的 INCLUDE_DIRS 中，
 *    因此其它模块直接 #include "service_urls.h" 即可，无需再改 CMake。
 */

#ifdef __cplusplus
extern "C" {
#endif

/* URL 构建结果缓冲区长度：当前最长 URL 约 63 字符，256 余量充足。 */
#define LG_URL_MAX_LEN 256

/* -------------------------------------------------------------------------- */
/* OTA                                                                        */
/* -------------------------------------------------------------------------- */

/*
 * OTA 文件下载接口（查询串下载形态，文件名通过 ?fileName= 传入）。
 * 旧形态为静态目录 http://<ip>:<port>/OTABin/（目录 + 文件名拼接），已随
 * 8080 端口下线失效，迁移过程见 OTA_SERVER_MIGRATION_DESIGN.md。
 * %s 为服务器 IP 占位，运行时由 lg_url_build() 用 SERVER_IP 填充：
 *   http://<SERVER_IP>:11282/api/OTAFiles/Download?fileName=update.json
 *   http://<SERVER_IP>:11282/api/OTAFiles/Download?fileName=xxx_v0.0.2.bin
 * 注意：末尾的 "?fileName=" 是接口契约的一部分，
 * simple_ota 按 SIMPLE_OTA_URL_STYLE_QUERY 原样拼接，不要再补 '/'。
 */
#define LG_URL_OTA_BASE_URL_FMT "http://%s:11282/api/OTAFiles/Download?fileName="

/*
 * 现网在用的协议拉取接口（整包协议）。
 * 由 test_http_getproto_New() 使用，老化启动时的设备协议下载走这条
 * （appTask.c 中启动老化与外接设备检查都会调用）。
 * 公网域名形态，不随 SERVER_IP 变化，保持完整 URL 宏。
 * HTTPS，需配合 esp_crt_bundle_attach 校验证书。
 */
#define LG_URL_HTTP_PROTOCOL_WHOLE_URL "https://ipc.poweroak.ltd:29009/DataCenter/V1/Protocol/GetWholeProtocol"

/*
 * 缺失数据补发查询接口（GetMissingIdNums）。
 * 由 test_http_post_record_query() 使用，老化结束后的丢号查询走这条
 * （appTask.c 的 AgingDataCheck 阶段调用）。
 * %s 为服务器 IP 占位，运行时由 lg_url_build() 用 SERVER_IP 填充：
 *   http://<SERVER_IP>:11282/DeviceServiceCallback/GetMissingIdNums
 */
#define LG_URL_GET_LOST_IDNUM_URL_FMT "http://%s:11282/DeviceServiceCallback/GetMissingIdNums"

/**
 * @brief 用运行时 SERVER_IP 填充 URL 模板
 *
 * url_fmt 必须且只能含一个 %s（IP 占位）。SERVER_IP 未配置（readconfig 默认
 * 哨兵 "default"）或指针为空时返回 ESP_ERR_INVALID_STATE；缓冲区不足返回
 * ESP_ERR_INVALID_SIZE。成功后 out 为 NUL 结尾的完整 URL。
 *
 * @param url_fmt   含单个 %s 占位的 URL 模板（LG_URL_*_FMT）
 * @param out       调用方提供的输出缓冲区（不会被本模块保留引用）
 * @param out_size  输出缓冲区长度
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE /
 *         ESP_ERR_INVALID_SIZE 失败
 *
 * @note 调用方负责缓冲区生命周期；本函数无内部状态，可重入。
 *       必须在 readconfig() 之后调用（SERVER_IP 已填充）。
 */
esp_err_t lg_url_build(const char *url_fmt, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
