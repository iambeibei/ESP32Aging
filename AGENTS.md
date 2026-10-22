# AGENTS.md

本文件为 AI 编码代理提供本仓库的工作指引。**主要架构文档见 [CLAUDE.md](CLAUDE.md)**（任务布局、
双轴设备抽象、老化状态机、数据流、持久化、已知陷阱均在其中，此处不重复）。编码规范权威是
[.ai/coding-standard.md](.ai/coding-standard.md)。

网络与指令专题设计文档（根目录）：

- [CHILD_NODE_ROOT_LOSS_DESIGN.md](CHILD_NODE_ROOT_LOSS_DESIGN.md) —— 根节点掉线时子节点
  不得异常重启：依赖/保活/故障转移/恢复流程、根因定位（BLE 异常断开的无条件重启）、
  修复与实机验收、代码审查记录。
- [AGING_CMD_PN_GUARD_DESIGN.md](AGING_CMD_PN_GUARD_DESIGN.md) —— start_aging 缺 PN 时
  直接拒绝回 IDLE 并经 MQTT 返回老化异常：消息格式、补偿机制、兼容性。
- [NETWORK_RESTART_DESIGN.md](NETWORK_RESTART_DESIGN.md) —— 一键重启 30s 传播窗、暂停 Mesh、
  根先深层后重启；父连接失败计数与清 Mesh RTC 后的有界自愈。

## 编译与烧录（代理可直接执行）

必须先 source ESP-IDF 环境，否则 `idf.py` 不可用：

```bash
cd /home/wbb/espidf/esp-idf-mesh-lite && . ./export.sh
cd /home/wbb/espidfProject/ESP32Aging
IDF_SKIP_CHECK_SUBMODULES=1 idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig build
```

- 迭代固件用 `idf.py -p /dev/ttyUSB0 app-flash`（保留 SQLite 数据库）。
- 修改 `data/` 下配置文件后必须完整 `idf.py flash`（SPIFFS 镜像随 storage 分区烧录）。
- `tests/` 提供部分模块宿主回归；OTA 可运行 `python3 tests/test_mesh_ota_binary.py`。
  无线链路、真实 flash/NVS 掉电行为仍需编译 + 串口实机验证。

## version.txt —— 固件版本号的唯一来源

根目录 `version.txt` 是一行纯文本（具体版本以文件内容为准，**必须保持三段式 `MAJOR.MINOR.PATCH`**，
四段号会使 OTA 在读取本地版本处直接失败），由 ESP-IDF 构建系统在 `project()` 宏中读取
（`tools/cmake/project.cmake`，优先级最高：version.txt > project(VERSION) > git describe > 默认 1），
最终作为 `PROJECT_VER` 编译进 app 镜像头部的 `esp_app_desc_t.version` 字段。

运行时读取点（**不经过 NVS，也不经过 baseconfig.json**）：

| 位置 | API | 用途 |
|---|---|---|
| `main/APP/OTA/ota.c` | `esp_ota_get_partition_description()` | OTA 新旧版本比较 |
| `main/APP/MQTT/mqtt_app.c` | `esp_app_get_description()->version` | MQTT 上行 JSON 的 `Version` 字段 |

修改版本号只需改 `version.txt` 并重新编译；构建系统通过 `CMAKE_CONFIGURE_DEPENDS` 跟踪该文件。

## OTA 升级（main/APP/OTA/）

触发：MQTT 收到 topic 含 `ota` 的命令 → `parse_jsonCommand_MQTT()` →
`mesh_ota_handle_mqtt_command()` 非阻塞通知服务任务。根节点由下载任务调用
`simple_ota_check_and_update_default()`；子节点采用 Mesh-Lite BINARY 分发。
当前为“不自动重启”的分发验证版本，流程与实机验收见 `MESH_LITE_OTA_INTEGRATION.md`。

根节点 HTTP 流程（`simple_ota_check_and_update()`）：

1. 读本地版本（来自 `esp_app_desc_t`，即 version.txt 的编译期值），要求严格 `MAJOR.MINOR.PATCH`。
2. 下载 `LG_URL_OTA_BASE_URL_FMT`（`main/Task/service_urls.h` 的 `_FMT` 模板）经
   `lg_url_build()` 填充后的 `update.json`（≤1024 字节），字段：`version` + `file`。
   **IP 运行时取自 `SERVER_IP`**（baseconfig.json，与 MQTT broker 同一机制），
   端口/路径为编译期常量（当前 `http://<SERVER_IP>:11282/api/OTAFiles/Download?fileName=`，
   查询串拼接风格）。旧静态目录形态（`http://<ip>:<port>/OTABin/`）已随 8080 端口
   下线失效，迁移详情见 `OTA_SERVER_MIGRATION_DESIGN.md`。
3. 校验：semver 严格三段式（**`version.txt` 必须为三段式**，四段号会在读取本地版本处
   直接失败）；`file` 只允许同目录 `*.bin` 文件名（另拒绝 `?`/`&`/`#` 查询串元字符），
   且必须以 `_v<version>.bin` 结尾（防止 JSON 版本与文件名不一致）。
