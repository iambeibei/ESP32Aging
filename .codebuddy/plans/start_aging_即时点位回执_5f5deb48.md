---
name: start_aging 即时点位回执
overview: 收到有效 start_aging 时，在 mesh 层级错峰延时之前立即向 device/&lt;DEVICE_ID&gt;/cmd/reply/point 回一条 Code=1「已收到指令」，并移除成功路径末尾的延时回复；重复启动改为立即回 Code=1「指令重复，老化已启动」。
todos:
  - id: start-aging-immediate-ack
    content: 重构 appTask.c start_aging 分支：RUNNING 前移改 Code=1 中文、PN 判定前移、有效指令即时回执
    status: completed
  - id: ready-pn-reply-cleanup
    content: 调整 start_aging_with_ready_pn：删除延时成功回执，统一定义性 RUNNING 回复语义
    status: completed
    dependencies:
      - start-aging-immediate-ack
  - id: doc-sync
    content: 同步 AGING_CMD_PN_GUARD_DESIGN.md 的 start_aging 回执时序说明
    status: completed
    dependencies:
      - start-aging-immediate-ack
      - ready-pn-reply-cleanup
  - id: build-and-verify
    content: Use [skill:embedded-coding-standard] 做规范自查，编译并跑宿主回归
    status: completed
    dependencies:
      - doc-sync
---


## 需求概述
下发 `start_aging` 后，点位订阅方必须尽快拿到“指令已被设备收到”的回执，而不是等到错峰延时和初始化流程结束才收到结果。

## 核心功能
- 收到**有效**的 `start_aging`（Data/ProgramId 校验通过且 PN 已就绪）后，**立即**向 `device/<DEVICE_ID>/cmd/reply/point` 发布一条回执：
  `{"Seq":<时间戳>,"Data":{"CmdSeq":<命令Seq>,"Code":1,"Msg":"已收到指令","CmdData":null}}`
- 老化正在运行时收到新的 `start_aging`，立即回复 `Code=1` + `Msg="指令重复，老化已启动"`，不再先等待层级错峰。
- 成功启动老化后，**不再**发送原先那条延迟的 `"Aging started, command received and processed"`，确保同一条老化指令在成功场景下只有一条 point 回执。
- 既有失败语义保持不变：缺少 PN、初始化失败、Aging.json 保存失败等仍回 `Code=0` 并带原因。

## 边界
- 缺 PN 的拒绝路径不发送“已收到指令”，仍只回一条缺 PN 的错误回执（保持既有 PN 门契约与拒绝错峰）。
- 主题结构、JSON 键名、`CmdData:null`、外层 Seq 语义（时间戳）、订阅 QoS 均不变。
- 仅新增两条中文 Msg，其余失败文案维持现状。


## 技术栈
沿用现有工程：ESP-IDF + Bluedroid/Mesh-Lite + FreeRTOS + cJSON，C 语言。仅修改既有源文件，不新增文件、不改 `main/CMakeLists.txt`、不改 sdkconfig/分区表/NVS schema。

## 实现方案
在 `main/Task/appTask.c` 的 `parse_jsonCommand_MQTT()` → `start_aging` 分支内调整时序与回执，复用既有出口 `device_response_publish_point()`（内部走 `create_device_response()` 生成 JSON 并经 `app_mqtt_publish("device/%s/cmd/reply/point", ...)` 发送，同时登记 Seq 重放缓存）。

### 目标控制流（改造后）
```
Data/ProgramId 非法      → 立即 Code=0（不变）
RUNNING                  → 立即 Code=1「指令重复，老化已启动」→ goto cleanup（前移到错峰之前）
PN 未就绪                → 保持错峰延时 → reject_start_aging_missing_pn()（Code=0，不变）
PN 就绪                  → 立即 Code=1「已收到指令」→ 错峰延时 → ProgramId/RecordId 落盘 → start_aging_with_ready_pn()
                              ├─ 启动失败 → 追加 Code=0 失败原因
                              └─ 启动成功 → 不再发 point 回执（仅 aging_state_publish("agingStart") + 日志）
```

