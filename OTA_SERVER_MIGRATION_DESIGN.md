# OTA 服务器迁移设计（8080 静态目录 → 7088 HTTPS REST 接口）

## 1. 背景与现象

服务器下线 `192.168.40.116:8080` 的 OTA 静态目录后，设备侧现象：

- MQTT 下发 ota 指令后，串口报 `Connection reset by peer`、`ESP_ERR_HTTP_CONNECT`，
  无法下载 `update.json`；
- 开发机（与设备同网段）用浏览器/curl 访问
  `http://192.168.40.116:8080/OTAFiles/update.json` 同样失败；
- 改用 `https://192.168.40.116:7088/api/OTAFiles/Download?fileName=update.json`
  可正常访问。

## 2. 实测证据（2026-10-13，开发机 curl）

| 探测 | 结果 |
|---|---|
| `http://192.168.40.116:8080/OTAFiles/update.json` | curl (7) Failed to connect to 192.168.40.116 port 8080: Could not connect to server —— **8080 端口无监听** |
| `https://192.168.40.116:7088/api/OTAFiles/Download?fileName=update.json` | HTTP 200，body `{"version": "0.0.1", "file": "Light_Gateway_ESP32s3_v0.0.1.bin"}` |
| `…?fileName=Light_Gateway_ESP32s3_v0.0.1.bin` | HTTP 200，size=2036944，content_type=application/octet-stream |
| `…?fileName=not_exist.bin` | HTTP 404，body `{"code":-1,"message":"文件不存在"}` |

## 3. 根因分析

### 3.1 直接原因：旧地址整体失效（非设备代码缺陷）

设备与 PC 在同一网段、同一 URL 均失败，且 8080 端口连接被拒
（ECONNREFUSED，设备侧表现为 connect 阶段 `ESP_ERR_HTTP_CONNECT` /
`Connection reset by peer`）。OTA 文件服务已迁移为 7088 端口的 HTTPS
REST 下载接口，与仓库内已在用的 `LG_URL_Get_LOST_IDNUM_URL`
（`https://192.168.40.116:7088/DeviceServiceCallback/GetMissingIdNums`）
同主机同端口。

### 3.2 仅改 `LG_URL_OTA_BASE_URL` 不够：三个代码层阻塞点

已实测确认：把 `LG_URL_OTA_BASE_URL` 直接改为
`https://192.168.40.116:7088/api/OTAFiles/Download?fileName=` 而其他不动，
OTA 仍然失败。按 `simple_ota_check_and_update()`（ota.c:597-669）的执行顺序，
依次撞上：

| # | 阻塞点 | 代码位置 | 现象 |
|---|---|---|---|
| 1 | `version.txt` 为四段 `0.0.1.13`，`parse_semver()` 严格要求三段 | ota.c:96-117, 604-616 | 读取本地版本即返回 `ESP_ERR_INVALID_VERSION`，**请求发不出去** |
| 2 | 无证书 HTTPS 被主动拒绝 | `apply_https_config()` ota.c:208-234 | `ESP_ERR_INVALID_ARG`，日志 `HTTPS URL requires cert_pem or certificate bundle`，**请求发不出去** |
| 3 | `build_url()` 对不以 `/` 结尾的 base 插入 `/` | ota.c:70-94 | 生成脏 URL `…Download?fileName=/update.json`，服务器 404 `文件不存在` |

### 3.3 新接口与现有解析契约兼容

- `update.json` 仍是 `version` + `file` 两个字符串字段；
- `file` 为纯文件名（`Light_Gateway_ESP32s3_v0.0.1.bin`），通过
  `validate_firmware_filename()`（无 `/`、`\`、`..`、`://`，`.bin` 结尾）
  与 `_v<version>.bin` 后缀规则；
- 固件 2036944 字节 < ota_0/ota_1 分区 0x240000（2359296），可装入。

## 4. 接口修改设计

### 4.1 `service_urls.h`：新 OTA 下载地址

```c
#define LG_URL_OTA_BASE_URL "https://192.168.40.116:7088/api/OTAFiles/Download?fileName="
```

（宏名保持不变，注释说明新形态；编译期常量，改后需重新编译烧录。）

### 4.2 `ota.h`：URL 拼接风格枚举

```c
typedef enum {
    SIMPLE_OTA_URL_STYLE_PATH = 0,  /**< 目录拼接：base + '/' + name（旧静态目录形态） */
    SIMPLE_OTA_URL_STYLE_QUERY      /**< 查询串拼接：base + name（base 已含 "?fileName="） */
} simple_ota_url_style_t;
```

`simple_ota_config_t` 末尾新增 `simple_ota_url_style_t url_style;`
（默认值 0 = PATH，既有外部调用方行为不变）。

### 4.3 `ota.c` 三处改动

1. **`build_url()` 按风格分支**：
   - PATH：现状（base 不带 `/` 时补 `/` 再拼接）；
   - QUERY：**原样拼接**（base 已含 `?fileName=`，不插任何字符）。
