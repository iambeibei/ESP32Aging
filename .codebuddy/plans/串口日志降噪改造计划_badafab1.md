---
name: 串口日志降噪改造计划
overview: 按「只关 INFO、保留 W/E」的口径，逐 TAG/逐点关闭工程内高频串口日志：修正 2 处把级别强制拉回 INFO 的调用、删除加解密与 BLE 收发热路径的逐包打印、把每采样 2 行的 SQLite 状态日志降级、给 1Hz 离线告警与 10s 探测日志限频/降级，并用宏把裸 printf 协议 dump 默认关闭；保留全部 ERROR/WARN 与启动一次性诊断。
todos:
  - id: fix-level-gates
    content: 修正 appTask.c 与 SqLite.c 两处把日志级别强制拉回 INFO 的调用，改为集中设置 app_enc 为 WARN
    status: completed
  - id: silence-enc-ble-hotpath
    content: 删除 app_enc 逐包与每 16 字节打印，蓝牙收发/扫描/服务发现 INFO 降为 ESP_LOGD 并移除两处 HEX dump
    status: completed
    dependencies:
      - fix-level-gates
  - id: demote-sample-logs
    content: 将 SQLite 每采样两行、appTask 整包 JSON/写应答/动作、dual_net 探测结果降为 ESP_LOGD
    status: completed
    dependencies:
      - fix-level-gates
  - id: throttle-offline-warn
    content: 将离线 1Hz 告警改为状态翻转即打加 30 秒节流，保持 WARN 级别不变
    status: completed
    dependencies:
      - fix-level-gates
  - id: gate-bare-printf
    content: 新增 lg_log_config.h 宏，包裹 Modbus/SCPI/老化配置裸 printf，并把 LG_MESH_DIAG_LOG 默认改为 0
    status: completed
    dependencies:
      - fix-level-gates
  - id: build-verify
    content: 编译并串口实测统计日志行数，确认 W/E 仍输出；用 [skill:embedded-coding-standard] 复核改动合规性
    status: completed
    dependencies:
      - silence-enc-ble-hotpath
      - demote-sample-logs
      - throttle-offline-warn
      - gate-bare-printf
---

## 用户需求
对 ESP32-S3 老化测试网关固件做串口日志降噪：识别可关闭的日志点，并按已确认口径落地修改。

## 口径与范围
- 关闭力度：只关 INFO，保留 WARN 与 ERROR（错误、告警、恢复、OTA、重启、鉴权等诊断信息必须继续输出）。
- 目标是消除高频重复刷屏，降低串口占用与 CPU/栈开销，顺带减少日志与设备 SCPI 报文在 UART0 上的交织干扰。
- 不改业务逻辑、不改协议文本、不改任务栈与优先级、不改分区与 sdkconfig 的日志上限。

## 需达成的效果
1. 加解密、蓝牙收发不再按包/按 16 字节打印十六进制内容。
2. 每 5 秒老化采样不再产生固定的 3 行落库/状态日志。
3. 断网时 1Hz 的 “Network is not available” 告警降为低频，不再刷屏。
4. 蓝牙扫描失败重连不再每轮输出十余行流程日志（仅保留连接成功/断开等关键一行）。
5. 协议层裸 printf 的全量 dump 默认静默，可通过一个编译宏按需打开。
6. 全部 ERROR/WARN 与启动一次性诊断日志行为保持不变。

## 验收
- 编译通过，镜像体积不增（预计略降）。
- 串口实测：老化采样期每采样周期日志行数为 0；子节点 BLE 重连循环日志行数从约 13 行/轮降至 1-2 行；根节点离线告警频率不高于每 30 秒 1 行。
- 人为触发故障（断开设备、断网、OTA 失败）仍能看到原有的 W/E 日志。

## 技术栈与约束
- 语言/框架：C（近 C99）+ ESP-IDF v5.4.4 + FreeRTOS，日志走 `esp_log`（`ESP_LOGI/LOGW/LOGE`）与少量裸 `printf`，无新增依赖。
- 当前 sdkconfig：`CONFIG_LOG_DEFAULT_LEVEL_INFO=y`、`CONFIG_LOG_MAXIMUM_LEVEL=3`（INFO）。**不提高上限**（`sdkconfig.defaults` 已注明会撑大 OTA 分区），因此 `ESP_LOGD` 在编译期被剪除，等价于“删除但保留源码可读性”。
- 设备 SCPI 与控制台同用 UART0（TXD43/RXD44/115200），日志量与仪表通信直接竞争同一物理链路，降噪有通信稳定性收益。

## 实现方案（三层控制，避免大改）
**第 1 层 全局默认：不动。** 保留 INFO 默认级别，保证错误面与启动诊断完整。

