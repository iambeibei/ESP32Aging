# AGENTS.md

本文件为 AI 编码代理提供本仓库的工作指引：先读「架构总览」建立全局图景，专题细节以
[CLAUDE.md](CLAUDE.md) 与根目录设计文档为准。编码规范权威是 [.ai/coding-standard.md](.ai/coding-standard.md)。

网络与指令专题设计文档（根目录）：

- [BLE_MAC_TARGET_DESIGN.md](BLE_MAC_TARGET_DESIGN.md) —— 蓝牙目标绑定、白名单控制，以及
  明文/加密设备切换时按连接代次隔离加密状态和接收队列。
- [MQTT_EVENT_RETRY_DESIGN.md](MQTT_EVENT_RETRY_DESIGN.md) —— 关键业务事件本次开机内的
  PSRAM FIFO、PUBACK/过期/客户端重建补发、TaskCP有效性及构建配置门控。
- [MQTT_SAMPLE_GAP_DESIGN.md](MQTT_SAMPLE_GAP_DESIGN.md) —— 移除全库采样后台扫描，
  整轮结束后由http_auto按服务器缺号限速补数，会话身份固化及队尾结算。

- [CHILD_NODE_ROOT_LOSS_DESIGN.md](CHILD_NODE_ROOT_LOSS_DESIGN.md) —— 根节点掉线时子节点
  不得异常重启：依赖/保活/故障转移/恢复流程、根因定位（BLE 异常断开的无条件重启）、
  修复与实机验收、代码审查记录。
- [AGING_CMD_PN_GUARD_DESIGN.md](AGING_CMD_PN_GUARD_DESIGN.md) —— start_aging 缺 PN 时
  直接拒绝回 IDLE 并经 MQTT 返回老化异常：消息格式、补偿机制、兼容性。
- [NETWORK_RESTART_DESIGN.md](NETWORK_RESTART_DESIGN.md) —— 一键重启 30s 传播窗、暂停 Mesh、
  根先深层后重启；父连接失败计数与清 Mesh RTC 后的有界自愈。
- [PARTITION_LAYOUT_REVIEW.md](PARTITION_LAYOUT_REVIEW.md) —— 8 MiB 布局、日志扩容至 832 KiB 及
  core dump 移除；仅扩日志分区，更新布局需烧录分区表并安排旧日志卷迁移。
- [MESH_LITE_MIGRATION.md](MESH_LITE_MIGRATION.md) —— 经典 Mesh 迁移 Mesh-Lite 的改造、构建门控与联调。
- [DUAL_NET_DESIGN.md](DUAL_NET_DESIGN.md) —— 根节点以太网（iot_bridge/KSZ8851SNL）+ WiFi STA
  双上行裁决、探测与抢占规则。
- [OTA_SERVER_MIGRATION_DESIGN.md](OTA_SERVER_MIGRATION_DESIGN.md) —— OTA 服务器从静态 OTABin 目录
  迁移到查询串下载接口的过程。
- [MESH_LITE_OTA_INTEGRATION.md](MESH_LITE_OTA_INTEGRATION.md) —— Mesh BINARY 分发集成、ota_cache
  边界行为与实机验收记录。
- [STATUS_LED_DESIGN.md](STATUS_LED_DESIGN.md) —— IO18 PN 灯与 IO17 老化运行灯的纯事件驱动
  规则（点亮/双灭）、代码注入点与验收表。
- [MODIFICATION_NOTES.md](MODIFICATION_NOTES.md) —— 上传队列与采样时刻固化 Value 格式的设计依据。
- `SQLITE_FIRST_BOOT_FIX.md` / `SQLITE_FIX_NOTES.md` / `SQLITE_SCHEMA_FIX_V3.md` /
  `SQLITE_SCHEMA_PROBE_FIX_V4.md` —— SQLite 首启动与 schema 探测/迁移的排障记录。