2. **`validate_config()` QUERY 防呆**：QUERY 风格要求 base_url 同时含
   `://` 与 `?`，否则报 `ESP_ERR_INVALID_ARG`，防止配置写错生成脏 URL。
3. **`apply_https_config()` 放开无证书 HTTPS**：当 `cert_pem == NULL` 且
   `use_crt_bundle == false` 且 URL 为 https 时：
   - `#ifdef CONFIG_ESP_TLS_INSECURE`：打 `ESP_LOGW`（含 URL 前缀，现场可
     识别为不安全连接）后放行；
   - 否则维持 `ESP_ERR_INVALID_ARG`。
   - cert_pem / bundle 的既有优先序不变（提供了证书仍优先校验）。

   依据：sdkconfig 已启用 `CONFIG_ESP_TLS_INSECURE=y` 与
   `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`；仓库内
   `test_http_post_record_query()`（GetProto.c:511-520，`crt_bundle_attach`
   注释掉）访问同一 7088 主机即为无证书 HTTPS 的既有先例。

4. **`simple_ota_check_and_update_default()` 显式配置**（MQTT ota 指令唯一入口）：

```c
simple_ota_config_t config = {
    .base_url = LG_URL_OTA_BASE_URL,
    .metadata_file = SIMPLE_OTA_DEFAULT_METADATA_FILE,
    .cert_pem = NULL,
    .skip_cert_common_name_check = true,   /* IP 地址 + 内网服务，CN 校验必失败 */
    .use_crt_bundle = false,
    .url_style = SIMPLE_OTA_URL_STYLE_QUERY,
};
```

### 4.4 固件下载通路统一（2026-10-13 追加，修复固件下载失败）

**实机现象**：元数据下载成功后，固件下载报
`esp_https_ota: No option for server verification is enabled in esp_http_client
config.` → `ESP_ERR_INVALID_ARG`。

**根因**（IDF 源码 esp_https_ota.c:269-297）：`esp_https_ota` 组件的
`is_server_verification_enabled()` 只接受 `cert_pem`、`use_global_ca_store`、
`crt_bundle_attach` 三者之一，**`skip_cert_common_name_check` 不在接受列表**，
因此无证书 HTTPS 被该组件拒绝。`CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` 虽可跳过校验，
但 IDF help 明确 "ONLY FOR TESTING PURPOSE"，不启用、不改 sdkconfig。

**对照证据**：同一设备、同一无证书 HTTPS 配置下，元数据下载走
`esp_http_client_perform()` 成功（200 + 解析出版本/文件名）——本工程的无证书
HTTPS 策略在 esp_http_client 通路可用，问题仅在 esp_https_ota 组件的额外校验。

**修复**：`perform_firmware_ota()` 不再按 scheme 分流到 `esp_https_ota`，
`http://` 与 `https://` 统一走 `perform_http_ota()` 手动 OTA 通路
（`esp_http_client_open` + `esp_ota_begin/write/end`，scheme 无关），init 前
对其 config 应用 `apply_https_config()`——固件下载与元数据下载共用同一证书策略。

**顺带删除**（用户要求：OTA 文件中未使用函数）：通路统一后
`perform_https_ota()` 成为唯一未使用函数（引用计数核实），连同
`#include "esp_https_ota.h"` 一并删除；其余函数均有实际调用。
镜像由 0x204f10 缩小到 0x203b30。

### 4.4 `version.txt`：`0.0.1.13` → `0.0.1`

三段式对齐服务器当前版本（产品决策：以服务器 0.0.1 起编，后续 0.0.2 /
0.1.0 递增，历史四段号作废）。`parse_semver()` 三段式契约不变。

## 5. 兼容性与回退

- **回退旧服务器**：把 `LG_URL_OTA_BASE_URL` 改回
  `http://10.16.160.51:8888/OTABin/` 并将默认配置 `url_style` 改回
  `SIMPLE_OTA_URL_STYLE_PATH` 即可，代码无需回退（PATH 分支保留原逻辑）。
- **升级服务器证书**：后续若服务器改用公共 CA 证书，只需在默认配置中
  置 `use_crt_bundle = true`（sdkconfig 已开
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`）或提供 `cert_pem`。
- **update.json 契约不变**：服务器字段、`_v<version>.bin` 命名规则均沿用。
- 不改 `perform_firmware_ota()` 的 scheme 分发：新 URL 为 `https://`，
  自动走 `esp_https_ota()` 通路。

## 6. 执行后数据流

```text
MQTT topic 含 ota
  → simple_ota_check_and_update_default()
  → parse_semver(本地 0.0.1)                       [阻塞点1 消除]
  → download_metadata_json()
      → build_url(QUERY): base + "update.json"
        = https://192.168.40.116:7088/api/OTAFiles/Download?fileName=update.json
      → apply_https_config(): 无证书 + INSECURE → 放行(WRN)   [阻塞点2 消除]
      → HTTP 200 → parse_metadata_json()  (version=0.0.1, file=...bin)
  → 版本比较：0.0.1 vs 0.0.1 相等 → "already up to date"，ESP_OK
      （服务器发布 0.0.2+ 时才触发下载）
  → 触发时 build_url(QUERY): ?fileName=Light_Gateway_ESP32s3_v0.0.2.bin
  → perform_firmware_ota(): http/https 统一 → perform_http_ota()
      → apply_https_config()（与元数据同一证书策略）
      → esp_http_client_open/read + esp_ota_begin/write/end → 重启
```

