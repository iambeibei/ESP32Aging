# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

电池**老化（老化）测试网关**的 ESP-IDF 固件 —— "Light_Gateway"。设备驱动被测电池包执行多步充放电老化程序，
从电池包及外接仪表采集测量值，将采样数据缓存到 flash 上的 SQLite，并通过 MQTT 上传。
同时运行 ESP-WIFI-MESH 网络，由根节点将子节点桥接到云端。

- **目标芯片：** ESP32-S3（8 MB flash），ESP-IDF **v5.4.4**
- **CMake 工程名：** `Light_Gateway_UDP`（历史遗留名称；当前上行主链路是 MQTT）
- **语言：** C（接近 c99）、FreeRTOS、cJSON
- **界面语言为中文：** 所有日志、注释和 JSON 协议键名（`名称`、`精度`、`补偿`、
  `寄存器地址`、`长度`、`类型`）都是中文。不要把这些"翻译"成英文标识符 ——
  它们是线上/协议契约。

## 编译与烧录

必须先 source ESP-IDF 环境（它会设置 `IDF_PATH`、工具链、Python 环境）：

```bash
cd /home/wbb/espidf/esp-idf && . ./export.sh        # 或：get_idf
cd /home/wbb/espidfProject/Light_Gateway_sqlite_fixed_v4
```

`IDF_PATH` 在普通 shell 中**未设置** —— 跳过 `export.sh` 这一步，`idf.py` 会直接失败。

| 操作 | 命令 |
|---|---|
| 编译 | `idf.py build` |
| 全量清理重编译 | `idf.py fullclean && idf.py build` |
| 编译 + 烧录 | `idf.py -p /dev/ttyUSB0 flash` |
| 仅烧录 app（保留数据） | `idf.py -p /dev/ttyUSB0 app-flash` |
| 串口监视 | `idf.py -p /dev/ttyUSB0 monitor`（退出：`Ctrl-]`） |
| 烧录 + 监视 | `idf.py -p /dev/ttyUSB0 flash monitor` |
| 配置菜单 | `idf.py menuconfig` |
| 擦除 flash（会销毁数据库/配置） | `idf.py -p /dev/ttyUSB0 erase-flash` |
| 体积分析 | `idf.py size` / `idf.py size-components` |

注意事项：

- **分区表为自定义** —— `partition.csv`（`CONFIG_PARTITION_TABLE_CUSTOM=y`）。修改后必须重新编译并
  重新烧录分区表；只烧 app 是不够的。
- `data/` 会在编译期被打成 **SPIFFS 镜像**（`storage` 分区），由根 `CMakeLists.txt` 中的
  `spiffs_create_partition_image(storage data FLASH_IN_PROJECT)` 完成。修改 `data/` 下的配置文件后需要
  **完整执行 `idf.py flash`**，不能用 `app-flash`。
- **本项目没有测试套件。** `pytest_hello_world.py` 是未修改的 ESP-IDF 模板残留，并不测试本工程。
  验证手段是编译 + 上机串口观察。
- `sdkconfig.ci` 是空文件；`CMakePresets.json` 定义了 `default`/`production` 两个 preset，指向
  `build/<preset>/`，但实际生效的构建产物在顶层 `build/`。
- 迭代固件时用 `app-flash`，避免擦掉正在测试用的 SQLite 数据库。

## 架构

### 启动 / 任务布局

