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
| `DUAL_NET_ETH_HW_ENABLE` | **1** | 1 = 接管 iot_bridge 创建的 `ETH_WAN` netif 并参与上行裁决（置 0 可回到纯 WiFi） |
| `DUAL_NET_ENABLE_ROUTE_CONTROL` | **1** | 1 = 允许 `esp_netif_set_default_netif()` 并写 `Network_Flag` |
| `DUAL_NET_ENABLE_WIFI_FALLBACK` | 0 | 1 = 允许 dual_net 主动 `esp_wifi_connect()`（默认不开，避免打断 Mesh-Lite 自组织） |
| `LG_DUAL_NET_MONITOR_IDLE_PERIOD_MS` | 60000 | 仅观察模式下的监控轮询周期 |
| `LG_DUAL_NET_MONITOR_ACTIVE_PERIOD_MS` | 10000 | 允许主动连接时的轮询周期 |
| `LG_DUAL_NET_AUTH_RETRY_MAX_COUNT` | 30 | 认证失败后的重试次数上限 |
| `LG_DUAL_NET_WIFI_RETRY_INTERVAL` | 3 | 无网络时每 N 次轮询尝试一次 WiFi |
| `LG_DUAL_NET_ETH_LINK_UP_DEBOUNCE_MS` | 3000 | 网线插入去抖确认时间 |
| `LG_DUAL_NET_ETH_LINK_DOWN_DEBOUNCE_MS` | 1000 | 网线拔出去抖确认时间 |
| `LG_DUAL_NET_UPLINK_PROBE_TIMEOUT_MS` | 2000 | 单次连通性探测超时（探测会阻塞，仅可在任务上下文调用） |
| `LG_DUAL_NET_UPLINK_PROBE_OK_COUNT` | 2 | 连续探测成功达到该次数才认定上行可用 |
| `LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT` | 3 | 连续探测失败达到该次数才取消以太网优先 |

> 连通性探测目标是 `SERVER_IP:SERVER_UDP_Port`（MQTT 服务器），
> 而非网关——因为它才是业务真正依赖的目标；「网关通但服务器不通」同样应判定为上行不可用。

### 5.2 以太网引脚与硬件（menuconfig / `sdkconfig.defaults`）

以太网硬件由 **iot_bridge 的 `bridge_eth.c`** 负责，`dual_net.h` 中**不再**定义引脚。
引脚全部是 Kconfig 项，已在 `sdkconfig.defaults` 中覆盖为原硬件接线：

| Kconfig | 本工程取值 | bridge 默认(ESP32-S3) |
|---|---|---|
| `CONFIG_BRIDGE_EXTERNAL_NETIF_ETHERNET` | **y** | n |
| `CONFIG_BRIDGE_ETH_SPI_HOST` | 1（=SPI2_HOST） | 1 |
| `CONFIG_BRIDGE_ETH_SPI_SCLK_GPIO` | **6** | 12 |
| `CONFIG_BRIDGE_ETH_SPI_MOSI_GPIO` | **15** | 11 |
| `CONFIG_BRIDGE_ETH_SPI_MISO_GPIO` | **7** | 13 |
| `CONFIG_BRIDGE_ETH_SPI_CS0_GPIO` | **16** | 10 |
| `CONFIG_BRIDGE_ETH_SPI_INT0_GPIO` | 4 | 4 |
| `CONFIG_BRIDGE_ETH_SPI_CLOCK_MHZ` | 16 | 16 |
| `CONFIG_ETH_SPI_ETHERNET_KSZ8851SNL` | y（原已开） | — |

**`sdkconfig.defaults` 只对生成新配置生效，实际生效的是 `build/mesh-lite/sdkconfig`，两处都已同步修改。**

