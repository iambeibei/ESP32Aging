# ESP32-S3 RAM / PSRAM 优化修改计划（备份）

> 历史计划快照，以下“尚未实施”等状态保留备份时原文。用户随后提供 BLE＋HTTPS 内存不足日志并
> 授权优化，2026-10-22 已实施其中的安全迁移及 TLS / Bluedroid 配置，详见
> [RAM_PSRAM_优化实施记录.md](RAM_PSRAM_优化实施记录.md)。未裁剪或外置任务栈，实机验收待完成。

- 整理日期：2026-10-22。
- 状态：**代码识别与计划备份已完成，尚未实施本计划。用户编辑确认后，再通知继续修改。**
- 依据：当前工作区代码、`build/mesh-lite/sdkconfig`、最新 ELF / MAP 和 ESP-IDF v5.4.4 源码。
- 本文件是可编辑的讨论稿，不代表以下所有项目都要同时实施。

## 1. 目标与范围

保持 ESP-Mesh-Lite、每节点独立 MQTT、现有协议与业务行为，通过合理使用 PSRAM 释放内部 RAM，
改善并发运行时的分配余量与连续空闲块，避免内存不足造成任务创建失败或崩溃。

本轮优先顺序：

```text
诊断与分配失败保护
  → MQTT 接收队列
  → UART / RS485 / BLE 应用缓冲
  → JSON 分配与回退预算
  → 任务栈测量及裁剪
  → 独立评估 SDK 的 PSRAM 配置
```

本计划不修改 MQTT Topic、JSON 字段、QoS 分类、队列深度、老化流程、OTA 镜像校验和重启时序。
不通过降低消息容量来获得内存收益，不清理数据库或用户配置。

## 2. 当前判断与依据

### 2.1 综合判断

ESP32-S3＋PSRAM 方案可以继续使用。从现有代码预算看，**首要压力是内部 RAM**，不能仅凭
PSRAM 剩余容量或编译通过判断高负载运行一定够用。

目前缺少最新固件在完整并发负载下的内部堆最低余量、最大连续空闲块和全任务栈水位，
因此尚不能确认所有工况均有足够余量，也不能据此把现场 MQTT 断连直接归因于内存不足。

21 个节点各自运行一个 MQTT 客户端。根节点主要承受转发负载，并不是在根上创建 21 个
MQTT 客户端；根与子节点应分别测量。

### 2.2 最新构建的静态预算

分析对象：

- `build/mesh-lite/Light_Gateway_UDP.elf`
- `build/mesh-lite/Light_Gateway_UDP.map`
- `build/mesh-lite/sdkconfig`

| 项目 | 识别结果 | 解释 |
|---|---:|---|
| DIRAM 共享内部 RAM 区域静态占用 | 160259 B，约 156.5 KiB | 包括驻留代码及静态数据 |
| 其中 `.data`＋`.bss` | 53740 B，约 52.5 KiB | 不包括启动后动态分配 |
| 上述共享区域链接余量 | 181501 B，约 177.2 KiB | **不是运行时空闲堆** |
| 已外置的 `.ext_ram.bss` | 26052 B，约 25.4 KiB | 已包含部分 SDK 静态数据 |
| MQTT 接收队列数据区 | 34880 B，约 34.1 KiB | 当前由普通 FreeRTOS 队列创建接口分配在内部 RAM |
| 根节点应用任务栈估算 | 56832 B，约 55.5 KiB | 对应功能启用时的应用任务合计 |
| 子节点老化期间应用任务栈估算 | 73216 B，约 71.5 KiB | 不包含 BLE / RS485 / CAN 分支 |

任务栈估算不包括 SDK 任务、TCB、通信分支，以及 `net_init`、SNTP 等初始化任务的临时重叠。
实际并存任务取决于配置和运行阶段，不能将该表当作已经测得的峰值。

链接余量还需要承担任务栈、队列、Wi-Fi、Mesh、网络连接及其他运行期分配。
Flash 分区剩余空间、链接报告的 IRAM 布局比例，均不能替代运行时堆指标。

### 2.3 当前生效配置

