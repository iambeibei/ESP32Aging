---
name: parent-reject-fast-restart
overview: 子节点连续 5 次因父节点满员类原因（Wi-Fi reason 2/4/5/6）断开后，经 mesh_health_task 快通道直接重启（复用 RTC 每上电周期 3 次预算防疯循环），比 180s 无父 watchdog 更快恢复"10 节点同时上电撞满员父"场景；检测在既有断开事件里用 reason 码过滤，GOT_IP 清零保证连续语义，仅 mesh.c 单文件改动。
todos:
  - id: add-reject-const
    content: 在 mesh.c 新增 MESH_PARENT_REJECT_RESTART_THRESHOLD(5) 常量、原子计数器 s_parent_reject_count 与 is_parent_reject_reason() 判定（reason 2/4/5/6）
    status: completed
  - id: count-and-clear
    content: network_event_handler：STA_DISCONNECTED 按 reason 计数（仅子节点、限频日志），GOT_IP 清零计数
    status: completed
    dependencies:
      - add-reject-const
  - id: fast-path-restart
    content: 新增 mesh_heal_reject_fast_path() 并在 mesh_health_task 调用：达 5 次复用 RTC 预算重启，耗尽则清零交回 watchdog L3
    status: completed
    dependencies:
      - count-and-clear
  - id: build-and-review
    content: 编译验证 mesh.c 零告警；用 [skill:embedded-coding-standard] 对照本地规范自审本次改动
    status: completed
    dependencies:
      - fast-path-restart
---


## 用户需求

子节点连接父节点时，若连续 5 次因父节点满员类原因被拒绝，直接触发重启。应用场景：10 个节点同时上电都发现同一个父节点，父节点 SoftAP 仅 5 槽已满仍被连接，现场日志为 `max connection, deauth!`（mesh.c:36-41 注释已记录该现象），当前只能等待 180s 无父 watchdog 软复位，恢复太慢。

## 需求分析（已调研确认）

- **检测信号现成**：`network_event_handler` 的 STA_DISCONNECTED 分支（mesh.c:376-388）已提取 `disc->reason` 并原子写入 `s_last_sta_disconnect_reason`（mesh.c:121），日志 `STA disconnected, reason=%u`——无需新事件源。
- **满员拒绝 reason 集合**（ESP reason code，esp_wifi_types.h）：2=AUTH_EXPIRE（满员 AP 忽略 auth 的典型表现）、4=ASSOC_EXPIRE、5=ASSOC_TOOMANY（关联数已满，最准确）、6=NOT_AUTHED。现场可依据 `STA disconnected, reason=N` 日志扩展，需在注释中说明。
- **清零点**：`IP_EVENT_STA_GOT_IP` 分支（mesh.c:343-365）`mesh_update_network_flag(true)` 处——保证"连续"语义（中间成功过就不算连续）。
- **执行上下文**：`mesh_health_task` 循环（mesh.c:251-276）掉线态 5s 周期，`mesh_heal_watchdog(online, now)` 调用点（272 行后）是快通道插入位；仅子节点创建该任务，天然满足"仅子节点"。
- **预算复用**：`mesh_heal_watchdog` 的 L2 路径（mesh.c:216-226）已有 RTC 每上电周期重启预算（`s_heal_restart_count < MESH_RESTART_MAX_PER_CYCLE=3`），重启前 `vTaskDelay(MESH_HEAL_SOFT_RESET_GAP_MS=100ms)` 落盘是既有惯例——快通道直接复用该预算与延迟范式，防"根持续满员时无限重启"的疯循环。
- **根节点不参与**：根连路由器无父概念，由 root_failover_task 管理；计数器仅子节点累加（`IsRoot != 1`）。
- **触发时机**：重连节奏 10s×5/15s 重扫（mesh.c:880-882），5 次被拒约 50~100s 触发，快于 watchdog 的 180s。

## 设计决策

1. **快通道复用 RTC 预算而非无条件重启**：根长时间满员时重启也无济于事，预算耗尽（3 次/上电周期）后清零计数、交回 watchdog L3 继续扫描（无上行存活），符合 CHILD_NODE_ROOT_LOSS_DESIGN.md 的有界自愈哲学。
2. **检测在事件上下文、执行在 health task**：事件处理器只做原子计数（快、非阻塞）；`vTaskDelay + esp_restart` 放 5s 周期串行上下文，最多延迟 5s 执行。
3. **GOT_IP 清零、非拒绝类断开不计不清**：库正常换父/重扫不误触。
4. 常量 `MESH_PARENT_REJECT_RESTART_THRESHOLD 5`（用户指定）放 mesh.c 顶部 MESH_* 宏区。

