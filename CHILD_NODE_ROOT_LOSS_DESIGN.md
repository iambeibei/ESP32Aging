# 根节点掉线时子节点异常重启：排查与处理设计

本文对应当前代码（ESP-IDF v5.4.4 / esp-mesh-lite 1.0.2 / iot_bridge 1.0.1）。
目标：明确根/子节点依赖关系与保活、故障转移、重启恢复流程，定位并消除
「根节点挂掉 → 子节点异常重启」的根因，使子节点在根掉线期间保持存活并继续老化。

## 1. 问题现象与结论摘要

现象：根节点掉线（断电/重启/上联中断导致 SoftAP 消失）后，子节点随之重启，
而非保持本地组网与老化执行。

结论（根因定位方法见第 6 节）：

1. 子节点固件中**唯一可被纯链路事件触发、且无服务器命令参与**的重置源是
   `ble_disconnect_cb()` 中 BLE 异常断开的无条件 `esp_restart()`
   （[appTask.c:1602](../main/Task/appTask.c)）。根节点消失后 mesh-lite 按默认节奏
   反复重扫父节点，与老化用的 BLE 链路争用射频，BLE 断开被判定为「异常断开」，
   进而整机重启，杀掉本可继续的老化会话。
2. mesh-lite 核心为闭源库，其父节点重扫路径有历史崩溃记录（见第 6.3 节），
   无法源码级修复，采取「降低暴露频率 + 可观测性 + 诊断标记」缓解。

## 2. 角色与依赖关系

```text
路由器/以太网
    │  WiFi STA / ETH_WAN（根的唯一上联出口）
┌───┴────────────────────┐
│ 根节点  IsRoot=1        │  SoftAP(LGMesh_xx) + NAPT
│  - set_allowed_level(1) │
│  - dual_net 裁决上行     │
│  - root_failover_task   │  主/备路由器故障转移
└───┬────────────────────┘
    │ SoftAP 关联（mesh 父链路）+ DHCP/DNS 经 NAPT
┌───┴────────────────────┐
│ 子节点  IsRoot=0        │
│  - set_disallowed_level(1)（禁止升层为根）│
│  - STA IP 来自 mesh 父节点              │
│  - 独立 MQTT 连接 + HTTP 协议拉取        │
└───┬────────────────────┘
    │ BLE / RS485 / CAN 直连（与根无关）
被测电池包（老化执行对象）
```

依赖矩阵：

| 依赖方 | 被依赖方 | 内容 | 根掉线后的正确行为 |
|---|---|---|---|
| 子节点 | 根节点 | SoftAP 父链路关联 | mesh-lite 自动重连/重扫，不可升层 |
| 子节点 | 根节点 | STA IP、DHCP、NAPT 上行 | 丢 IP → `Network_Flag=0`，MQTT 断开 |
| 子节点 | MQTT Broker | 命令/数据/日志通道 | SDK 自动重连；数据缓存 SQLite 后补发 |
| 子节点 | 被测电池包 | BLE/RS485/CAN 老化链路 | **与根无关，必须继续执行** |
| 根节点 | 路由器/以太网 | 上联出口 | root_failover_task 主备切换 |

**关键结论：老化执行链路（子节点 ↔ 电池包）不依赖根节点。根掉线只影响上行
（MQTT/HTTP），不应影响老化执行，更不应导致子节点重启。**

## 3. 心跳与保活机制

### 3.1 mesh-lite 父节点保活（闭源，默认参数）

`esp_mesh_lite_set_wifi_reconnect_interval()` 文档（managed_components/
espressif__mesh_lite/include/esp_mesh_lite_core.h:604-625）：

- STA 断连后先重连当前父节点：默认 5s 间隔、最多 2 次；
- 全部失败后开始扫描新父节点：默认每 10s 一次，持续进行；
- 库内在 `IP_EVENT_STA_LOST_IP` 时调用 `esp_mesh_lite_connect()`
  （src/esp_mesh_lite.c:368-376），`CORE_STARTED` 时同样触发连接（382-385）。