> 注意：`CONFIG_BRIDGE_EXTERNAL_NETIF_ETHERNET` 与 `CONFIG_BRIDGE_DATA_FORWARDING_NETIF_ETHERNET` 互斥；
> 与 `CONFIG_BRIDGE_EXTERNAL_NETIF_STATION`（WiFi）**可以同时开启**，二者是两个独立的 external netif。
>
> **默认路由的实际优先级（重要）**：`WIFI_STA_DEF` 的 `route_prio` 是 **100**
> （`esp_netif_defaults.h:54`），而 `ETH_WAN` 只有 **50**（`bridge_eth.c:403`）。
> 也就是说**自动选择会偏向 STA，以太网不会自动成为出口**。
> 「以太网优先」是靠 dual_net 显式调用 `esp_netif_set_default_netif(eth_netif)` 实现的：
> 该调用会置位 `s_is_last_default_esp_netif_overridden`，后续 STA 的 GOT_IP 事件
> 不会再把它抢回去（`esp_netif_lwip.c:316-322`）；以太网失效后该覆盖自动解除，回落到 STA。
> 因此 `DUAL_NET_ENABLE_ROUTE_CONTROL` 必须保持为 1，否则以太网拿不到出口。

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

## 6. 有线以太网接入（L1，已实现）

### 6.1 场景结论：WiFi 全断 + 网线可用，根节点身份能否保持？

**拓扑层能保持，功能层原本会失效、且系统会被重启；本轮改造后功能层与稳定性均已补上。**

| 层次 | 判定依据 | 改造前 | 改造后 |
|---|---|---|---|
| 配置层 | `IsRoot` 静态配置（`appTask.c:992`） | 保持 | 保持 |
| 拓扑层 | `esp_mesh_lite_set_allowed_level(1)`（`mesh.c:441`）+ SoftAP 独立于 STA | **保持 level 1**，子节点仍挂根下 | 保持 |
| 抢根 | 子节点 `set_disallowed_level(1)`（`mesh.c:443`） | 无人可抢（天然抑制） | 保持 |
| 功能层 | NAPT 出口 | **失效**（external 固定 STA） | 以太网接管为默认出口 |
| 稳定性 | `root_failover_task` | **会被 `esp_restart()`** | 加以太网守卫，不重启 |

### 6.2 根节点身份维持的判断逻辑（三层叠加）

1. **配置层（静态）**：`IsRoot` 来自 `baseconfig.json`，启动期一次性赋值（`appTask.c:992`），
   `mesh.c:440-444` 据此调用 `esp_mesh_lite_set_allowed_level(1)`（根）或 `set_disallowed_level(1)`（子节点）。
   **与上行介质完全无关**。语义经核实为「固定在该层级」，不是「层级上限」。
2. **拓扑层（半静态）**：`esp_mesh_lite_get_level() == ROOT(1)`。根节点 SoftAP 是子节点的父，
   不依赖 STA，故 STA 断开不会让子节点脱网。Mesh-Lite 内部**只以 WiFi STA 是否连上路由器来判断上行**
   （`User_Guide_CN.md:102`「根节点的上行连接只能是路由器」），**看不见以太网**。
3. **功能层（本轮补齐）**：能否带孩子上网、能否不被 failover 重启。

**结论**：不试图让闭源的 Mesh-Lite 认识以太网，而是在其之上加一层「上行裁决」——
dual_net 综合以太网与 STA 的**真实连通性**得出上行是否可用，据此决定默认路由、`Network_Flag`
以及是否抑制 failover 重启。

> 副作用提示：`User_Guide_CN.md:274` 指出，若除根节点外所有节点都被设定不允许 Level 1，
> 则根节点故障后**无法自动修复**。本工程正是这种配置，因此换来「无人抢根」的确定性，
> 代价是没有备份根。

### 6.3 影响因素（改造前为什么失败）

