# ESP-Mesh-Lite 迁移与联调

本工程使用 ESP32-S3、ESP-IDF v5.4.4、`espressif/mesh_lite` 1.0.2 和
`espressif/iot_bridge` 1.0.1。依赖版本记录在 `main/idf_component.yml` 与
`dependencies.lock`。IoT-Bridge 构建时可能修改 ESP-IDF 的 DHCP/NAPT 源码，
因此要使用独立的 ESP-IDF v5.4.4 副本或 worktree，不要直接用日常开发的 IDF 目录构建。

## 构建和烧录

先初始化独立的 IDF v5.4.4 环境，再在本项目目录执行：

```bash
idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig build
idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig -p /dev/ttyUSB0 flash monitor
```

若在 `ESP_MESH_LITE_DEFAULT_INIT()` 处报 `CONFIG_MESH_LITE_VENDOR_ID_0`、
`CONFIG_MESH_LITE_ID` 或 `CONFIG_DEVICE_CATEGORY` 未定义，通常是当前编译配置未启用
Mesh-Lite。这些宏由组件的 Kconfig 生成；`sdkconfig.defaults` 只在生成新配置时生效，
不会覆盖另一台电脑上已有的 `sdkconfig`。先备份旧配置，再用上面的命令生成
`build/mesh-lite/sdkconfig`；若该路径已经有旧配置，也需先备份它。检查新配置中有
`CONFIG_BRIDGE_DATA_FORWARDING_NETIF_SOFTAP=y` 和 `CONFIG_MESH_LITE_ENABLE=y`。
如果命令行编译成功而 VS Code 仍提示相同错误，应让 ESP-IDF 扩展重新使用该构建目录
并刷新代码索引。

如果独立 IDF worktree 中的子模块源码是从原 IDF 复制而来，且缺少子模块 Git 元数据，
可在命令前加 `IDF_SKIP_CHECK_SUBMODULES=1`。这仅跳过元数据检查，子模块源码仍需齐全。
`data/baseconfig.json` 会打包进 SPIFFS；修改它后必须执行完整 `flash`，
仅用 `app-flash` 不会更新配置。根节点和子节点可使用同一个固件，分别配置 `IsRoot`。

## 节点配置

- `UDP_Port` 为历史网络使能开关：设为字符串 `"1"` 才启动 Mesh-Lite 和 MQTT。
- `IsRoot` 为 `"1"` 的节点固定在第 1 层；`"0"` 的节点禁止成为第 1 层。
- `Mesh_ID` 是恰好两位十六进制字符串，范围 `"01"` 至 `"FF"`，例如 `"66"`。
  同一网络所有节点使用相同 `Mesh_ID` 与 `Mesh_PS`；相邻网络使用不同 `Mesh_ID`。
  开启 `BRIDGE_SOFTAP_SSID_END_WITH_THE_MAC` 时，各节点的 SoftAP 名称带本机 MAC 后缀，
  例如 `LGMesh_66_a1b2c3`；关闭时统一为 `LGMesh_66`。两种模式都要求所有节点使用
  相同的编译选项，且 Mesh-Lite 初始化的 SSID 与实际 SoftAP 名称一致。启动时会核对实际 SSID。
  旧的 6 字节 MAC 格式会被拒绝，需要统一更新配置并重刷所有节点。
- 根节点用 `SSID`/`WIFI_PS` 连接主路由器，用 `SSID1`/`WIFI_PS1` 连接备用路由器。
  两者 SSID 必须不同，且两个网络都应能访问同一个 `SERVER_IP`/`SERVER_UDP_Port`。
- 备用路由器在连续 30 秒无本机 STA IP 后进入扫描；扫描到备用 SSID 后
  重启试连，取得 IP 才保存所选路由器并再次重启。若连接失败，最多试连两次，随后等待 5 分钟再尝试，
  避免持续重启。本地 Mesh-Lite 在上游离线时继续组网。