**本项目未调用该设置接口，全部使用默认值**（重扫节奏 = 每 10s）。

### 3.2 MQTT 保活（mqtt_app.c）

| 参数 | 根节点 | 子节点 | 位置 |
|---|---|---|---|
| keepalive | 60 s | 15 s | mqtt_app.c:46-47,644 |
| 自动重连等待 | SDK 默认退避 | 2000 ms | mqtt_app.c:48,657 |
| 网络超时 | 10000 ms | 10000 ms | mqtt_app.c:655 |
| 会话 | clean session，QoS1 | 同左 | mqtt_app.c:645-646 |
| 遗嘱（LWT） | `device/public/down_line` | 同左 | mqtt_app.c:647-653 |

断线不断客户端、队列与 `mqtt_rx` 任务，由 SDK 自动重连兜底
（mqtt_app.c:459-468；DUAL_NET_DESIGN.md 第 5 节）。

### 3.3 应用层网络标志

- 子节点：`Network_Flag` 唯一写者是 mesh.c `network_event_handler`
  （STA_GOT_IP=1，STA_LOST_IP/STA_DISCONNECTED=0，mesh.c:149-182）。
- 根节点：由 dual_net 综合「以太网 + WiFi」裁决（mesh.c:80-86 让路）。
- `Init_ByNetwork_Flag`（appTask.c:2982-3168）仅在启动时等一次 Network_Flag，
  建成 MQTT/HTTP 后即退出；**后续网络波动不重建任何任务**。

### 3.4 数据保活

老化采样按周期入库（`pushed=0`），上传失败仅计数并缓存
（`aging_data_upload_result`，appTask.c:408-450；`app_DataUpload_Functiong`
appTask.c:4982），根恢复后经 SDK 重连 + 补发路径收回。

## 4. 故障转移策略

| 场景 | 根节点 | 子节点 |
|---|---|---|
| 主路由器断 | root_failover_task：30s 无 IP → 扫备用 SSID → 重启试连；最多 2 次，冷却 5 分钟；有线守卫（mesh.c:227-317） | 不参与 |
| 根节点整体掉线 | —— | 无应用层故障转移任务；mesh-lite 重连当前父（默认 5s×2）失败后每 10s 重扫；**禁止升层**（`set_disallowed_level(1)`，mesh.c:511），根恢复后自动重挂 |
| MQTT 断 | 上行切换时主动请求断开+立即重连一次（mqtt_app.c:151-206） | SDK 自动重连（2s 等待），客户端/队列/mqtt_rx 不重建 |

设计取舍：子节点禁止升层为根是产品约束（单一根拓扑，根才有路由器/以太网凭据）。
因此「根消失」期间子节点必须能够**无限期存活于无上行状态**，这是本次修复的验收核心。

## 5. 重启后的恢复流程（既有机制，本次不改）

1. NVS `SelfRecovery`：`aging_valid`、`current_step`、`Deadline`、`CycleIndex`、
   序列化老化 JSON（Aging.json）、`PN`、`RecordId`、Ready PN 与 WAIT_PN 标志。
2. `Init_ByNetwork_Flag`（appTask.c:3062-3154）：
   - `aging_valid=1` → 读断点，解析 Aging.json 成功则回到 RUNNING（断点续化）；
   - `start_pending=1` → 恢复 WAIT_PN 挂起命令；
   - Ready PN 独立恢复（与 aging_valid 解耦）。
3. 数据补发：SQLite `pushed=0` 记录经 `QueryLatestRecordByPNAndPushState` /
   `get_lost_aging` 命令补发。

## 6. 根因定位（可复现、可证伪）

### 6.1 子节点重置源枚举

对子节点固件（IsRoot=0）全部 `esp_restart()` 调用点及崩溃路径：

