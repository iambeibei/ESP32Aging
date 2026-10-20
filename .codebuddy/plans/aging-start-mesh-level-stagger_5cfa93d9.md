---
name: aging-start-mesh-level-stagger
overview: 在 start_aging_with_ready_pn() 的 PN 门通过后、Aging.json 落盘前，按 mesh 层级 ×2s 同步错峰延时后再启动老化：打散全网同时下发 start_aging 时各节点经 http_auto 并发拉取设备协议的峰值（根出口/服务器减压）。覆盖直接 start_aging、set_pn 触发 pending、掉电恢复三个启动入口；拒绝路径与 WAIT_PN 缓存逻辑零影响。
todos:
  - id: add-stagger-const
    content: 在 appTask.c 顶部宏区新增 LG_AGING_START_STAGGER_BASE_SEC(2s) 命名常量，附注释说明用途
    status: completed
  - id: insert-level-stagger
    content: 在 start_aging_with_ready_pn() PN 门后插入层级错峰：mesh_get_current_level()×2s 延时（level=0 按 1，含 aging_runtime_log 日志与调用上下文注释）
    status: completed
    dependencies:
      - add-stagger-const
  - id: build-and-review
    content: 编译验证 mesh_ota 与 appTask 改动零告警；用 [skill:embedded-coding-standard] 对照本地规范自审本次改动
    status: completed
    dependencies:
      - insert-level-stagger
---

## 用户需求

收到 start_aging 老化指令后，不立即处理，而是按 **mesh 层级 × 2 秒**延时后再启动老化。目的是：全网同时下发 start_aging 时，各节点会在老化启动后经 `http_auto` 任务并发拉取设备协议（`test_http_getproto_New`），打散该峰值，避免根节点 WiFi 出口与服务器同时受压。

## 需求分析与确认细节

- **延时公式**：`delay = mesh_get_current_level() × LG_AGING_START_STAGGER_BASE_SEC(2s)`。根节点（level=1）延时 2s，L2 延时 4s……L6（`MESH_MAX_LEVEL=6`）延时 12s，总延时 ≤12s 有界。
- **插入位置**：`start_aging_with_ready_pn()`（appTask.c:2428）内 PN 门通过之后（2451 行 `}` 之后）、`save_json_to_spiffs("Aging.json")` 之前（2453 行注释处）。该函数是三个真实启动入口的唯一汇聚点，插入一处即全覆盖：
  1. 直接 start_aging（PN 已就绪，appTask.c:2637）
  2. set_pn 触发 pending 启动（appTask.c:2750）
  3. 掉电恢复后 pending + PN 均就绪（appTask.c:3203）
- **拒绝路径零影响**：空 JSON、RUNNING 拒绝、无 PN 拒绝均在延时之前返回，服务器侧拒绝应答不延迟。
- **WAIT_PN 缓存逻辑不触碰**：PN 未就绪时挂起命令的状态机完全不动。
- **防御处理**：`mesh_get_current_level()` 返回 0（库未就绪的异常态）时按 1 处理；根节点同样延时 2s（统一逻辑，单节点无峰值问题）。
- **同步延时**：跑在 mqtt_rx 上下文（`parse_jsonCommand_MQTT` 的调用者），最长 12s。依据：老化启动低频（一次老化周期一次）、既有路径本就含 SPIFFS/NVS 写阻塞、≤12s 内到达其他命令概率极低；异步化需改 WAIT_PN/挂起 seq 状态机，风险与收益不成比例。
- **不覆盖**：`aging_command_json_parse` 直连的掉电恢复路径（appTask.c:3158，恢复"运行中"老化而非新启动，属单节点事件无全网峰值）。
- 新增日志 `aging_runtime_log` 输出 level 与延时时长，便于现场验证错峰梯度。

## 技术方案

### 技术栈

- 语言/框架：C（gnu17）、FreeRTOS、ESP-IDF v5.4.4、ESP32-S3
- 复用既有能力：`mesh_get_current_level()`（mesh.h，已在 appTask.c:22 引入）、`vTaskDelay`（appTask.c 内 38 处使用范式）、`aging_runtime_log`（既有老化日志函数）
- 无新依赖、无新文件、无新任务，改动限于 appTask.c 单文件

### 实现方式

1. **新增命名常量**（appTask.c 顶部宏区，遵循"禁止散落魔法数"规范）：
   ```c
   /* 老化启动 mesh 层级错峰基数（秒）：delay = level × 基数，见 start_aging_with_ready_pn。 */
   #define LG_AGING_START_STAGGER_BASE_SEC 2
   ```
2. **插入错峰延时**（appTask.c:2451 与 2453 之间）：PN 门通过后、Aging.json 落盘前：
   - 取 `level = mesh_get_current_level()`；`level == 0` 防御性按 1 处理
   - `stagger_ms = level × LG_AGING_START_STAGGER_BASE_SEC × 1000`（uint32_t）
   - `aging_runtime_log` 输出 level 与延时时长（现场可观测错峰是否生效、验证层间 2s 梯度）
   - `vTaskDelay(pdMS_TO_TICKS(stagger_ms))`
   - 注释标注调用上下文（mqtt_rx，同步阻塞上限 12s）与设计依据（打散协议拉取峰值、三入口汇聚、拒绝路径不受影响）

### 关键决策与权衡

- **汇聚点单点插入 vs 三个入口各自插入**：汇聚点 DRY，且未来新增启动入口自动继承错峰
- **同步 vs 异步**：同步（≤12s 低频有界）——异步化需重构挂起 seq/WAIT_PN 状态机，YAGNI
- **level=0 按 1**：防库异常时零延时失去错峰意义，同时不阻塞（2s 下限）

### 性能与可靠性

- 时间/空间复杂度：O(1)；无内存分配；无新增锁
- 热点路径影响：仅老化启动路径，每个老化周期一次；MQTT 命令通道最长阻塞 12s（低频，队列深度 10 可吸收）
- 失败模式：`vTaskDelay` 不可能失败；`mesh_get_current_level()` 只读无失败态

### Skill
- **embedded-coding-standard**
  - 用途：改动完成后对照 `.ai/coding-standard.md` 本地规范自审（常量命名、注释调用上下文标注、错误路径、日志规范）
  - 预期结果：确认改动合规，无规范回归