4. 版本比较：服务器 **>** 本地才升级；相等跳过，更旧**阻止降级**，均返回 `ESP_OK`。
5. 下载固件：`http://`/`https://` 统一走手动 `esp_ota_begin/write/end`
   （2048 字节缓冲、进度日志、长度和镜像版本校验），生成实际下载结果快照。
   Mesh OTA 服务计算真实 BIN 的 SHA、提交专用 NVS 记录后才调用
   `esp_ota_set_boot_partition()`，再登记文件名和恢复下级请求，不自动重启。
   不使用 `esp_https_ota()` 组件——其强制证书校验与无证书 HTTPS 策略冲突。
6. 分区表为 `ota_0`/`ota_1` 双槽（各 0x240000）+ `otadata`，见 `partition.csv`。

注意事项：

- OTA 与 GetMissingIdNums 的服务器 **IP 运行时取自 `SERVER_IP`**（`lg_url_build()`，
  service_urls.c）：**改 IP 只需改 baseconfig.json 并完整 flash，无需重新编译 app**；
  端口/路径仍是编译期常量（`_FMT` 模板），改它们才需重新编译烧录。
- 当前 OTA 下载为**明文 HTTP**（内网 `11282`），无需证书；`apply_https_config()` 的
  无证书 HTTPS 放行路径保留备用（换回 HTTPS 内网地址时自动生效，打 WRN 可识别）。
- `version.txt` 必须使用真实三段式版本并与服务器 BIN 对齐；发布同一版本后禁止替换其内容。
- OTA 下载完成后**不自动重启**：下载并设置启动分区后继续运行旧固件。BINARY 在校验、文件名登记和恢复下级 OTA 后输出 `OTA_BINARY_READY`，并向 `device/<DEVICE_ID>/event/OTACP` 以 QoS1 上报 `{"Seq":完成时间戳,"Data":{"IsComplete":1,"AgingNumber":"本机配置"}}`；断线/入队失败每 5 秒重试。`PENDING_RESTART` 不等于供数已就绪。外部 `server/rack/<AgingNumber>/CanRestart` 交给统一重启服务：联网保持 30s 后暂停 Mesh，按 `30+(层数-1)*30` 秒根先深层后重启；原主题/下行 JSON 不变。详情见 `NETWORK_RESTART_DESIGN.md`。期间掉电/其他重启仍会启动已选定的新固件。
- `ota_cache.c` 使用 NVS `ota_cache/image_v1` 保存单份 256 字节供数记录（文件名、版本、
  真实长度、分区身份、SHA、原 OTACP Seq/设备身份、格式版本和 CRC）。启动先校验本地记录和
  完整镜像再恢复供数，可恢复备用槽或当前运行槽，不能按“另一个槽”盲选。恢复下载记录后用
  原 Seq 补报，PC 按设备和 Seq 去重。已运行目标版本时回到 IDLE，可继续升级下一版本。
- 无记录时只在服务器版本和长度与当前已校验镜像一致后建立当前槽供数记录；仅请求元数据和
  响应头，不下载 BIN 正文，不伪造下载完成通知。擦写前必须先使重叠记录失效并提交；NVS
  错误阻止擦写或启动选择，禁止为 OTA 清空整个 NVS。详细边界见 `MESH_LITE_OTA_INTEGRATION.md`。
- 升级只写另一个 ota 槽，SQLite 数据库（`data_storage` 分区）与配置（`storage` 分区）不受影响。

## 重启与网络自愈

- `APP/Restart/system_restart.c` 只有一个 CPU1 服务任务；任务初始化失败拒绝命令，不能退回
  MQTT 接收任务内延时重启。首次排程冻结层数和单调时钟截止时间，重复命令不得延长。
- 正常一键重启保留 Mesh RTC；子节点两条故障自愈路径先清 Mesh RTC 再重启，保留项目 RTC
  重启预算（每上电周期 3 次）。禁止为换父清 NVS/老化恢复/OTA 记录。
- 接收计划起抑制网络自愈重启，30s 后禁止应用重新连接 Mesh。STA 控制共用 mutex，事件回调
  只维护状态，不等待控制锁。计划提交与自愈重启必须经过统一仲裁。
- 非主动 STA_DISCONNECTED 计一次失败，LOST_IP 不重复计数；GOT_IP 连续在线 10s 才清零，
  断开后结束在线窗。具体运行语义及接收回执见 `NETWORK_RESTART_DESIGN.md`。

## 关键约定速查（详见 CLAUDE.md）

- 新增源文件必须显式加入 `main/CMakeLists.txt` 的 `SRCS` 和 `INCLUDE_DIRS`。
- 协议 JSON 键名（`名称`、`精度`、`补偿` 等）是线上契约，不要翻译成英文。
- 新任务固定在 CPU1（`create_cpu1_task()`），栈/优先级用 `task_config.h` 的 `LG_*` 常量。
- 大块内存用 `heap_caps` / `app_malloc_prefer_psram`；`QueryResult` 约 4.7 KB，禁止放栈上。
- 数据库 schema 检测靠探测实际列而非 `PRAGMA user_version`；日志 pragma 保持
  `journal_mode=DELETE` + `synchronous=FULL`。
