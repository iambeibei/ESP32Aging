# 一键重启与父节点自愈（2026-10-22）

本文件描述当前实现；替代旧的 CanRestart 固定 5 秒重启，以及“子节点普通软件重启即可
绕开原父缓存”的假设。保留 ESP-Mesh-Lite 1.0.2、单根架构、每节点独立 MQTT。
OTA 下载本身仍不自动重启，供数恢复见 `MESH_LITE_OTA_INTEGRATION.md`。

## 一键重启

主题和下行格式保持兼容：

```text
server/rack/<AgingNumber>/CanRestart
{"Seq":1782373476,"Data":{"Restart":1}}
```

仅接受数值 `Restart=1`；缺失、字符串、布尔、其他数值均返回失败。不再保留可被错误负载
沿用的全局 CanRestart 值。沿用现有命令 Seq 去重机制，发送方应为不同命令使用不同 Seq。

`system_restart.c` 使用一个长期 CPU1 服务任务。`app_task_init()` 在 Mesh/MQTT 启动前初始化，
通过 `create_cpu1_task()` 封装创建，栈 3072、优先级 5（`task_config.h`）。创建失败时拒绝
命令，不降级为 MQTT 接收任务内的同步延时重启。初始化可重试且成功后幂等。

接收任务只校验、提交并应答。首次接受时保存 Seq、层数、单调时钟截止时间：

```text
收到命令 → 保护窗口（保持联网 30s）→ 暂停 Mesh 上行重连 → 按本层截止时间重启
```

`LG_RESTART_BASE_DELAY_SEC=30`、`LG_RESTART_LEVEL_INTERVAL_SEC=30`，计算：

```text
暂停时间 = 接收时间 + 30s
重启时间 = 接收时间 + 30s + (层数 - 1) × 30s
```

| 层数 | 暂停连接 | 重启 |
|---|---|---|
| 根（第一层） | 收到后 30s | 收到后 30s |
| 第二层 | 收到后 30s | 收到后 60s |
| 第三层 | 收到后 30s | 收到后 90s |
| 第四层 | 收到后 30s | 收到后 120s |
| 第五层 | 收到后 30s | 收到后 150s |
| 第六层 | 收到后 30s | 收到后 180s |

根固定第一层。子优先当前有效层数 2..6，其次最近有效层数，否则取配置最大层数 6。
IP 获取及健康轮询均采样层数。首次排程后层数冻结；掉线后库返回 0、校时、重复命令或
倒计时期间新 Seq 均不改变截止时间。计划只保存在本次运行 RAM，启动后回到正常组网。
任务空闲时等待通知，排程后每 100ms 检查，实际执行存在调度及 SDK 调用耗时。

接收回执复用 QoS1 的 `device/<DeviceId>/cmd/reply/point` 与既有字段：

```json
{"Seq":1782373477,"Data":{"CmdSeq":1782373476,"Code":1,"Msg":"Restart scheduled: plan_seq=1782373476 level=2 pause=30s restart=60s","CmdData":null}}
```

失败 `Code=0`；新 Seq 遇到既有计划时，回执的 CmdSeq 关联本次请求，Msg 列明原计划 Seq。
重复 Seq 使用既有应答缓存重放。SDK 接受后由 QoS1 重传；不把“已入队”当作上位机已收到，
入队失败沿用现有诊断/命令重试机制。本轮未增加全员确认后的第二阶段提交：30 秒仅是传播
窗口，不能保证离线/拥塞节点均收到，跨层时间顺序以接收时间差小于层间隔为前提。
上位机需汇总回执识别漏收，命令应使用非 retained 发布。

## 暂停期间的网络控制

- 从接受计划起，子节点无父 watchdog、连续失败重启、根主备故障转移均停止介入。
- 前 30 秒保持常规连接；30 秒后服务标记暂停，再调用 `mesh_pause_for_restart()`。
- 应用 STA 连接/断开共用静态 mutex，避免有线仲裁中的在途连接晚于暂停调用完成。
  获取锁最多等 50ms；SDK 接口自身耗时依其实现，未使用不安全的强制任务取消。
- 使用 `esp_mesh_lite_disconnect()` 暂停上行与重连，不清连接记录、不关闭 SoftAP 或整个
  Wi-Fi；不触碰 BLE、数据库或配置。根节点有线仲裁在暂停标志有效时也不得重新 connect。
- 断开失败最多尝试 3 次、间隔 1 秒；失败不会推迟截止时间。根在 30 秒先尝试一次断开
  随即重启，不为重试额外等待。等待重启的更深层可完成后续有限重试。
