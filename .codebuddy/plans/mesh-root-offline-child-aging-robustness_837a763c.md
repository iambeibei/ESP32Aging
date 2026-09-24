---
name: mesh-root-offline-child-aging-robustness
overview: 两项改造：①SQLite 淘汰收紧为"仅淘汰已上传(pushed=1)"，空间不足时停止写入并明确告警，并复核容量阈值；②群控放行指令必须返回明确响应（上位机以收到响应为放行成功），断网重连后补发一次"已完成任务"响应并支持指令幂等去重。
todos:
  - id: verify-semantics
    content: 用 [subagent:code-explorer] 核实 app_mqtt_publish 返回语义、Seq 取值、create_device_response 结构与 FATFS 可用空间
    status: completed
  - id: sqlite-evict-only-pushed
    content: 改造 SqLite.c 淘汰 SQL 为仅选 pushed=1，新增缓存满返回码并在 SqLite.h 固化契约
    status: completed
    dependencies:
      - verify-semantics
  - id: sqlite-caller-alarm
    content: 调用方区分缓存满错误码，节流告警与计数，禁止因写满中止老化
    status: completed
    dependencies:
      - sqlite-evict-only-pushed
  - id: sqlite-capacity-review
    content: 用 print_fatfs_usage 实测后复核 MAX_RECORDS_COUNT 与 DB_MIN_FREE_BYTES 阈值
    status: completed
    dependencies:
      - verify-semantics
  - id: nextstep-reply
    content: 新增 device_response_publish_nextstep 并在 CanNextstep 分支返回明确响应与失败分支
    status: completed
    dependencies:
      - verify-semantics
  - id: nextstep-idempotent
    content: 实现 cmd_seq 幂等去重与等待态校验标志 s_waiting_nextstep
    status: completed
    dependencies:
      - nextstep-reply
  - id: pending-reply-resend
    content: 实现待补发响应持久化并在 MQTT_EVENT_CONNECTED 后补发已完成任务响应
    status: completed
    dependencies:
      - nextstep-idempotent
  - id: groupctl-timeout
    content: 将群控等待改为超时轮询自动放行，支持中止唤醒与离线推进留痕
    status: completed
    dependencies:
      - nextstep-idempotent
  - id: null-guard
    content: 为 g_db1_result 各使用点补充空指针防护
    status: completed
  - id: build-regression
    content: 用 [skill:lsp-code-analysis] 复核影响面，编译验证并输出断网场景上机回归清单
    status: completed
    dependencies:
      - sqlite-evict-only-pushed
      - sqlite-caller-alarm
      - sqlite-capacity-review
      - nextstep-reply
      - nextstep-idempotent
      - pending-reply-resend
      - groupctl-timeout
      - null-guard
---

## 需求概述

针对"根节点（IsRoot=1）断网 → 子节点（IsRoot=0）老化中断"场景做两项改造，方案需完整、可执行，并明确各部分处理逻辑：

1. **SQLite 淘汰策略改造**：先核实现有实现是否有对应处理，再收紧为"未上传记录绝不淘汰"——仅淘汰 `pushed=1`；空间不足时停止写入并明确告警，且不能因写入失败而中止老化。
2. **群控放行响应与补发**：
   - 收到群控放行指令（`CanNextstep`）时必须**返回明确响应**；上位机**只有收到该响应**才判定放行成功。
   - 断网恢复连接后，需**补发一次"已完成任务"响应**（`publish_aging_complete(1, ...)` / `device/%s/event/TaskCP`），保证状态同步与放行结果可追溯。
   - 放行响应发送失败时上位机会重发同一 `Seq` 指令，设备侧必须**幂等去重**（不重复推进步骤，但要重发响应）。

## 已确认的产品策略（继续有效）

- 群控模式（`AgingCMode == 2`）等待放行**超时后本地自动放行**，避免断网导致永久卡死；离线推进须留痕并在联网后补偿上报。
- SQLite 零丢数据优先：`pushed=0` 绝不淘汰，空间不足停止写入并告警。

## 核心功能点

- SQLite：淘汰 SQL 加 `WHERE pushed = 1` 硬约束；新增"缓存满"专用返回码；调用方区分处理、节流告警、计数；按实测空间复核容量阈值。
- 群控：新增放行响应（沿用 `device_response_publish_*` 范式与 `create_device_response` 载荷）；指令按 `cmd_seq` 幂等去重；待补发响应持久化（NVS，单条去重）；`MQTT_EVENT_CONNECTED` 后触发补发；等待循环改超时轮询并支持中止唤醒。
- 边界不变：不改 SQLite schema v5 与日志 pragma；不改"先落库再发 MQTT、成功才置 `pushed=1`"；不动既有主题语义与中文键名，新增字段/主题向后兼容。


## 技术栈

沿用现有工程：ESP-IDF v5.4.x、ESP32-S3（PSRAM）、C + FreeRTOS + cJSON、ESP-WIFI-MESH + esp-mqtt + SQLite（`/fatfs/data.db`）。不引入新组件、不改分区与构建配置。

