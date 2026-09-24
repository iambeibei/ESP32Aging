---
name: mesh-root-offline-child-aging-robustness
overview: 分析"根节点断网导致子节点老化中断"的触发条件与影响范围，并给出针对性修复：群控模式加超时自动放行+联网补记、SQLite 未上传记录零淘汰、时间基准防跳变、断网可观测与自愈。
todos:
  - id: verify-facts
    content: 用 [subagent:code-explorer] 核实淘汰 SQL、AgingCMode 取值、协议缓存与 QueryResult 现状
    status: pending
  - id: groupctl-timeout
    content: 改造 appTask.c:4932-4942 群控等待为超时自动放行并实现离线推进补记上报
    status: pending
    dependencies:
      - verify-facts
  - id: sqlite-nodrop
    content: 改造 SqLite.c 淘汰策略为仅淘汰 pushed=1，写满返回错误并告警，复核容量阈值
    status: pending
    dependencies:
      - verify-facts
  - id: monotonic-timing
    content: 将步骤计时改单调时钟并重建 NVS Deadline 恢复逻辑，消除 SNTP 跳变误判
    status: pending
    dependencies:
      - verify-facts
  - id: net-observability
    content: 增加 Network_Flag 翻转日志与事件上报、MQTT/HTTP 接收任务重建、联网恢复后补发
    status: pending
    dependencies:
      - verify-facts
  - id: startup-nullfix
    content: 启动期协议下载容错与 g_db1_result 空指针防护
    status: pending
    dependencies:
      - verify-facts
  - id: build-regression
    content: 用 [skill:lsp-code-analysis] 复核影响面并编译验证，输出断网场景上机回归清单
    status: pending
    dependencies:
      - groupctl-timeout
      - sqlite-nodrop
      - monotonic-timing
      - net-observability
      - startup-nullfix
---

## 需求概述
分析"根节点（IsRoot=1）断网 → 子节点（IsRoot=0）老化中断"这一场景，定位触发条件、影响范围与现场表现，并给出针对性修复方案，使根节点失联后子节点仍能稳定运行，不因老化机制误判而中断。

## 已确认的产品策略（必须遵守）
- 群控模式：等待上位机 `CanNextstep` 超过 N 分钟后**本地自动放行**继续下一步；联网后以"离线自动推进"事件**补偿上报**，保证上位机可对账。
- 数据策略：`pushed=0` 的未上传记录**绝不淘汰**，只淘汰 `pushed=1`；空间不足时**停止写入并告警**，不允许静默覆盖。

## 核心修复范围
1. 群控步骤间死等改超时放行 + 离线推进可追溯（补记上报）。
2. SQLite 淘汰策略改为"仅淘汰已上传"，写满时明确失败 + 告警，并复核容量阈值与分区空间是否匹配。
3. 步骤计时改单调时钟，防止 SNTP 首次同步造成时间跳变导致步骤被跳过/延长（误判）。
4. 断网可观测与自愈：网络状态翻转日志/事件、MQTT/HTTP 接收任务失败后重建、联网恢复后主动补发。
5. 启动阶段协议下载容错；`g_db1_result` 空指针防护。

## 边界
不改 SQLite schema v5 与日志 pragma；不改"先落库再发 MQTT、成功才置 `pushed=1`"顺序；不动中文 JSON 键名与既有 MQTT 主题语义。


## 技术栈
沿用现有工程：ESP-IDF v5.4.x、ESP32-S3（PSRAM）、C + FreeRTOS + cJSON、ESP-WIFI-MESH + esp-mqtt + SQLite（`/fatfs/data.db`）。不引入新组件。

## 失效机理（已核实，带行号）

