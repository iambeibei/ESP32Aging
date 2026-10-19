---
name: mesh-lite-lan-ota-integration
overview: 启用 Mesh-Lite 原生 OTA 分发（已实现适配层 + 1 根 1 子 gate 实测）：gate 证明闭源库要求供数方 running 分区版本匹配，故采用方案 A——根本身下载完成后先行重启供数（子保持错峰/重试/CanRestart 统一点火）；新增 OTACP 完成上报（device/<DEVICE_ID>/event/OTACP，{"Seq":ts,"Data":{"IsComplete":1,"AgingNumber":n}}，复用 create_json_agingComplete，cmd_task 统一发布，根+子均发）。
todos:
  - id: impl-root-restart-plan-a
    content: mesh_ota.c handle_root_path：DL_EVT_DONE 分支改为发 OTACP → WRN 日志 → 延迟 3s → esp_restart，移除该路径的 notify_child_node_restart 调用
    status: completed
  - id: add-otacp-publish
    content: mesh_ota.c 新增 mesh_ota_publish_otacp（复用 create_json_agingComplete + app_mqtt_publish OTACP topic，AgingNumber 空值防护）+ CMD_EVT_PUBLISH_OTACP 事件位与 cmd_task 发布调度
    status: completed
    dependencies:
      - impl-root-restart-plan-a
  - id: wire-otacp-triggers
    content: get_file_done_cb 成功末尾与 OTA_FINISH(SUCCESS) 兜底路径置 CMD_EVT_PUBLISH_OTACP 位（回调上下文只置位不发布）
    status: completed
    dependencies:
      - add-otacp-publish
  - id: build-verify
    content: 编译通过并确认固件尺寸与分区余量
    status: completed
    dependencies:
      - wire-otacp-triggers
  - id: coding-standard-review
    content: 用 [skill:embedded-coding-standard] 对照本地规范自审 mesh_ota.c 改动
    status: completed
    dependencies:
      - build-verify
  - id: update-docs
    content: 更新 MESH_LITE_OTA_INTEGRATION.md：方案 A 决策依据（gate 日志证据）、OTACP 协议、根先行重启运维流程、重试覆盖时序窗说明；version.txt 更新为 0.0.2
    status: completed
    dependencies:
      - coding-standard-review
---


## 用户需求

基于 1 根 1 子 gate 实测结论，实施两项确认的变更：

1. **方案 A（根先行重启供数）**：gate 实测证明闭源库要求供数方 running 分区版本与请求版本一致，根节点"已下载未重启"态会被库拒绝（`Current running version is not same as the OTA firmware`）。因此根本身下载完成后，先发送 OTACP 完成上报，延迟数秒后主动 `esp_restart()`，使根以新版本上线成为供数方；子节点保持错峰 + mesh 内请求 + 有限重试 + CanRestart 统一点火不变。
2. **OTACP 完成上报**：节点（子必备、根同样发送）在固件下载完成（含 mesh 内分发落盘、set_boot_partition 完成）后，向 `device/<DEVICE_ID>/event/OTACP` 发布 `{"Seq":<时间戳>,"Data":{"IsComplete":1,"AgingNumber":"<AgingNumber>"}}`。Seq 取 `time(NULL)`（与既有 publish 一致），AgingNumber 取运行时配置全局量，JSON 构造复用 `create_json_agingComplete()`，零新协议代码。

## 功能完整性要点

- OTACP 发布必须在任务上下文执行：`get_file_done_cb` 与 dl_task 可能位于闭源库任务上下文，`esp_mqtt_client_publish` 最坏阻塞 10 秒（network timeout），故由 cmd_task 主循环统一发布。
- 根节点 OTACP 在重启前发出（服务器据此判断根就绪并调度子节点升级）。
- 子节点 OTACP 在 mesh 内下载完成后发出（服务器据此汇总进度并下发 CanRestart）。
- AgingNumber 为 NULL/空时跳过发布并记录错误日志，不崩溃。
- 发布失败不回滚 PENDING_RESTART 状态，仅记录日志。
- 根重启前需延迟（约 3 秒）等待 QoS 0 报文出网与日志落盘。
- 根重启后子节点首次请求可能仍失败（时序竞争），由既有 2 次 @90s 重试覆盖；需在日志中明确该行为。

