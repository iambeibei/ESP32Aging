---
name: ble-connect-establishment-fix
overview: 修复 BLE「连接建立失败被误判为异常断开并一次中止老化」的缺陷：在 Ble_control.c 引入连接尝试状态机、把 OPEN_EVT 失败与建立过程中的 DISCONNECT_EVT 上报告警回调；在 appTask.c 的 AgingDeviceCheck 中等待连接结果（带超时），失败即按 FailCount 额度重试（含退避与重扫），而非一次失败就 agingError 中止。
todos:
  - id: ble-ctrl-establishment-state
    content: 修改 Ble_control.c/h：新增连接尝试状态机，建立失败经 connect_cb(false) 一次性上报
    status: completed
  - id: aging-connect-result-wait
    content: 修改 appTask.c：ble_connect_cb 记录结果，AgingDeviceCheck 带超时等待并按 FailCount 重试
    status: completed
    dependencies:
      - ble-ctrl-establishment-state
  - id: review-coding-standard
    content: 用 [skill:embedded-coding-standard] 审查 BLE 回调与跨任务标志并发合规性
    status: completed
    dependencies:
      - ble-ctrl-establishment-state
      - aging-connect-result-wait
  - id: build-regression
    content: 按 AGENTS.md 编译，串口回归验证建立失败重试与掉线中止不回归
    status: completed
    dependencies:
      - review-coding-standard
  - id: update-design-doc
    content: 在 CHILD_NODE_ROOT_LOSS_DESIGN.md 补充建立失败与掉线分流的设计说明
    status: completed
    dependencies:
      - build-regression
---


## 用户需求

修复子节点日志反映的 BLE 连接不成功问题。前置分析已确认：该断开不是被动断开，而是连接建立失败（HCI rsn 0x3E「Connection Failed to be Established」4 次 + `ESP_GATTC_OPEN_EVT status=133`，全程无 `ESP_GATTC_CONNECT_EVT`，GATT 连接从未建立；RSSI -61 信号强，最可能 DUT 暂时不可连接，如刚上电未就绪或被其他主机占用）。应用层缺陷放大后果：`ble_connect_by_name()` 返回 true 仅代表 `enh_open` 被接受，状态机却立即认定已连接并空等 20s；`ESP_GATTC_OPEN_EVT` 失败只打日志不上报；`ESP_GATTC_DISCONNECT_EVT` 不区分「建立失败的断开」与「已建立后被踢」，统一触发 `ble_disconnect_cb` 置异常标志，导致一次建立失败即 `agingError` 中止，`FailCount > 5` 的 5 次重试额度完全未使用。

用户决策点「是否需要加蓝牙重连」的结论：需要**有界重试**（建立失败用满既有 FailCount 额度：重扫+重连+退避），不做无限重连；老化执行中已建立链路掉线维持现有「优雅中止、整机不重启」设计（CHILD_NODE_ROOT_LOSS_DESIGN.md 7.1/8 已实机验收，不破坏）。

## 功能内容

1. Ble_control 层新增连接尝试状态机，区分「建立失败」与「已建立后掉线」：`ESP_GATTC_OPEN_EVT` 失败分支、连接尝试期间到达的 `ESP_GATTC_DISCONNECT_EVT`，统一经既有 `s_connect_cb(success=false, name)` 通道一次性上报；已建立连接（`s_is_connected` 为真）的断开仍走 `s_disconnect_cb()` 原路径。
2. appTask 的 `AgingDeviceCheck`：`ble_connect_by_name()` 返回 true 后不再盲目认定已连接，改为等待连接结果（带 15s 超时，参照既有 `ScanComplete` 轮询模式）；失败/超时打错误日志并退避 2-3s 后走既有 FailCount 路径重试（每轮含 10s 重新扫描），满 5 次才 `agingError` 中止。
3. 附带：编码规范审查、编译与串口回归、设计文档补充记录。

## 验收效果

- 复现原场景时：出现 `Open failed: 133` 或建立中断开 → 立即看到「连接建立失败 attempt=N」日志并按额度重试，不再一次失败即删 Aging.json/中止老化；
- 人为拉远电池包造成已建立连接断开：仍为 `agingError` 优雅中止、整机不重启（既有验收项不回归）。



## 技术栈

- 现有工程：ESP-IDF v5.4.4、FreeRTOS、C（近 c99）、Bluedroid GATT Client、esp-mesh-lite + iot_bridge；不引入新组件、新任务、新堆分配、新锁。

## 实现方法

**Ble_control.c（连接结果分流 + 一次性上报）**
- 新增两个文件级 `static volatile bool`：`s_connect_attempting`（连接请求在途）与 `s_connect_result_pending`（结果尚未交付）。
- `ble_connect_by_name()` / `ble_connect_by_address()`：`esp_ble_gattc_enh_open()` 返回 ESP_OK 后置 `s_connect_attempting = true; s_connect_result_pending = true;`。
- `gattc_profile_event_handler()` 三个事件分支统一经静态辅助函数 `notify_connect_result(bool success)` 消费（内部检查 `s_connect_result_pending`，保证 DISCONNECT_EVT 与 OPEN_EVT 都到达时只上报一次——日志顺序为先 DISCONNECT 后 OPEN 133）：
  - `ESP_GATTC_CONNECT_EVT`：`notify_connect_result(true)`（成功路径，保留现有 `s_connect_cb` 调用与 MTU 请求）。
  - `ESP_GATTC_OPEN_EVT` 且 `status != ESP_GATT_OK`：保留现有 `Open failed: %d` 错误日志，追加 `notify_connect_result(false)`。
  - `ESP_GATTC_DISCONNECT_EVT`：若断开前 `s_is_connected` 为真 → 走现有 `s_disconnect_cb()`（已建立链路掉线，行为不变）；若 `s_connect_attempting` 为真（建立失败）→ `notify_connect_result(false)` 且**不**调 `s_disconnect_cb()`；两者都不满足（迟到/陈旧事件）→ 仅记 WRN 不上报。`notify_connect_result` 内统一清 `s_connect_attempting` / `s_connect_result_pending`。
