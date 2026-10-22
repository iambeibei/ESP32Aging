# ESP32Aging 设备端独立分析

日期：2026-10-21。由独立设备端代码分析任务完成，主代理整理留档。已读取仓库指引、实际 MQTT/SQLite/老化/OTA 路径与设备端修改计划。只做静态读取，未改代码、未编译、未连接 Broker 或设备。

## 1. 诊断

设备常规上行发布 QoS 1，全部下行订阅请求 QoS 0，运行日志例外为 QoS 0。修改 QoS 有价值，但计划未解决业务身份、确认时机、接收组包与跨重连幂等。直接实施可能从丢数据发展为丢数据同时串错老化记录。

## 2. 连接与会话

每个节点独立连接 Broker；根节点提供 Mesh 网络出口，不替子节点执行 MQTT 应用转发（main/Task/appTask.c:2966-2982）。

| 配置 | 实际值 | 证据 |
|---|---|---|
| 协议 | MQTT 3.1.1 | main/APP/MQTT/mqtt_app.c:702 |
| Clean Session | true | mqtt_app.c:701，disable_clean_session=false |
| Keepalive | 根/子均 60 秒 | mqtt_app.c:53-55、:699 |
| QoS1 重传间隔 | 5000ms | mqtt_app.c:700 |
| outbox | 总预算 64KiB，业务准入 56KiB | mqtt_app.c:62-63、:879-887 |
| outbox 过期 | 120000ms | sdkconfig.defaults:118 |
| 删除通知 | 开启，但业务未按记录回滚状态 | mqtt_app.c:535-538 |
| 离线时常规发布 | 返回失败 | mqtt_app.c:861-867 |
| 应用成功判据 | SDK 入队成功就置 pushed=1 | mqtt_app.c:887；appTask.c:5220-5227 |

enqueue 由 MQTT 任务异步发送，并不意味着 PUBACK；官方 API 语义与本地调用一致。[ESP-IDF ESP-MQTT 文档](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32/api-reference/protocols/mqtt.html)

## 3. 发布 topic 逐项清单

常规 app_mqtt_publish → mqtt_enqueue_for_session → esp_mqtt_client_enqueue(...,qos=1,retain=0,store=true)，见 mqtt_app.c:887。

| Topic | QoS | retain | 发布流程/位置 |
|---|---:|---:|---|
| device/public/up_line | 1 | 0 | 订阅确认后上线，等待 PUBACK；mqtt_app.c:1165-1173 |
| device/public/down_line | 1 | 0 | 遗嘱；mqtt_app.c:703-704 |
| device/public/deviceMode | 1 | 0 | appTask.c:932 |
| device/public/agingStage | 1 | 0 | appTask.c:851 |
| device/{id}/data/aging | 1 | 0 | 实时 :5221，未推送补传 :4759，缺口补传 :4788，点播 :2734 |
| device/{id}/cmd/reply/point | 1 | 0 | 启停、CanNextstep、CPower；appTask.c:601 |
| device/{id}/cmd/reply/state | 1 | 0 | appTask.c:617、:2687 |
| device/{id}/cmd/reply/pn | 1 | 0 | appTask.c:2145 |
| device/{id}/cmd/reply/config | 1 | 0 | appTask.c:650；上线主动配置 mqtt_app.c:1177-1184 |
| device/{id}/cmd/reply/setlog | 1 | 0 | appTask.c:633；服务无匹配回复处理器 |
| device/{id}/event/state | 1 | 0 | appTask.c:791、:804 |
| device/{id}/event/StepId | 1 | 0 | appTask.c:817 |
| device/{id}/event/TaskCP | 1 | 0 | appTask.c:896 |
| device/{id}/event/warn | 1 | 0 | appTask.c:2100、:3008；当前含裸字符串 |
| device/{id}/log/runtime | **0** | 0 | 独立有界 best-effort 队列；mqtt_app.c:903-918、:1192-1202 |
| device/{id}/log/error | 1 | 0 | appTask.c:355 |
| device/{id}/log/local | 1 | 0 | appTask.c:578 |

这些 topic 均命中服务八条订阅之一，但 Broker→服务请求 QoS 为 0。warn 有 JSON 格式不兼容，setlog 有处理器白名单不兼容，不能仅看订阅命中就认定业务成功。

