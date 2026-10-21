# 根节点双网络与 MQTT 恢复流程

本文对应当前代码。设备日志中的两次 `StoreProhibited` 均发生在 FreeRTOS 就绪链表操作；第二次发生在 MQTT 启动前。当前修改控制了 WiFi 断开后的内部重连路径，但尚未通过设备复现确认最初的内存破坏来源。

## 1. 角色与调用链

`app_task_init()` 读取配置：

```text
UDP_Port == 1
  → mesh_init_Custom()
      → bridge 创建 SoftAP、ETH_WAN、WIFI_STA_DEF
      → 根节点 set_allowed_level(1)，子节点 set_disallowed_level(1)
      → 启动 Mesh-Lite
  → 根节点 dual_net_init()
      → 接管现有 netif，注册 ETH/IP/WIFI 事件
      → 读取已存在的链路/IP（覆盖开机已插线）
      → 创建现有 net_monitor 任务
  → 创建 net_init / Init_ByNetwork_Flag()
```

`IsRoot`、设备 ID、Mesh ID、SoftAP 配置不随出口变化。子节点不初始化 dual_net，也不执行根节点 STA 暂停控制。

以太网硬件由 iot_bridge 初始化；应用不重复初始化 SPI、不访问 SDK 不透明句柄内部字段。

## 2. 状态与默认路由

| 状态/API | 含义 |
|---|---|
| `dual_net_has_eth_ip()` | 物理链路在线，取得有效 IP，模块已初始化；不保证服务器可达 |
| `dual_net_is_eth_uplink_ready()` | 有线有有效 IP，现有 MQTT TCP 探测连续通过 |
| `dual_net_is_uplink_ready()` / `Network_Flag` | 本机有线或 STA 具备链路/IP条件；不代表 MQTT 已连接 |
| `dual_net_get_active_uplink()` | 已成功提交默认路由且对应接口仍有效的出口 |
| `dual_net_get_uplink_state()` | 有线 NONE/PROBING/READY/STALE；无有线时反映 STA IP 状态 |

初始化后，根节点 `Network_Flag` 仅由 net_monitor 发布。初始化前/失败时，mesh 的 STA 事件提供兜底；子节点沿用父节点 STA IP 判据。

| 场景 | 路由与业务启动 |
|---|---|
| WiFi 有 IP，有线正在探测 | 保持 WiFi，MQTT 正常运行 |
| 有线探测连续通过 | 有线抢占默认路由；已有健康 STA 保持连接 |
| STA 断开，有线有有效 IP | 立即选有线，Network_Flag 保持 1；不等待下一轮探测 |
| 开机已插线，WiFi 不存在 | 读取现有有线 IP并允许 MQTT 启动；首次探测可随后执行 |
| 有线失去物理链路/IP | 清空有线探测，恢复 STA 重连；STA 有 IP后选 WiFi |
| 有线探测连续失败 | 取消有线抢占资格并恢复 STA；STA 尚无 IP时继续尝试唯一有线出口 |
| 两接口均无有效 IP | active=NONE、Network_Flag=0，原无线恢复策略继续运行 |

`esp_netif_set_default_netif()` 成功且事件状态没有变化时，才发布选择结果。路由失败不打印切换成功。状态变化发生在 SDK 调用期间时，下一轮重新裁决。

SDK 默认 STA 路由优先级高于 ETH_WAN，因此使用显式默认路由。出口切换、IP/DNS事件后，以实际选择的接口为源重新下发 DNS 给数据转发网卡（包括 SoftAP）。`CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF` 的配置不由本次修改调整。

## 3. STA 重连与根节点身份

```text
ETH/IP/WIFI 事件
  → 锁内更新状态和代次，通知 net_monitor
  → net_monitor 提交路由、发布 Network_Flag
  → mesh_root_manage_sta_reconnect(wired_preferred, sta_has_ip)
      → 有线承担出口且 STA 已断开：一次 esp_mesh_lite_disconnect()
      → 有线丢失或探测失败达到阈值：一次 esp_mesh_lite_connect()
```

暂停状态由 net_monitor 单独维护，每次转换只调用一次。公开 disconnect 接口返回错误时，内部可能已暂停重连；保留已请求暂停的标志，使有线失效后仍能执行恢复。错误只记录，不在断开事件中反复调用。

不调用 `esp_wifi_stop()`，不重建 Mesh，不修改根节点层级约束。已连接的健康 STA 不因有线接入而主动断开；暂停后出现迟到的 STA IP事件，也不反复恢复/暂停。

`root_failover_task()` 的有线守卫改用链路/IP条件，覆盖尚未完成探测的启动阶段。扫描结果和所有网络切换重启前再次检查有线，避免过期扫描结果触发重启。无线主备策略在有线失效后继续生效。

SoftAP、根层级以及子节点连接能否在本版本 Mesh-Lite 公开 disconnect 调用后全部保持，需要实际设备验证；宿主测试不验证闭源库的内部行为。

## 4. 探测与并发约束

