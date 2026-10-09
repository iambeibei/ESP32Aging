# dual_net 双网模块设计

面向「WiFi STA 上行 + 有线以太网」的双网通信能力。本文回答五件事：
集成方式、与现有子节点连接逻辑的关系、切换策略、配置项、后续有线接入的兼容设计。

## 1. 子节点成功连接根节点的现有实现（零改动保护区）

| 环节 | 位置 | 内容 |
|---|---|---|
| 网络总开关 | `main/Task/appTask.c:5119` | `if (UDP_Port == 1)` → `mesh_init_Custom()` |
| 子节点入网配置 | `main/Communication/Internet/Mesh/mesh.c:422-423` | `join_mesh_ignore_router_status=true`、`join_mesh_without_configured_wifi = !IsRoot` |
| 层级约束 | `mesh.c:443` | `esp_mesh_lite_set_disallowed_level(1)`，子节点禁止成为第 1 层 |
| 父节点 SSID 反解 | `mesh.c:264-279` `lg_mesh_get_ssid_by_mac()`，注册于 `mesh.c:451` | 子节点靠 BSSID 重建父 SoftAP SSID |
| **成功判定点** | `mesh.c:116-133` `network_event_handler()` | `IP_EVENT_STA_GOT_IP` → `Network_Flag = 1` |
| 断链判定点 | `mesh.c:134-139` | `IP_EVENT_STA_LOST_IP` / `WIFI_EVENT_STA_DISCONNECTED` → `Network_Flag = 0` |
| 业务消费方 | `appTask.c:2987` `Init_ByNetwork_Flag()` | 轮询 `Network_Flag==1` → MQTT / HTTP / 老化上传 / 掉电恢复 |
| 任务编排 | `appTask.c:5128-5170` | dual_net 初始化、`net_init`、`aging_upload` 任务创建 |

`Network_Flag` 的两种语义（**两者都只由 `mesh.c` 写入**）：

- 根节点：STA 连上外部路由器取得 IP → 1
- 子节点：经 mesh 从根节点 SoftAP 通过 NAPT 取得网段 IP → 1

## 2. 集成方式

### 2.1 构建接入

`main/CMakeLists.txt`：`SRCS` 增加 `Communication/Internet/dual_net/dual_net.c`，
`INCLUDE_DIRS` 增加 `Communication/Internet/dual_net`。

### 2.2 调用点与时序

`appTask.c` 中 `mesh_init_Custom()` 返回 `ESP_OK` 之后、`Init_ByNetwork_Flag` 任务创建之前：

```c
if (IsRoot == 1) {
    esp_err_t dn_err = dual_net_init();
    if (dn_err != ESP_OK) {
        ESP_LOGE(TAG, "dual_net init failed: %s", esp_err_to_name(dn_err));
        storage_write_record_cyclic(current_log_pn(), "dual_net_init failed");
    }
}
```

**时序硬约束**：必须在 `esp_bridge_create_all_netif()`（`mesh.c:378`）之后调用，
否则 `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")` 返回 NULL。

### 2.3 双重根节点门控

| 层级 | 位置 | 作用 |
|---|---|---|
| 主防线 | `appTask.c` 调用点 `if (IsRoot == 1)` | 子节点根本不调用 |
| 兜底 | `dual_net.c:dual_net_init()` 开头 `if (IsRoot != 1) return ESP_OK;` | 即使调用点判断被误删，子节点也不会进入模块 |

`IsRoot` 只在 `appTask.c:992`（`load_baseconfig_json()`，启动期一次性）被写，
且 `mesh.c:440-444` 用 `set_allowed_level` / `set_disallowed_level` 把层级静态固定，
因此入口判断一次即可，不需要跟踪运行时层级变化。

### 2.4 根节点 / 子节点职责对照