**第 2 层 运行期 TAG 级（兜底）：** 删除两处“把级别强制拉回 INFO”的调用，改为一个集中函数，只对整 TAG 均为噪音的 `app_enc` 设 `ESP_LOG_WARN`。理由：`ESP_LOGI` 被抑制时不会格式化参数，开销可忽略；但 `printData()` 在调用日志前已做 VLA + `sprintf`，必须单独短路。

**第 3 层 逐行降级（精确）：** 高频点由 `ESP_LOGI` 改为 `ESP_LOGD`（编译期剪除，源码语义仍保留，日后排查只需提高 MAX 或改一行），无价值的重复行直接删除；1Hz 告警改为“状态翻转 + 30 秒节流”，级别仍为 WARN。

**裸 printf（不受日志级别控制）：** 新增一个仅含宏的头文件 `main/Task/lg_log_config.h`（放在已有 INCLUDE_DIRS 的 `Task/` 下，无需改 CMake），用 `LG_LOG_PROTO_DUMP`（默认 0）包住 Modbus/SCPI/老化配置的全量 dump。`sqllib1.c` 的 34 处 printf 位于无调用方的函数，属死代码，不动。

### 逐点改动表
| 文件:行 | 现状 | 目标 | 说明 |
|---|---|---|---|
| `Task/appTask.c:5969-5970` | `esp_log_level_set("*", ESP_LOG_INFO)` | 删除，改调 `app_log_level_init()` | 唯一把所有 TAG 拉回 INFO 的调用，且位于 `mesh_init_Custom()` 之后，会覆盖任何降级 |
| `DataStorage/SqLite/SqLite.c:2541` | `esp_log_level_set(TAG, ESP_LOG_INFO)` | 删除 | TAG 默认即 INFO，强制设置多余且阻碍后续降级 |
| `Tool/app_enc.c:50-61` | `printData()` 内 VLA + 逐字节 `sprintf` + `ESP_LOGI` | 函数体用 `LG_LOG_PROTO_DUMP` 包裹并及时 return | 抑制日志也会先执行 sprintf，必须真正短路 |
| `Tool/app_enc.c:641,646` | 循环内 `ENTER` + `every dec` | 删除 | 每 16 字节 2 行，单包 64 字节即 8 行 |
| `Tool/app_enc.c:257-267` | 每包 10 行长度/首字节 | 删除 | 保留 `invalid aec-cbc len` 的 E |
| `Tool/app_enc.c:201,622,638` | 每次加解密 1 行 | 改 `ESP_LOGD` | 低频但无运维价值 |
| `Ble_control.c:1527-1531` | 原始接收 INFO + HEX dump | INFO 改 `ESP_LOGD`，HEX 删除 | 每包 2 行 |
| `Ble_control.c:1136-1139` | 发送已受理 INFO + HEX dump | 同上 | 每包 2 行 |
| `Ble_control.c:359` | `ble a recv len` INFO | 改 `ESP_LOGD` | 每接收事件 |
| `Ble_control.c:604,607,832,928,990,1199,1218,1348,1355,1365,1385,1422,1448,1465,1559,218` | 扫描/连接/服务发现 INFO | 改 `ESP_LOGD` | 保留 `1279` 连接成功、`1579` 断开原因 |
| `DataStorage/SqLite/SqLite.c:1713` | `structured record committed` INFO | 改 `ESP_LOGD` | 每采样 1 行 |
| `DataStorage/SqLite/SqLite.c:1855` | `Push-state updated` INFO | 改 `ESP_LOGD` | 每采样 2 行 |
| `Task/appTask.c:3448,3463,3472,3486` | 整包 JSON INFO | 改 `ESP_LOGD` | 每条 MQTT 命令 |
| `Task/appTask.c:3969,4043,4571,4651` | 写应答/动作/在线检查 INFO | 改 `ESP_LOGD` | 每次动作 |
| `Task/appTask.c:3757` | 1Hz `Network is not available` WARN | 状态翻转即打 + 30s 节流 | 保留 W 语义，去掉刷屏 |
| `Communication/Internet/dual_net/dual_net.c:256` | 10s 探测结果 INFO（长行） | 改 `ESP_LOGD` | 仅根节点，每分钟 6 行 |
| `Communication/Internet/Mesh/mesh.c:1096` | `LG_MESH_DIAG_LOG` 默认 1 | 改 0 | 实测启动期 `[vendor_ie]` 25 行 |
| `Protocol/ModBus/modbus_cmd.c`（21 处）、`modbus_json_parser.c:104,119,196-205`、`Protocol/SCPI/scpi_dynamic.c:704-731`、`ConfigData/aging_config.c:857-901` | 裸 `printf` 全量 dump | `LG_LOG_PROTO_DUMP` 包裹（默认 0） | 不受 `esp_log_level_set` 控制 |