| # | 位置 | 触发条件 | 根掉线可达？ |
|---|---|---|---|
| R1 | appTask.c:1602 `ble_disconnect_cb` | BLE 异常断开（`BTDisConnect==0`） | **是（链路事件驱动）** |
| R2 | mesh.c:257/267/313 `root_failover_task` | 主备路由器试连 | 否（仅 `IsRoot=1` 创建，mesh.c:546） |
| R3 | appTask.c:1275/1292/1347 | UART 本地标定/配置命令 | 否（命令驱动） |
| R4 | appTask.c:2758 | MQTT `set_config` 命令 | 否（命令驱动） |
| R5 | ota.c:537/578 | OTA 成功 | 否（命令驱动） |
| R6 | mesh-lite 库内 `CONFIG_OTA_AUTO_RESTART` | LAN OTA 成功 | 否 |
| R7 | panic / TWDT | 内存破坏、任务饿死 | 间接可能（见 6.3） |

### 6.2 排除与锁定

- R2–R6 均为命令驱动或仅根节点创建，根掉线不可触发 → 排除。
- R1 是唯一「无命令、由链路事件触发」的应用层重置源 → **锁定为根因**。
- 触发链：根 SoftAP 消失 → 子 STA 断开 → mesh-lite 每 10s 重扫（射频与 BLE
  争用，老化设备正在 BLE 链路上做充放电）→ BLE 断开 → `BTDisConnect==0` 判定
  「异常断开」→ `esp_restart()`。重启后根仍未恢复，理论上可形成重启循环。

### 6.3 无法源码修复的次要风险（闭源库）

- `sdkconfig.defaults:30` 注释记录过「LoadProhibited crash during mesh scan retry」；
  coredump 已启用（`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`）。
- `DUAL_NET_DESIGN.md` 记录过两次 FreeRTOS 就绪链表 `StoreProhibited`，未最终定位。
- 缓解：降低重扫频率（第 7.3 节）+ 健康监视任务提供现场证据（第 7.2 节）。

### 6.4 实机诊断标记（用于证实/证伪）

复现方式：拔根节点电源（保持子节点与电池包老化运行），抓子节点串口：

| 观察 | 结论 |
|---|---|
| 重启前出现 `BLE disconnected,Restarting...` / `BLE abnormal disconnection; gateway is restarting` | R1 确认，本次修复直接消除 |
| 出现 `Guru Meditation` / `StoreProhibited` / `LoadProhibited` | R7，用 coredump（已烧录启用）解析；修复降暴露后仍复现则需升级 mesh-lite 组件 |
| 均无，但日志出现 `mesh_health` 父丢失记录后静默重启 | 查 brownout（`esp_reset_reason()`，建议在启动日志补充打印）。2026-10-16 起：无父约 12/24/36 分钟的带日志重启（`restarting node (n/3 this power cycle)`）属 7.7 watchdog 预期行为，不再是故障信号 |
| 子节点在根掉线期间存活：最坏 3 次 watchdog 重启（约 12/24/36 分钟）后稳定扫描，老化经 SQLite(`pushed=0`)+NVS 断点恢复继续，缓存数据在根恢复后补发 | 修复生效（2026-10-16 起按 7.7 修订的验收标准） |

## 7. 处理逻辑（修复设计）

### 7.1 BLE 断开去重启化（appTask.c）

- `ble_disconnect_cb()` 不再调用 `esp_restart()`：仅置
  `volatile bool s_ble_abnormal_disconnect` 并打 `ESP_LOGW`。
  回调上下文不碰存储/MQTT/任务删除（符合 .ai/coding-standard.md ISR/回调约束）。
- `Aging_Test_Task` 主循环每轮检查该标志：
  - 当前老化设备 `CommunicationMode` 为 `"Bluetooth"` → 视为老化链路故障，
    走既有 `aging_state_publish("agingError")` + `Aging_Task_Abort_And_Delete()`
    优雅中止（清理、回 IDLE、恢复 ble_data 任务），**整机不重启**；
  - 非 BLE 老化设备或空闲期 → 记录一次警告后忽略。
