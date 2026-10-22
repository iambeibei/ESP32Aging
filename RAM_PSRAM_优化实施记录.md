# RAM / PSRAM 优化实施记录

日期：2026-10-22。范围：用户提供 BLE 老化指令触发 HTTPS 失败日志后，授权将安全业务分配迁往 PSRAM。
原讨论稿保存在 [RAM_PSRAM_优化修改计划.md](RAM_PSRAM_优化修改计划.md)，本记录说明实际实施范围。

## 1. 日志结论

- `mbedtls_ssl_setup returned -0x7F00`：当前 SDK 定义为 `MBEDTLS_ERR_SSL_ALLOC_FAILED`。
  这可以确认发生过 TLS 分配失败；不能仅凭后续空闲值确定失败申请的具体字节数。
- 原构建强制 mbedTLS 动态内存使用内部 RAM。协议获取走 HTTPS，TLS 接收 / 发送内容长度配置
  分别是 16384 / 4096 字节，实际 TLS 开销还包括上下文、协议头、证书及握手临时对象。
- 按键日志的内部可用内存仅 14183 字节、最大连续块 7680 字节、历史最低 3547 字节，
  同时 PSRAM 空闲约 7.8 MiB，符合内部堆压力导致 TLS 分配失败的表现。
- `mesh_health.internal_free_bytes` 使用 SDK 的 `esp_get_free_internal_heap_size()`，
  当前 SDK 额外要求 `MALLOC_CAP_DMA`；按键日志仅要求内部、8 位可访问。因此两个值口径不同。
  新诊断同时记录普通内部堆和内部 DMA 堆，避免混淆。
- 协议获取失败后低水位持续存在，不能单凭这些日志认定为泄漏：BLE 初始化成功后模块会常驻，
  供后续老化及外接设备复用。本轮保留该行为，需用反复启动 / 停止后的趋势判断泄漏。

## 2. 实施内容

| 部分 | 实际修改 | 保持的边界 |
|---|---|---|
| mbedTLS | `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`，关闭内部专用分配 | 仍使用 HTTPS；TLS 报文长度与证书设置保持原值 |
| Bluedroid | `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y` | 由 SDK 决定可外置对象，控制器及强制内部对象继续内部 |
| MQTT 接收 | 32 项、每项 1090 字节，34880 字节数据区改为 PSRAM | 容量、超时、消息内容、溢出行为保持原值 |
| MQTT 运行日志队列 | 3 项、每项 324 字节，972 字节数据区改为 PSRAM | 原运行日志 QoS、截断规则和会话过滤不变 |
| UART0 / RS485 | 每路 3×1026 字节数据队列和 3076 字节静态业务缓冲外置 | 驱动 RX ring buffer、事件队列、ISR 数据继续内部 |
| BLE 应用层 | 扫描列表 3650 字节、重组缓冲 1200 字节静态外置；通知数据、特征 / 描述符列表 PSRAM 优先 | 仅任务 / GAP / GATTC 任务回调访问，不迁移控制器 DMA |
| JSON | 在 `app_main()` 最早阶段安装一次 cJSON hooks | 分配与释放配对，运行中不更换；失败保持原有 NULL 语义 |
| 其他业务对象 | SPIFFS JSON 文件缓冲、JSON 输出字符串、Modbus 帧、AP 扫描记录、ID 数组 PSRAM 优先 | 所有权与 `free()` 接口保持一致 |
| 失败路径 | 数据库结果缓存判空并复用；老化配置先检查设备存在再写入 ProtoID | 避免内存不足后 NULL 解引用，不格式化或清理数据库 |
| 诊断 | 增加分阶段堆指标、无分配的失败回调计数及内部回退次数 | 仅串口日志，不递归触发 MQTT 发布 |

新队列封装是 `main/Tool/app_queue.c/.h`：由内部静态控制块持有外部存储区，使用
`xQueueCreateStatic()`。PSRAM 启用时，大队列数据区分配失败直接失败，不占用内部保留池；
初始化回滚使用 `app_psram_queue_delete()`。创建操作限定初始化任务串行执行，删除前必须无使用者。
队列封装不提供 ISR 使用保证。

现有 `app_malloc_prefer_psram()` 等通用业务封装保留原有回退语义；统计成功回退内部内存的次数。
未调整全局 malloc 阈值 16384 字节及内部保留池 32768 字节，避免改变未核验的 SDK 分配路径。
原已外置的 SQLite 核心、MQTT outbox 等保持现有实现。

保留内部 RAM 的部分还包括任务栈、RTOS 控制块、驱动控制结构、小指针 / 确认队列、CAN 中断队列。
FATFS 空白分区检查的 4 KiB 临时读取缓冲也保留内部，避免 Flash 读路径增加外部目标中转分配。
未修改任务栈尺寸、Wi-Fi / lwIP 分配开关、MQTT Topic / JSON 字段 / QoS、老化流程、OTA 与重启时序。