## 以太网上行状态（已启用，L1）

**当前状态：以太网已正式接入。** 硬件（KSZ8851SNL over SPI）由 iot_bridge 的 `bridge_eth.c` 初始化，
`dual_net` 只接管 `ETH_WAN` netif 并做上行裁决。固件**仍只在根节点**启用 dual_net。

关键行为：

- 以太网与 WiFi STA 同时作为 external netif；注意 STA 的 `route_prio` 为 100、ETH_WAN 为 50，
  自动选择偏向 STA，「以太网优先」由 dual_net 显式 `esp_netif_set_default_netif()` 覆盖实现；
- 以太网需通过连通性探测才被认定可用，避免「有 IP 却上不了网」；
- 以太网可用时 `root_failover_task` 暂停扫描与 `esp_restart()`，即插着网线不会因 WiFi 断而重启；
- `Network_Flag` 在根节点由 dual_net 裁决（以太网或 WiFi 任一可用），子节点仍由 `mesh.c` 维护；
- 硬件缺失不会 abort，降级为纯 WiFi。

引脚与 CONFIG 见 `DUAL_NET_DESIGN.md` 第 5.2 节；设计、容错矩阵与验收步骤见第 6、7 节。
已知限制：以太网 MAC 由 iot_bridge 写死为 `02:00:00:12:34:56`（见 6.6）。

---

## 以太网上行状态（L0 阶段的历史说明，已被上一节取代）

`main/Communication/Internet/dual_net/dual_net.c/h` 已剔除 `esp_mesh.h` 与
`esp_mesh_post_toDS_state()` 等 ESP-WIFI-MESH 遗留依赖，改造为 Mesh-Lite 兼容版本，
并已加入 `main/CMakeLists.txt` 参与编译。

**作用范围：仅根节点。** `appTask.c` 中 `mesh_init_Custom()` 成功之后，只有 `IsRoot == 1`
才调用 `dual_net_init()`；`dual_net_init()` 内部还有一道 `IsRoot != 1` 兜底判断。
子节点**零代码运行**——不注册事件处理器、不创建监控任务、不触碰 ETH。
（这与历史 ESP-WIFI-MESH 版本的 `if (mesh_layer == 1)` 判定一致。）

**默认行为：只观察、不改路由。** 三级能力门控全部默认关闭副作用
（`DUAL_NET_ETH_HW_ENABLE=0`、`DUAL_NET_ENABLE_ROUTE_CONTROL=0`、
`DUAL_NET_ENABLE_WIFI_FALLBACK=0`）。因此：

- 不初始化 KSZ8851SNL 硬件，不占用 SPI2_HOST 与 GPIO；
- 不调用 `esp_netif_set_default_netif()`，不破坏 IoT-Bridge 的 NAPT 转发链；
- 不主动 `esp_wifi_connect()`，不打断 Mesh-Lite 自组织的父节点选择；
- **不写 `Network_Flag`** —— 该标志的唯一写入者仍是 `mesh.c` 的 `network_event_handler()`。

当前固件仍只使用 Wi-Fi STA 上行，子节点入网与 MQTT 行为不变。
设计细节、配置项与后续有线接入改造清单见 `DUAL_NET_DESIGN.md`。

## 实机验收

1. 给一台根节点和两台子节点刷入新固件与同组配置，确认出现至少两层，
   三台都取得 STA IP，分别连接 MQTT 并收到各自订阅主题的消息。
2. 第二组设备使用不同 `Mesh_ID`，确认两组没有串网。
3. 关闭路由器，观察本地组网和 SQLite 缓存；恢复路由器，确认 MQTT 重连和缓存补发。
4. 关闭主路由器并打开备用路由器，确认根节点切换、子节点重连、
   MQTT 恢复，并检查只有一组 MQTT 接收任务。

上述无线和断网场景必须用实机验证；编译只能确认代码和依赖集成。