| 维度 | 根节点（`IsRoot==1`） | 子节点（`IsRoot==0`） |
|---|---|---|
| dual_net 是否初始化 | 是 | **否，入口直接返回** |
| 事件处理器注册 | 是（IP / WIFI / ETH） | 无 |
| 监控任务 | 1 个（CPU1，仅观察模式 60s 快照） | 无 |
| 上行来源 | 外部路由器 STA 或有线 ETH（ETH 本轮预留） | mesh 父节点 → 根节点 SoftAP → NAPT |
| `Network_Flag` 写入者 | `mesh.c:network_event_handler` | `mesh.c:network_event_handler` |
| ETH 硬件 | 开关默认关闭 | 不初始化 |
| 主备路由器切换 | `root_failover_task`（`esp_restart`） | 不涉及，由 mesh 重新选父 |

## 3. 与现有子节点连接逻辑的关系

**dual_net 是观察层，mesh.c 是判定层，二者平行且 dual_net 只读。**

- `Network_Flag` **唯一写入者**是 `mesh.c:network_event_handler()`（第 123、136 行）。
  改造前的 `dual_net.c` 有三处写 `Network_Flag` 并调用 `esp_mesh_post_toDS_state()`，
  现已全部移除：后者是 ESP-WIFI-MESH 遗留 API，在 Mesh-Lite 构建下无法链接；
  前者改为受 `DUAL_NET_ENABLE_ROUTE_CONTROL` 保护（默认关闭）。
  这消除了两个写者竞争同一全局量的风险。
- dual_net 默认不调用 `esp_netif_set_default_netif()`：IoT-Bridge 的 NAPT 面向单一
  external netif（当前是 STA），强切默认网卡会让子节点回程包错乱。
- dual_net 默认不调用 `esp_wifi_connect()`：Mesh-Lite 自行管理 STA 与父节点选择，
  外部强连会打断自组织入网（这正是 `mesh.c:264-279` 注释里警告的坑）。
- 事件处理器注册用 `ESP_EVENT_ANY_ID`，与 `mesh.c` 按具体 event_id 注册的 handler
  **共存于同一事件循环、互不屏蔽**；handler 内只做轻量状态赋值，不阻塞、不做 I/O。

## 4. 切换策略

### 4.1 以太网优先 / WiFi 兜底（本轮策略）

同一时刻只承认一条上行，优先级：

1. `DUAL_NET_UPLINK_ETH` —— ETH 拿到 IP（`IP_EVENT_ETH_GOT_IP`）时置 `eth_up`
2. `DUAL_NET_UPLINK_WIFI` —— ETH 不可用时回落（`IP_EVENT_STA_GOT_IP` 时置 `wifi_up`）
3. `DUAL_NET_UPLINK_NONE` —— 两者都不可用

由 `dual_net_get_active_uplink()` 返回，`dual_net_update_route()` 在变化时
通过 `dual_net_notify_uplink_change()` 通知订阅者。

### 4.2 为什么不做并行双上行（L2 仅预留）

- lwIP 只有一条默认路由，ETH 与 WiFi 同时承载上行会导致源地址选择不确定；
- IoT-Bridge 的 NAPT 面向单一 external netif，双上行会让回程包错乱、MQTT 连接抖动。

后续若要并行，需要按目的地址做策略路由或按业务分流，属独立的 L2 改造，
本轮只预留 `dual_net_select_uplink()` 与 `dual_net_register_uplink_change_cb()` 接口。

### 4.3 能力门控矩阵

| 级别 | 开关组合 | 行为 |
|---|---|---|
| L0（**默认**） | 全部副作用开关 = 0 | 只维护 `eth_up`/`wifi_up` 状态供查询与日志；不改路由、不写 `Network_Flag`、不连 WiFi、不初始化 ETH 硬件 |
| L1 | `DUAL_NET_ETH_HW_ENABLE=1` + `DUAL_NET_ENABLE_ROUTE_CONTROL=1` | 初始化 KSZ8851SNL 硬件，ETH 拿到 IP 后切默认路由，ETH 断开回落 WiFi |
| L2（预留） | 追加策略路由 / 分流 | 并行双上行；需改造 IoT-Bridge NAPT 与 lwIP 路由策略 |

## 5. 配置项