## 边界

- 仅改 mesh.c 单文件；不动 watchdog 既有 L1/L2/L3 逻辑（快通道是并列分支）。
- 无测试套件，验证 = 编译 + 串口观察（10 节点上电：被拒节点 `parent reject N/5` 日志 → 重启 → 归位）。



## 技术方案

### 技术栈

- 语言/框架：C（gnu17）、FreeRTOS、ESP-IDF v5.4.4、ESP32-S3、esp_mesh_lite + esp_event
- 复用既有能力：`s_last_sta_disconnect_reason` 的 reason 提取、`s_heal_restart_count`/`MESH_RESTART_MAX_PER_CYCLE` RTC 预算、`MESH_HEAL_SOFT_RESET_GAP_MS` 落盘延迟范式、`mesh_health_task` 5s 串行循环
- 无新文件、无新任务、无新锁（原子变量 + 单写者 health task）

### 实现方式（mesh.c 单文件，4 处改动）

1. **常量与状态**（mesh.c 顶部宏区 + 静态变量区）：
   - `#define MESH_PARENT_REJECT_RESTART_THRESHOLD 5`，附注释（用户决策、触发时序 50~100s、快于 180s watchdog）
   - `static atomic_uint s_parent_reject_count;`（事件任务写、health task 读，`__atomic_*` RELAXED）
   - `static inline bool is_parent_reject_reason(uint8_t reason)`：命中 {2,4,5,6} 返回 true，注释注明集合依据与现场可扩展

2. **计数**（`network_event_handler` STA_DISCONNECTED 分支，mesh.c:388 附近）：仅子节点（`IsRoot != 1`）且 reason 命中集合时 `__atomic_fetch_add`，打 `Parent rejected N/5` 限频日志（每 2s 一条防刷屏）；非命中 reason 不动计数器。

3. **清零**（`network_event_handler` GOT_IP 分支，mesh.c:355 `mesh_update_network_flag(true)` 后）：`__atomic_store_n(&s_parent_reject_count, 0, ...)`，注释说明保证"连续"语义。

4. **快通道**（新增 `static void mesh_heal_reject_fast_path(void)`，置于 `mesh_heal_watchdog` 之后）：
   - 读计数 < 阈值直接返回；≥ 阈值时检查 RTC 预算：未耗尽则递增计数、打醒目 WRN 日志、`vTaskDelay(100ms)` 落盘、`esp_restart()`；已耗尽则清零计数 + 限频 WRN 日志交回 watchdog L3
   - 在 `mesh_health_task` 循环 `mesh_heal_watchdog(online, now)` 之后调用（mesh.c:272 后），注释标注调用上下文（仅子节点、5s 串行周期）

### 关键取舍

- **快通道与 watchdog 并列不改其内部**：watchdog 逻辑经现场验证（CHILD_NODE_ROOT_LOSS_DESIGN.md），不动 L1/L2/L3；新机制只增量的处理"满员拒绝"这一特定快速失败模式
- **重启前不清零计数**：重启后若仍被拒，计数从 0 重新累计，行为可预期；预算才是防疯循环的闸门
- **reason 过滤而非任何断开都计数**：避免库正常换父/重扫（reason 202 等）误触重启

### 性能与可靠性

- 时间/空间复杂度 O(1)；事件上下文仅一次原子加与比较，无阻塞
- 重启路径最多延迟 5s（health 周期）+ 100ms（落盘），有界
- 失败模式：`esp_restart` 前日志落盘 100ms（对齐既有惯例）；RTC 预算跨重启延续（`s_heal_magic` 守卫，mesh.c:798-801），上电/Brownout 清零开启新预算



## Agent Extensions

### Skill
- **embedded-coding-standard**
  - 用途：改动完成后对照 `.ai/coding-standard.md` 本地规范自审（原子变量同步、调用上下文注释标注、魔法数宏化、错误路径非空、日志限频）
  - 预期结果：确认 mesh.c 改动合规，无规范回归
