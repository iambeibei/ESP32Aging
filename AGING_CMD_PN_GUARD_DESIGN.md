# 老化指令 PN 缺失处理设计（start_aging 前置 PN 门）

## 1. 需求与结论

收到老化指令（`start_aging`）后**首先判断 pn 是否已设置**：

- 已设置（`s_new_pn_ready==1 && PN_Code_Ready[0]!='\0'`）→ 按既有流程启动；
- **未设置 → 立即经 MQTT 返回老化异常，明确提示缺少 pn，状态回 IDLE；
  服务器需按 `set_pn` → `start_aging` 顺序重新下发**（产品已确认：直接拒绝，
  不进入 WAIT_PN 缓存等待）。

## 2. 现状（改动前）

`parse_jsonCommand_MQTT()` 的 `start_aging` 分支（appTask.c:2515-2599）：

```text
Data 对象校验(2519) → ProgramId 校验(2529) → 存 RecordId(2541-2554)
  → RUNNING 拒绝(2560) → WAIT_PN 重复/互斥处理(2566)
  → s_new_pn_ready==1 启动(2586)
  → else cache_pending_start_command() 进 WAIT_PN(2597)   ← 本次移除
```

问题：`start_aging` 先于 `set_pn` 到达时，设备只缓存等待，不反馈异常；
服务器无法区分「已受理」与「缺 PN 未启动」，且遗留挂起状态需 stop_aging 清理。

## 3. 处理逻辑设计（改动后）

分支顺序（顺序即契约，不得调整）：

```text
Data 对象校验
  → ProgramId 校验（仅校验类型/非空，不发布任何状态消息）
  → RUNNING 拒绝（"Aging already started, ignoring new command"）
  → 【新增】PN 门：未设置 → reject_start_aging_missing_pn(seq) 后结束本条命令
  → aging_state_publish_ProgramId()（仅启动路径发布，拒绝路径不产生“已受理”信号）
  → 存 RecordId（仅通过 PN 门后写入，避免拒绝路径产生 NVS 副作用）
  → start_aging_with_ready_pn() 启动
```

### 3.1 PN 门判定

```c
static bool aging_pn_is_ready(void)
{
    return s_new_pn_ready == 1 && PN_Code_Ready[0] != '\0';
}
```

### 3.2 拒绝路径 `reject_start_aging_missing_pn(start_seq)`

按序执行四步，任一步失败不影响其余步骤（错误路径不空，规范要求）：

1. **清理旧固件遗留 WAIT_PN 挂起**（兼容性，见第 6 节）：
   `s_aging_cmd_state==AGING_CMD_WAIT_PN` 时删除 `Aging.json`、
   `persist_pending_start_state(false, 0)`、`s_pending_start_seq=0`、
   `s_aging_cmd_state=AGING_CMD_IDLE`，并 `aging_error_log` 记录被丢弃的挂起 Seq；
2. **指令回复**（明确提示缺少 pn）：
   `device_response_publish_point(start_seq, 0, AGING_START_REPLY_NO_PN)`
   → topic `device/%s/cmd/reply/point`；
3. **老化异常状态**：`aging_state_publish("agingError")`
   → topic `device/%s/event/state`；
4. **错误日志**：`aging_error_log("Start aging rejected: PN is not set, cmd_seq=%d", start_seq)`
   （本地环形日志必写；`AgingLogActive` 时同步 MQTT log/runtime + log/error）。

消息文案：

```c
#define AGING_START_REPLY_NO_PN "Aging start rejected: PN is not set, please send set_pn first"
```

（沿用现有英文回复风格，与 "Aging already started, ignoring new command" 一致。）

### 3.3 同步移除

- 删除 `cache_pending_start_command()`（appTask.c:2322-2359）及其唯一调用
  （start_aging 分支 2597 行处）；`start_aging` 不再产生 WAIT_PN 挂起命令。
- `aging_cmd_state_name()` 保留 `WAIT_PN` 分支（旧固件 NVS 恢复路径仍可能进入，
  见第 6 节），仅不再由新命令产生。

