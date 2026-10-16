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

## 编译与烧录（代理可直接执行）

必须先 source ESP-IDF 环境，否则 `idf.py` 不可用：

```bash
cd /home/wbb/espidf/esp-idf-mesh-lite && . ./export.sh
cd /home/wbb/espidfProject/ESP32Aging
IDF_SKIP_CHECK_SUBMODULES=1 idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig build
```

- 迭代固件用 `idf.py -p /dev/ttyUSB0 app-flash`（保留 SQLite 数据库）。
- 修改 `data/` 下配置文件后必须完整 `idf.py flash`（SPIFFS 镜像随 storage 分区烧录）。
- 本项目**没有测试套件**；验证手段是编译 + 串口观察。

## version.txt —— 固件版本号的唯一来源

根目录 `version.txt` 是一行纯文本（当前为 `0.0.1`，**必须保持三段式 `MAJOR.MINOR.PATCH`**，
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

触发：MQTT 收到 topic 含 `ota`（含 `public/cmd/ota`）的命令 → `parse_jsonCommand_MQTT()`
（appTask.c 约 2499 行）→ `simple_ota_check_and_update_default()`。

流程（`simple_ota_check_and_update()`）：

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
5. 下载固件：`http://`/`https://` 统一走手动 `esp_ota_begin/write/end` +
   `esp_ota_set_boot_partition()` + `esp_restart()`（2048 字节缓冲、进度日志、长度校验）。
   不使用 `esp_https_ota()` 组件——其强制证书校验与无证书 HTTPS 策略冲突。
6. 分区表为 `ota_0`/`ota_1` 双槽（各 0x240000）+ `otadata`，见 `partition.csv`。

注意事项：

- OTA 与 GetMissingIdNums 的服务器 **IP 运行时取自 `SERVER_IP`**（`lg_url_build()`，
  service_urls.c）：**改 IP 只需改 baseconfig.json 并完整 flash，无需重新编译 app**；
  端口/路径仍是编译期常量（`_FMT` 模板），改它们才需重新编译烧录。
- 当前 OTA 下载为**明文 HTTP**（内网 `11282`），无需证书；`apply_https_config()` 的
  无证书 HTTPS 放行路径保留备用（换回 HTTPS 内网地址时自动生效，打 WRN 可识别）。
- `version.txt` 现为 `0.0.1`（三段式对齐服务器；为 `0.0.0` 时任何服务器版本都会触发升级，
  发布正式固件前应设置真实版本号）。
- OTA 下载完成后**不再自动重启**（2026-10-16 起）：`simple_ota_check_and_update*()` 下载并设置启动分区后正常返回，设备继续运行旧固件，状态进入 `PENDING_RESTART`（`simple_ota_get_state()` 可查询）。重启由外部触发：向 `server/rack/<AgingNumber>/CanRestart` 发 `{"Data":{"Restart":1}}`（子节点已订阅），节点随即重启运行新固件；pending 期间重复 OTA 命令会被忽略。
- 升级只写另一个 ota 槽，SQLite 数据库（`data_storage` 分区）与配置（`storage` 分区）不受影响。

## 关键约定速查（详见 CLAUDE.md）

- 新增源文件必须显式加入 `main/CMakeLists.txt` 的 `SRCS` 和 `INCLUDE_DIRS`。
- 协议 JSON 键名（`名称`、`精度`、`补偿` 等）是线上契约，不要翻译成英文。
- 新任务固定在 CPU1（`create_cpu1_task()`），栈/优先级用 `task_config.h` 的 `LG_*` 常量。
- 大块内存用 `heap_caps` / `app_malloc_prefer_psram`；`QueryResult` 约 4.7 KB，禁止放栈上。
- 数据库 schema 检测靠探测实际列而非 `PRAGMA user_version`；日志 pragma 保持
  `journal_mode=DELETE` + `synchronous=FULL`。