1. **群控死等（核心中断点）** `appTask.c:4932-4942`：`while (Cannextstep == 0) vTaskDelay(10000);` 无限等待。`Cannextstep`（`appTask.c:100`）唯一来源是 MQTT 主题 `server/rack/<AgingNumber>/CanNextstep`（`appTask.c:3256-3263`，订阅见 `mqtt_app.c:216-223`，仅群控订阅）。根节点断网即永久收不到 → 步骤不推进、`agingDataAMode=IdleState` 使 `app_AgingData_Get_handle`（`:5284`，采样条件 `agingDataAMode != IdleState`）零采样。全工程无 `esp_task_wdt` 注册，不复位、静默挂起。
2. **启动期依赖外网** `appTask.c:4298 test_http_getproto_New` + `:4308 WaitAgingDeviceProtocolReady(10000)` 失败即 `start_failed`；`ExDevice_Check()`（`:4397` 起）只查本地设备，不受网络影响。
3. **时间基准跳变** 步骤用 `time(NULL)` 与 NVS 持久化的 `Deadline`（`:4818-4840`、写入 `:4828`、恢复 `:3507`）。SNTP 仅在 `MESH_EVENT_TODS_STATE` 可达时启动（`mesh.c:1074-1088`），且 `s_time_synced` 后不再同步（`mesh.c:686-689`）。若首次同步发生在老化运行中，`time()` 从开机秒数跳到 epoch → `now >= deadline` 立即成立 → 步骤被瞬间跳过（误判中断）。
4. **缓存淘汰丢数据** `SqLite.c:1366-1388` 在 `record_count >= 1200` 或剩余 < 512KB 时取 `overwrite_local_id` 删除（`:1281`）；无 `pushed=1` 可淘汰时会退化到 `pushed=0`。5 秒一包约 100 分钟打满。
5. **补发不重试** `appTask.c:4951-4969`：`QueryLatestRecordBySNAndPushState` 循环内 publish 失败即 `break`（`:4965`）。
6. **RX 任务自删不再重建** `:3311-3315`（MQTT）、`:2544-2548`（HTTP）；`Init_ByNetwork_Flag`（`:3425-3600`）只执行一次（`break` → `:3599 vTaskDelete`），放大第 1 条后果。
7. **上传路径本身正确**：`:5372-5384` `upload_success = Network_Flag && (app_mqtt_publish(...) > 0)`，断网不尝试 publish，先以 `pushed=0` 落库；失败仅计数（`aging_data_upload_result` `:405-448`），不中断老化。**不可改坏此顺序**。
8. **顺带**：`g_db1_result` 已是 PSRAM 指针（`SqLite.h:31` / `SqLite.c:32`、分配 `:2067`），但 `:1740/:1747` 与 `appTask.c:4953` 无 NULL 校验。

## 实施方法

### 1. 群控超时自动放行
- 将 `:4937` 改为带超时的等待循环：以 `xTaskGetTickCount()` 计时，超时常量集中定义（建议放在 `task_config.h`，与 `LG_*` 同风格，或 appTask 顶部），默认建议 10 分钟级，可配置。
- 超时后：写 `aging_runtime_log` + 本地环形日志（含 PN、步骤索引、等待时长），置 `Cannextstep = 0` 继续下一步，并置"离线自动推进"标记与计数。
- 联网后（`Network_Flag` 由 0 变 1）把累计的离线推进记录**补偿上报**（复用既有 `device/%s/event/state` 或新增兼容字段，不得破坏既有主题语义），使上位机可对账。
- 保留退出条件：等待期间仍要能被停止/中止指令唤醒（轮询中检查中止标志，而不是纯 `vTaskDelay` 死睡）。

### 2. 零丢数据淘汰策略
- 改 `get_overwrite_local_id_locked`（`SqLite.c:1270` 附近）：SELECT 严格限定 `pushed = 1`；无命中时 `InsertStructuredRecord` 返回明确错误码（不写入、不覆盖）。
- 调用方（`appTask.c:5348` 附近）据此写 `aging_error_log` + 环形日志并计数，采样继续，不中断老化。
- 复核容量：`MAX_RECORDS_COUNT 1200` × `DB_VALUE_DATA_MAX_LEN 2048` 理论约 2.4 MB，与 `data_storage` 分区 `0x260000`（约 2.4 MB，`partition.csv:9`）几乎相等，需按 FATFS 实际可用空间重算 `MAX_RECORDS_COUNT` / `DB_MIN_FREE_BYTES`，必要时下调条数或提高低空间阈值。