探测方法保持不变：目标为配置中的 `SERVER_IP:SERVER_UDP_Port`（历史键名，实际 TCP）。socket 同时绑定以太网接口和源 IPv4，非阻塞 connect + select/SO_ERROR，只验证 TCP 建连，不验证 MQTT 认证。超时 2000 ms，每轮关闭 socket。

连续成功 2 次才允许有线抢占 WiFi；连续失败 3 次撤销抢占资格并恢复无线重连。新 IP 首次立即探测，随后默认每 60000 ms 一轮；事件唤醒不额外计一次探测。打开 `DUAL_NET_ENABLE_WIFI_FALLBACK` 时保持原 10000 ms 周期。

状态由短临界区保护，SDK调用、探测、日志和回调均在锁外。探测前后比较以太网代次；探测期间拔线、丢失 IP或换地址时，旧结果被丢弃。相同 IP/网关/掩码的续租保留探测确认。初始化时以太网/无线分别比较事件代次，避免一个接口的事件覆盖另一个接口已存在的 IP。

网线插入/拔出去抖时间为 3000/1000 ms，用于稳定状态确认日志；物理断开立即使该接口失效。监控任务默认最长等待 1000 ms，事件通知可提前唤醒。没有新增任务或队列，原任务栈大小不变。

手动选择接口 API改为向同一监控任务提交请求；`ESP_OK` 表示已受理，以状态查询/回调确认提交结果；NONE 恢复自动选择。所有状态查询及控制入口仅限任务/事件上下文，不可在 ISR调用。

## 5. MQTT 生命周期与日志

```text
Network_Flag=1
  → mqtt_init() 建立队列（已有则复用）
  → mqtt_app_start() 启动单个 SDK客户端（已有则复用）
  → 创建一个 mqtt_rx
  → 根节点 net_init 退出

MQTT连接断开
  → 根节点：原 SDK客户端自动重连（出口变化可快速重连）
  → 子节点：mqtt_service 调度随机指数退避，复用原 SDK客户端
  → 保留 Topic_queue、Data_queue、mqtt_rx、mqtt_service
```

上行变化回调在 dual_net 初始化前注册。默认路由成功改变后，根节点记录最新出口并请求 MQTT 断开；`MQTT_EVENT_DISCONNECTED` 中请求立即重连，跳过普通重连等待。客户端尚未启动时直接使用当前出口，不产生多余断开；无出口时不加速重连。有线与 WiFi 相互切换都执行此流程，重复探测成功和 DNS刷新不会触发新请求。

连接尝试记录上行代次。连接期间换出口时，旧尝试即使成功也不发布上线或恢复订阅，而是重新连接到最新出口。快速切换合并为最新代次；断开请求最多失败重试3次，每次上行变化仅加速一次连接，业务服务器不可达后恢复 SDK正常退避，避免快速重试循环。

WiFi断开、上行切换均不调用 `mqtt_app_restart()`，不销毁客户端、不重建队列或 mqtt_rx。
根/子节点 keepalive 均为60秒，网络超时10000毫秒。根节点普通重连保持SDK默认10秒；
子节点关闭SDK自动重连，首次0～3秒随机等待，后续失败使用2～4、4～8、8～16、16～32、
30～60秒随机区间，稳定在线30秒后才复位。无父链路/IP时暂停连接尝试、恢复后重新错峰。

CONNECTED回调仅更新状态和唤醒CPU1 `mqtt_service`：批量恢复原订阅 → 完整成功SUBACK →
原 `device/public/up_line`（QoS1）→ 该消息PUBACK → 0～5秒随机等待 → 原完整配置上报。
每次有效连接恢复均执行；订阅拒绝/删除、上线入队失败/过期会退避重试，10秒无SUBACK只告警，
不重复提交仍在SDK outbox中的订阅。连接代次和消息ID隔离过期工作；回调不保留SDK数据指针。
提前确认通过PSRAM中的32KiB消息ID位图捕捉，清表在临界区外执行，模块终生复用。

QoS1发布使用SDK enqueue。消息字节预算64KiB，普通业务最多56KiB，余量保留给订阅和上线；
满时返回-2，不递归上报网络诊断。outbox负载使用PSRAM，重传5秒，过期120秒并记录删除事件。
只有运行日志使用QoS0：当前SDK的enqueue(qos=0,store=false)不会发送，因此运行日志通过
3项×256字节的临时队列交给服务任务发送一次，队满、断连或跨连接代次则丢弃，不持久补发。
返回非负值仅表示接受（运行日志为0），不表示broker确认或服务器落库；SQLite的pushed语义本次未改。

生命周期使用静态互斥锁，串行保护启动、停止、后台上线工作和外部重连请求。net_monitor 不阻塞等待该锁，每轮通过 `mqtt_app_process_uplink_reconnect()` 处理延后的请求；SDK事件回调不获取生命周期锁，并使用 event中的客户端句柄。发布采用在途引用计数，停止时先拒绝新发布，等待已开始的发布结束再销毁。停止/销毁失败或发布等待超时保留句柄，避免悬空引用。

