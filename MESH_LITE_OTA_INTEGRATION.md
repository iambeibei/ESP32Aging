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
                                    fetch update.json + 版本门          pause children
                                    probe Content-Length 作 size         dl_task 外网下载 1 份
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
                                                                    → 置 CMD_EVT_RESTART）
                                                                              │
                                                                    cmd_task 延迟 3s 后重启
                                                                    （根+子：完成即重启）
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

**完成即重启（2026-10-19 用户决策，替代原 CanRestart 统一点火）**：根与子下载/
分发完成后均延迟 `LG_MESH_OTA_RESTART_DELAY_MS`（3s）自行 `esp_restart()`，不再等待
CanRestart 统一点火，亦**不再上报 OTACP**（原 OTACP 上报已整体移除）；服务器以上线
报文（`device/public/up_line` 的 `Version` 字段）获知节点已升级。子节点重启前正在
运行的老化任务由 NVS SelfRecovery 瞬断恢复、SQLite 补发，数据不丢；其下 L3 的重连
由 50s 重连窗口（mesh.c `MESH_PARENT_RETRY_*`）覆盖子重启恢复期。

**运维流程（重要）**：先给根发 ota → 根自动重启上线 → 再给子节点发 ota → mesh 内
分发 → 各子完成即自行重启。根/子重启瞬间的拓扑瞬断由
`CHILD_NODE_ROOT_LOSS_DESIGN.md` 的既有自愈机制兜底（子节点不异常重启、老化数据不丢）。

### 3.3 时序控制与兜底（无外网兜底后的可靠性）

**升级时序由上位机分级下发完全控制**（2026-10-20 起，设备侧层级错峰已移除）：
先给根发 ota → 根下载完成自动重启上线 → 上位机确认根上线（up_line 报文 Version）→
再逐层下发子节点。设备侧收到子路径命令后立即发起 mesh 内请求，无延时。

设备侧保留的两层兜底：

- 根下载期间 `notify_child_node_pause/restart` 让子节点请求在库内排队（官方范式）；
- 失败有限重试 `LG_MESH_OTA_RETRY_MAX=2` 次 @`LG_MESH_OTA_RETRY_INTERVAL_SEC=90s`，
  重试复用首次 metadata/size，不重复外网请求。

**重试覆盖时序窗**（gate 数据）：若上位机同时全层下发（未分级），子首次请求会撞上
根未就绪（库 91s 穿透周期后禁用桩 FAIL），根就绪约 t=195s（外网下载 160s + 重启
入网 ~30s）；90s 间隔的第二次重试（t≈296s）命中——功能正确，多一轮失败日志。
分级下发时子节点零失败直达。

### 3.4 提供方分区匹配（双路径 + 缓存）

`provide_file_cb` 按 `param->fw_version` 匹配 app 分区：running 分区优先（方案 A 主路径），
次为另一 ota 槽（覆盖中间父"已下载未重启"的级联场景）；匹配结果按 fw_version 缓存
（短临界区），读取按分区边界钳位、64KB 对齐 padding 段填 0xFF。

### 3.5 完成即重启（2026-10-19 决策，OTACP 上报已移除）

根与子下载/分发完成后均延迟 `LG_MESH_OTA_RESTART_DELAY_MS`（3s）自行重启：

- **触发**：`get_file_done_cb` 成功末尾置 `CMD_EVT_RESTART`（OTA_FINISH(SUCCESS) 非常规
  顺序兜底同样置位，双置位无害——第一次 `esp_restart` 后系统即重置）；
- **执行**：cmd_task 主循环统一处理——状态闸门（仅 PENDING_RESTART）→ WRN 日志 →
  `vTaskDelay(3s)`（串口日志与 MQTT 在途报文落盘）→ `esp_restart()`；闭源库回调上下文
  不做延迟/重启；
- **服务器感知**：节点重启后 MQTT 重连发上线报文（`device/public/up_line`），其中
  `Version` 字段（取自 app_desc）即升级完成信号，另含 `MeshLevel` 层级字段；
- **原 OTACP 上报已整体移除**（`mesh_ota_publish_otacp`/`CMD_EVT_PUBLISH_OTACP`/
  去重标志均已删除）；CanRestart 订阅与处理保留但不再参与 OTA 流程（幂等，无害）。

## 4. 可标定常量（mesh_ota.c 顶部）

| 常量 | 初值 | 标定方法 |
|---|---|---|
| `LG_MESH_OTA_RETRY_MAX` / `LG_MESH_OTA_RETRY_INTERVAL_SEC` | 2 / 90 | 覆盖"根下载+重启"全程即可（上位机未分级下发时兜底） |
| `LG_MESH_OTA_ROOT_RESTART_DELAY_MS` | 3000 | 日志落盘等待，一般无需调 |

> 注：原层级错峰常量（`LG_MESH_OTA_LEVEL_STAGGER_SEC` / `_JITTER_SEC`）已于 2026-10-20
> 移除——升级时序由上位机分级下发控制，见 3.3。

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
- 子：探测/transmit_file_start/禁用桩命中全部符合设计；库供数判定条件已证实。
- 待复测项：方案 A 先行重启 + 完成即重启（子节点）+ 重试命中窗口。

### 6.2 1 根 5 子全场（待执行）

按 3.2/3.3 运维流程：上位机先给根发 ota → 根自动重启上线（up_line Version=新版本）→
再给 5 子同时发 ota（分级下发，子侧零错峰直达）→ 观察根出口仅 1 份、禁用桩不命中、
各子完成后 3s 自行重启、重启后上线报文 Version=新版本，全网完成升级。

### 6.3 50 节点扩展（待执行）

多层树（根 5 槽/层，1+5+15~20+29）：上位机按层级分级下发（L2 → L3 → L4），
中间父级联缓存分摊供数压力；穿透请求为逻辑转发，根 SoftAP 满员不影响分发。