- 效果：BLE 掉线从「整机重启」降级为「单台老化任务中止」，根掉线期间子节点存活。

### 7.2 子节点 mesh 健康监视（mesh.c，新增任务）

- 仅 `IsRoot==0` 创建（根节点行为零变化），CPU1，栈/优先级用
  `task_config.h` 新增 `LG_STACK_MESH_HEALTH` / `LG_PRIO_MESH_HEALTH`。
- 周期 5s：无父关联（`Network_Flag==0`）时打印 `level`、`Network_Flag`、
  最近一次 STA 断开原因、内部堆余量；恢复关联后每 60s 打一次心跳。
- **观测 + 有界自愈**（2026-10-16 起）：常规职责仍是打心跳证据；另增
  无父自愈 watchdog（见 7.7）：连续无父先软复位（`disconnect`+`connect`）、
  无效再重启、每上电周期重启上限 3 次后永久放弃。日常重连/重扫与父选择
  策略仍归 mesh-lite 库，本任务不干预父选择，只在库自身无法恢复时
  按有界阶梯介入。
- `WIFI_EVENT_STA_DISCONNECTED` 事件补记断开原因（`wifi_event_sta_disconnected_t`
  无 reason 字段时用事件 id + 关联状态代替，以 SDK 实际结构为准）。

### 7.3 mesh-lite 重连节奏调优（mesh.c）

- 调用 `esp_mesh_lite_set_wifi_reconnect_interval(5, 2, 15)`（2026-10-16 起）：
  重连当前父 5s×2 次（窗口约 10s），失败后每 15s 重扫一次。
- 参数沿革：库默认 5s×2/10s → 曾调 10s×3/30s 压 BLE 射频争用与闭源重扫
  暴露 → 现场发现「中间父断开 + 根已满」时 30s 只重连原父 + 30s 才重扫
  使子节点长时间卡循环（根日志反复 `max connection, deauth!`）→
  折中为 5s×2/15s，重扫暴露介于原设计与库默认之间；持续无父场景
  改由 watchdog（7.7）有界兜底，不再依赖拖长节奏。
- 根节点同样生效（参数对根的重连/重扫一致，根另有 root_failover_task 兜底）。

### 7.4 BLE 扫描等待超时（appTask.c）

- `Aging_Test_Task` 中 `while (!ScanComplete)`（appTask.c:4174）加总超时
  （如 30s）与重试上限，超时按既有 FailCount 路径走 `agingError` 中止，
  消除「回调丢失导致任务永久自旋」的次级风险。

### 7.5 明确不做的改动

- 不改 `set_disallowed_level(1)`（产品约束：单一根拓扑）。
- 不让子节点在根消失时重启 MQTT 客户端/重建任务（SDK 自动重连已覆盖）。
- 不动老化数据采集/缓存/补发链路（已验证只降上行不降执行）。

### 7.6 BLE 连接建立失败与掉线的分流（2026-10-15 增补）

实机日志（子节点）暴露一类被误分类的 BLE 事件：`enh_open` 被接受后控制器建链失败
（4 次 `hcif disc complete rsn 0x3e`「Connection Failed to be Established」+
`ESP_GATTC_OPEN_EVT status=133`，全程无 `ESP_GATTC_CONNECT_EVT`，RSSI -61 信号强）。
旧代码把 `ESP_GATTC_DISCONNECT_EVT` 一律当作「异常断开」，一次建链失败即走 7.1 的
中止路径，`FailCount>5` 的 5 次重试额度完全未使用；且 `ble_connect_by_name()` 返回
true 仅代表 `enh_open` 被受理，状态机却立即认定已连接并空等 20s。

分流规则（`Ble_control.c` 连接尝试状态机 + `appTask.c` 结果等待与重试）：