本次控制仅操作根节点 MQTT会话，不改变 SoftAP、子节点 IP/关联或 Mesh层级，不广播子节点重连命令。出口变化仍可能影响子节点 MQTT TCP连接；会话恢复与 Mesh连接保持需要分开验收。

新增日志：

- `MQTT client started ... start_count`：客户端启动次数。
- `mqtt_rx started ... start_count`：应用接收任务启动次数。
- `MQTT_EVENT_BEFORE_CONNECT ... client/task/uplink`：原日志 event id=7 的明确含义。
- `MQTT_EVENT_CONNECTED ... connection_count/uplink`：当前出口对应的连接恢复。
- `MQTT 上行变化 ... generation` / `上行切换请求 MQTT 断开` / `上行切换立即重连 MQTT`：实际切换请求及代次。
- `连接期间出口已变化，重新建立 MQTT 连接`：在途连接过期，不误报为切换成功。
- `MQTT_EVENT_DISCONNECTED ... keep client and queues`：断开保留对象。
- `有线接管：已暂停 STA 内部重连` / `有线上行失效：恢复根节点 STA 重连`：实际控制转换。

`uplink` 表示当前选中的默认出口，并非读取该 MQTT socket 的实际本地地址；刚切路由时的旧连接仍可能使用旧源地址。

## 6. 验证与设备验收

宿主回归：`python3 tests/test_root_uplink.py`（Linux，需 C编译器）。直接编译 dual_net.c，mock SDK/网络 I/O，提取并执行实际 Mesh STA控制函数，覆盖热插拔、冷启动、续租、探测期间拔线、路由失败、启动快照并发、初始化回滚、暂停错误恢复、子节点隔离以及通知不加速探测。

MQTT宿主回归：`python3 tests/test_mqtt_uplink.py`。直接编译实际 dual_net.c/MQTT代码，检查根节点主动重连、快速切换合并、连接途中变更、服务不可用时的退避、子节点配置/订阅恢复，并使用 pthread并发验证停止与上行请求、在途发布的句柄安全。mock只能验证应用控制流程，不能验证设备转发、TCP时序及 Mesh-Lite库行为。

本机编译使用 ESP-IDF v5.4.4；日志对应的实际固件是 v5.5.4。二者编译通过不等于设备稳定性验证。请在实际 v5.5.4 环境保留 ELF/hash，并依次验收：

1. 根节点及至少一个子节点无网线启动，WiFi/MQTT正常；保持上游 WiFi在线，插线并等待有线探测通过。确认根节点主动断开/重连，客户端和 mqtt_rx启动次数保持1；通过 broker连接记录或抓包确认新连接使用有线上行。
2. 关闭上游 WiFi；默认出口保持有线、STA重连暂停、MQTT重连后继续收发。
3. 从开机起只有有线，MQTT能够启动，不因无线失联重启。
4. 拔线恢复无线，重新插线恢复有线，多次重复；正常运行期间客户端和 mqtt_rx 启动次数均保持 1。
5. 子节点分别持续收发及空闲运行，切换后确认自动重连、原订阅恢复和双向业务；记录恢复耗时。同步观察根层级、SoftAP、子节点 STA关联/IP，确认身份和拓扑保持。
6. MQTT服务关闭时切换，确认只进行一次快速尝试，随后恢复普通退避；开启服务后恢复收发。连接途中快速插拔，以及配置停止操作与切换同时发生时，确认无重连风暴、重复客户端、死锁或崩溃。

若仍出现同类就绪链表崩溃，使用该固件对应 ELF及栈/堆诊断继续定位首次内存破坏，不能据此直接认定 MQTT重复创建或任务栈不足。

## 7. 硬件与现有配置

以太网 SPI硬件由 bridge管理：SPI2_HOST，SCLK=6、MOSI=15、MISO=7、CS=16、INT=4，KSZ8851SNL。当前修改没有调整硬件、分区、sdkconfig或 SDK版本。

iot_bridge 1.0.1 在 SPI以太网初始化中设置固定 MAC `02:00:00:12:34:56`。同一二层网络多根节点可能冲突，本次不修改 managed_components；需后续在驱动集成层处理。

## HTTP并发协议请求（2026-10-21）

每请求通过user_data持有独立响应缓冲与目标票据（设备类别/索引/ProtoID/请求代次）。
完整且状态为2xx的响应最多16KiB，一次性向3项队列转移所有权；不剥离JSON字符串空白，
不经1024字节分片或8KiB累计缓冲。消费者核验目标和请求代次后安装协议，消费后释放响应。
老化设备释放/替换使旧票据失效，与协议安装共享生命周期互斥锁；网络等待不持锁。
诊断请求不会进入协议安装队列。HTTP回归覆盖并发、乱序、取消、错误、溢出、内存失败和队满。

宿主回归：`python3 tests/test_mqtt_uplink.py`、`python3 tests/test_root_uplink.py`、
`python3 tests/test_http_protocol.py`（HTTP脚本默认使用本机工程指定ESP-IDF中的cJSON，可用IDF_PATH覆盖）。
宿主测试不覆盖闭源Mesh库和实际射频；最终仍需21节点并发老化/补传与根断网恢复验收。