- `RAM_PSRAM_优化修改计划.md` / `RAM_PSRAM_优化实施记录.md` —— PSRAM 分配改造与实机验收。

## 架构总览（详细论述以 CLAUDE.md 为准）

ESP32-S3（8 MB flash，ESP-IDF **v5.4.4**）电池老化测试网关固件。C（近 c99）+ FreeRTOS + cJSON。
CMake 工程名 `Light_Gateway_UDP` 是历史遗留，当前上行主链路是 MQTT。所有日志、注释和 JSON 协议键名
（`名称`、`精度`、`补偿`、`寄存器地址`…）为中文线上契约，不得英文化。

**启动与任务布局**：`app_main()` 只调用 `app_memory_init()`（安装全局 cJSON PSRAM 分配器）和
`app_task_init()`（[appTask.c](main/Task/appTask.c)，约 5.5k 行，编排中枢：UART/RS485/BLE 接收处理、
MQTT/HTTP JSON 命令解析、老化状态机）。初始化顺序：
`nvs_flash_init → spiffs_init → SqLite_Init → log_storage_mount → SelfRecovery_Init →
status_led_init → app_uart_init → readconfig() → Device_Init()`，再按角色建 mesh/MQTT/老化任务。
业务任务全部经 `create_cpu1_task()` 固定在 **CPU1**（CPU0 留给 Wi-Fi/Bluedroid），栈/优先级取
[task_config.h](main/Task/task_config.h) 的 `LG_*` 常量；单核配置（`CONFIG_FREERTOS_UNICORE`）会编译报错。

**三个 flash 文件系统并存**（各有独立互斥锁，布局见 [partition.csv](partition.csv)）：

| 挂载点 | 分区 | 文件系统 | 用途 |
|---|---|---|---|
| `/spiffs` | `storage` 256 KB | SPIFFS | 配置 JSON（`baseconfig.json`、`device*.json`） |
| `/fatfs` | `data_storage` ~2.4 MB | FATFS+磨损均衡 | SQLite `data.db` |
| `/log` | `log_storage` 832 KiB | FATFS | 环形日志 `records.dat`，上限 3000 条 |

**双轴设备抽象（核心设计）**：每个外接设备是 [Externaldevice.h](main/ConfigData/Externaldevice.h)
的 `Externaldevice`，在两条正交轴上绑定——总线（`CommunicationMode` 字符串决定
`CurrentSendFunc`/`CurrentRecvFunc` 函数指针）× 协议（`ProtoType` 0=SCPI/1=Modbus/2=CAN +
导入解析好的协议对象）。统一入口 `Data_Get_Method()` / `Data_Set_Method()`；末参 `is_name` 决定按
中文 `名称`（1）还是 `id`（0）寻址，弄错表现为 "item not found"，酷似总线故障。
标度换算：物理量 = 原始值 × 精度 ± 补偿；**Modbus 加补偿、CAN 减补偿，同一 JSON 键符号相反**。
三协议族流水线：SCPI（`scpi_dynamic_*`，UART/RS485/BLE）、Modbus（`ModbusJson_*` + `Generate03/06`，
RS485/BLE）、CAN Bluetti（`can_json_parser` + `make_can_*` → TWAI，仅 CAN）。

**老化状态机**：`AgingIdle → AgingStartCheck → AgingDeviceCheck → AgingAction → AgingComplete`，
由 `Aging_Test_Task` 驱动；步骤来自 `device*.json` 内嵌 `AgingSteps`，由
[aging_config.c](main/ConfigData/aging_config.c) 的 `parse_steps_Ex()` 解析，每步含判断条件、
采样列表、前/后置动作。

**数据流**：`app_AgingData_Get_handle` 周期采样构造 `AgingUploadPacket`（PN、RecordId、会话token、
步骤、时间戳和 `Value` 在**采样时刻固化**）。上传先写SQLite `pushed=0`，MQTT入队置1、PUBACK置2。
正常/异常/人工停止关闭本轮，队尾结算后保留结束意图；已有http_auto在联网空闲时查询服务器
GetMissingIdNums，按PN+RecordId+IDNUM精确限速补发，不再重连或每60秒全库扫描。

