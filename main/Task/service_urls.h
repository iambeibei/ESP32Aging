#pragma once

/*
 * 服务端地址集中配置
 *
 * 目的：把散落在 APP/HTTP、APP/OTA 里的硬编码 URL 收到一处，后期换服务器/换端口
 * 只改本文件即可，不必翻找业务代码。
 *
 * 使用约定：
 * 1) 这里只放"地址"本身，不放超时、buffer 等业务参数；
 * 2) 修改后需要重新编译烧录（这些是编译期常量，不通过 baseconfig.json 下发）；
 * 3) 本文件位于 main/Task，该目录已在 main/CMakeLists.txt 的 INCLUDE_DIRS 中，
 *    因此其它模块直接 #include "service_urls.h" 即可，无需再改 CMake。
 */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* OTA                                                                        */
/* -------------------------------------------------------------------------- */

/*
 * OTA 固件根目录。
 * 目录末尾带不带 '/' 都可以，simple_ota 内部的 build_url() 会自动补分隔。
 * 实际请求地址 = 该目录 + update.json（版本描述） / + 固件文件名。
 */
#define LG_URL_OTA_BASE_URL "http://10.16.160.51:8888/OTABin/"


/*
 * 现网在用的协议拉取接口（整包协议）。
 * 由 test_http_getproto_New() 使用，老化启动时的设备协议下载走这条
 * （appTask.c 中启动老化与外接设备检查都会调用）。
 * HTTPS，需配合 esp_crt_bundle_attach 校验证书。
 */
#define LG_URL_HTTP_PROTOCOL_WHOLE_URL "https://ipc.poweroak.ltd:29009/DataCenter/V1/Protocol/GetWholeProtocol"


#define LG_URL_Get_LOST_IDNUM_URL "https://192.168.40.116:7088/DeviceServiceCallback/GetMissingIdNums"

#ifdef __cplusplus
}
#endif