## 现状核实结论

### 任务 1：SQLite 淘汰 —— "有部分处理，但不严格、不可观测"

已有处理：
- `get_overwrite_local_id_locked`（`main/DataStorage/SqLite/SqLite.c:1246-1275`）SQL 为 `ORDER BY pushed DESC, local_id ASC LIMIT 1` → **已实现"优先淘汰已上传"**。
- 无可淘汰行时 `goto rollback`（`:1371-1375`），`InsertStructuredRecord` 返回 -1 → **已有"拒绝写入"路径**。
- 触发条件（`:1366-1368`）：`record_count >= MAX_RECORDS_COUNT(1200)` 或 `sqlite_storage_low_on_free_space()`（`< DB_MIN_FREE_BYTES = 512KB`，`:1210`），仅在插入**新**记录时触发。

缺失（必须改）：
1. **无 `WHERE pushed = 1` 硬约束** → 全表 `pushed=0` 时退化为淘汰未上传数据（长时间断网必然发生）。
2. **写满不可观测**：仅 `ESP_LOGE` + 调用方 `aging_error_log`，无独立错误码、无计数、无上报（已有 `device/%s/event/warn` 先例 `appTask.c:2685`）。
3. **返回码无法区分**参数错误与缓存满（都是 -1）。
4. **容量阈值可能不匹配**：`MAX_RECORDS_COUNT 1200` × `DB_VALUE_DATA_MAX_LEN 2048` ≈ 2.4 MB，而 `data_storage` 分区 `0x260000` 约 2.4 MB（`partition.csv:9`），叠加 FATFS 元数据会提前触发 `low_space`，需实测重算。

### 任务 2：群控放行 —— 目前既无响应也无补发

- `CanNextstep` 分支（`appTask.c:3256-3264`）只置标志，**无回执、无 Seq 回传、无失败分支**。
- 其它命令已有回复范式：`device_response_publish_point/state/setlog`（`appTask.c:505-551`）→ `device/%s/cmd/reply/{point,state,setlog}`，载荷 `create_device_response(time(NULL), cmd_seq, code, msg)`（`Json_data.h:63`），`cmd_seq` 取 `Seq->valueint`。
- `publish_aging_complete`（`appTask.c:779-812`）仅 `ret < 0` 时记环形日志，**无重试/补发**；群控步骤完成后调用（`appTask.c:4932-4942`）与 `while (Cannextstep == 0)` 死等叠加 → 断网即永久卡死。
- 联网钩子已存在：`MQTT_EVENT_CONNECTED` 分支末尾 `Config_Report(100);`（`mqtt_app.c:225`），订阅含群控主题 `server/rack/%s/CanNextstep`（`mqtt_app.c:216-222`）。

## 实施方法

### A. SQLite 淘汰改造（零丢数据）

1. 收紧 SQL：`SELECT local_id FROM data_cache WHERE pushed = 1 ORDER BY local_id ASC LIMIT 1;`（保留最旧优先）。
2. 无 `pushed=1` 可淘汰时：不删除、不插入，返回**新增专用错误码 -2**（与既有 -1 参数/校验错误区分），在 `SqLite.h` 注释固化"未上传记录绝不淘汰"契约。
3. 调用方（`appTask.c:5348` 附近）对 -2 单独处理：`aging_error_log` + 环形日志 + 计数 + **节流**（复用 `aging_data_upload_result` 的 60 秒节流思路，`appTask.c:404-448`）上报一次 `device/%s/event/warn`；**不得视为致命错误中止老化**。
4. 容量复核：用 `print_fatfs_usage()`（`SqLite.c:397`）/`get_record_count_locked` 实测可用空间后重设 `MAX_RECORDS_COUNT` / `DB_MIN_FREE_BYTES`，注释写明推导依据。
5. 不改：schema v5、`journal_mode=DELETE` + `synchronous=FULL`、中文键名。

### B. 群控放行响应与补发

1. **放行响应**：新增 `device_response_publish_nextstep(cmd_seq, code, msg)`（严格仿 `appTask.c:505-551`），在 `CanNextstep` 分支中：取 `Seq->valueint`；校验 `Data.IsCanNext` 为数字且 0/1；用新增的 `s_waiting_nextstep` 标志校验是否处于等待放行态（由等待循环置位/清除）；成功置 `Cannextstep` 后**立即**回复 `code=1`（携带 PN/步骤索引便于对账）；非法态回 `code=0` + 原因。主题用新增的 `device/%s/cmd/reply/nextstep`（不动既有主题语义），实施前与上位机确认主题名。
2. **幂等去重**（因"上位机只有收到响应才判定成功"）：同一 `cmd_seq` 重复到达 → **不重复推进步骤**，但**重新回一次响应**；用 `last_nextstep_cmd_seq` 记录。
3. **待补发响应**：`publish_aging_complete(1, ...)` 或放行响应发送失败时记录 pending（类型、seq/cmd_seq、PN、AgingNumber、步骤索引、时间戳），持久化到 NVS（复用 `SelfRecovery_*` 风格 key），**只保留最近一条并去重写**（NVS 磨损考虑）。
4. **联网恢复补发**：在 `MQTT_EVENT_CONNECTED`（`mqtt_app.c:225` 之后）调用补发入口 `aging_resend_pending_replies()`：重发"已完成任务"响应（幂等，按 seq/时间戳去重），成功后清除 pending；带最大重试次数与退避。
5. **超时自动放行联动**：`appTask.c:4937` 死等改为带超时的轮询等待（超时常量集中定义，放 `task_config.h` 与 `LG_*` 同风格）；超时自动放行并记"离线自动推进"事件，联网恢复时与"已完成任务"响应一并补发；等待期间轮询检查中止标志而非纯 `vTaskDelay`。
6. 返回值语义统一：现有代码混用 `> 0`（`:4957`、`:5370`）与 `< 0`（`:761`、`:806`）判定 `app_mqtt_publish`，实施第一步必须核实其真实返回语义并统一判定函数。