## 4. 订阅 filter 逐项清单

源码 mqtt_app.c:988-1019。14 是根/子角色并集；单台根节点 5 条，子节点 13 条，全部请求 QoS 0。

| Filter | 角色 | QoS | 服务对应 |
|---|---|---:|---|
| server/{id}/cmd/ota | 全部 | 0 | OTA服务/普通命令 |
| server/rack/{rack}/CanRestart | 全部 | 0 | 本服务无专门发布，外部调用未核实；设备仍有执行代码 |
| server/{id}/cmd/set_config | 全部 | 0 | 通用 action 通道可发布 |
| server/{id}/cmd/get_config | 全部 | 0 | 普通命令 |
| server/{id}/cmd/CPower | 根 | 0 | 群控供电 |
| server/{id}/cmd/start_aging | 子 | 0 | 普通/老化命令队列 |
| server/{id}/cmd/stop_aging | 子 | 0 | 普通命令 |
| server/{rack}/public/cmd/stop_aging | 子 | 0 | 当前未见专门发布 |
| server/{id}/cmd/set_pn | 子 | 0 | 普通命令 |
| server/{id}/cmd/get_state | 子 | 0 | 普通命令；根节点未订阅 |
| server/{id}/cmd/get_lost_aging | 子 | 0 | 通用 action 通道可发布 |
| server/{id}/cmd/remote_log | 子 | 0 | 通用 action 通道可发布 |
| server/{id}/cmd/local_log | 子 | 0 | 通用 action 通道可发布 |
| server/rack/{rack}/CanNextstep | 子 | 0 | 群控放行 |

服务常规发布 QoS 1，但 Broker→设备以 QoS 0 投递。SUBACK 目前仅检查返回码≤2（mqtt_app.c:512-519）；改请求 QoS 1 后应验证实际授予为 1。

## 5. 可靠性与补偿现状

1. 实时采样先保存 SQLite pushed=0，再尝试 MQTT enqueue。离线发布失败时保留待发；入队成功立即置 pushed=1，尚未得到 PUBACK。
2. 通用 QueryUnpushedRecords 已实现，当前无通用重连 worker 调用。
3. 正常老化结束时会补当前 PN 的 pushed=0（appTask.c:4747）；随后 HTTP 查询业务记录缺失 ID 再补发（:4774-4799），可能覆盖部分 pushed=1 缺口。不能说完全无补偿。
4. 中止、换 PN、长期断线与补传失败没有通用闭环；点播/HTTP缺口补传按 seq_no 不带 PN 查询，还可能读错记录。
5. 命令 Seq 去重为 16 项内存表，命中后静默丢弃、不重放回复（appTask.c:2378、:2438-2443）。普通命令回复丢失后原 Seq 重试无法获得结果。

## 6. 现有设备计划的必改项

### 6.1 跨 PN 补传先修 RecordId 与工步，不然会串批

SQLite schema 与 QueryResult 没保存采样所属 RecordId（main/DataStorage/SqLite/SqLite.c:790-800；sqllib1.h:33-50）。查询历史 JSON 时 sqllib1.c:183 使用当前全局 RecordId，新的 start_aging 会覆盖它（appTask.c:2538-2546）。

新 worker 跨 PN 扫描时，旧 A 轮记录可能带上 B 轮 RecordId；服务把原 payload 发给 API，形成业务串包。

另实时 JSON 发 current_step+1（Json_data.c:871），数据库存原始 0 基工步（appTask.c:4988、:5201），补传却发 record->current_step（sqllib1.c:184），同条数据工步相差 1。

应固化采样时的 RunId/RecordId 和协议字段，实时/补传使用同一序列化路径；旧记录归属无法恢复时要有隔离/迁移策略。

### 6.2 “入队即 pushed=1”使新 worker 漏补在途丢失

设备计划 §12.2:761 称 PUBACK 跟踪收益有限、服务幂等与补传已覆盖丢失，此判断不成立。入队后掉电、客户端销毁、outbox 过期都会使 pushed=1 记录未送达，扫描 pushed=0 的 worker 找不到它。

应至少区分 pending/inflight/broker_acked，关联 MQTT msg_id、PN/RunId、IDNUM，并处理 DELETED/重启恢复。若承诺平台完整入库，还需应用确认或定期缺口对账；PUBACK 只到 Broker。