| # | 因素 | 位置 | 后果 | 处置 |
|---|---|---|---|---|
| 1 | `root_failover_task` 重启 | `mesh.c:205-260` | STA 断 30s 后扫描备用 SSID，扫到即 `esp_restart()`，插着网线也重启 | **已修**：加以太网守卫 |
| 2 | `Network_Flag` 只认 STA | `mesh.c:123/136`（原） | 以太网在线仍判为 0，触发因素 1 | **已修**：根节点改由 dual_net 裁决 |
| 3 | NAPT 出口固定 STA | `CONFIG_BRIDGE_EXTERNAL_NETIF_STATION=y`；NAPT 在 `bridge_wifi.c:246` 基于 SoftAP IP 开启；STA 的 `route_prio=100` 高于 ETH_WAN 的 50 | 子节点流量不走以太网，且以太网不会自动接管 | **已修**：以太网就绪后显式 `esp_netif_set_default_netif(eth_netif)` 覆盖默认路由 |
| 4 | DNS 下发被 STA 覆盖 | `bridge_wifi.c:232-241` 固定 `#if` 顺序，`WIFI_STA_DEF` 写在 `ETH_WAN` 之后并覆盖之 | 出口已是以太网，子节点仍拿 STA 的 DNS | **已修**：以太网生效后显式 `esp_bridge_update_dns_info()` |
| 5 | 假连通（有 IP 但服务器不通） | — | 误判上行可用 → 不回落 WiFi → 业务卡死 | **已修**：连通性探测 + 连续计数 |
| 6 | 网线热插拔抖动 | `ETHERNET_EVENT_*` | 频繁切路由、日志刷屏 | **已修**：上下沿分别去抖 3s / 1s |
| 7 | SPI 总线双初始化 | `bridge_eth.c:323` `ESP_ERROR_CHECK(spi_bus_initialize)` | 与旧 `dual_net_eth_init()` 争用 SPI2_HOST 会 abort | **已修**：删除 dual_net 的 SPI 初始化 |
| 8 | 硬件缺失 | — | 芯片不响应时 iot_bridge 只留下拿不到 IP 的 netif，不 abort | 已确认安全，降级纯 WiFi |

### 6.4 已实现的代码处理

| 处理 | 位置 |
|---|---|
| 接管 `ETH_WAN` netif（不再自建、不碰 SPI） | `dual_net.c:dual_net_eth_takeover()` |
| 上行裁决（以太网优先 + WiFi 兜底，含连通性探测与去抖） | `dual_net.c:dual_net_process_uplink_probe()` / `dual_net_process_link_debounce()` |
| `Network_Flag` 统一裁决入口 | `dual_net.c:dual_net_arbitrate_network_flag()` |
| 以太网生效后重下发 DNS；回落时改回 WiFi 侧 | `dual_net.c:dual_net_refresh_dns_to_softap()` / 探测失败分支 |
| `root_failover_task` 加以太网守卫 | `mesh.c:217` |
| `Network_Flag` 写入权按角色划分（根→dual_net，子节点→mesh.c） | `mesh.c:126-137`、`:149-158` |

**Network_Flag 不存在双写**：`mesh.c` 的两处写点位于 `IsRoot == 1` 的 `else` 分支（仅子节点执行）；
dual_net 的两处写点仅在根节点（dual_net 只在根节点初始化）执行。同一台设备运行期只有一个写者生效。

### 6.5 容错矩阵

| 异常 | 检测 | 处置 |
|---|---|---|
| 以太网硬件缺失/未焊接 | `esp_netif_get_handle_from_ifkey("ETH_WAN")` 为 NULL 或永不获得 IP | 降级纯 WiFi，日志记录，不 abort |
| SPI 总线冲突 | 已消除（dual_net 不再 `spi_bus_initialize`） | — |
| 网线反复插拔 | link up/down 去抖计时（3s / 1s） | 抖动期间不改路由、不改 `Network_Flag` |
| DHCP 失败/慢 | 未收到 `IP_EVENT_ETH_GOT_IP` | 保持 WiFi 上行 |
| 有 IP 但服务器不通 | 探测连续失败 `LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT` 次 | 取消以太网优先，回落 WiFi，DNS 改回 WiFi 侧 |
| 以太网中途断开 | `ETHERNET_EVENT_DISCONNECTED` / `IP_EVENT_ETH_LOST_IP` | 清 `s_eth_ready`，路由切回 STA，failover 恢复工作 |
| dual_net 初始化失败 | `s_dual_net_initialized == false` | 裁决入口退化为「STA 是否取得 IP」，根节点不会丢 `Network_Flag` |
| WiFi 仍在但网线不通 | 路由切换要求 `s_eth_ready \|\| !wifi_up` | 不把可用上行换成不可用上行 |
| 多机 MAC 冲突 | 见「已知限制」 | 暂未处理 |

### 6.6 已知限制：以太网 MAC 固定

iot_bridge 在 `bridge_eth.c:344` 为 SPI 以太网写死 MAC `02:00:00:12:34:56`。
`esp_eth_netif_glue_t` 是不透明结构、`esp_eth_netif_glue.h` 未提供获取 `esp_eth_handle_t` 的 API，
**应用层无法在不动 `managed_components/` 的前提下覆写硬件 MAC**
（本地编码规范也禁止访问 SDK handle 内部字段）。