### C. 顺带稳定性项

- 补发历史数据**不要**用 `QueryStructuredRecordsBySNAndPushState`（`SqLite.c:1749`）/`QueryUnpushedRecords`（`:1838`）的大数组接口（内部 `memset(sizeof(QueryResult) * max_count)`，`QueryResult` 约 4.7 KB 易爆栈），改用单条循环 `QueryLatestRecordBySNAndPushState`（`:1808`），缓冲必须在堆/PSRAM。
- `g_db1_result` 现为 PSRAM 指针（`SqLite.h:31`、`SqLite.c:32`、`:2067` 分配），`SqLite.c:1740/1747` 与 `appTask.c:4953/4955/4957/4959` 无 NULL 校验，顺带补防护。

## 执行要点

- 新任务须经 `create_cpu1_task()` 钉 CPU1；新常量沿用 `task_config.h` 的 `LG_*` 风格。
- 不改"先落库再发 MQTT、成功才 `pushed=1`"的掉电可靠性顺序（`appTask.c:5372-5384`）。
- 新增上报字段/主题必须向后兼容；`device/%s/event/TaskCP` 既有字段语义不变。
- 补发与告警路径需节流，避免断网恢复瞬间的消息风暴。

## 验证方法

`idf.py build` 通过；上机模拟：根节点拔 WAN → 观察子节点 `Network_Flag` 翻转日志、群控步骤超时自动推进、放行响应是否发出；恢复联网 → 观察 `MQTT_EVENT_CONNECTED` 后"已完成任务"响应被补发且上位机状态同步、重复 `Seq` 指令被幂等去重；长跑后检查 SQLite 记录数与 `pushed` 分布（`storage_print_all_records()` `appTask.c:451`、`print_db_files`、`print_fatfs_usage`），确认无 `pushed=0` 被淘汰；按键取栈水位与空闲堆（`appTask.c:3607` 起）确认无泄漏/爆栈。

## 目录结构（涉及文件）

```
main/
├── DataStorage/SqLite/
│   ├── SqLite.c      # [MODIFY] 淘汰 SQL 加 WHERE pushed=1；新增缓存满返回码 -2；NULL 防护
│   └── SqLite.h      # [MODIFY] 新增错误码注释与"未上传绝不淘汰"契约说明；容量阈值注释
├── Task/
│   ├── appTask.c     # [MODIFY] CanNextstep 分支加响应+校验+Seq 去重；新增 device_response_publish_nextstep；
│   │                 #          等待循环改超时轮询+中止唤醒+离线推进留痕；pending 补发入口；
│   │                 #          InsertStructuredRecord 失败分支差异化处理；g_db1_result NULL 防护
│   ├── appTask.h     # [MODIFY] 暴露补发入口/等待态标志（如需跨文件）
│   └── task_config.h # [MODIFY] 新增群控放行超时等常量（LG_* 风格）
├── APP/MQTT/
│   └── mqtt_app.c    # [MODIFY] MQTT_EVENT_CONNECTED 后触发补发入口
└── Tool/
    └── SelfRecovery.c/.h  # [MODIFY] 新增 pending 响应的持久化读写（如需新增 key）
```


## Agent Extensions

### SubAgent
- **code-explorer**
  - Purpose：核实 `app_mqtt_publish` 的真实返回语义、`Seq` 在各命令分支的获取方式、`create_device_response` 字段结构、FATFS 实际可用空间相关实现，以及 `pushed` 字段在 schema 中的类型/取值。
  - Expected outcome：给出可依赖的事实清单，确保返回码判定、响应载荷与容量阈值推导不基于猜测。

### Skill
- **lsp-code-analysis**
  - Purpose：对 `Cannextstep`、`AgingCMode`、`publish_aging_complete`、`InsertStructuredRecord`、`g_db1_result` 做引用/调用层级与影响面分析。
  - Expected outcome：明确每处改动的调用方与回归点，防止遗漏唤醒条件、中止路径与补发触发点。
