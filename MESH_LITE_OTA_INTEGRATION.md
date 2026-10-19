# Mesh-Lite 原生 OTA 分发集成设计

> 实施状态：代码完成并编译通过（2026-10-19），1 根 1 子 gate 已实测；50 节点全场验收与参数标定待现场执行。
> 关联文档：`MESH_LITE_MIGRATION.md`（组网）、`OTA_SERVER_MIGRATION_DESIGN.md`（服务器接口）、
> `AGENTS.md`（CanRestart 统一点火决策）、`CHILD_NODE_ROOT_LOSS_DESIGN.md`（根掉线子节点自愈）。

## 1. 背景与目标

原 OTA 路径：每个节点（含子节点）各自经根节点 WiFi 出口从服务器全量下载固件。1 根 5 子
同时升级时，根单条 STA 成为漏斗（5×2.1MB），笔记本热点下整体慢、单点易失败，且下载同步
阻塞 `mqtt_rx` 任务（命令通道拥塞、队列深度 10 溢出）。

目标：**子节点固件获取全部走 Mesh 内部通道**——根节点外网只下载 1 份，其余经 mesh 内
LAN OTA 文件传输逐级分发；根 WiFi 出口流量 N×2.1MB → 1×2.1MB。

## 2. 架构

```
服务器 ──MQTT ota 定点命令(server/<DEVICE_ID>/cmd/ota)──▶ 每节点 mqtt_rx（零阻塞，仅通知）
                                                              │
                                                              ▼
                                                    mesh_ota cmd_task
                                              ┌──────────────┴──────────────┐
                                    level>1 子节点                    level=1 根节点
                                    错峰 (level-1)×60s+抖动            pause children
                                    fetch update.json + 版本门          dl_task 外网下载 1 份
                                    probe Content-Length 作 size         ↓ 成功
                                    transmit_file_start               WRN 日志 → 3s → esp_restart
                                    （extern_url_ota_cb=禁用桩）        （方案 A：先行重启供数）
                                              │
                            Mesh-Lite 库（闭源）：逐级向父请求直到根
                                              │
                      ┌───────────────────────┼───────────────────────┐
                      ▼                       ▼                       ▼
              provide_file_cb           get_file_cb            get_file_done_cb
              （供数方按 fw_version      （惰性 begin +           （end + set_boot
                匹配分区读出，带缓存）     write_with_offset）       → PENDING_RESTART
                                                                    → 置 CMD_EVT_PUBLISH_OTACP）
                                                                              │
                                                                    cmd_task 发布 OTACP
                                                                    device/<DEVICE_ID>/event/OTACP
```

模块：`main/APP/OTA/mesh_ota.c/h` 为唯一耦合点（同时依赖 esp_mesh_lite 与 ota.c）；
`ota.c` 保持外网下载与状态机所有权，新增暴露 `simple_ota_fetch_metadata` /
`simple_ota_probe_firmware_size` / `simple_ota_version_is_newer` / `simple_ota_set_state`
四个复用点。appTask.c 仅两处接线（ota 命令分支 + mesh_init 成功分支）。

## 3. 关键设计决策

### 3.1 子节点禁用外网兜底（extern_url_ota_cb = 禁用桩）

mesh 内全链路拿不到目标版本即失败（打 WRN + 返回 ESP_FAIL），由有限重试吸收，绝不回落
外网。不用字面 NULL：闭源库对 NULL 回调行为不可见，桩函数保证任何路径有确定返回值。

### 3.2 方案 A：根本身下载完成后先行重启（2026-10-19 gate 实测决策）

**1 根 1 子 gate 实测结论**（根/子串口日志）：适配层全链路按预期工作（错峰 62.317s 精确
生效、探测、禁用桩命中、mqtt_rx 零阻塞），但闭源库的供数判定为：

```
W [ESP_Mesh_Lite_OTA]: The currently running firmware version is 0.0.0, the firmware version
requested by the child node is 0.0.1
W [ESP_Mesh_Lite_OTA]: Current running version is not same as the OTA firmware or different
categories. We will not continue the update.
```

**库要求供数方 running 分区版本 == 子请求版本**；根"已下载未重启"态（新固件在另一 ota 槽、
otadata 已指向、running 仍旧版）被拒。子请求穿透约 91s 后失败。

由此：mesh 内分发成立的前提是**根先重启运行新固件**。方案 A = 根下载完成 → 延迟 3s
（日志/MQTT 报文落盘，对齐 CanRestart 前等待范式）→ `esp_restart()`；重启后
running=新版本，子节点 mesh 内请求命中根 `provide_file_cb`（双路径的第一条）。
子节点保持错峰 + 重试 + CanRestart 统一点火不变。

**运维流程（重要）**：先给根发 ota → 根自动重启上线 → 再给子节点发 ota → mesh 内分发
→ 全部就绪后 CanRestart 统一点子。根重启瞬间的拓扑瞬断由
`CHILD_NODE_ROOT_LOSS_DESIGN.md` 的既有自愈机制兜底（子节点不异常重启、老化数据不丢）。

