---
name: ota-defer-restart-to-external
overview: 将 OTA 流程从"下载成功后自动 esp_restart()"改为"下载完成后挂起，通过外部触发重启"：ota.c 新增状态机（IDLE/RUNNING/PENDING_RESTART/FAILED）与查询接口、移除 ota.c:588-589 的自动重启、实现已订阅但空体的 CanRestart MQTT 处理器作为外部重启触发入口，并同步 ota.h/AGENTS.md/OTA_SERVER_MIGRATION_DESIGN.md 文档。原下载流程（入口、URL 构建、版本/文件名校验、HTTP 下载、双槽写入、set_boot_partition）完全不变。
---


## 产品概述

将 ESP32Aging 工程的 OTA 升级流程从「下载成功后自动 esp_restart()」改为「下载完成后挂起，由外部命令触发重启」，使多点位（mesh 组网）场景下各节点可先全部完成下载、再由服务器统一下发重启指令，避免自动重启打断老化/MQTT 会话与 mesh 拓扑。

## 用户需求

1. MQTT 接收到 OTA 指令后，进入 OTA 升级下载流程（保持现有入口与完整下载链路不变）；
2. 移除原先「OTA 成功后直接 esp_restart()」的逻辑，不再自动重启；
3. 重启操作交由用户/服务器控制：保留 OTA 下载完成后可供外部触发重启的接口与状态标识，代码内不自动执行重启；
4. 结合现有 OTA 路径说明调整位置，确保原下载流程（入口、URL 构建、版本/文件名校验、HTTP 下载、双槽写入、set_boot_partition）完整可用。

## 附加分析结论（中间节点 OTA 对子节点 OTA 的影响）

- **功能上不阻塞**：esp-mesh-lite 每节点独立 MQTT/HTTP 连接，子节点流量经每跳 NAPT（bridge_wifi.c:245-246）转发，各节点 OTA 是独立 TCP 连接，协议层可并发下载；
- **性能上争用明显**（按强度排序）：① 单信道空口（全网同信道，中间节点每个转发包占两跳空口，自身 OTA 再加一条流）；② 中间节点 CPU0（WiFi 驱动 + lwIP + NAPT 转发随子树流量线性增长）；③ 根节点单条 WiFi STA 上行是全网漏斗；④ NAPT 表 512 条（10 节点余量充足，非瓶颈）；
- **并发风险**：所有流变慢；深层子节点 MQTT keepalive 15s 在拥塞多跳下有被 broker 判死风险；WiFi/BLE 共放下 BLE 建连/监控窗口被挤占（实机日志 rsn 0x08 掉线的诱因）；ota.c 的 10s/15s HTTP 超时更易触发；
- **最坏触发方式**：广播 `server/public/cmd/ota` 令全网（含中间节点与其子节点）同时下载；
- **建议**（写入文档与验收）：服务器端分批/错峰（叶子先行→中间节点→根最后），OTA 窗口观察 MQTT 重连与 BLE 老化掉线；本次不做限速（非目标）。

## 验收效果

- MQTT OTA 命令 → 完整下载流程保留 → 串口见「下载完成、等待外部重启」日志，**设备不重启**，老化/MQTT 继续；
- `simple_ota_get_state()` 可查询 IDLE/RUNNING/PENDING_RESTART/FAILED；pending 期间重复 OTA 命令被忽略（防二次下载覆盖运行槽）；
- 外部触发：向 `server/rack/<AgingNumber>/CanRestart` 发 `{"Data":{"Restart":1}}` → 节点重启 → 运行新固件；
- 版本不新/更旧：行为不变（跳过/阻止降级，状态回 IDLE）。


## 技术栈

- 现有工程：ESP-IDF v5.4.4、FreeRTOS、C（近 c99）、esp-mesh-lite + iot_bridge；不引入新组件/任务/锁/堆分配；
- 仅改动 3 个代码文件 + 2 个文档：`main/APP/OTA/ota.c`、`main/APP/OTA/ota.h`、`main/Task/appTask.c`、`AGENTS.md`、`OTA_SERVER_MIGRATION_DESIGN.md`。

## 现状代码走查（已确认）

- **入口**：`app_MQTT_Rdata_handle()`（appTask.c:2560-2570，mqtt_rx 任务上下文）——`strstr(TopicBuf,"ota")` 命中即调 `simple_ota_check_and_update_default()`；ota 订阅主题：子节点 `server/