| 事件序列 | 语义 | 处置 |
|---|---|---|
| `enh_open` 返回 ESP_OK | 请求在途（`s_connect_attempting`） | `AgingDeviceCheck` 等待结果（≤`BLE_CONNECT_WAIT_TIMEOUT_MS` 15s） |
| `ESP_GATTC_CONNECT_EVT` | 连接建立成功 | `notify_connect_result(true)` → 原 20s 初始化等待 + 通信检查流程 |
| `ESP_GATTC_OPEN_EVT` status≠OK（如 133） | 建立失败 | `notify_connect_result(false)` → `aging_error_log` 记 attempt，退避 3s 后按 FailCount 重试（每轮含 10s 重扫），满 5 次才 agingError |
| 建链过程中到达 `ESP_GATTC_DISCONNECT_EVT`（rsn 0x3E 等，先于 OPEN_EVT 约 20ms） | 建立失败 | 同上；OPEN_EVT 后续到达被 `notify_connect_result` 的 once 语义吞掉，不二次上报 |
| 已建立连接（`s_is_connected`）后的 `ESP_GATTC_DISCONNECT_EVT` | 链路掉线 | 维持 7.1：`s_disconnect_cb()` → 优雅中止、整机不重启 |
| 未连接且无在途尝试的迟到 `ESP_GATTC_DISCONNECT_EVT` | 陈旧事件 | 仅记 WRN，不触发任何回调（消除陈旧事件误判） |

设计取舍：建链失败按「瞬时故障」处理（DUT 刚上电未就绪、被其他主机占用、RPA 轮换
均可能表现为 0x3E/133），用满既有 FailCount 额度（最坏约 2 分钟：
5 × (10s 扫描 + ≤15s 结果等待 + 3s 退避)）后 agingError；已建立链路掉线仍按 7.1
中止（老化步骤时序不可中断重连）。回调上下文只置 volatile 标志与打日志，
审查结论见第 9 节增补行。

### 7.7 子节点无父自愈 watchdog（mesh.c，2026-10-16 增补）

现场问题：子节点的**中间父节点断开且根节点已满**时，子节点每轮扫描都选中
满员根被 deauth，库自身无法恢复，长时间卡在重连循环（用户实测重启 ESP32
可恢复）。配套修改：重连节奏调整为 5s×2/15s（7.3）+ 本 watchdog。
容量说明：每节点子节点上限 `MESH_MAX_CHILDREN` 曾于 2026-10-16 由 5 调到 8
（`mesh.c:659/668` + `CONFIG_BRIDGE_SOFTAP_MAX_CONNECT_NUMBER` 三处同步），
**同日应现场要求回退为 5**——「根已满无替代父」场景改由本 watchdog 兜底
（软复位→重启），不再依赖加槽位。

分级 escalate（用户 2026-10-16 确认；实现为 `mesh_health_task` 内的
`mesh_heal_watchdog()`，仅子节点，触发条件只看 `Network_Flag`，
与 BLE/MQTT 事件解耦）：

| 级别 | 触发 | 动作 | 说明 |
|---|---|---|---|
| L1 软复位 | 连续无父 3min（`MESH_PARENT_LOST_SOFT_RESET_MS`） | `esp_mesh_lite_disconnect()` + 100ms + `esp_mesh_lite_connect()` | 强制库走「扫描匹配节点并连接」的 fresh 路径，绕开可能卡住的内部重试状态与 RTC 原父偏好；不重启、不动老化任务、不丢数据；每次软复位后重置计时，给 3 分钟观察窗；上限 `MESH_SOFT_RESET_MAX=3` 次 |
| L2 重启 | 软复位 3 次仍无父 | `esp_restart()` | 重启前 100ms 等日志落盘；受每上电周期预算约束 |
| L3 放弃 | 重启预算耗尽 | 只限频（60s）打 WRN，继续扫描 | 回到「无限期存活于无上行状态」 |

重启预算：`RTC_NOINIT_ATTR` 计数（`s_heal_restart_count`，镜像 7.3 failover
的 `s_failover_attempts` 范式）：`ESP_RST_SW` 延续计数，上电/Brownout 清零；
每上电周期上限 `MESH_RESTART_MAX_PER_CYCLE=3` 次。

