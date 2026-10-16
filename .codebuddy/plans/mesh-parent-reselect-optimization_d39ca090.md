---
name: mesh-parent-reselect-optimization
overview: 针对「中间父节点断开+根已满时子节点一直连不上/连原父」的问题（成因 A/C，排除 B flap），做三项改动：每节点子节点上限 MESH_MAX_CHILDREN 5→8（mesh.c + sdkconfig 同步）、父节点重连节奏 10s×3/30s → 5s×2/15s（mesh.c 宏）、全节点 SoftAP 站点连接/断开事件观测（mesh.c 新增 handler），并同步更新设计文档与提供串口回归清单。
design:
  architecture:
    framework: html
todos:
  - id: capacity-max-children
    content: mesh.c 宏 MESH_MAX_CHILDREN 5→8，sdkconfig.defaults 与 build/mesh-lite/sdkconfig 的 SOFTAP_MAX_CONNECT_NUMBER 同步为 8
    status: completed
  - id: retry-rhythm-constants
    content: mesh.c 重连节奏宏改 5s×2/15s 并同步 esp_mesh_lite_set_wifi_reconnect_interval 实参与注释权衡说明
    status: completed
  - id: ap-station-observability
    content: mesh.c 新增 AP_STACONNECTED/AP_STADISCONNECTED 处理器并注册，打印站点进出与槽位占用 n/8（只观测不动作）
    status: completed
  - id: review-coding-standard
    content: 用 [skill:embedded-coding-standard] 审查 AP 事件回调与计数器并发合规性
    status: completed
    dependencies:
      - capacity-max-children
      - retry-rhythm-constants
      - ap-station-observability
  - id: build-regression
    content: 按 AGENTS.md 编译并回读 sdkconfig/AP 配置确认槽位 8 生效，附串口回归清单
    status: completed
    dependencies:
      - review-coding-standard
  - id: update-design-doc
    content: 在 CHILD_NODE_ROOT_LOSS_DESIGN.md 更新 7.3 节奏取舍与新增 AP 观测说明，记录本次参数与理由
    status: completed
    dependencies:
      - build-regression
---

## 产品概述

针对 esp-mesh-lite 组网中「子节点的中间父节点断开、且根节点已满时，子节点长时间反复重连原父/撞满员根」的问题进行优化。前序分析已定位成因 A（射程内无有槽位替代父，每轮扫描选中满员根被 deauth 形成循环）与成因 C（库重连路径优先使用 RTC 存储的原父），成因 B（原父 flap）已排除。本计划从容量、节奏、可观测性三方面修改。

## 用户确认的三项范围

1. 每节点子节点上限 MESH_MAX_CHILDREN 5 → 8（mesh.c 宏 + sdkconfig.defaults + 构建生效 sdkconfig 三处同步）；
2. 父节点重连节奏直接改代码常量：重连 10s×3 → 5s×2，重扫 30s → 15s（mesh.c 宏 + esp_mesh_lite_set_wifi_reconnect_interval 实参）；
3. 新增 SoftAP 站点观测：注册 WIFI_EVENT_AP_STACONNECTED / AP_STADISCONNECTED，打印站点增减与槽位占用 n/8，所有节点（含根）生效，只观测不动作。

## 验收效果

- 中间父断开后子节点约 10s 内进入重扫、15s 节奏发现并挂接新父（日志见 STA IP/level 变化），不再出现「30s 窗口 + 每轮撞满员根被 deauth」循环；
- 根/L2 槽位 5→8，10 节点场景候选父增多、拓扑层数下降；
- 根侧串口可直接看到 SoftAP 站点进出与占用 n/8，证实/证伪「撞满员的根」；
- 既有行为不回归：根掉线期间子节点不重启、BLE 老化掉线优雅中止、老化任务流程不变。

## 非目标

- 不做 baseconfig 可配化（重连节奏保持编译期常量）；
- 不改 mesh-lite 闭源库的父选择算法（只调节奏与容量参数）；
- 不动 BLE、MQTT、老化状态机、SQLite 链路。

## 技术栈