| 配置 | 当前值 / 状态 | 含义 |
|---|---|---|
| PSRAM | 启用、QUAD、40 MHz、类型 AUTO | 实际容量仍需开机日志或运行时 API 确认 |
| `CONFIG_SPIRAM_USE_MALLOC` | 启用 | 普通 malloc 可使用 PSRAM，但分配位置受策略影响 |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | 16384 B | 不超过阈值的普通分配优先内部 RAM |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` | 32768 B | 为明确要求内部 RAM 的分配保留池，并非永远有 32 KiB 空闲 |
| `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` | 启用 | 允许指定外部栈；当前普通建任务接口仍使用内部栈 |
| `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST` | 未启用 | 可后续单独评估 |
| `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | 未启用 | 可后续单独评估 |
| `CONFIG_MQTT_OUTBOX_DATA_ON_EXTERNAL_MEMORY` | 启用 | outbox 消息负载已经外置 |
| `CONFIG_FATFS_ALLOC_PREFER_EXTRAM` | 启用 | 文件系统相关分配已配置优先外部内存 |

**8 MB Flash 不代表有 8 MB PSRAM。**实际 PSRAM 容量不能根据工程注释或 Flash 型号推断。

### 2.4 已经使用 PSRAM 的部分

- SQLite 核心的 malloc / realloc 在当前组件实现中直接使用 `MALLOC_CAP_SPIRAM`。
- MQTT outbox 的消息负载直接使用 PSRAM；64 KiB 字节预算不是整个 MQTT 模块的堆上限。
- MQTT 32 KiB ACK 位图、命令去重表、发布跟踪表走 PSRAM 优先分配。
- HTTP 完整响应、协议结构、老化配置、数据库查询结果和部分 JSON 缓冲走 PSRAM 或优先路径。
- 链接结果表明部分 BLE、Wi-Fi、lwIP 静态 BSS 已经外置。

“优先 PSRAM”不等于“始终在 PSRAM”：`app_mem.c` 等封装允许分配失败后回退。
现有日志中写“PSRAM”也不能替代对实际指针位置的核验。

SQLite 当前 `cache_size=-64` 是页缓存目标，不是 SQLite 整体内存上限；HTTP 队列最多 3 份、
每份响应上限 16 KiB，也未包含在途请求、JSON 解析树及安装后的协议结构。

## 3. 第一阶段：诊断与分配失败保护（P0）

### 3.1 补齐内存诊断

基于 `main/Task/appTask.c` 现有 `show_free_heap()` 和任务栈输出扩展：

- 内部 RAM：当前空闲、历史最低空闲、最大连续空闲块。
- PSRAM：实际容量、当前空闲、历史最低空闲、最大连续空闲块。
- 必要的 DMA / 内部专用分配余量，避免只查看总 heap。
- 关键分配失败的大小、能力标志、模块、运行阶段。
- 关键大块分配实际落在内部还是外部，核验回退是否发生。
- 使用限频串口诊断，诊断日志不递归触发 MQTT 发布。

采样阶段至少包括：

1. PSRAM 初始化、数据库及配置加载完成。
2. Mesh 入网、MQTT 上线与完整配置上报完成。
3. BLE 初始化与连接、最大协议文件解析和安装。
4. 老化采样、入库、补传与日志同时运行。
5. 根节点 OTA 下载、子节点 BINARY 接收、父节点供数。
6. 重启后的 OTA 记录恢复及反复断网恢复。

### 3.2 补齐任务栈水位

保留现有任务水位输出，并覆盖当前遗漏的任务：

- `mqtt_service`、`mqtt_backfill`。
- `mesh_ota_cmd`、根节点 `mesh_ota_dl`。
- `restart_service`。
- `mesh_health`、`mesh_failover`、`net_monitor`。
- 实际启用的 BLE / UART / RS485 / CAN 任务；必要时观察 SDK 任务。

取得有效任务句柄或使用合适的任务统计接口；诊断不得访问已删除任务的陈旧句柄。

### 3.3 修复明确的分配失败缺陷

`main/DataStorage/SqLite/SqLite.c` 当前约 2432 行：