**根不发送 OTACP**：先行重启本身即完成信号；OTACP 仅子节点发送。

### 3.3 三层时序保护（无兜底后的可靠性）

- 层级错峰 `delay(level) = (level-1)×LG_MESH_OTA_LEVEL_STAGGER_SEC + rand(0, 15s)`；
- 根下载期间 `notify_child_node_pause/restart` 让子节点请求在库内排队（官方范式）；
- 失败有限重试 `LG_MESH_OTA_RETRY_MAX=2` 次 @`LG_MESH_OTA_RETRY_INTERVAL_SEC=90s`，
  重试复用首次 metadata/size，不重复外网请求。

**重试覆盖时序窗**（gate 数据）：子首次请求约 t=116s（60s 错峰 + 库 91s 穿透周期失败），
根就绪约 t=195s（外网下载 160s + 重启入网 ~30s）；90s 间隔的第二次重试（t≈296s）命中。

### 3.4 提供方分区匹配（双路径 + 缓存）

`provide_file_cb` 按 `param->fw_version` 匹配 app 分区：running 分区优先（方案 A 主路径），
次为另一 ota 槽（覆盖中间父"已下载未重启"的级联场景）；匹配结果按 fw_version 缓存
（短临界区），读取按分区边界钳位、64KB 对齐 padding 段填 0xFF。

### 3.5 OTACP 完成上报（仅子节点）

- topic：`device/<DEVICE_ID>/event/OTACP`；payload 复用老化完成同构构造函数
  `create_json_agingComplete(time(NULL), 1, AgingNumber)`（Json_data.c）：
  `{"Seq":<ts>,"Data":{"IsComplete":1,"AgingNumber":"<n>"}}`；
- 发布位置：回调（`get_file_done_cb`/OTA_FINISH 兜底）只置 `CMD_EVT_PUBLISH_OTACP` 位，
  由 cmd_task 统一发布——闭源库任务上下文不可做最坏 10s 的阻塞 MQTT 发布；
- 去重：`s_otacp_published` 单周期去重（done 与 FINISH 双路径不双发）；新 OTA 周期
  （服务器新命令）重置；发布失败不回滚 PENDING_RESTART（服务器可重发命令查询）。

## 4. 可标定常量（mesh_ota.c 顶部）

| 常量 | 初值 | 标定方法 |
|---|---|---|
| `LG_MESH_OTA_LEVEL_STAGGER_SEC` | 60 | 实机抓两值：根日志外网下载首末进度时间戳差、子节点 OTA PROGRESS 10%→90% 时间戳差；步长 = max(两值)×1.5 |
| `LG_MESH_OTA_STAGGER_JITTER_SEC` | 15 | 同层请求峰值打散，一般无需调 |
| `LG_MESH_OTA_RETRY_MAX` / `LG_MESH_OTA_RETRY_INTERVAL_SEC` | 2 / 90 | 覆盖"根下载+重启"全程即可 |
| `LG_MESH_OTA_ROOT_RESTART_DELAY_MS` | 3000 | 日志落盘等待，一般无需调 |

## 5. 兼容性与约束

- **固件大小硬顶**：ota_0/ota_1 各 0x240000=2359296 字节（partition.csv），超限在下载后
  写入/校验阶段失败；建议 ≤2.2MB。
- **旧固件节点**（未刷本次改动）：只认自己 topic 的 ota 命令走原外网路径，mesh OTA 消息
  不影响它们（定点下发）。
- **Kconfig 关闭时**：mesh_ota.c 整体 `#ifdef CONFIG_ESP_MESH_LITE_OTA_ENABLE` 守卫，
  仅留降级 stub；`OTA_AUTO_RESTART=n` 保持 CanRestart 外部点火。
- **sdkconfig 再生成坑**：`sdkconfig.defaults` 已回填历史丢失配置（Flash 8MB/自定义分区表/
  PSRAM/BT/TLS 等），重新生成不再丢；备份在 `build/mesh-lite/sdkconfig.bak.pre-mesh-ota`。

## 6. 实机验收记录

### 6.1 1 根 1 子 gate（2026-10-19，已通过）

- 根：pause → 外网下载 2096688B/160s → set_boot → 方案 A 重启路径就绪（本次为 gate 版本，
  未含先行重启与 OTACP，见 3.2 决策依据）。
- 子：错峰/探测/transmit_file_start/禁用桩命中全部符合设计；库供数判定条件已证实。
- 待复测项：方案 A 先行重启 + OTACP 上报 + 重试命中窗口。

### 6.2 1 根 5 子全场（待执行）

按 3.2 运维流程：先升根 → 根上线 → 5 子同时发 ota → 观察错峰打散、根出口仅 1 份、
禁用桩不命中（除时序边缘）、OTACP 5 条到服务器、CanRestart 统一点火后全网新版本。

### 6.3 50 节点扩展（待执行）

多层树（根 5 槽/层，1+5+15~20+29）：错峰顺序与层级天然一致（L2 最短、L4 最长），
中间父级联缓存分摊供数压力；穿透请求为逻辑转发，根 SoftAP 满员不影响分发。