**持久化**：SQLite schema **v6**，表 `data_cache`，`(pn, seq_no)` 唯一（旧列名 `sn`，启动时
`migrate_legacy_schema_to_v6()` 迁移）；schema 检测探测实际列而非 `PRAGMA user_version`（该嵌入式
SQLite 上不可靠）；日志 pragma 固定 `journal_mode=DELETE` + `synchronous=FULL`，禁止改回。
NVS `SelfRecovery` 保存掉电恢复状态（`aging_valid`、`current_step`、`Deadline`、`CycleIndex`、
序列化老化 JSON）。

**网络**：Mesh-Lite 组网，每个取得 STA IP 的节点独立连接 MQTT（broker IP 与 `SERVER_IP` 同源）。
根节点额外运行 dual_net 有线优先（仅根节点，接管 iot_bridge 的 ETH_WAN netif）。
每次 MQTT 重连均恢复订阅和发送上线通知；完整配置自动上报每次启动成功入队一次，RAM
标志只随设备重启清零，客户端重建/出口切换不清零。上位机显式 `get_config` 请求照常响应。
服务器地址集中在 [service_urls.h](main/Task/service_urls.h)：`_FMT` 模板的 IP 在运行时经
`lg_url_build()` 用 `SERVER_IP`（baseconfig.json）填充——**换 IP 只改 baseconfig.json 并完整 flash，
无需重编译**；端口/路径是编译期常量，改它们才需重新编译。

## 编译与烧录（代理可直接执行）

必须先 source ESP-IDF 环境，否则 `idf.py` 不可用：

```bash
cd /home/wbb/espidf/esp-idf-mesh-lite && . ./export.sh
cd /home/wbb/espidfProject/ESP32Aging
IDF_SKIP_CHECK_SUBMODULES=1 idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig build
```

- 迭代固件用 `idf.py -p /dev/ttyUSB0 app-flash`（保留 SQLite 数据库）。
- 修改 `data/` 下配置文件后必须完整 `idf.py flash`（SPIFFS 镜像随 storage 分区烧录）。
- 其他常用命令：`idf.py -p /dev/ttyUSB0 monitor`（退出 `Ctrl-]`）、`idf.py size` /
  `idf.py size-components`（体积分析）、`idf.py menuconfig`、`idf.py fullclean`。
  `idf.py erase-flash` 会销毁数据库与配置，慎用。
- 分区表为自定义（`partition.csv`）：改分区表必须完整 `idf.py flash`，只烧 app 无效；
  日志分区扩容另需安排旧 FATFS 卷迁移（见 `PARTITION_LAYOUT_REVIEW.md`）。

## 测试（宿主回归，无需设备）

`tests/` 下脚本把真实 C 源码配 mock SDK 在 Linux 上编译运行，**不需要 ESP-IDF 环境或设备**，
需要 C 编译器（cc）、pthread；`test_mesh_ota_binary.py` 另需 OpenSSL 头文件与 libcrypto。
逐个运行（每个脚本即独立用例入口）：

```bash
python3 tests/test_root_uplink.py      # dual_net.c 与 Mesh STA 控制竞态
python3 tests/test_mqtt_uplink.py      # MQTT 上行、交接与 stop 竞态
python3 tests/test_system_restart.py   # 一键重启服务与 Mesh 控制/事件
python3 tests/test_mesh_ota_binary.py  # OTA 准入/校验/重试/供数（不含 Mesh-Lite 库本体）
python3 tests/test_http_protocol.py    # HTTP 累积与目标路由
python3 tests/test_aging_gap.py       # 结束会话缺号补查（另需宿主libsqlite3.so.0）
python3 tests/test_ble_target.py      # 蓝牙目标绑定、白名单和控制生命周期
python3 tests/test_ble_session.py     # 蓝牙连接加密状态隔离、原始收发与鉴权接入
python3 tests/test_psram_memory.py     # PSRAM 分配器、cJSON hooks、TLS 分配
```

