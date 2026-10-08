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

如果独立 IDF worktree 中的子模块源码是从原 IDF 复制而来，且缺少子模块 Git 元数据，
可在命令前加 `IDF_SKIP_CHECK_SUBMODULES=1`。这仅跳过元数据检查，子模块源码仍需齐全。
`data/baseconfig.json` 会打包进 SPIFFS；修改它后必须执行完整 `flash`，
仅用 `app-flash` 不会更新配置。根节点和子节点可使用同一个固件，分别配置 `IsRoot`。

## 节点配置

- `UDP_Port` 为历史网络使能开关：设为字符串 `"1"` 才启动 Mesh-Lite 和 MQTT。
- `IsRoot` 为 `"1"` 的节点固定在第 1 层；`"0"` 的节点禁止成为第 1 层。
- `Mesh_ID` 是恰好两位十六进制字符串，范围 `"01"` 至 `"FF"`，例如 `"66"`。
  同一网络所有节点使用相同 `Mesh_ID` 与 `Mesh_PS`；相邻网络使用不同 `Mesh_ID`。
  旧的 6 字节 MAC 格式会被拒绝，需要统一更新配置并重刷所有节点。
- 根节点用 `SSID`/`WIFI_PS` 连接主路由器，用 `SSID1`/`WIFI_PS1` 连接备用路由器。
  两者 SSID 必须不同，且两个网络都应能访问同一个 `SERVER_IP`/`SERVER_UDP_Port`。
- 备用路由器在连续 30 秒无本机 STA IP 后进入扫描；扫描到备用 SSID 后
  重启试连，取得 IP 才保存所选路由器并再次重启。若连接失败，最多试连两次，随后等待 5 分钟再尝试，
  避免持续重启。本地 Mesh-Lite 在上游离线时继续组网。

## 以太网上行状态

`main/Communication/Internet/dual_net/dual_net.c/h` 保留了原有 KSZ8851SNL
硬件初始化和网卡切换代码，当前不参与编译。旧实现中的 Ethernet 初始化与启动原本就被注释，
且含 `esp_mesh_post_toDS_state()` 等 ESP-WIFI-MESH API，不能直接加入 Mesh-Lite 构建。
当前固件只使用 Wi-Fi STA 上行，`Network_Flag` 也只跟踪 STA IP。
IoT-Bridge 1.0.1 提供 Ethernet 作为外网接口的配置；后续启用时需要改造
Ethernet 驱动初始化、桥接接口选择、ETH IP 事件和根节点路由切换，并在硬件上验证。

## 实机验收

1. 给一台根节点和两台子节点刷入新固件与同组配置，确认出现至少两层，
   三台都取得 STA IP，分别连接 MQTT 并收到各自订阅主题的消息。
2. 第二组设备使用不同 `Mesh_ID`，确认两组没有串网。
3. 关闭路由器，观察本地组网和 SQLite 缓存；恢复路由器，确认 MQTT 重连和缓存补发。
4. 关闭主路由器并打开备用路由器，确认根节点切换、子节点重连、
   MQTT 恢复，并检查只有一组 MQTT 接收任务。

上述无线和断网场景必须用实机验证；编译只能确认代码和依赖集成。