影响：同一二层网络内若存在**多个根节点**（例如多个不同 `Mesh_ID` 的组网共用一台路由器），
会出现 MAC 冲突。规避方式：确保同一 L2 网络内只有一个根节点，或后续在独立 IDF worktree 中
修改 `bridge_eth.c` 改为按 WiFi STA MAC 派生。

### 6.7 事件映射

| 语义 | 来源 |
|---|---|
| 根节点取得上行 IP（无线） | `IP_EVENT_STA_GOT_IP` |
| 子节点取得根节点分配网段 | 同上（Mesh-Lite 侧另有 `ESP_MESH_LITE_EVENT_CORE_INHERITED_NET_SEGMENT_CHANGED`） |
| 根节点上行路由器变更 | `ESP_MESH_LITE_EVENT_CORE_ROUTER_INFO_CHANGED` |
| 网线物理链路通断 | `ETHERNET_EVENT_CONNECTED` / `DISCONNECTED` |
| 有线取得 / 丢失 IP | `IP_EVENT_ETH_GOT_IP` / `IP_EVENT_ETH_LOST_IP` |

### 6.8 已预留但未接入的 API

`dual_net_get_active_uplink()`、`dual_net_get_uplink_netif()`、`dual_net_select_uplink()`
（`ROUTE_CONTROL=0` 时返回 `ESP_ERR_NOT_SUPPORTED`）、`dual_net_register_uplink_change_cb()`。

## 7. 验证

### 7.1 已完成

- 全量编译通过，`bridge_eth.c` 已编入，`CONFIG_LWIP_IPV4_NAPT=y` 已确认。
- `-fsyntax-only` 验证三种门控组合均可编译：`ETH_HW_ENABLE=1`（当前）、`=0`（L0 回退）、
  `ROUTE_CONTROL=0`（L0 回退）。
- 静态核查：`dual_net.c` 已无任何 `spi_bus_initialize` / `SPI2_HOST` / `esp_eth_driver_install` 引用，
  SPI2_HOST 归 iot_bridge 独占。
- 静态核查：`Network_Flag` 在单台设备运行期只有一个写者生效（子节点走 `mesh.c`，根节点走 dual_net）。

### 7.2 实机验收（待执行）

**A. 基础回归（先做，确认没改坏）**
- 子节点串口**不出现** `=== Dual Network Init`，也无 `net_monitor` 任务创建；
- 根节点串口出现 `=== Dual Network Init (Ethernet Priority Mode, root only) ===`；
- 两种角色下子节点入网、MQTT 连接、老化上传行为与改造前一致。

**B. 以太网启动**
- 开机插网线：出现 `已接管以太网上行 netif: ETH_WAN` → `✓ Ethernet Got IP` →
  `以太网连通性已确认，上行切换为以太网` → `已将以太网 DNS 下发给数据转发 netif（含 SoftAP）`；
- 子节点仍能通过根节点上网（NAPT 出口已切到以太网）。

**C. 目标场景：WiFi 全断 + 网线可用**
1. 正常 WiFi 启动并接入网线，等待以太网就绪；
2. 关闭路由器（或让根节点 STA 断开），子节点仍挂在根节点 SoftAP 下；
3. 预期：
   - 出现 `以太网上行可用，暂停主备路由器切换（WiFi 侧无 IP 属正常）`；
   - **不应出现** `Restarting to verify uplink` / `Alternate SSID found` 等重启相关日志；
   - `Network_Flag` 保持 1（`Init_ByNetwork_Flag` 不重试 MQTT）；
   - `esp_mesh_lite_get_level()` 仍为 1，子节点不掉线；
   - MQTT / 老化上传持续正常。

**D. 回落与容错**
- 拔网线：约 3 次探测失败后取消以太网优先，路由切回 STA，DNS 改回 WiFi 侧，failover 恢复工作；
- 反复插拔：不应出现路由频繁切换或日志刷屏（去抖生效）；
- 以太网取得 IP 但网关/服务器不通：不应锁死在以太网，应回落 WiFi；
- 硬件未焊接：不应 abort，应降级纯 WiFi 并正常运行。