### 5.1 编译期（`main/Communication/Internet/dual_net/dual_net.h`，`#ifndef` 可外部覆盖）

| 宏 | 默认 | 含义 |
|---|---|---|
| `DUAL_NET_ENABLE` | 1 | 模块总开关 |
| `DUAL_NET_ROOT_ONLY` | 1 | 仅根节点运行；子节点入口直接返回 |
| `DUAL_NET_ETH_HW_ENABLE` | 0 | 1 = 初始化 KSZ8851SNL 硬件并 `esp_eth_start()` |
| `DUAL_NET_ENABLE_ROUTE_CONTROL` | 0 | 1 = 允许 `esp_netif_set_default_netif()` 并写 `Network_Flag` |
| `DUAL_NET_ENABLE_WIFI_FALLBACK` | 0 | 1 = 允许 dual_net 主动 `esp_wifi_connect()` |
| `LG_DUAL_NET_MONITOR_IDLE_PERIOD_MS` | 60000 | 仅观察模式下的监控轮询周期 |
| `LG_DUAL_NET_MONITOR_ACTIVE_PERIOD_MS` | 10000 | 允许主动连接时的轮询周期 |
| `LG_DUAL_NET_AUTH_RETRY_MAX_COUNT` | 30 | 认证失败后的重试次数上限 |
| `LG_DUAL_NET_WIFI_RETRY_INTERVAL` | 3 | 无网络时每 N 次轮询尝试一次 WiFi |
| `LG_DUAL_NET_ETH_SPI_CLK_HZ` | 5000000 | KSZ8851SNL 的 SPI 时钟 |
| `LG_DUAL_NET_ETH_SPI_QUEUE_SIZE` | 20 | SPI 传输队列长度 |

### 5.2 以太网引脚（`dual_net.h`）

`SPI_MOSIPIN 15`、`SPI_MISOPIN 7`、`SPI_SCLKPIN 6`、`SPI_CSPIN 16`、`ETH_INT 4`。
仅在 `DUAL_NET_ETH_HW_ENABLE=1` 时占用，默认不占用 SPI2_HOST 与 GPIO。

### 5.3 运行期配置：**无**

dual_net **不提供运行期配置项**，`data/baseconfig.json` 中没有相关字段。理由：

- 模块只在根节点启动，不需要按节点差异化配置；
- 是否启用以太网硬件、是否切路由属于固件能力，改它必须重新编译验证（涉及
  SPI 总线与 GPIO 占用、IoT-Bridge NAPT 链路），做成运行期开关只会让人误以为可以热切换；
- 避免改 `data/` 后必须整片重刷 SPIFFS（`app-flash` 不会更新配置分区）。

需要调整行为时，直接改 `dual_net.h` 的编译宏默认值并重新编译即可。

> 补充：「以太网优先、WiFi 兜底」的顺序由 `dual_net_update_route()` 写死
> （先判 `eth_up` 再判 `wifi_up`），本来就不具备运行期可调性，
> 因此也不存在对应的优先级配置项。

## 6. 后续接入有线网络的兼容设计

### 6.1 关键约束：不要启用 IoT-Bridge 自带的 SPI 以太网 netif

`managed_components/espressif__iot_bridge/src/bridge_eth.c` 也会 `spi_bus_initialize(SPI2_HOST, ...)`
（`bridge_eth.c:323`，`ESP_ERROR_CHECK` 会在重复初始化时 abort），
且其默认引脚（SCLK=12 / MOSI=11 / MISO=13 / CS=10 / INT=GPIO4）与本模块的
SPI2_HOST + GPIO4 中断脚**直接冲突**。

当前 `build/mesh-lite/sdkconfig` 状态（安全）：

- `CONFIG_BRIDGE_ETHERNET_NETIF_ENABLE=y`（父开关，只是让 `bridge_eth.c` 参与编译）
- `# CONFIG_BRIDGE_EXTERNAL_NETIF_ETHERNET is not set`
- `# CONFIG_BRIDGE_DATA_FORWARDING_NETIF_ETHERNET is not set`
- `# CONFIG_BRIDGE_NETIF_ETHERNET_AUTO_WAN_OR_LAN is not set`