**根掉线场景时间线（验收标准随之修订）**：约 12/24/36 分钟三次重启后
稳定存活（每次软复位有 3 分钟观察窗），不再承诺「30 分钟绝对不重启」；
老化经 SQLite（`pushed=0`）+ NVS 断点恢复继续，采样数据不丢。
BLE 老化掉线、MQTT 断连不改变 `Network_Flag`，不会触发本 watchdog
（7.1 的「BLE 掉线优雅中止不重启」不受影响）。

## 8. 验证与实机验收

1. 编译通过（`idf.py -B build/mesh-lite build`），无新增 warning。
2. 一台根 + 一台子 + 一台老化电池包：
   - 拔根节点电源，子节点串口持续观察 ≥40 分钟：
     `mesh_health` 父丢失日志规律出现；watchdog 按 7.7 时间线动作
     （约 12/24/36 分钟三次带日志重启后进入 L3 放弃态，每 60s 一条
     `heal restart budget exhausted`）；老化状态机经断点恢复继续推进；
     采样数据以 `pushed=0` 入库；上传失败计数增长但无崩溃。
   - BLE 老化期间人为拉远电池包造成 BLE 断开：出现 agingError 与优雅中止，
     **整机不重启**，`ble_data` 任务恢复，可重新 start_aging
     （watchdog 只看父链路，本场景不触发）。
   - 恢复根节点：15s 内子节点重挂（mesh_health 心跳恢复），MQTT 自动重连，
     缓存数据补发，服务器侧无重复/丢号。
3. 回归：根节点拔主路由器（主备切换）、插拔网线，确认根行为与 0.0.1.12 一致。
4. BLE 建链失败回归（2026-10-15 增补，对应 7.6）：DUT 未就绪/被占用时串口应见
   `Open failed: 133`（或建链中断开）→ `BLE connection establishment failed to: <PN>` →
   运行日志 `Bluetooth connection establishment failed ... attempt=N`（MQTT）按 1→5
   循环重试（每轮重扫 10s），满 5 次才 `agingError`；已建立连接的人为断开仍一次即
   优雅中止、整机不重启（7.1 验收不回归）。
5. 无父自愈回归（2026-10-16 增补，对应 7.7）：
   - 拔掉中间父节点电源（根有槽位）：子节点约 10s 内进入重扫、15s 节奏
     发现并挂接新父，串口见 `STA IP <ip>, level=N` 与
     `SoftAP station connected: ... occupied=n/8`（新父侧），**不触发**
     watchdog（无父时长不足 3 分钟）；
   - 根节点刻意占满（≥8 个子节点）后拔中间父：子节点进入重连循环时
     根侧应见反复 `max connection, deauth!`；约 3 分钟起子节点串口出现
     `mesh soft reset 1/3` → `2/3` → `3/3`（每轮间隔约 3 分钟），
     软复位后若挂接成功（`STA IP` 日志）即恢复，watchdog 状态清零；
   - 软复位 3 次仍无效：约 12 分钟见 `restarting node (1/3 this power cycle)`
     并重启；重启后重新计时，最坏 12/24/36 分钟三次后进入
     `heal restart budget exhausted (3/3)` L3 放弃态，之后不再重启；
   - 每次重启后老化会话经 SelfRecovery 断点恢复（`aging_valid` → RUNNING），
     SQLite `pushed=0` 记录在恢复后补发，无静默丢数。
6. 容量回归：组网后各节点串口确认 SoftAP 槽位为 5（`occupied=n/5`，
   构建 sdkconfig 回读 `CONFIG_BRIDGE_SOFTAP_MAX_CONNECT_NUMBER=5`；
   2026-10-16 曾短暂调 8，同日回退）；BLE 老化扫描/连接不受重扫节奏
   加快（15s）的射频争用影响。