默认配置 `sdkconfig.defaults` 与实际 `build/mesh-lite/sdkconfig` 已同步；后者由构建目录保存，
生成的 `build/mesh-lite/config/sdkconfig.h` 已核验 TLS 外部分配和 Bluedroid PSRAM 优先均生效。

## 3. 构建与静态收益

以下是 ELF / MAP 可确认的静态变化，不是实机空闲堆测量。

| 项目 | 修改前计划快照 | 修改后构建 | 差额 |
|---|---:|---:|---:|
| `.dram0.data`＋`.dram0.bss` | 53740 B | 43140 B | 内部静态数据减少 10600 B，约 10.35 KiB |
| `.ext_ram.bss` | 26052 B | 37068 B | PSRAM 静态区增加 11016 B，约 10.76 KiB |

新增队列控制块仍在内部 RAM，静态外置对象还有对齐开销，因此上表两侧差额不完全相等。
`xtensa-esp32s3-elf-nm -S -n` 已确认：UART0、RS485、BLE 上述静态对象地址在 `0x3c1f....`，
四个队列控制记录在 `0x3fca....`。

运行期数据区另计：MQTT 两队列共 35852 字节，UART0 3078 字节，RS485 3078 字节。
四路均启用时合计 42008 字节，约 41.02 KiB，从内部动态分配改为 PSRAM；
仅 MQTT＋UART0 时为 38930 字节，约 38.02 KiB。动态分配发生与否取决于对应功能初始化。
这些是代码容量预算，仍有 RTOS 控制块、分配器元数据等差异，不能直接推导实机余量。
JSON、TLS、Bluedroid 的进一步收益随配置、连接状态及并发负载变化，待实机测量。

指定 ESP-IDF 构建通过：

```bash
. /home/wbb/espidf/esp-idf-mesh-lite/export.sh
IDF_SKIP_CHECK_SUBMODULES=1 idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig build
```

构建产物：`build/mesh-lite/Light_Gateway_UDP.bin`。未修改 `version.txt`，未烧录设备。
保留项目已有的未使用符号及 `BRIDGE_SOFTAP_MAX_CONNECT_NUMBER` Kconfig 警告。

## 4. 宿主验证

- 新增 `python3 tests/test_psram_memory.py`，覆盖真实 cJSON 解析 / 打印 / 释放、部分分配失败清理、
  hooks 幂等、realloc 失败保持原对象、内部回退与迁回、静态队列创建失败回滚及重试、容量 / 顺序、
  并发收发完整性、释放与重建。
- 使用本机 SDK 的真实 `components/mbedtls/port/esp_mem.c` 验证：模拟内部堆仅 7680 字节时，
  内部 16384 字节申请失败，而外部模式 TLS 分配成功。这仅验证分配选项与封装，未模拟完整 TLS 握手。
- 更新 MQTT / HTTP 宿主替身；现有 MQTT、HTTP 协议、Mesh 出口、OTA、统一重启回归通过。
- PSRAM、MQTT、HTTP 回归执行 ASan / UBSan 并通过。当前执行环境不支持 LeakSanitizer 的进程扫描，
  因此这些运行使用 `ASAN_OPTIONS=detect_leaks=0 TEST_SANITIZE=1`；新内存测试自身对每个分配进行登记，
  并在故障清理、删除后断言活动分配为零。该断言不能代替所有模块的实机泄漏测试。

## 5. 实机复测与判断

先安装本次固件，使用原 BLE 老化配置重复相同操作，保留以下阶段的完整日志：

```text
MEM_STAGE=startup
MEM_STAGE=config_loaded
MEM_STAGE=before_ble_init
MEM_STAGE=after_ble_init
MEM_STAGE=protocol_http_start
MEM_STAGE=protocol_http_done
MEM_STAGE=aging_started / aging_start_failed
MEM_STAGE=button
```

每行包含内部堆、内部 DMA 堆和 PSRAM 的当前 / 历史最低 / 最大连续块，以及 PSRAM 总容量、
累计分配失败次数、最近失败的字节数 / capabilities、通用分配器成功回退内部的次数。
失败次数包含最终被回退成功补救的分配尝试，不能直接当作业务失败次数。
最近失败参数为并发观测快照，不用于同步或恢复决策。
队列初始化日志应显示 `external=1 control_internal=1`。

重点验证：

1. 使用相同 BLE 指令，协议 HTTPS 获取成功，老化正常进入运行；不再出现 TLS 分配错误。
2. BLE 收发、串口 / RS485、SQLite 入库、MQTT 上报 / 补传 / 日志同时工作，数据不丢配、不串包。
3. 多次启动、停止和失败重试后，内部堆及 PSRAM 的回收后余量没有持续下降。
4. 并发断网恢复、OTA 接收 / 供数及 Flash 写入时，无缓存关闭访问、栈溢出或任务创建失败。
5. 对比内部 DMA 堆最大块与驱动分配需求；PSRAM 有余量不能保证内部 DMA 分配一定成功。

目前仅完成代码、宿主回归和构建验证；尚未取得优化后实机数据，不能承诺全部工况内存充足或现场问题已消除。