## 7. 实机验收

1. 编译烧录（`app-flash` 即可，未改 `data/`）。**必须用规定命令**
   （`idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig …`）：
   顶层 `build/` 目录可能是旧配置缓存（曾出现 `PROJECT_VER=0.0.0` 导致设备
   上报 `Current firmware version: 0.0.0`），烧录后首条日志应为
   `Current firmware version: 0.0.1`。
2. 串口触发 ota 指令（或等服务器下发），确认日志：
   - `Current firmware version: 0.0.1`
   - `Checking OTA metadata: https://192.168.40.116:7088/api/OTAFiles/Download?fileName=update.json`
   - 无 `HTTPS URL requires cert_pem` 报错；若打印不安全连接 WRN 属预期
   - 当前服务器 0.0.1 == 本地 0.0.1 → `Firmware is already up to date`
3. 服务器发布 0.0.2（或更高）后再次触发：确认
   `Firmware URL: …?fileName=…_v0.0.2.bin`、`Starting OTA download:`、
   下载进度日志、`OTA successful. Restarting...`，重启后
   `esp_app_desc_t.version` 为新版本。
4. 子节点经根节点 NAPT 访问 7088 的回程路由需实机确认（与
   GetMissingIdNums 同一路径，属既有网络能力）。
5. 回归：根/子节点 MQTT 主链路不受影响（本改动只碰 OTA 模块）。

## 8. 代码审查记录（CERT C + FreeRTOS + 本地规范，单模型；规则组合见 `.em_skill.json`）

审查范围：`ota.c` / `ota.h` / `service_urls.h` 本次改动（70 增 12 删，SMALL，
无 ISR/并发关键路径）。

| # | 级别 | 问题 | 结论 |
|---|---|---|---|
| 1 | low | `validate_firmware_filename()` 未拒绝查询串元字符（`?`/`&`/`#`），QUERY 风格下可破坏 `?fileName=` 参数边界 | 已修：加入拒绝列表 |
| 2 | — | `build_url()` 缓冲区与返回值处理 | 确认合规：`out_size==0` 与 NULL 防护、snprintf 截断检查（`written<0 \|\| >=out_size`）、`base_len>0` 后置下标访问均正确 |
| 3 | — | `apply_https_config()` 无证书放行分支 | 确认合规：函数入口已判定 https；放行绑定编译期 `CONFIG_ESP_TLS_INSECURE`，`#else` 维持原报错；打 WRN 保证现场可识别 |
| 4 | — | `simple_ota_config_t` 新增 `url_style` 字段兼容性 | 确认合规：枚举默认值 0=PATH，仓库内唯一配置构造点（`simple_ota_check_and_update_default()`）已显式赋值 |
| 5 | — | FreeRTOS / 并发 / 内存 | 不涉及：无新增任务、队列、动态分配；均为纯函数与配置改动 |

无证书 HTTPS 的运行时行为（esp-tls 在 `CONFIG_ESP_TLS_INSECURE` +
`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` 下确实免验）以仓库内
`test_http_post_record_query()` 访问同一 7088 主机的既有先例为依据，
需按第 7 节实机确认。

### 8.1 第二轮审查记录（固件下载通路统一，2026-10-13）

审查范围：`perform_http_ota` 签名/证书策略、`perform_firmware_ota` 通路统一、
`perform_https_ota` 及 `esp_https_ota.h` 删除（约 60 增 45 删）。

| # | 级别 | 问题 | 结论 |
|---|---|---|---|
| 1 | high（编译期） | `perform_http_ota` 前部新增 `esp_err_t err = apply_https_config(...)` 与原函数体内 `esp_err_t err = esp_http_client_open(...)` 重定义 | 已修：后者改为赋值 `err = ...`，编译通过 |
| 2 | — | 删除函数后的引用完整性 | 已确认：全仓无 `perform_https_ota`/`esp_https_ota` 残留引用（仅注释说明）；`is_https_url()` 仍被 `apply_https_config()` 使用 |
| 3 | — | `apply_https_config()` 返回值处理 | 确认合规：失败即 return，错误路径非空 |
| 4 | — | 其他未使用函数排查（用户要求） | 引用计数核实：删除后 OTA 文件无未使用函数；`ota.h` 宏与公开 API 均在用 |

## 9. 已知限制

- 无证书 HTTPS 不防中间人：内网部署可接受，决策显式绑定
  `CONFIG_ESP_TLS_INSECURE` 编译开关；若该选项被关闭，OTA 会明确报错而非
  静默降级。
- `fileName` 若含需要 URL 编码的字符（空格、中文等）暂不支持——当前命名
  规范（`_v<version>.bin`）不涉及。
- 服务器 404 返回 JSON 错误体，设备侧以 `status_code != 200` 捕获，
  错误体内容不解析（仅日志可见性足够）。