### 关键实现要点
1. `main/Task/appTask.c`（约 2908 行起的 start_aging 分支）
   - 将 `AGING_CMD_RUNNING` 判定从错峰之后（约 2971–2976）**前移**到 Data/ProgramId 校验之后、`vTaskDelay(stagger)` 之前；回复由 `Code=0` 英文改为 `Code=1` + `"指令重复，老化已启动"`，随后 `goto cleanup`。
   - 将 `aging_pn_is_ready()` 判定前移到错峰之前（仅用于决策分支）；PN 未就绪仍按原顺序“错峰后再拒绝”，不新增回执。
   - PN 就绪时，在错峰延时**之前**调用 `device_response_publish_point(start_seq, 1, "已收到指令")`。
2. `start_aging_with_ready_pn()`（约 2346 行起）
   - 删除末尾成功回执 `device_response_publish_point(start_seq, 1, "Aging started, command received and processed")`（约 2444 行），保留 `aging_state_publish("agingStart")` 与日志。
   - 内部防御性 RUNNING 分支（约 2354 行）回复改为 `Code=1` + `"指令重复，老化已启动"`，与入口语义一致；其余失败回执保留 `Code=0` 不变。
3. 文档同步：`AGING_CMD_PN_GUARD_DESIGN.md` 增补“有效指令收到即回执、成功路径不再发延时 point 回复”的说明，避免契约描述过时。

## 影响与风险
- **服务器/点位依赖变化**：成功路径不再有 `"Aging started, command received and processed"`。若上位机按该 Msg 判断成功，需改用 point 回执 + `event/state` 的 `agingStart`。这是本次最主要的行为变化。
- **单条/两条回执**：成功 1 条；失败 2 条（先回执、后失败原因）；缺 PN 1 条。与用户“成功只保留即时回执”的诉求一致。
- **Seq 重放缓存**：`mqtt_seq_record_reply()` 为单槽语义，成功时缓存“已收到指令”，失败回执会覆盖为失败原因，同 Seq 重试重放正确；窗口仍为 10 分钟、容量 32。
- **批量下发共振**：即时回执绕过层级错峰，N 台节点同刻各增 1 条小 payload。相比此前“状态+日志”多发布场景风险显著更小，但实机批量下发时需观察 mesh 上行；必要时可加 0～200ms 随机抖动（本次不默认加入）。
- **状态判定的时序**：`s_aging_cmd_state`/`s_new_pn_ready` 由 mqtt_rx 任务单写，`stop_aging`/`set_pn` 同任务串行，前移判定不会引入新的跨任务竞态；`start_aging_with_ready_pn()` 内部仍保留防御性复查。
- 不影响 MQTT 保活、重连调度、Mesh 层数错峰、SQLite 补发等既有机制。

## 验证
1. `idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig build` 通过，无新增告警。
2. `python3 tests/test_mqtt_uplink.py` 保持通过（该用例不编译 appTask.c，用于防误伤）。
3. 实机日志与报文字段核对：
   - 有效指令后立即出现 point 回执（Code=1/已收到指令），随后才打印 `Start aging stagger: level=..`；
   - 成功启动后不再有第二条 point 回执，但有 `agingStart`；
   - 运行中下发新 Seq → 立即收到 Code=1「指令重复，老化已启动」；
   - 同 Seq 重试 → 日志 `Duplicate MQTT command seq=.., replaying last reply (point)`，不重复启动；
   - 缺 PN 指令 → 仍只有一条缺 PN 的 Code=0 回执。

## Agent Extensions
### Skill
- **embedded-coding-standard**
  - Purpose: 在改动定稿前对齐工程 `.ai/coding-standard.md`（命名、注释语言、错误处理、调用上下文约束），对新增的两处中文 Msg 与分支重排做一次规范自查。
  - Expected outcome: 输出符合工程既有风格的实现要点与潜在规范问题清单，避免引入与既有代码风格冲突的写法。