- `Ble_control.h`：不改签名（`ble_connect_cb_t(bool success, const char *device_name)` 的 success=false 通道已存在，Ble_control.h:45），仅补注释说明失败语义。

**appTask.c（结果等待 + 有界重试）**
- 新增常量 `#define BLE_CONNECT_WAIT_TIMEOUT_MS 15000`（与既有 `BLE_SCAN_WAIT_TIMEOUT_MS 30000` 并列，appTask.c:263 附近）。
- 新增两个 `static volatile bool`：`BleConnectComplete` / `BleConnectSuccess`（命名与轮询模式对齐既有 `ScanComplete`，appTask.c:1593）。
- `ble_connect_cb()`（appTask.c:1613-1623）：在现有日志基础上写 `BleConnectSuccess = success; BleConnectComplete = true;`（回调上下文只置标志+打日志，符合编码规范）。
- `AgingDeviceCheck` 的 BLE 分支（appTask.c:4288-4306）重构：
  1. `ble_connect_by_name(PN_Code)` 返回 false → 维持现有错误日志 + break（FailCount 路径）；
  2. 返回 true → 先复位 `BleConnectComplete = false`，打「连接请求已接受，等待建立结果」；
  3. 以 500ms 步进轮询 `BleConnectComplete`，总超时 `BLE_CONNECT_WAIT_TIMEOUT_MS`；
  4. 超时未建立或 `BleConnectSuccess == false` → `aging_error_log("Bluetooth connection establishment failed, attempt=%u", FailCount)`，`ble_stop_scan()` 防御性清理（若仍在扫描），`vTaskDelay(pdMS_TO_TICKS(3000))` 退避后 `break`（下一轮 while 重新扫描+重连；FailCount 在 4244 行已自增，满 5 次走 4234-4242 现有 agingError 中止）；
  5. 成功 → 维持现有 20s 初始化等待、删除 ble_data 任务、通信检查流程不变。

## 关键设计决策与权衡

- **复用 `s_connect_cb(false)` 通道而非新增回调类型**：`ble_connect_cb_t` 已带 success 参数（Ble_control.h:45），零接口变更、零新注册点（唯一注册点 appTask.c:3320 不动）。
- **once 语义用 `s_connect_result_pending` 实现而非时间窗去抖**：DISCONNECT_EVT 与 OPEN_EVT 双到达（实测间隔 20ms）时首个事件即交付结果并清 pending，第二个事件自然被吞；无定时器、无额外状态。
- **用满既有 FailCount>5 额度而非新设重试计数器**：保持 appTask.c:4244/4234-4242 单一计数语义，重试总时长上限约 2 分钟（5 轮 × (10s 扫描 + 连接等待 + 3s 退避)），与老化架次容忍度匹配；不做无限重连，避免 DUT 永久缺失时任务永久挂起。
- **已建立链路掉线路径零改动**：`s_is_connected` 为真的 DISCONNECT_EVT 仍触发 `ble_disconnect_cb` → `s_ble_abnormal_disconnect` → 优雅中止，CHILD_NODE_ROOT_LOSS_DESIGN.md 第 8 节验收行为保持。

## 性能与可靠性

- 全部新增为 O(1) volatile 标志读取；无新任务、无堆分配、无互斥量；GATT 回调上下文仅置标志与打日志（符合 .ai/coding-standard.md 的 ISR/回调约束）。
- 主动断开路径（`BTDisConnect=1` 前置，appTask.c:2642/3800/4100/4727）断开时 `s_is_connected` 为真，仍走 `s_disconnect_cb` → `ble_disconnect_cb` 的「user 断开」分支，语义不变。
- 失败路径最坏耗时从「20s 空等 + 一次中止」变为「≤15s 结果等待 + 最多 5 轮重试 ≈ 2 分钟内 agingError」，可观测、可退出。

## 兼容性与边界

- 不改 mesh、MQTT、老化数据链路、SQLite 补发逻辑；不改 `set_disallowed_level` 等 mesh-lite 策略。
- 未连接且非尝试中的迟到 DISCONNECT_EVT 仅记 WRN，不再误触发中止（消除陈旧事件误判这一类隐性风险）。
- 本项目无测试套件，验证手段为 AGENTS.md 编译命令 + 串口观察。


## Agent Extensions

### Skill
- **embedded-coding-standard**
  - Purpose：对本次 C 改动（Ble_control.c 的状态机与 appTask.c 的跨任务标志）按工程 `.ai/coding-standard.md` 及回调/ISR 上下文约束进行合规审查，重点核查 volatile/atomic 使用、回调上下文不碰存储/MQTT/任务删除、既有命名与 pragma region 分区风格一致。
  - Expected outcome：输出审查结论；若发现回调上下文违规、标志同步缺陷或风格偏离，按结论修正后再进入编译验证。