## 4. 返回消息格式

### 4.1 指令回复 `device/%s/cmd/reply/point`（失败）

```json
{"Seq":<unix_ts>,"Data":{"CmdSeq":<start_seq>,"Code":0,"Msg":"Aging start rejected: PN is not set, please send set_pn first","CmdData":null}}
```

- 生成：`create_device_response()`（Json_data.c:1078-1115），发布：
  `device_response_publish_point()`（appTask.c:508-522）。
- `Code=0` 与现有失败回复一致（RUNNING 拒绝、参数非法同风格）。
- `Seq` 为上报时间戳；`CmdSeq` 回带服务器下发时的 `Seq`，供服务器关联。

### 4.2 老化异常状态 `device/%s/event/state`

```json
{"Seq":<unix_ts>,"Data":{"AgingState":"agingError"}}
```

- 生成：`create_aging_state_json()`（Json_data.c:894-928），发布：
  `aging_state_publish()`（appTask.c:698-709）。
- 与 `Aging_Test_Task` 中止路径（appTask.c:4113/4160）同一状态通道，
  平台侧按既有 agingError 语义处理（告警/记录，不进入步骤流转）。

### 4.3 错误日志

- 本地：`storage_write_record_cyclic` 环形日志（`aging_error_log` 无条件写）。
- MQTT（`AgingLogActive` 时）：`device/%s/log/runtime` 与 `device/%s/log/error`，
  受 `Mqtt_Log_Mode` 门控（mqtt_app 侧 remote_log 指令可调）。

### 4.4 服务器交互全流程（补偿机制）

```text
服务器                              子节点
  │  start_aging {Seq:N}              │
  │ ─────────────────────────────────> │  PN 门未过
  │ <───────────────────────────────── │  cmd/reply/point {CmdSeq:N, Code:0, Msg:缺PN}
  │ <───────────────────────────────── │  event/state {AgingState:"agingError"}
  │                                   │  本地错误日志；状态回 IDLE，无残留
  │  set_pn {Data:{PN:"..."}}         │
  │ ─────────────────────────────────> │  PN_Code_Ready 就绪并持久化
  │ <───────────────────────────────── │  cmd/reply/pn {Code:1, CmdData:{GetPN}}
  │  start_aging {Seq:N}（重发）       │
  │ ─────────────────────────────────> │  start_aging_with_ready_pn()
  │ <───────────────────────────────── │  cmd/reply/point {CmdSeq:N, Code:1}
  │ <───────────────────────────────── │  event/state {AgingState:"agingStart"}
```

补偿机制要点：

1. **设备侧零残留**：拒绝路径不写 `Aging.json`、不写 `RecordId`、不动
   `aging_valid`/`PN_Code`/Ready PN（保留已就绪的 Ready PN 不被误清）；
   服务器重发即可启动，无需额外清理指令。
2. **重发即可用**：Ready PN 在拒绝路径中保持有效，服务器甚至可只补发
   `start_aging`（不必重新 `set_pn`）；文档建议仍按 set_pn→start_aging 全序重发，
   语义最清晰。
3. **幂等**：重复收到缺 PN 的 start_aging，每次独立拒绝并各自回复，不进入
   WAIT_PN，不产生第二条挂起。
4. **平台侧契约**：`cmd/reply/point` 的 `Code=0` + `Msg` 含 "PN is not set"
   即表示「未启动、需先 set_pn」；配合 `event/state=agingError` 双通道可区分
   「缺 PN 拒绝」与「老化运行中异常中止」。

## 5. 状态机影响

| 状态 | 改动前（缺 PN 的 start_aging） | 改动后 |
|---|---|---|
| IDLE | → WAIT_PN（缓存 Aging.json + NVS start_pending=1） | 保持 IDLE，回复失败 |
| WAIT_PN | 由新 start_aging 维持/互斥 | 不再由新命令产生；仅旧固件遗留（见第 6 节） |
| RUNNING | 已消费 Ready PN，PN 门恒真，不受影响 | 同左 |