→ `esp_bridge_create_all_netif()` 中两处 `esp_bridge_create_eth_netif()` 调用都被 `#if` 排除，
不会创建任何 ETH netif、不会 `spi_bus_initialize`。**保持这个状态。**

### 6.2 有线接入改造清单

1. **选路**：若以太网只是「本节点当终端上网」，用现有
   `esp_netif_new(ESP_NETIF_DEFAULT_ETH())` 即可（`dual_net_eth_init()` 现状）。
   若要让以太网**承担根节点上行并转发子节点流量**，必须改为
   `esp_bridge_create_eth_netif()` + `esp_bridge_netif_list_add()`，
   否则绕过 NAPT/DHCP 注册，子节点流量无法被正确转发。
   届时需要二选一地开启 `CONFIG_BRIDGE_EXTERNAL_NETIF_ETHERNET`（WAN）
   或 `CONFIG_BRIDGE_DATA_FORWARDING_NETIF_ETHERNET`（LAN），
   并把 bridge 侧 SPI 引脚改成与本模块一致（或反过来改用 bridge 的驱动，删除本模块硬件初始化）。
2. **MAC 派生**：`dual_net_set_valid_eth_mac()` 用 WiFi STA MAC 派生一个本地单播 MAC
   （bit0 清零、bit1 置 1、末字节 +3），启用硬件时需确认与 `esp_bridge_create_eth_netif()`
   传入的 mac 参数不冲突。
3. **路由切换**：打开 `DUAL_NET_ENABLE_ROUTE_CONTROL` 后，
   `dual_net_update_route()` 才真正执行 `esp_netif_set_default_netif()`。
   建议同时把 `Network_Flag` 的写入权交给 dual_net（此时它同时感知 ETH 与 WiFi），
   并**移除或旁路 `mesh.c` 中的对应写点**——但这一步会触及零改动保护区，需单独评审。
4. **root failover 交互**：`mesh.c:185-249` `root_failover_task` 目前靠 `esp_restart()`
   在 `SSID` / `SSID1` 之间切换主备路由器。ETH 启用后应改为
   「ETH 可用时不重启、直接优先 ETH」，否则插着网线也会因主路由不可达而反复重启。
5. **事件映射**：

   | 语义 | 来源 |
   |---|---|
   | 根节点取得上行 IP（无线） | `IP_EVENT_STA_GOT_IP` |
   | 子节点取得根节点分配网段 | 同上（Mesh-Lite 侧另有 `ESP_MESH_LITE_EVENT_CORE_INHERITED_NET_SEGMENT_CHANGED`） |
   | 根节点上行路由器变更 | `ESP_MESH_LITE_EVENT_CORE_ROUTER_INFO_CHANGED` |
   | 网线物理链路通断 | `ETHERNET_EVENT_CONNECTED` / `DISCONNECTED` |
   | 有线取得 / 丢失 IP | `IP_EVENT_ETH_GOT_IP` / `IP_EVENT_ETH_LOST_IP` |

6. **预留 API**（已实现，当前在门控关闭时返回 `ESP_ERR_NOT_SUPPORTED`）：
   `dual_net_get_active_uplink()`、`dual_net_get_uplink_netif()`、
   `dual_net_select_uplink()`、`dual_net_register_uplink_change_cb()`。

## 7. 验证

- 编译（含 `DUAL_NET_ETH_HW_ENABLE=1` 的 `-fsyntax-only` 语法检查）通过，无新增警告。
- **实机串口冒烟尚未执行**（无设备）。按 `MESH_LITE_MIGRATION.md` 的「实机验收」执行时，需额外确认：
  - 子节点串口**不出现** `=== Dual Network Init` 日志，也无 `net_monitor` 任务创建；
  - 根节点串口出现 `=== Dual Network Init (Ethernet Priority Mode, root only) ===`；
  - 两种角色下子节点入网、MQTT 连接、老化上传行为与改造前一致。