### 6.3 不应随连接 epoch 清空业务去重

计划 :406 要在每次 epoch 变化清表。断线重连正是服务用原 Seq 重试的高概率场景，清表会重新执行旧命令。若从 SDK 回调清应用表，还破坏原单写者假设。

业务去重应绑定稳定 CommandId/RunId、动作与有效期；缓存期限覆盖服务重试窗口，关键动作考虑持久化或从业务状态恢复。

### 6.4 回复应先缓存结果，再尝试发送，且覆盖所有路径

计划 :373-391 只在首次 app_mqtt_publish 成功之后缓存。执行成功、回复入队失败时无结果可重放，问题仍存在。get_state 又直接构造发布（appTask.c:2682-2692），没有经过计划所改的 state 辅助函数。

512B 超长不缓存、分配失败关闭去重、仅 16 条记录都需明确安全降级。旧条目被挤掉后，再收到旧命令会重新执行，不是仅“不重放”。回滚为 NULL 的方案也会使新 mqtt_seq_record 直接返回，连原防重一起关闭。

### 6.5 两个接收 FIFO 可能错配，扩容不够

topic 先入 Topic_queue（mqtt_app.c:565-587），payload 再分片入 Data_queue（:594-613）。任何一侧溢出都可能留下孤立项。appTask.c:2417-2429 又只在 JSON/Seq 合法后才取 topic；完整但 Seq 非法的报文也可留下旧 topic，使后续合法 payload 进入错误命令分支。

应以统一消息封装带 topic、完整长度/offset 和消息身份进行组包、消费和取消；解析失败也消耗同一整包。扩大队列与计数只能改善容量和观测。

### 6.6 IDNUM、容量、坏记录需先定义

- IDNUM 规则已可查：PN 变化通常从 0 开始，恢复才续 SQLite 最新值（appTask.c:5258-5287）。A→B→A 等会复用 PN+IDNUM，现有 SQLite 唯一键还可能更新旧行。服务去重和设备存储都应包含轮次身份。
- 上行 Seq 常为秒级 time(NULL)，同秒冲突、补传重建，不能当唯一采样 ID 或直接作为严格单调状态版本。
- 缓存上限 1200 条（SqLite.h:25），满时连 pushed=0 也可能被淘汰（SqLite.c:1318-1321、:1435-1445）；按 5 秒一条约 100 分钟。
- worker 空 JSON 直接 continue 却不隔离坏记录，可能不断扫描坏头批次并饿死后续；状态更新失败也需退避和错误处理。
- get_lost_aging 与结束缺口补传按 seq_no 查询最新匹配，不带 PN（SqLite.c:1790-1794），会在跨 PN 同号时误取。

### 6.7 死订阅与 OTA 回滚不能按文档直接处理

服务 RedisCmdWorker.cs:754-761 是通用 action 发布，缺少专用 publisher 不证明 set_config/remote_log/local_log/get_lost_aging 无人用。外部系统与排障工具也未包含在这两个目录。

CanRestart 仍有执行分支，但“OTA 唯一重启入口”注释已过时；实际 mesh_ota.c:28-31、:530、:640 是完成后自动重启。

计划要求 OTA 回退旧版本，当前 ota.c:700-710/mesh_ota.c:576 阻止低版本，bootloader rollback 也未启用。应先准备可执行恢复方案，例如更高版本重新发布旧实现或经过验证的受控恢复通道。

local_log 还会长时间占用 mqtt_rx：vTaskDelay(100) 是原始 tick，100Hz 时每条约 1 秒，3000 条约 50 分钟；加深 32 项队列仍不解决这种阻塞。

## 7. 建议顺序和验证

先统一并持久化采样身份/协议字段；修完整消息接收；命令结果先缓存、跨连接去重；与服务建立可靠入库及对账；再开放跨 PN worker；最后分方向启用 QoS 1 并校验 SUBACK。

必测：入队后立即断电、outbox 过期、业务执行后回复入队失败、重连原 Seq 重试、超过 16 条、大配置回复、同 PN 重测、跨 PN 补传、非法 Seq、分片/队列溢出、缓存耗尽与实际 OTA 恢复。

可靠性验收应同时检查采样完整性、RecordId/工步归属正确和动作只执行一次，不能只观察“收到 PUBACK”或“queue_overflow=0”。