- 计划提交与自愈最终重启通过短临界区仲裁：计划先成功则禁止自愈；自愈已经提交则拒绝
  新计划。不会一边返回排程成功，一边由另一条自愈路径清缓存提前重启。

## 父节点连接失败自愈

保留重连节奏 10s×5 次、失败后 15s 重扫，单节点最多 5 个子节点，最大层数 6。
自愈仍有无父 180s 后断开/连接（最多 3 次）及连续失败 5 次的快通道，二者共享
每上电周期最多 3 次重启的 RTC 预算。健康任务固定每 5s 检查，健康态日志每 60s 输出。

两条子节点重启路径统一进入 `mesh_restart_without_parent()`：

```text
确认无计划且仍无网络 → 检查预算 → 最终 100ms 观察窗 → 再核对网络/仲裁
  → 增加 RTC 预算 → esp_mesh_lite_erase_rtc_store() → esp_restart()
```

清理的是 Mesh-Lite 自身的 RTC 连接信息，不是清空 RTC RAM 或 NVS，因而不重置项目的
重启预算、老化断点及 OTA 供数记录。软件重启延续预算，上电等非 SW 重置沿用既有清零规则。
正常一键重启保留连接记录；故障自愈丢弃旧记录后重新扫描，仍不保证一定选择不同父节点。
disconnect/connect 软恢复本身不承诺清掉原父缓存，也不等同物理断电。

失败计数：

- 只在非主动 `WIFI_EVENT_STA_DISCONNECTED` 累加；`LOST_IP` 结束在线窗但不重复计数。
- `GOT_IP` 只开始在线窗，不立即清零；重复 GOT_IP/续租不重置起点。
- 连续在线 10s 后清零；断开时若在线已满 10s 也先清零，再将本次失败计为 1。
- 在线状态与时间在掉线时同步结束，旧 GOT_IP 的时间不再反复重置后续失败。
- 应用主动 disconnect 对应的本地 `ASSOC_LEAVE` 及整个计划重启期间的断开不计失败。
- 仅丢 IP 而无断开事件的情况由无父超时处理，不凭空构造连接失败次数。

关键日志：

```text
RESTART_SCHEDULED seq=... plan_seq=... level=... pause=30s restart=...s
RESTART_MESH_PAUSE seq=... level=... remaining_ms=...
RESTART_EXECUTE seq=... level=... keep_mesh_record=1 disconnected=...
Parent connected: bssid=... rssi=...
STA disconnected, reason=... ssid=... bssid=... local=... planned=...
MESH_RECOVERY_RESTART reason=... failures=... last_parent=... reason_code=... budget=.../3 erase_mesh_rtc=1
```

## 验证与实机验收

```bash
python3 tests/test_system_restart.py
ASAN_OPTIONS=detect_leaks=0 TEST_SANITIZE=1 python3 tests/test_system_restart.py
python3 tests/test_root_uplink.py
python3 tests/test_mqtt_uplink.py
python3 tests/test_http_protocol.py
python3 tests/test_mesh_ota_binary.py
```

专项回归使用真实重启服务、Mesh 的控制/事件/自愈函数及实际命令处理函数，模拟 SDK/RTOS，
覆盖六层时序、非法负载、任务失败、重复命令、锁竞争、断开重试、故障预算、持续在线判断
及计划/自愈竞争；不模拟闭源 Mesh 库的射频行为。构建执行 AGENTS.md 指定命令。

实机待验收：

1. 根+第二层+第三层均先安装本实现，再发一次非 retained CanRestart；汇总每台的接收回执。
2. 核对前 30s 网络正常，各设备在各自 30s 到期时暂停，根/L2/L3 分别约 30/60/90s 重启。
   暂停后无应用重新 connect、无自愈提前重启；记录父 SoftAP 恢复耗时后评估 30s 层间隔。
3. 覆盖重复/新 Seq、层数 0、MQTT 中断、有线插拔、断开接口失败；本地截止时间保持不变。
4. 故意使父不可连接，核对自愈日志清 RTC 与预算，再对比重连 BSSID；分别对比普通软件
   重启、清记录重启和物理断电，确认现场是否真正换父，检查候选父容量与信号。
5. 连续失败跨软件重启最多消耗 3 次预算，之后只扫描；短暂 GOT_IP 不清计数，稳定 10s 才清。
6. OTA 暂存 B 后一键重启，确认运行 B、恢复供数及原 OTACP；核对 SQLite、老化断点及配置。

尚未取得上述实机结果，不宣称清记录重启必定换父或全网严格同步。