## 9. 代码审查记录（CERT C + FreeRTOS + 本地规范，双模型交叉）

审查范围：本次全部 C 改动（mesh.c / appTask.c / task_config.h）。规则组合经用户确认；
`esp_mesh_lite_set_wifi_reconnect_interval()` 经核实为 `void` 返回值（组件头文件
esp_mesh_lite_core.h:623-625，单位秒），两位审查员提的「返回值未处理」不成立。

审查发现并已修复：

| # | 级别 | 问题 | 修复 |
|---|---|---|---|
| 1 | high | BLE 断开标志只在主循环顶部消费，`AgingAction` 各长循环（Standing 数小时等待、Discharge/Recharge 条件轮询、群控等待、DataCheck 补发）不返回外层，导致掉线后迟迟不中止 | 新增 `aging_ble_link_lost_pending()` 检查点，下沉到全部 5 处长循环内部 + 主循环顶部，秒级响应 |
| 2 | medium | `ble_start_scan()` 返回值被忽略，与新增 30s 超时耦合：扫描未启动时空转满超时，重试全部无效 | 检查返回值，未启动即按尝试失败 break |
| 3 | medium | 扫描超时后未停止底层扫描，`s_is_scanning` 残留导致后续重试空转 | 超时分支调用 `ble_stop_scan()` |
| 4 | medium | `Lost_Data_List`（1KB PSRAM）分配未判空、各退出路径不释放；新中止路径放大泄漏 | 提为文件静态指针，入口判空，自删除式中止/完成路径统一 `free()` |
| 5 | medium | `ScanComplete` 跨任务（Bluedroid 写/老化任务读）非 volatile | 改 `static volatile bool` |
| 6 | low | 事件标志「检测-清除」窗口可吞第二次事件 | 改 `atomic_bool` + `atomic_exchange` 消费 |
| 7 | low | `%.*s` 打印 ssid 未截断 `ssid_len`；事件分支未校验 event_id | clamp 到 32 + 补 `WIFI_EVENT_STA_DISCONNECTED` 判断 |
| 8 | low | 健康任务固定 5s 唤醒与 60s 上报周期不匹配 | 休眠改用当前态 `interval_ms` |
| 9 | low | 诊断变量注释与 `atomic_uint` 类型不符 | 更正为 4 字节对齐访问说明 |
| 10 | low | 健康任务创建失败未在启动日志体现 | 最终 `Mesh-Lite started` 增加 `mesh_health=on/OFF` |
| 11 | low | `start_aging_with_ready_pn` 内 PN 检查重复定义且失败无 MQTT 应答 | 复用 `aging_pn_is_ready()`，防御分支补拒绝回复 |
| 12 | low | `BTDisConnect` 跨任务非 volatile | 改 `volatile uint8_t` |

2026-10-15 增补审查（7.6 BLE 建立失败分流，范围 `Ble_control.c` / `Ble_control.h` /
`appTask.c`，单模型，规则组合 CERT C + FreeRTOS + Local coding standard，
见 `.em_skill.json`）。API 依据：`esp_ble_gattc_open` 文档明确成功时
「always triggers `ESP_GATTC_CONNECT_EVT` and `ESP_GATTC_OPEN_EVT`」
（esp_gattc_api.h:325），失败时仅 OPEN_EVT 带错误码——分流设计成立。

| # | 级别 | 问题 | 处理 |
|---|---|---|---|
| 13 | low | `BleConnectComplete/Success`、`s_connect_attempting/pending` 为跨任务 volatile bool，非原子类型 | 均为「置位-观察」单向标志、无读-改-写、无并发双写窗口（请求写与结果写由 `enh_open` 调用/事件交付排序），与既有 `ScanComplete` 模式一致；ESP32-S3 内部 SRAM 无 cache，volatile 跨 CPU0/CPU1 可见性成立。理论残余（超时尝试的迟到事件落入下一轮复位微秒窗口）后果有界（多计一次失败重试），记录不改 |
| 14 | info | OPEN_EVT(OK) 成功路径未交付结果，依赖 CONNECT_EVT 必达 | API 文档已保证；15s 等待超时兜底为失败重试，不会永久挂起 |