### 性能与正确性
- `ESP_LOGx` 在级别不足时由宏内联判断提前返回，不做格式化；改为 `ESP_LOGD` 后在 MAX=3 下被编译器完全消除，零运行成本。
- `printData()` 的 `char hex_str[2*text_len+1]` 是栈上 VLA，且逐字节 `sprintf`，即使日志被抑制仍会执行，必须删除/短路，否则降级无效。
- `esp_log_level_set("*", ESP_LOG_INFO)` 删除后，`SqLite.c` 等模块不会再把级别改回 INFO，TAG 级策略才能生效。
- 仅改日志语句与一处节流判断，不触碰锁、队列、任务调度与协议解析结果，爆炸半径极小。

### 风险与回退
- 排障能力下降：高频点未删源，仅改 `ESP_LOGD`；需要时把 `CONFIG_LOG_MAXIMUM_LEVEL` 提到 DEBUG（临时）或把单行改回 `ESP_LOGI`。
- 协议 dump 默认关闭：`lg_log_config.h` 注释写明开启方法与影响（UART0 与设备报文交织）。
- 行号会随改动漂移，执行时以“日志文本内容”为定位依据，不唯行号。

## 目录结构
```
ESP32Aging/main/
├── Task/
│   ├── lg_log_config.h      # [NEW] 日志分级开关宏：LG_LOG_PROTO_DUMP（默认 0），供协议层与 app_enc 复用；仅头文件，放在已有 INCLUDE_DIRS 的 Task 下，无需改 CMakeLists
│   └── appTask.c            # [MODIFY] 删除 esp_log_level_set("*", INFO)；新增 app_log_level_init() 集中设置 app_enc 为 WARN；3757 离线告警改状态翻转+30s 节流；3448/3463/3472/3486/3969/4043/4571/4651 降为 ESP_LOGD
├── Tool/
│   └── app_enc.c            # [MODIFY] printData 短路；删除 641/646 每 16 字节打印与 257-267 每包 10 行；201/622/638 降 ESP_LOGD
├── Communication/
│   ├── Ble_control/Ble_control.c              # [MODIFY] 收发与扫描/服务发现 INFO 降 ESP_LOGD，删除两处 HEX dump，保留连接/断开关键行
│   ├── Internet/dual_net/dual_net.c           # [MODIFY] 256 行探测结果降 ESP_LOGD
│   └── Internet/Mesh/mesh.c                   # [MODIFY] LG_MESH_DIAG_LOG 默认 1 改 0
├── DataStorage/SqLite/SqLite.c                # [MODIFY] 删除 2541 的 TAG 强制 INFO；1713/1855 降 ESP_LOGD
├── Protocol/ModBus/modbus_cmd.c               # [MODIFY] 21 处裸 printf 用 LG_LOG_PROTO_DUMP 包裹
├── Protocol/ModBus/modbus_json_parser.c       # [MODIFY] 104/119/196-205 同上
├── Protocol/SCPI/scpi_dynamic.c               # [MODIFY] 704-731 协议全量 dump 同上
└── ConfigData/aging_config.c                  # [MODIFY] 857-901 老化配置 dump 同上
```

## 关键代码结构
```c
/* main/Task/lg_log_config.h —— 只放开关宏，不引入依赖 */
#ifndef LG_LOG_CONFIG_H
#define LG_LOG_CONFIG_H

/* 协议层全量 dump：Modbus 帧/寄存器、SCPI 协议树、老化步骤。
 * 0=关闭（默认，量产）；1=打开。注意 UART0 与设备 SCPI 共用，
 * 打开后日志会与仪表报文交织，仅限离线排障。 */
#ifndef LG_LOG_PROTO_DUMP
#define LG_LOG_PROTO_DUMP 0
#endif

#endif
```

```c
/* main/Task/appTask.c —— 替代 esp_log_level_set("*", ESP_LOG_INFO) */
static void app_log_level_init(void);  /* 仅对整 TAG 均为噪音的 app_enc 设 ESP_LOG_WARN，其余保持默认 INFO */
```

## Agent Extensions
### Skill
- **embedded-coding-standard**
  - Purpose: 在改动完成后按工程 `.ai/coding-standard.md` 复核被修改的 C 文件（`appTask.c`、`app_enc.c`、`Ble_control.c`、`SqLite.c` 等），确认条件编译包裹、VLA 使用、返回值与并发访问未引入新违规。
  - Expected outcome: 输出一份针对本次日志改动的规范核对结论，明确指出是否满足工程本地规范，并列出需要修正的条目（若全部合规则明确说明无新增违规）。