`stop_aging` 对 WAIT_PN 的清理分支（appTask.c:2633 无条件删 Aging.json）保持不变。

## 6. 兼容性（旧固件升级）

- 旧固件可能已通过 NVS 持久化 `start_pending=1`；上电恢复路径
  （`Init_ByNetwork_Flag`，appTask.c:3121-3147）保持兼容：恢复 WAIT_PN 挂起，
  若 Ready PN 已在则直接补启动（旧数据不丢）。
- 新固件运行中收到缺 PN 的 start_aging：若设备正处于旧固件遗留的 WAIT_PN，
  按 3.2 第 1 步清理挂起并回 IDLE，然后走拒绝回复——一条命令完成
  「清遗留 + 拒绝 + 提示」，服务器按全序重发即可。
- 不再产生新的 WAIT_PN 持久化标志；`NVS_KEY_START_PENDING/START_SEQ` 键保留
  （读取兼容），无写入方后可择期清理。

## 7. 验收用例

| # | 场景 | 期望 |
|---|---|---|
| 1 | 空闲发 start_aging（无 set_pn） | cmd/reply/point Code=0 Msg 缺 PN；event/state agingError；本地错误日志；状态 IDLE；无 Aging.json |
| 2 | 用例 1 后直接重发 start_aging | 同样拒绝（幂等），无第二条挂起 |
| 3 | 用例 1 后发 set_pn 再 start_aging | set_pn 回复 Code=1 GetPN；start_aging 正常启动，Code=1 + agingStart |
| 4 | RUNNING 中发 start_aging | 仍为 "Aging already started"（RUNNING 检查在 PN 门之前，顺序正确） |
| 5 | start_aging 缺 Data / ProgramId | 维持现有参数校验回复（先于 PN 门） |
| 6 | 旧固件遗留 WAIT_PN（NVS start_pending=1）+ 新 start_aging | 清理挂起 + 拒绝回复；日志记录被丢弃 Seq |
| 7 | 拒绝路径后 ReadId/配置 | RecordId 未被污染（PN 门后才写） |

## 8. 改动清单

- `main/Task/appTask.c`：
  - `start_aging` 分支：新增 PN 门与 `reject_start_aging_missing_pn()`，
    RecordId 与 ProgramId 状态发布移到 PN 门后，删除 `cache_pending_start_command()` 调用；
  - 新增 `aging_pn_is_ready()` 辅助函数与 `AGING_START_REPLY_NO_PN` 宏；
  - 删除 `cache_pending_start_command()` 函数本体（确认无其他引用后）；
  - `start_aging_with_ready_pn()` 内层 PN 检查复用 `aging_pn_is_ready()`，
    防御分支补 MQTT 拒绝应答（审查项）。
- `version.txt`：版本号递增（与子节点抗根掉线修复同批发布）。

## 9. 代码审查记录（CERT C + FreeRTOS + 本地规范，双模型交叉）

| # | 级别 | 问题 | 结论 |
|---|---|---|---|
| 1 | medium | 拒绝路径仍先发布 ProgramId 状态（可能被服务器误判为受理） | 已修：发布移入启动分支 |
| 2 | low | PN 门条件两处重复 | 已修：内层复用 `aging_pn_is_ready()` |
| 3 | low | `start_aging_with_ready_pn` PN 失败分支无 MQTT 应答（潜伏） | 已修：补拒绝回复 |
| 4 | low | RecordId NVS 写失败仅记日志仍启动 | 不修：与 current PN 写失败处理一致，列为已知限制 |
| 5 | — | PN 门顺序（RUNNING 检查在前） | 确认正确：RUNNING 时 Ready PN 已消费，PN 门前置会误报 |

平台侧契约注意：`event/state` 的 ProgramId 消息现在只在真正启动前发布，
可作为「已受理并将启动」的信号；拒绝路径只有 `cmd/reply/point`(Code=0) +
`event/state`(agingError) + 日志三路反馈。