## 行为效果

- 根日志出现 `Root OTA complete; restarting to serve new firmware` + OTACP 发送记录，随后重启。
- 子日志出现 `OTA complete; OTACP published`，随后等待 CanRestart。
- 服务器端可在 `device/<id>/event/OTACP` 收到全部节点的完成通知，再下发 CanRestart 统一点火。



## 技术方案

### 现状与改动点

已实现并编译通过的模块（固件 0x207700，分区余量 10%）：

- `main/APP/OTA/mesh_ota.c/h`：三回调 + 禁用桩 + OTA 事件 + cmd/dl 双任务 + 错峰 + 重试 + 分区匹配缓存，`#ifdef CONFIG_ESP_MESH_LITE_OTA_ENABLE` 守卫完整（91–690 行）
- `ota.c/h`：暴露 `fetch_metadata/probe_firmware_size/version_is_newer/set_state`
- `appTask.c`：ota 分支改调 `mesh_ota_handle_mqtt_command()`；mesh_init 成功分支调 `mesh_ota_init()`

### 改动 1：根先行重启（方案 A）

**位置**：`mesh_ota.c` 的 `handle_root_path()`，dl_task 成功分支（`DL_EVT_DONE`）。

当前代码（下载完成后仅 resume children + 返回，根停留在 PENDING_RESTART）：
```c
if (bits & DL_EVT_DONE) {
    ESP_LOGI(TAG, "Root OTA download completed; resume children");
} else { ... }
err = esp_mesh_lite_ota_notify_child_node_restart();
...
if (!(bits & DL_EVT_DONE)) { schedule_retry(); }
```

改为：dl_task 成功且本节点为根（level<=1，该分支已是根路径）时：
1. 调 `mesh_ota_publish_otacp()`（新增，见改动 2）；
2. `ESP_LOGW` 醒目日志（含"子节点由 CanRestart 统一点火、根先行重启供数"语义）；
3. `vTaskDelay(3000ms)`（等 QoS 0 报文出网 + 日志落盘；与 CanRestart 既有 5s 延迟同思路，取 3s 足够）；
4. `esp_restart()`。

注意：`esp_restart()` 前不再调用 `notify_child_node_restart()`——根即将重启，恢复子节点无意义（子节点请求穿透到根时根已不在线，会走父丢失自愈或失败重试；根重启上线后子重试命中）。

**dl_task 完成事件与 cmd_task 的交互不变**：dl_task 置 `DL_EVT_DONE` 后即返回循环，根路径在 cmd_task 中收尾并重启，无死锁面。

### 改动 2：OTACP 完成上报

**发布函数**（mesh_ota.c 内 static，任务上下文）：
```c
static void mesh_ota_publish_otacp(void)
{
    /* AgingNumber 运行时配置（readconfig 加载，默认 "-1"） */
    if (AgingNumber == NULL || AgingNumber[0] == '\0') {
        ESP_LOGE(TAG, "OTACP skipped: AgingNumber not configured");
        return;
    }
    char *json = create_json_agingComplete(time(NULL), 1, AgingNumber);
    if (json == NULL) {
        ESP_LOGE(TAG, "OTACP skipped: JSON build failed");
        return;
    }
    int ret = app_mqtt_publish("device/%s/event/OTACP", json, DEVICE_ID);
    if (ret < 0) {
        ESP_LOGE(TAG, "OTACP publish failed");
    } else {
        ESP_LOGI(TAG, "OTACP published: IsComplete=1");
    }
    free(json);
}
```