- 现有工程：ESP-IDF v5.4.4、FreeRTOS、C（近 c99）、esp-mesh-lite 1.0.2 + iot_bridge 1.0.1；不引入新组件、新任务、新锁、新堆分配。
- 硬件约束（已验证）：ESP32-S3 SoftAP 单节点站点上限 15；CONFIG_LWIP_DHCPS_MAX_STATION_NUM=64 ≥ 8；iot_bridge Kconfig:108-116 要求 DHCPS ≥ SOFTAP_MAX；NAPT 表 512 条。
- 关键陷阱：`esp_bridge_wifi_set_config()`（bridge_wifi.c:57）会用组件配置**无条件覆盖** app 设置的 `ap.max_connection`——只改 mesh.c 宏不改 sdkconfig，SoftAP 实际槽位不会变。且 AGENTS.md 构建命令 `-D SDKCONFIG=build/mesh-lite/sdkconfig` 指向已生成文件，改 sdkconfig.defaults 不会自动生效，必须同步改 build sdkconfig（或重新生成）。

## 实现方法

改动集中在 `main/Communication/Internet/Mesh/mesh.c` 与 `sdkconfig.defaults` / `build/mesh-lite/sdkconfig`：

1. **容量**：`MESH_MAX_CHILDREN` 5→8，经既有 mesh.c:659/668 自动传递到 `ap_config.ap.max_connection` 与 `config.max_connect_number`；sdkconfig.defaults 与 build sdkconfig 的 `CONFIG_BRIDGE_SOFTAP_MAX_CONNECT_NUMBER` 同步 5→8。
2. **节奏**：mesh.c:44-46 三个宏改为 5/2/15，mesh.c:697-699 的 `esp_mesh_lite_set_wifi_reconnect_interval()` 实参同步；更新 44-46 处注释记录新权衡（库默认 5s×2/10s → 原 10s×3/30s 压 BLE 争用 → 本次 5s×2/15s 折中：重连窗口 30s→10s、重扫 30s→15s）。
3. **SoftAP 站点观测**：新增 `mesh_ap_station_event_handler()`，注册于 mesh.c:709-714 既有注册点之后（仅注册 WIFI_EVENT 的 AP_STACONNECTED/AP_STADISCONNECTED 两个 id，不用 ESP_EVENT_ANY_ID）；用静态计数器记录当前占用，事件里 ESP_LOGI 打印 `aid`/MAC 与 `占用 n/8`。不动作、不重连、不删站点（遵守 CHILD_NODE_ROOT_LOSS_DESIGN.md 7.2 只观测范式）。注意事件回调上下文只打日志+改计数器，不调用任何阻塞 API。

## 性能与可靠性

- 全部改动为常量与两个事件回调，无新增任务/堆/锁；回调仅 ESP_LOGI + 计数器自增/自减，O(1)；
- 槽位 5→8 的代价：单信道 SoftAP 带机与空口争用上升，但 10 节点规模下远低于硬件上限（15），且换来每节点候选父翻倍、重挂成功率上升；
- 重扫 30s→15s 使闭源重扫路径暴露频次回升至原值的 2 倍（仍低于库默认 10s 的 1.5 倍？——实际 15s 比库默认 10s 保守，比原 30s 激进），BLE 争用风险介于原设计与库默认之间，属可接受折中；串口回归需观察老化期间 BLE 扫描/连接是否受射频争用影响。

## 兼容性与边界

- 不改 mesh-lite 库、不改 iot_bridge 组件源码、不改 BLE/MQTT/老化/SQLite；
- sdkconfig 改动需三处一致（defaults + build 生效文件 + 编译验证回读），漏一处即槽位不生效；
- AP 事件日志为 INFO 级，10 节点满负荷时每节点事件量极小（仅站点进出），不刷屏。

本次为嵌入式固件的参数与诊断增强，不涉及 UI 设计，无设计章节内容。

## Agent Extensions

### Skill
- **embedded-coding-standard**
  - Purpose：对 mesh.c 新增的 AP 站点事件回调与计数器改动按工程 `.ai/coding-standard.md` 及 `.em_skill.json` 已确认规则组合（CERT C + FreeRTOS + Local coding standard）做合规审查，重点核查事件回调上下文约束（不阻塞/不碰存储/MQTT/任务）、静态计数器并发安全（仅本任务写读可加 volatile 或用注释说明单写入者）、命名与 pragma region 风格一致。
  - Expected outcome：输出审查结论；若发现回调上下文违规或风格偏离，修正后进入编译验证。
