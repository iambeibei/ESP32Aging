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
| 均无，但日志出现 `mesh_health` 父丢失记录后静默重启 | 查 brownout（`esp_reset_reason()`，建议在启动日志补充打印） |
| 子节点 30 分钟不重启、老化继续、缓存数据在根恢复后补发 | 修复生效 |

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
- **只观测、不动作**：不做任何重启/重连决策（重连策略归 mesh-lite），
  为 6.4 诊断提供现场证据，同时证实「任务调度正常」。
- `WIFI_EVENT_STA_DISCONNECTED` 事件补记断开原因（`wifi_event_sta_disconnected_t`
  无 reason 字段时用事件 id + 关联状态代替，以 SDK 实际结构为准）。

### 7.3 mesh-lite 重连节奏调优（mesh.c）

- 调用 `esp_mesh_lite_set_wifi_reconnect_interval(10, 3, 30)`：
  重连当前父 10s×3 次，失败后每 30s 重扫一次。
- 取舍：根恢复后发现时间最坏 30s（根自身重启恢复通常分钟级），
  换取 BLE 老化链路稳定与闭源重扫路径暴露下降 3 倍。
- 根节点同样生效（参数对根的重连/重扫一致，根另有 root_failover_task 兜底）。

### 7.4 BLE 扫描等待超时（appTask.c）

- `Aging_Test_Task` 中 `while (!ScanComplete)`（appTask.c:4174）加总超时
  （如 30s）与重试上限，超时按既有 FailCount 路径走 `agingError` 中止，
  消除「回调丢失导致任务永久自旋」的次级风险。

### 7.5 明确不做的改动

- 不改 `set_disallowed_level(1)`（产品约束：单一根拓扑）。
- 不让子节点在根消失时重启 MQTT 客户端/重建任务（SDK 自动重连已覆盖）。
- 不动老化数据采集/缓存/补发链路（已验证只降上行不降执行）。

## 8. 验证与实机验收

1. 编译通过（`idf.py -B build/mesh-lite build`），无新增 warning。
2. 一台根 + 一台子 + 一台老化电池包：
   - 拔根节点电源，子节点串口持续观察 ≥30 分钟：
     不重启；`mesh_health` 父丢失日志规律出现；老化状态机继续推进；
     采样数据以 `pushed=0` 入库；上传失败计数增长但无崩溃。
   - BLE 老化期间人为拉远电池包造成 BLE 断开：出现 agingError 与优雅中止，
     **整机不重启**，`ble_data` 任务恢复，可重新 start_aging。
   - 恢复根节点：30s 内子节点重挂（mesh_health 心跳恢复），MQTT 自动重连，
     缓存数据补发，服务器侧无重复/丢号。
3. 回归：根节点拔主路由器（主备切换）、插拔网线，确认根行为与 0.0.1.12 一致。

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