```c
g_db1_result = app_malloc_prefer_psram(sizeof(QueryResult));
/* 当前代码未判空就 memset。 */
memset(g_db1_result, 0, sizeof(QueryResult));
```

计划增加判空与明确失败处理，核验后续使用点，避免内存不足演变为 NULL 指针崩溃。
同时检查本轮涉及的分配出口、资源释放和初始化重试，禁止失败后泄漏或重复创建。

## 4. 第二阶段：大队列与应用缓冲外置（P1）

### 4.1 MQTT 接收队列——优先实施

修改 `main/APP/MQTT/mqtt_app.c`：

- 保留 32 个槽位及 `mqtt_data_msg_t` 的 Topic / Payload 同队结构。
- 队列控制块保留内部 RAM，数据存储区显式放入 PSRAM。
- 使用适合的静态队列创建方式，明确存储区所有权与模块生命周期。
- 初始化幂等，重连复用；失败时释放本次未完成初始化的资源，不泄漏、不重复创建。
- 不改成复杂指针池，不缩短队列，不改变接收顺序、超时和溢出语义。
- 大块数据区分配失败不能无记录地占用大量内部 RAM；失败 / 回退策略应明确。

预期释放内部 RAM 数据区 **34880 B，约 34.1 KiB**，不含小额控制结构与分配开销。

### 4.2 UART / RS485 应用层缓冲

修改：

- `main/Communication/Uart/Uart.c`
- `main/Communication/RS485/RS485.c`

候选项：

| 项目 | 两模块合计 | 计划 |
|---|---:|---|
| 应用数据队列，分别为 3 槽 | 6156 B，约 6 KiB | 控制块内部，数据区外置；只有对应驱动启用后才产生动态收益 |
| `s_rx_acc_buf`、`s_queue_cache`、`s_read_cache` | 6152 B，约 6 KiB | 核验访问路径后使用外部 BSS 或显式外部分配 |

保留 UART 驱动内部缓冲、事件队列和互斥控制结构。必须核验候选缓存仅由任务访问，
没有 ISR、缓存关闭期间或未经验证的 DMA 直接访问；保持现有互斥及数据复制规则。

### 4.3 BLE 应用数据

修改 `main/Communication/Ble_control/Ble_control.c`：

- `s_scanned_devices[50]`：3650 B，约 3.6 KiB。
- `a_data[1200]`：1200 B，约 1.2 KiB。
- 核验后将应用扫描列表及拼包缓存迁入 PSRAM，保留扫描容量与协议行为。
- 接收负载等任务上下文业务分配，按访问链路评估 PSRAM 优先。
- 保留现有锁、所有权、队列满时释放规则和回调对象生命周期。
- 不直接移动 Bluetooth 控制器内部缓冲或 SDK 控制结构。

### 4.4 首批收益口径

上述候选数据区合计约 **52038 B，50.8 KiB**。这是全部相关功能启用且全部候选均通过
访问限制核验时的迁移预算，不是已经取得的运行时收益。

其中 MQTT 的约 34.1 KiB 最明确，其余收益取决于启用功能和最终核验结果；迁移后分别
对比链接静态占用和同负载下内部堆余量，避免重复计算。

## 5. 第三阶段：JSON 与内存回退策略（P2）

当前小于等于 16 KiB 的普通分配优先内部 RAM，JSON 树节点、字符串和短期缓冲容易造成
内部堆压力与碎片；协议最终结构外置并不代表解析过程中所有临时对象都已外置。

计划：

1. 梳理 `spiffs_config.c`、`Json_data.c`、`appTask.c` 等业务缓冲，明确 PSRAM 优先分配。
2. 评估一次性设置 `cJSON_InitHooks()`；若采用，必须在任何 JSON 使用及相关任务启动前完成，
   不能只放在已经创建 UART / 配置解析任务之后，也不能运行中切换全局 hooks。
3. 核验 Mesh-Lite 等使用相同 cJSON 库的调用方兼容性，以及所有释放方式。
4. 覆盖解析、节点创建、序列化、打印分配失败，保证完整清理，不发送残缺 JSON。
5. 为大块业务数据制定显式 PSRAM / 内部回退预算，避免外部不足时突然挤占内部关键资源。
6. PSRAM 优先封装统一诊断口径，不以函数名代替实际分配位置。