### 3. 时间基准防护
- 步骤剩余时长判定改用单调时钟：`esp_timer_get_time()`（微秒）或 `xTaskGetTickCount()` 计算"还需等待多久"，循环内按剩余时间推进；`time(NULL)` 仅用于生成上报时间戳。
- NVS 持久化的 `Deadline`（`:4822`、`:3507`）恢复时改为"恢复剩余时长"而非直接沿用绝对时刻；检测到 `time()` 相对上次基准跳变超过阈值时重算 deadline 并记日志。

### 4. 断网可观测与自愈
- `Network_Flag` 翻转（`mesh.c:1074-1088`）时记日志，复用 `aging_state_publish` / `device/%s/event/state` 上报，便于云端定位"断网窗口"。
- MQTT/HTTP 接收任务失败自删后，由常驻监控任务按周期检测句柄并重建（避免 `:3314`/`:2547` 后永久失效）。
- 联网恢复后触发一次补发循环：优先复用 `QueryLatestRecordBySNAndPushState` 单条循环（**不要**用 `QueryStructuredRecordsBySNAndPushState` 大数组接口，其 `memset(sizeof(QueryResult)*max_count)` 在 `QueryResult` 约 4.7 KB 时极易爆栈，见 `SqLite.c:1762/1846`）；补发失败要有退避与最大次数，避免忙等。

### 5. 启动容错与 NULL 防护
- 断网时优先复用本地已缓存协议（先确认 `import_scpi/modbus/can_protocol_to_device`（`:2228`/`:2264`/`:2302`）是否落盘可复用），失败时明确重试与上报，而非直接 `start_failed`。
- `g_db1_result` 全部使用点（`SqLite.c:1740/1747`、`appTask.c:4953/4955/4957/4959`）加空指针校验。

## 风险与注意事项
- 群控"自动放行"改变工艺纪律，必须保留可追溯记录与上报，便于上位机事后校正。
- 改淘汰策略后，写满是"停止写入"而非丢失，需确认上层对 `InsertStructuredRecord` 失败的容错（不能把失败当作致命错误去中止老化）。
- 补发/新增上报的缓冲区必须堆或 PSRAM 分配，`QueryResult` 约 4.7 KB 严禁放栈（`aging_upload` 栈仅 8192）。
- 所有新常量沿用 `task_config.h` 的 `LG_*` 风格；新任务仍须经 `create_cpu1_task()` 且钉 CPU1。

## 验证方法
`idf.py build` 编译通过；上机模拟：根节点拔 WAN → 观察子节点 `Network_Flag` 翻转日志、群控步骤是否在超时后自动推进、单控是否持续采样；长跑后检查 SQLite 记录数与 `pushed` 分布（`storage_print_all_records()` `appTask.c:451`、`print_db_files`）；按键取栈水位与空闲堆（`appTask.c:3607` 起）确认无泄漏/爆栈。


## Agent Extensions
### SubAgent
- **code-explorer**
  - Purpose：核实 `get_overwrite_local_id_locked` 的实际 SQL 与 `pushed` 过滤、`AgingCMode` 真实取值（`aging_config.c:680-685` 与 `appTask.h:39` 注释不一致）、协议导入是否有本地缓存可复用、以及 `QueryResult` 当前定义与各使用点。
  - Expected outcome：给出可依赖的事实清单，避免基于猜测改动淘汰逻辑与群控判定条件。

### Skill
- **lsp-code-analysis**
  - Purpose：对 `Cannextstep`、`AgingCMode`、`Network_Flag`、`g_db1_result`、`InsertStructuredRecord` 做引用/调用层级分析，确认改动的影响面（尤其补发与采样路径）。
  - Expected outcome：明确每处改动的调用方与回归点，防止遗漏唤醒条件与中止路径。