`main.c` 调用 `app_task_init()`（[appTask.c:5421](main/Task/appTask.c#L5421)），这是唯一的初始化和建任务入口。
顺序有讲究：

`nvs_flash_init` → `spiffs_init`（配置 FS）→ `SqLite_Init`（老化数据库）→ `log_storage_mount`
（日志 FS）→ `SelfRecovery_Init`（NVS 恢复状态）→ `app_uart_init` → `readconfig()` →
`Device_Init()` → 可选的 mesh/MQTT/老化任务。

三个独立的 flash 文件系统并存，各自有独立互斥锁：

| 挂载点 | 分区 | 文件系统 | 用途 |
|---|---|---|---|
| `/spiffs` | `storage`（256 KB） | SPIFFS | 配置 JSON（`baseconfig.json`、`device*.json`） |
| `/fatfs` | `data_storage`（约 2.4 MB） | FATFS + 磨损均衡 | SQLite `data.db` |
| `/log` | `log_storage`（320 KB） | FATFS | `records.dat` 环形日志 |

**任务固定在 CPU1** 上运行（双核 FreeRTOS）；CPU0 留给 Wi-Fi/Bluedroid。所有应用任务都通过
`create_cpu1_task()`（[appTask.c:174](main/Task/appTask.c#L174)）创建，栈大小和优先级取自
[task_config.h](main/Task/task_config.h)。**不要把新任务绑到 core 0** ——
`task_config.h` 里有一个编译期 `#error`，一旦 `CONFIG_FREERTOS_UNICORE` 被设置就会报错。

`appTask.c` 约 5.5k 行，是编排中枢：UART/RS485/BLE 接收处理、MQTT 与 HTTP 的 JSON 命令解析、
以及老化状态机都在这里。建议先读它的函数表
（`grep -n "^[a-zA-Z_].*(" main/Task/appTask.c`），而不是逐行通读。

### 双轴设备抽象（核心设计）

每个外接设备是一个 `Externaldevice`（[Externaldevice.h](main/ConfigData/Externaldevice.h)），
在**两个相互独立的轴**上完成绑定，两者都在配置阶段确定：

- **走哪条总线** —— `CommunicationMode` 字符串（"RS485"、"Bluetooth"…）在
  [appTask.c:3838](main/Task/appTask.c#L3838) 处决定 `CurrentSendFunc` / `CurrentRecvFunc` 函数指针。
- **用哪种协议** —— `ProtoType`（0=SCPI，1=Modbus，2=CAN）加上导入后解析好的协议对象
  （`scpi_protocol` / `modbusprotocol` / `canprotocol`）。

之后通用 I/O 就统一走 `Data_Get_Method()`
（[appTask.c:4820](main/Task/appTask.c#L4820)）和 `Data_Set_Method()`
（[appTask.c:3932](main/Task/appTask.c#L3932)）：解析条目 → 组帧 → `CurrentSendFunc` →
`CurrentRecvFunc` → 解码成 `double`。

**`is_name` 约定：** 每个通用入口的最后一个参数都是 `uint8_t is_name`。
`1` → 按中文 `名称` 查协议条目；`0` → 按 `id` 查。外接/只读设备按名称寻址，老化被测件按稳定的
数字 ID 寻址。弄错这里会报 "item not found"，表现得很像总线故障 —— 出问题先查它。

各协议族的流水线：

| 协议族 | 解析 | 查找 | 执行 | 总线 |
|---|---|---|---|---|
| SCPI | `scpi_dynamic_import_json` | `scpi_dynamic_find_by_name/_id` | `scpi_dynamic_build_command`（替换 `<NRf>`/`<NP1>`） | UART、RS485、BLE |
| ModBus | `ModbusJson_Parse` | `ModbusJson_FindItemByName/_ByID` | `Modbus_Generate03`（读）、`Generate06`（写单寄存器） | RS485、BLE |
| CAN（Bluetti） | `CanJson_Parse` | `CanJson_FindItemByName/_ByID` | `make_can_*_transfer_by_nameorid` → TWAI | 仅 CAN |

标度换算规则为 `物理量 = 原始值 * 精度 (+/−) 补偿`。**注意两个协议族的符号是相反的**：
Modbus 用 `+ compensation`，CAN 用 `− compensation`（参见
[can_json_parser.c:327](main/Protocol/Can/can_json_parser.c#L327) 与
[modbus_json_parser.c:266](main/Protocol/ModBus/modbus_json_parser.c#L266)）——
同一个 JSON 键，含义相反。写操作走反向的 `PhysicalToRaw`。

### 配置

- `data/baseconfig.json` —— Wi-Fi（主 + 备）、mesh ID/PSK、`DEVICE_ID`（兼容旧字段 `UDP_ID`）、
  `SERVER_IP`、`UDP_Port`（兼作**网络使能开关**）、`AgingNumber`。
- `data/device*.json` —— 外接设备及其内嵌的 `AgingSteps` 程序。
- `readconfig()`（[appTask.c:1043](main/Task/appTask.c#L1043)）枚举 `/spiffs`，并依据**文件名的子串匹配**
  进行分发：含 `config` → 基础配置；含 `device` → 外接设备。文件名是承载语义的，不能随便改。

### 老化状态机

`AgingProcessState`（[appTask.h](main/Task/appTask.h)）：`AgingIdle → AgingStartCheck →
AgingDeviceCheck → AgingAction → AgingComplete`，由 `Aging_Test_Task`
（[appTask.c:4310](main/Task/appTask.c#L4310)）驱动。`AgingDataAcquisitionMode` 标记每次采样的工况
（静置/充电/放电）。步骤来自 [aging_config.c](main/ConfigData/aging_config.c) 中的 `parse_steps_Ex()`，
每步包含判断条件（时间/SOC）、采样列表以及前置/后置动作列表。

### 数据流

1. `app_AgingData_Get_handle` 按周期采样，构造 `AgingUploadPacket`（SN、步骤、时间戳和 `Value` JSON
   在**采样时刻即被固化**，因此慢速上传者不会把不同步骤/SN 的数据混在一起），并以 `pushed=0` 插入。
2. `app_AgingData_Upload_handle` 出队、发布，然后把 `pushed` 置为 `1`。
3. `QueryLatestRecordBySNAndPushState` 驱动未上传记录的补发。

当前上传队列与 `Value` 格式的设计依据见 [MODIFICATION_NOTES.md](MODIFICATION_NOTES.md)。

### 持久化

**SQLite**（`/fatfs/data.db`，schema **v5**，表 `data_cache`）：`local_id` 是内部主键；
`(sn, seq_no)` 唯一，因此第二个电池包 IDNUM 从 0 重新开始也不会覆盖第一个的数据。
`pushed` 标记上传完成状态。容量限制（1200 条记录、512 KB 最小剩余空间）只在插入路径上生效，
且每插入一条淘汰一行 —— 优先淘汰最旧的 `pushed=1` 记录。

这一层来之不易的约束（分散记录在 `SQLITE_*.md` 中）：

- `PRAGMA user_version` 在该嵌入式 SQLite 上**不可靠**。schema 检测是探测**实际列**，而不是读版本
  pragma。不要把 `sqlite_schema_is_v5()`"简化"成读 `user_version`。
- `ALTER TABLE ... RENAME` 在这里会失败；迁移采用的是事务性的临时表拷贝/重建。
- 日志 pragma 被刻意设为 `journal_mode=DELETE` + `synchronous=FULL`（从 MEMORY/NORMAL 改过来，
  用于修复 `disk image is malformed` 损坏问题）。不要为了速度改回去。
- `cleanup_database_files()` **故意不设调用方** —— 它会删除数据库，仅供人工维修使用。
  绝不把它接进初始化或错误处理路径。

**NVS**（`SelfRecovery`）保存掉电恢复状态：`aging_valid`、`current_step`、`Deadline`、`CycleIndex`
以及序列化后的老化 JSON。

## 已知陷阱

以下是在梳理代码时发现的真实缺陷 —— 修改周边代码前请先自行核实：

1. **`readconfig()` 缓冲区不匹配**（[appTask.c:1058](main/Task/appTask.c#L1058)）：声明的是
   `char file_names[6][64]`，但 `spiffs_get_file_names` 会按 `SPIFFS_FILE_NAME_MAX_LEN` = 64 的步长写入，
   其上限由 `max_files` = **10** 控制。当 `/spiffs` 下有 7–10 个文件时就会数组越界。
   应声明为 `[MAX_EXTERNAL_DEVICE_COUNT][SPIFFS_FILE_NAME_MAX_LEN]` 并传入匹配的 `max_files`，
   或直接使用 `SPIFFS_MAX_FILE_LIST_COUNT`。
2. **`g_db1_result` 是无保护的共享全局量**（[SqLite.h:31](main/DataStorage/SqLite/SqLite.h#L31)）。
   互斥锁保护的是 DB 调用，而不是缓冲区的生命周期；`query_db1_*_to_global()` 填充它，调用方之后才使用。
   两个任务交错执行会互相发布对方的负载数据。应改用栈/堆上的 `QueryResult`
   （`appTask.c:5349` 处已经这么做了）。
3. **日志环形区大于其所在分区**：`MAX_RECORDS 3000 × RECORD_SIZE 128` = 384 KB，而 `log_storage`
   分区只有 320 KB（[log.h:7](main/DataStorage/Log/log.h#L7)），且文件从不预分配 ——
   一旦超出可用空间，写入就会开始失败。
4. **每条日志都写一次 NVS**（[log.c:143](main/DataStorage/Log/log.c#L143)）：每次写日志都会持久化
   `LogIndex` —— 这是一个显著的 flash 磨损来源。
5. **BLE 接收截断**：`copy_len = ble_data.data_len % (max_len + 1)`
   （[Ble_control.c:189](main/Communication/Ble_control/Ble_control.c#L189)）是回绕而不是钳位；
   UART/RS485 的钳位是正确的。300 字节的通知写入 256 字节缓冲区会静默返回 44 字节。
6. **CAN 读取会永久阻塞**：CAN 接收用 `portMAX_DELAY`，而 SCPI/Modbus 用的是 1000 ms ——
   CAN 设备无响应会导致调用方无限挂起。
7. **CAN 组帧逻辑不一致**：`can_json_parser.c` 中的构造代码与 `appTask.c` 实际走的路径在负载布局
   （`data[0]=pkt` 还是 `data[0]=data_type`）和字节序（`fill_start_len` 大端 vs
   `can_send_read_start` 小端）上存在分歧。以实际运行的路径为准。
8. **`QueryResult` 按值传递约 4.7 KB。** 数组类 API 会执行
   `memset(out_array, 0, sizeof(QueryResult) * max_count)` —— 10 个元素就是约 47 KB。
   这类缓冲区必须在堆上分配，绝不能放栈上。
9. **静态标志无同步保护**：`Use_Write_Flag`（RS485 的 -5 "发送成功但无应答"）和 `Use_Send_Flag`
   （CAN）在发送/接收这一对操作之间没有加锁。

## 约定

- **新增源文件必须显式加入 `main/CMakeLists.txt`** —— 同时加进 `SRCS` 和 `INCLUDE_DIRS`；
  源码树不会被自动 glob。每个源文件的目录都要在 `INCLUDE_DIRS` 里有对应项。
- **命名风格本就不统一（历史原因）**（`SqLite` vs `sqllib1`，`Data_Set_Method` vs `Uart.c`）。
  请贴合所在文件的局部风格，而不是强行统一。
- `main.c` 刻意保持近乎空白；初始化逻辑应放在 `app_task_init()` 中。
- 栈大小与优先级应以 `LG_*` 常量形式放在 `task_config.h` 中。
- 部分代码虽已失效但仍有上下文价值：`mesh_UartCommand`
  （[appTask.c:1092](main/Task/appTask.c#L1092)）被标注为未使用（"现在不使用"），`udp_client.c`
  **不在** `SRCS` 中（因此活跃的 uplink 是 `GetProto.c`/MQTT），`unmount_fatfs_storage()` 没有调用方。
  在假定某段代码会运行、或动手"修复"它之前，先查 `SRCS`。
- 大块内存分配优先使用 `heap_caps` / `app_malloc_prefer_psram`（见 `Tool/app_mem.c`）；
  此 ESP32-S3 带 PSRAM。