首批显式迁移时保留全局 malloc 阈值 16384 B，不同时改变 SDK 的普通分配策略。
如后续需要降低阈值，作为独立配置实验比较，不能直接把 1 KiB 等新值当作未经测量的最终值。

## 6. 第四阶段：任务栈测量与裁剪（P2）

- 先取得最复杂业务、失败重试、完整配置上报、最大 JSON 和 OTA 工况的最低栈余量。
- 优先检查多个 8 KiB 任务栈，按实际最深调用链保留余量后逐项缩减，不统一按比例裁剪。
- 大局部数组优先改为有明确所有权、同步和释放路径的 PSRAM 业务缓冲。
- 保持 CPU1 约定和 `task_config.h` 集中配置。
- **首轮不将所有业务任务栈整体外置。**OTA、NVS、文件系统、重启、ROM / SDK 调用链必须
  核验 Flash 缓存关闭期间是否仍会访问栈，证明安全后才能单独试验外部栈。

开启 `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` 并不会自动改变现有
`xTaskCreatePinnedToCore()` 的内部栈分配。

## 7. 第五阶段：独立评估 SDK 配置（P3）

按单项实验推进，避免同时修改后无法定位回归：

| 配置 | 目的 | 验证重点 |
|---|---|---|
| `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST` | Bluedroid 可外置动态内存优先 PSRAM | BLE 建链、通知、重连、Wi-Fi 共存及延迟 |
| `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | 支持外置的网络分配优先 PSRAM | Mesh 转发、并发上下行、MQTT、OTA 吞吐及抖动 |
| 全局 malloc 阈值调整 | 减少普通小块分配对内部 RAM 的占用 | SDK 热路径、碎片、性能及内部保留池压力 |

配置确需变更时，同步更新默认配置及实际构建配置，记录前后差异。

DMA 描述符、ISR 数据和关键控制结构保留内部；其他 DMA 数据是否可外置按具体驱动及
对齐要求核验，不能统一迁移。当前普通 HTTP / MQTT TCP 场景不优先调整 TLS 内存配置。
不为释放 RAM 首先降低 Wi-Fi / Mesh 缓冲容量，也不首先关闭影响吞吐的 IRAM 优化。

## 8. 预计涉及文件

| 文件 / 模块 | 计划内容 |
|---|---|
| `main/Task/appTask.c`、`main/Task/appTask.h` | 内存与任务诊断、初始化顺序、业务缓冲 |
| `main/Tool/app_mem.c`、`main/Tool/app_mem.h` | 明确分配位置、回退及诊断策略 |
| `main/APP/MQTT/mqtt_app.c` | 接收队列存储区外置，保留消息封装 |
| `main/Communication/Uart/Uart.c` | 应用队列与静态数据缓冲 |
| `main/Communication/RS485/RS485.c` | 应用队列与静态数据缓冲 |
| `main/Communication/Ble_control/Ble_control.c` | 应用扫描列表及拼包缓存 |
| `main/DataStorage/SqLite/SqLite.c` | 分配判空；不重复迁移已经外置的 SQLite 核心 |
| `main/DataStorage/SPIFFS/spiffs_config.c`、`main/Tool/Json_data.c` | 文件 / JSON 临时分配 |
| `main/main.c` | 仅在采用全局 JSON hooks 时调整早期初始化 |
| `main/Task/task_config.h` | 有实测依据后调整任务栈 |
| `sdkconfig.defaults`、`build/mesh-lite/sdkconfig` | 后续单项配置实验 |
| `tests/`、相关设计文档 | 分配位置、失败路径及原业务回归 |

具体修改范围按最终选定阶段确定，不直接修改 Mesh-Lite 闭源库或外部 SDK 源码。

## 9. 验证与验收

### 9.1 宿主与构建

- 检查队列创建失败、外部内存不足、初始化重试和资源释放，不泄漏、不重复释放。
- 验证迁移后队列容量、顺序、Topic / Payload 配对、超时及溢出行为保持一致。
- 验证 JSON 分配失败时正确清理、拒绝不完整输出。
- 更新必要的宿主替身及分配位置断言，保留现有 MQTT / HTTP / OTA / 重启 / 出口回归。
- 对涉及生命周期和并发的检查运行 ASan / UBSan；宿主替身不能证明实际物理内存位置。
- 执行指定 ESP-IDF 构建及前后 size / MAP 对比。

```bash
cd /home/wbb/espidf/esp-idf-mesh-lite && . ./export.sh
cd /home/wbb/espidfProject/ESP32Aging
IDF_SKIP_CHECK_SUBMODULES=1 idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig build
IDF_SKIP_CHECK_SUBMODULES=1 idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig size size-components
```

### 9.2 实机工况

| 工况 | 主要观察 |
|---|---|
| 根节点＋20 子节点正常运行 | 根转发压力、各节点内部最低余量及连续块 |
| 批量上线及完整配置上报 | MQTT 队列、JSON 峰值、分配失败 |
| BLE 老化＋入库＋补传＋日志 | 子节点堆 / 栈峰值、通信稳定性 |
| 多设备协议获取、解析与安装 | HTTP 响应、JSON 树和最终协议同时存在时的峰值 |
| 根 OTA 下载、子接收、逐层供数 | Flash 操作、缓存限制、网络缓冲与 SHA 调用栈 |
| 反复断网 / 重连 / 一键重启恢复 | 任务与队列复用、OTA 供数恢复、泄漏趋势 |
| 多个完整老化周期 | 内部 / PSRAM 无持续下降，连续块不持续恶化 |

### 9.3 通过标准

- 相同负载下，内部 RAM 释放收益与迁移预算基本对应，最大连续块没有恶化。
- 无未处理的分配失败、NULL 解引用、栈溢出或缓存访问异常。
- 内部与 PSRAM 的低水位、最大连续块、关键任务栈余量满足各自最大申请和调用链要求。
- 现有业务行为和数据完整性不回归，不依靠静默丢弃消息换取余量。
- 不能只以“编译通过”“总 heap 很大”或“PSRAM 还有空间”作为通过依据。
- 实机安全余量的最终门槛在取得 P0 数据后确定，不预先把一个固定空闲值当作全部工况保证。

## 10. 执行勾选与待调整项

### 已完成

- [x] 核对最新代码、实际 sdkconfig、ELF / MAP 与静态预算。
- [x] 识别已外置部分及内部 RAM 候选。
- [x] 保存本修改计划备份。

### 尚未实施

- [ ] P0：诊断与分配失败保护。
- [ ] P1：MQTT 接收队列外置。
- [ ] P1：UART / RS485 应用队列及缓冲外置。
- [ ] P1：BLE 应用列表及缓冲外置。
- [ ] P2：JSON 分配与大块回退策略。
- [ ] P2：依据实测裁剪任务栈。
- [ ] P3：独立评估 BLE / Wi-Fi / lwIP 外部分配配置。
- [ ] 宿主回归、构建与实机并发验收。

### 用户可编辑的执行范围

- 首批建议范围：P0＋P1，按模块逐项验证。
- 是否采用全局 cJSON hooks：待讨论。
- 是否调整全局 malloc 阈值：首批不调整，后续待实测。
- 是否尝试外部任务栈：首批不做，后续逐任务核验。
- 实际硬件 PSRAM 容量：待开机日志 / 运行时核实。
- 其他调整：

## 11. 参考资料

- [ESP-IDF v5.4.4：外部 RAM 支持与限制](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/api-guides/external-ram.html)
- [ESP-IDF v5.4.4：堆内存分配](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/api-reference/system/mem_alloc.html)
- 当前工程：`main/Tool/app_mem.c`、`main/Task/task_config.h`、实际构建 sdkconfig 和链接 MAP。
- 本机 ESP-IDF：`components/freertos/heap_idf.c`、`components/esp_psram/Kconfig.spiram.common`。
- 本机 SQLite 组件：`components/esp32-idf-sqlite3/sqlite3.c` 的 `SQLITE_MALLOC` / `SQLITE_REALLOC`。