ASan/UBSan：`ASAN_OPTIONS=detect_leaks=0 TEST_SANITIZE=1 python3 tests/<脚本>.py`。
采样补查栈帧检查需先完成ESP-IDF编译，再运行 `python3 tests/test_aging_gap_stack.py`；
读取实际Xtensa目标文件，要求14个相关应用函数栈帧不超过512字节，不能替代实机栈水位验收。
注意：`pytest_hello_world.py` 是未修改的 ESP-IDF 模板残留，不测试本工程；无线链路、真实
flash/NVS 掉电行为仍需编译 + 串口实机验证。


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
4. 版本比较：服务器 **>** 本地才升级；相等跳过下载，由 Mesh OTA 服务确认目标文件供数就绪后上报 OTACP 完成，
   更旧**阻止降级**且不生成新完成回执，均返回 `ESP_OK`。
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
  完整镜像再恢复供数，可恢复备用槽或当前运行槽，不能按“另一个槽”盲选。启动恢复不补报
  OTACP，本次启动内的新完成事件仍按原 Seq 做 MQTT 断线重试。已运行目标版本时回到 IDLE，
  可继续升级下一版本。
- 无记录时只在服务器版本和长度与当前已校验镜像一致后建立当前槽供数记录；仅请求元数据和
  响应头，不下载 BIN 正文，自动绑定本身不生成完成通知。同版本 OTA 命令须先恢复精确匹配
  的有效缓存或校验当前运行槽，完成持久化、文件登记和恢复下级请求后才上报 OTACP。
  完成事件保存在同一 `ota_cache/image_v1` 记录（origin=CURRENT），本次启动内断线每 5 秒
  重试，重启后只恢复供数、不补报；准备失败保留原目标有限重试，不提前确认完成。
  擦写前必须先使重叠记录失效并提交；NVS
  错误阻止擦写或启动选择，禁止为 OTA 清空整个 NVS。详细边界见 `MESH_LITE_OTA_INTEGRATION.md`。
- 升级只写另一个 ota 槽，SQLite 数据库（`data_storage` 分区）与配置（`storage` 分区）不受影响。
- 子节点收到 OTA 后若父链路/IP 丢失，取消未完成写入并进入 `WAIT_PARENT`；旧 SDK 传输
  结束、父连接取得 IP 稳定 10s 后随机 0～3s 从头重下原文件。MQTT 单独断线不取消，已就绪
  文件不重下，一键重启优先；等待意图仅在本次启动内保留，SDK 不结束时禁止并发新传输。
- BINARY 请求 10s 无有效首块时，子向直接父节点发送最多 3 次恢复请求，间隔 5～6s；
  父核验文件名/版本/长度及已验证供数、无自身接收任务后合并限频恢复。首块到达即停，
  不重开传输；供数先于恢复通知有效，通知提交失败保留供数但不提前报 OTACP。
  新增 Mesh 内部控制消息要求父子双方更新；不改变库内 Pending 超时及普通 90s 重试。
- OTA 命令入口原子接收标志为 1 时才受理，并立即置 0；忙时提示“命令已触发”。
  完成/无需升级/失败耗尽且旧会话退出后恢复为 1，WAIT_PARENT/自动重试期间保持 0。
  标志恢复不表示允许覆盖未重启的暂存固件；待重启时只查元数据，同版本同文件重发原
  OTACP 完成事实，不下载 BIN；新目标等重启后升级，目标无法核实时不报完成。

## 重启与网络自愈

- `APP/Restart/system_restart.c` 只有一个 CPU1 服务任务；任务初始化失败拒绝命令，不能退回
  MQTT 接收任务内延时重启。首次排程冻结层数和单调时钟截止时间，重复命令不得延长。