2026-10-16 增补审查（7.7 无父自愈 watchdog + 容量 8 + 节奏 5s×2/15s +
SoftAP 站点观测，范围 `mesh.c` / `sdkconfig.defaults` / build sdkconfig，
单模型，规则组合同 `.em_skill.json`）。

| # | 级别 | 问题 | 处理 |
|---|---|---|---|
| 15 | medium | watchdog 使「根掉线 30 分钟不重启」的原验收标准失效（最坏 12/24/36 分钟三次重启） | 用户 2026-10-16 明确接受（重启可恢复卡死是现场实测结论）；第 6.4/8 节验收标准已按 7.7 修订，重启有 RTC 上限（3 次/上电周期）与 L3 放弃态兜底，不会形成无限重启循环 |
| 16 | low | `mesh_heal_watchdog` 三个 static 状态量非 volatile | 仅 mesh_health_task 单任务串行读写（单写者，无跨任务/ISR 访问），注释已声明；与既有 `last_report_tick`/`was_online` 任务内静态量同模式 |
| 17 | low | AP 事件处理器局部 `wifi_sta_list_t`（约 240B）压在事件任务栈（2304B）上 | 改文件级静态 `s_ap_sta_list`：默认事件循环单任务串行，单访问者无需同步，栈压力归零 |
| 18 | info | 软复位/重启前 100ms 延时原为裸常量 | 提取 `MESH_HEAL_SOFT_RESET_GAP_MS` 命名宏（本地规范：非显然常量须可追溯） |
| 19 | info | 注释内 `MESH_PARENT_RETRY_*/...` 的 `*/` 提前闭合注释块导致编译失败 | 编译验证捕获并修复为 `MESH_PARENT_RETRY 与 MESH_RESCAN_INTERVAL_SEC`；提示：注释中引用宏名避免带 `*/` 序列 |
| 20 | info | `esp_mesh_lite_connect()` 为 void 返回，误赋值导致编译错误 | 编译验证捕获并修复；`disconnect()` 返回值仍检查并打日志 |

## 10. 已知限制（审查确认，本次不修）

1. **外部 `vTaskDelete` 删除老化任务的 mutex 悬空风险（既有，非本次引入）**：
   `stop_aging`（appTask.c）与 `aging_command_json_parse` 失败清理从 MQTT 任务上下文
   直接删除老化任务；若删除恰好落在该任务持有 `s_log_mutex`（log.c）或
   `s_sqlite_mutex`（SqLite.c）期间，mutex 将永久无人释放。本次新增的 BLE 中止走
   任务自删除（`vTaskDelete(NULL)`），无此窗口，是正确的范式。建议后续以
   「取消标志 + 任务自行退出」替代跨任务强删。
2. **外部强删路径的 `s_aging_lost_data_list` 残留泄漏**：同上，任务被外部删除时
   来不及释放 1KB 列表；自删除路径（正常完成/各类内部中止）已无泄漏。
3. **RecordId NVS 写入失败仅记日志仍继续启动**：与 `start_aging_with_ready_pn` 内
   current PN 写入失败的处理保持一致（既有风格）；NVS 写失败场景罕见，
   如后续出现补发查询异常可按此排查。
4. **`stop_aging` 后不恢复 `ble_data` 任务**（既有不对称）：`Aging_Task_Abort_Cleanup`
   会恢复，`stop_aging` 路径不会；无新 start_aging 时 BLE 数据任务保持缺失。
5. **mesh-lite 重扫闭源路径的历史崩溃只能降暴露，不能根除**；若 6.4 诊断命中 R7，
   需升级 espressif/mesh_lite 组件版本后复测。
6. 根消失超过 NVS 老化断点有效期以外的场景仍以既有掉电恢复语义为准。