**调用点（均在 cmd_task 任务上下文）**：

1. **子节点完成**：`get_file_done_cb()` 成功路径当前置 `PENDING_RESTART` 并返回。改为：新增事件位 `CMD_EVT_PUBLISH_OTACP`，`get_file_done_cb` 成功末尾只置位（回调可能在库任务上下文，不能阻塞发布）；cmd_task 主循环 `xEventGroupWaitBits` 掩码加入该位，命中且状态为 `PENDING_RESTART` 时调 `mesh_ota_publish_otacp()`。
   - 备选简化：`get_file_done_cb` 成功时直接调 `mesh_ota_publish_otacp()`——但闭源库任务上下文 + 最坏 10s 阻塞违反"回调不得长时间阻塞"原则，**不采用**。
2. **根节点完成**：`handle_root_path()` 的 `DL_EVT_DONE` 分支中、重启延迟前直接调 `mesh_ota_publish_otacp()`（cmd_task 上下文，安全）。
3. **事件兜底路径**：OTA_FINISH(SUCCESS) 事件处理器中，若状态从 RUNNING 兜底提升为 PENDING_RESTART（get_file_done 未先执行的非常规顺序），同样置 `CMD_EVT_PUBLISH_OTACP` 位，保证 OTACP 不遗漏。

**事件位设计**：`CMD_EVT_PUBLISH_OTACP BIT2`。cmd_task 循环中该位独立处理（不触发任何下载流程），处理完即清。与 `CMD_EVT_REQUEST/RETRY` 无交互（WaitBits 不自动清除请求/重试位的行为不变，新增位用同一 `pdTRUE` 清除语义）。

**依赖 include**：mesh_ota.c 需新增 `#include "mqtt_app.h"`（app_mqtt_publish）、`"ConfigData.h"`（AgingNumber）、`"Json_data.h"`（create_json_agingComplete）。头文件自包含检查：`Json_data.h` 与 `mqtt_app.h` 均可独立 include（既有模块，appTask.c 已在用）。

### 关键决策与取舍

- **OTACP 在 cmd_task 发布而非回调**：闭源库任务上下文不可假设，`esp_mqtt_client_publish` 最坏 10s 阻塞会拖死库的 OTA 协议任务（影响 mesh 内其他传输）。
- **根也发 OTACP**：服务器需汇总全部节点（含根）就绪状态后才能下发 CanRestart；若协议只要求子节点发，删除根分支调用即可（一行）。
- **根重启前 3s 延迟**：QoS 0 publish 无确认，3s 覆盖 TCP 缓冲刷新 + UART 日志落盘（与既有 CanRestart 的 5s 同思路，重启场景 3s 足够）。
- **不改 CanRestart 语义**：子节点仍只在 CanRestart 统一重启（AGENTS.md 2026-10-16 决策），根先行重启是方案 A 的组成部分，在日志与文档中醒目说明。

### 性能与复杂度

- 新增代码约 40 行 + 1 个事件位；无新增任务/内存分配（JSON 用既有 malloc 路径，free 闭环）。
- OTACP 每条报文 < 200 字节，对网络无压力。
- 根重启到子请求命中存在时间窗（重启约 10–20s + 重连），子节点 90s 重试间隔天然覆盖。

### 兼容性

- 旧固件节点不认 OTACP topic（未订阅），无影响；服务器需按新协议订阅。
- `#ifdef CONFIG_ESP_MESH_LITE_OTA_ENABLE` 关闭时，OTACP 发布函数随实现主体一起被守卫包裹，stub 不受影响。


本次改动不涉及 UI 变更，仅涉及嵌入式 C 代码逻辑与 MQTT 上报，无设计维度内容。

# Agent Extensions
- **embedded-coding-standard**：实现完成后对照 `.ai/coding-standard.md` 自审 mesh_ota.c 改动（返回值处理、共享状态同步、回调上下文注释、错误路径非空、静态分配、重试退出条件）。