- 正常一键重启保留 Mesh RTC；子节点两条故障自愈路径先清 Mesh RTC 再重启，保留项目 RTC
  重启预算（每上电周期 3 次）。禁止为换父清 NVS/老化恢复/OTA 记录。
- 接收计划起抑制网络自愈重启，30s 后禁止应用重新连接 Mesh。STA 控制共用 mutex，事件回调
  只维护状态，不等待控制锁。计划提交与自愈重启必须经过统一仲裁。
- 非主动 STA_DISCONNECTED 计一次失败，LOST_IP 不重复计数；GOT_IP 连续在线 10s 才清零，
  断开后结束在线窗。具体运行语义及接收回执见 `NETWORK_RESTART_DESIGN.md`。

## 已知陷阱速查（完整列表与根因见 CLAUDE.md）

修改周边代码前先自行核实，勿盲目“修复”：

1. `readconfig()` 的 `char file_names[6][64]` 与 `spiffs_get_file_names` 的 `max_files=10`
   不匹配，7–10 个配置文件即越界。
2. `g_db1_result`（SqLite.h）是无保护共享全局量，跨任务会串数据；应改用堆上 `QueryResult`。
3. `cleanup_database_files()` 故意无调用方，仅供人工维修，绝不接进初始化/错误处理路径。
4. 每条日志都写一次 NVS（`log.c` 持久化 `LogIndex`），是显著 flash 磨损来源。
5. BLE 加密状态及接收队列按连接代次隔离，超长报文明确拒绝；不得恢复全局 `IsEnc` 或取模截断。
6. CAN 接收用 `portMAX_DELAY` 永久阻塞（其他协议族 1000 ms）；CAN 设备无响应会挂起调用方。
7. `QueryResult` 按值传递约 4.7 KB，数组类 API memset 可达约 47 KB，必须堆分配。
8. `Use_Write_Flag`（RS485）/`Use_Send_Flag`（CAN）在收发之间无锁保护。

## 关键约定速查（详见 CLAUDE.md）

- 新增源文件必须显式加入 `main/CMakeLists.txt` 的 `SRCS` 和 `INCLUDE_DIRS`（不自动 glob）；
  在改某文件前先查 `SRCS` 确认它是否参与编译。
- 协议 JSON 键名（`名称`、`精度`、`补偿` 等）是线上契约，不要翻译成英文。
- `readconfig()` 按**文件名子串匹配**分发：含 `config` → 基础配置，含 `device` → 外接设备，
  文件名承载语义，不能随便改。
- 新任务固定在 CPU1（`create_cpu1_task()`），栈/优先级用 `task_config.h` 的 `LG_*` 常量。
- 大块内存用 `heap_caps` / `app_malloc_prefer_psram`；`QueryResult` 约 4.7 KB，禁止放栈上。
- MQTT/UART/RS485 大数据队列用 `Tool/app_queue`（控制块内部 RAM、数据区 PSRAM）；
  初始化回滚用 `app_psram_queue_delete()`。
- 服务器 URL 只经 `service_urls.h` 的 `_FMT` 模板 + `lg_url_build()` 生成，禁止把模板
  当 URL 直接用；`lg_url_build()` 必须在 `readconfig()` 之后调用。
- 数据库 schema 检测靠探测实际列而非 `PRAGMA user_version`；日志 pragma 保持
  `journal_mode=DELETE` + `synchronous=FULL`。
- 命名风格本就不统一（`SqLite`/`sqllib1` 等，历史原因），贴合所在文件局部风格，勿强行统一。
- 构建依赖 `CONFIG_MESH_LITE_ENABLE=1`：`main/CMakeLists.txt` 有 FATAL_ERROR 门控，
  关掉 Mesh-Lite 直接构建失败（见 `MESH_LITE_MIGRATION.md`）。
