# Mesh-Lite BINARY 分发验证

当前实现支持父节点保持旧固件运行时提供备用槽中的新镜像，也支持意外重启运行新固件后
恢复当前槽供数。已有根/子日志确认第一条链路中 BINARY 从根向第二层传输，真实文件长度
2118656 字节、SHA 一致；第三层转发及本次新增的掉电恢复仍待实机验收。宿主检查与构建
不能替代闭源 Mesh-Lite 1.0.2 的无线互操作验证。

关联：`MESH_LITE_MIGRATION.md`、`OTA_SERVER_MIGRATION_DESIGN.md`、
`CHILD_NODE_ROOT_LOSS_DESIGN.md`。本文件的当前策略替代原“完成即重启”策略。

## 1. 本阶段行为

```text
服务器 ──MQTT OTA 命令──> mqtt_rx ──通知──> cmd_task
                                              ├─ 根：pause → dl_task HTTP 下载一次
                                              │       → 本次下载结果快照
                                              └─ 子：update.json + 文件长度探测
                                                      → transmit_file_start(BINARY)
                                                      → 乱序写备用 OTA 槽
                                                      → 镜像/版本校验
                                          本地校验成功 + 库传输完成
                                              → cmd_task 分块计算原 BIN SHA-256
                                              → 提交 NVS 供数记录及原完成事件身份
                                              → 设置下一启动分区
                                              → 登记 32 字节补零文件名
                                              → 恢复下级 OTA 请求
                                              → OTA_BINARY_READY
                                              → MQTT event/OTACP 完成上报（QoS1）
                                              → 保持旧固件运行，等待外部重启
```

- 根与子都不自动重启。`CONFIG_OTA_AUTO_RESTART` 必须关闭，误开时编译报错。
- 就绪后发送 OTACP；外部 CanRestart 现由统一服务执行：先联网等待 30s，再暂停 Mesh，
  按 `30+(层数-1)*30` 秒根先深层后重启，详见 `NETWORK_RESTART_DESIGN.md`。
- `esp_ota_set_boot_partition()` 在镜像校验且 NVS 提交成功后执行；后续掉电或其他重启会启动新固件。
- 子节点仅从服务器获取元数据和 HTTP 响应头；固件正文只走 Mesh。禁用外网兜底。
- 上位机按层级逐台/逐组下发，可根据各设备 OTACP 推进下一层；串口 READY 用于交叉核对。
- 目标版本不更新时可以恢复已有供数或绑定当前槽；READY 表示可供数，不等于新一次下载完成。

## 2. 文件身份、回调与就绪事实

`update.json` 保持 `version` / `file` 两字段。局域网请求直接使用 `file`，最多 31 字节，
超限明确失败，禁止截断、自动改名或改用 FIRMWARE 模式。测试期间必须固定该 JSON 与 BIN。

`ota.c` 新增 `simple_ota_download_result_t` / `simple_ota_get_download_result()`，返回本次 HTTP
实际下载成功的元数据、真实字节数与分区。查询不访问网络，版本跳过/失败无有效结果。
状态与结果快照使用短临界区同步。

`mesh_ota.c` 的单份供数记录来自本次实际下载校验、有效持久记录恢复或当前运行镜像核对，
不扫描各 app 分区头匹配版本，也不固定选“另一个槽”。
子接收端将目标文件名与回调的 `fw_version` 字段匹配（该字段在 BINARY 模式作为文件标识）；
是否与闭源库实际回调一致属于实机验证项，标识不符明确拒绝。

- `get_file_cb`：按 offset 写入，检查会话有效、文件标识、对齐长度和分区边界。
- `get_file_done_cb`：`esp_ota_end` 校验镜像，再核验内嵌版本；持久化及 `set_boot` 在服务任务。
  end 成功或失败均消耗 handle，之后不得再次 abort。
- 完成事件：只记录库终态并唤醒 cmd_task。必须同时有本地校验成功事实和库成功终态，
  才进入登记流程；事件早到可等待 done 最多 10 秒，不能凭事件直接变成就绪。
- `provide_file_cb`：只允许精确匹配已就绪文件名。真实长度以内读该分区，真实长度之外的
  合法 64 KiB 对齐尾部一律填 `0xFF`，禁止泄露残留 flash。非法范围拒绝。
- `esp_mesh_lite_lan_ota_set_file_name()` 在当前库固定复制 32 字节，使用长期有效且补零的
  `char[32]`；只有镜像有效后才登记。`notify_child_node_restart()` 是恢复 OTA 请求，
  不是 `esp_restart()`。

供数读取和失败清理使用同一静态互斥锁，防止并发清理/擦写；SHA 每次读 2048 字节并让出 CPU1。
SHA 对应真实 BIN 的全部字节，排除 LAN 对齐填充，可与服务器 `sha256sum` 直接比较。
启动校验与收尾期间不允许启动新擦写；新一轮升级撤销供数前须等待在途读取结束。

## 3. 持久化与启动恢复

`ota_cache.c` 在独立 NVS 命名空间 `ota_cache` 中保存单个 `image_v1` blob，固定格式 v1、
256 字节，不包含指针。记录文件名、目标版本、真实 BIN 长度、分区 label/address/size/subtype、
全文件 SHA-256、来源、原完成时间 Seq、DeviceId、AgingNumber、结构长度与 CRC32。
更新和失效均检查提交结果，只操作本模块 key，绝不清空整个 NVS。

```text
启动 → 读取记录 → 校验格式/CRC/分区表/完整镜像/内嵌版本/真实 BIN SHA
                 ├─ 记录目标版本 > 运行版本：恢复暂存镜像，设置启动，PENDING_RESTART
                 ├─ 记录目标版本 = 运行版本：恢复供数，IDLE，允许下一版本升级
                 └─ 旧记录/不匹配/损坏：不供数、不补报，失效自己的记录
```

本地恢复无需访问服务器；供数仍使用同一文件名严格匹配及 FF 尾部规则。
`OTA_CACHE_RESTORED source=running|staged file=...` 区分当前运行槽与暂存槽。
普通 NVS 读取错误不会触发清空或覆盖未知记录。NVS 提交失败时不设置新启动分区、不报 READY，
保留已经下载好的候选镜像有限重试收尾，不重新下载。

擦写前若目标分区与记录重叠，必须先删除该记录并提交成功；失败阻止擦写。下次升级写入另一槽
时可保留当前镜像的记录，直至新镜像完整就绪并提交替换；恢复时只提供对应文件，不用旧镜像
冒充新文件。两个槽随运行分区轮换使用，SQLite 和配置分区不变。

首次直接烧录或旧固件没有记录时，等待 MQTT 联网后最多尝试 3 次、间隔 90 秒：获取服务器
元数据及响应头，只有版本严格等于运行版本、文件名合法、SDK 镜像校验成功且解析长度等于
服务器真实长度，才计算 SHA 并绑定当前运行槽。不会下载 BIN 正文，也不会生成 OTACP。
这要求**每个版本对应唯一、发布后不可替换的 BIN**；元数据没有服务器 SHA，因此版本和长度
核对无法证明同版本不同构建内容相同。无法核实时拒绝绑定；后续 OTA 命令可再次尝试。
带额外尾部/签名等导致解析长度不等于文件长度的镜像也拒绝自动绑定，不猜测真实长度。

本功能需先安装到接收升级的节点，才能为后续下载生成记录。历史无记录的备用槽不能仅靠
镜像头恢复文件名、真实长度及原完成事件，因而不会自动登记。

## 4. 重试与诊断

- 保留最多 2 次自动重试，每次等待 90 秒，首试加重试最多 3 次。
- 失败会话停止接收、abort 未消耗的 handle 并清理就绪事实；重试重新开始接收，复用已取得的
  子节点元数据/长度。新一轮人工命令重新获取元数据。
- 持久化/启动选择/登记失败只重试收尾，不重新下载、不擦写。重试耗尽保留候选，新命令可重试收尾。
- 重复命令不会重开正在运行/等待重试的会话，待重启镜像不会再次擦写；运行目标镜像的节点
  恢复供数后处于 IDLE，可接受更高版本。
- 根下载超过 10 分钟记录告警，等待既有 worker 退出才允许下一次尝试，避免旧 HTTP 任务仍写
  flash 时并发启动新下载；服务任务保持可运行。这不是 HTTP 任务强制取消功能。
- 根/子权限依据 `IsRoot`，子失去父节点而层级变为 0 也不能进入根的外网下载路径。

重点日志：

```text
OTA_BINARY_REQUEST level=2 file=... target=... bytes=...
OTA_BINARY_PROVIDE level=1 request=... partition=ota_1 bytes=...
OTA_BINARY_READY level=2 running=A target=B file=... partition=ota_1 bytes=... sha256=...
```

仅 `OTA_BINARY_READY` 表示本地校验、SHA、文件登记和恢复下级均完成。READY 前仍可能已设置
新启动分区，因此 pending 状态不能作为网络供数就绪的替代判断。

每个根/子节点实际下载并就绪后，经本机主题 `device/<DEVICE_ID>/event/OTACP` 上报：

```json
{"Seq":1782373476,"Data":{"IsComplete":1,"AgingNumber":"2"}}
```

`Seq` 沿用项目事件约定，取完成时的 Unix 秒时间戳，示例数值不写死；`AgingNumber` 来自
下载完成时的本机配置且保持字符串类型。首次绑定、下载/校验失败、尚未登记就绪均不产生新完成消息。
通知由 OTA 服务任务发布，未连接或 MQTT 入队被拒时每 5 秒重试，复用同一 Seq/负载。
本次运行中 SDK 接受一次后交由 QoS1 重传；入队接受不等于上位机业务确认，不追踪 SDK
接受后的过期删除。NVS 下载记录保留原 Seq、设备和老化架身份，每次启动校验恢复后补报一次
同一事件，避免掉电丢失。上位机按 DeviceId + Seq 去重；配置变更不会改变旧事件归属。
补报未入队前的新升级命令合并等待，避免覆盖唯一记录中的待报事件。

## 5. 检查与实机验收

应用侧宿主回归使用真实 `ota.c`、`ota_cache.c`、`mesh_ota.c` 和 cJSON，模拟 SDK/HTTP/flash/NVS：

```bash
python3 tests/test_mesh_ota_binary.py
ASAN_OPTIONS=detect_leaks=0 TEST_SANITIZE=1 python3 tests/test_mesh_ota_binary.py
```

依赖宿主 C 编译器、OpenSSL 开发库。38 组场景覆盖原 BINARY/OTACP 以及提交前后断电、
新固件运行后离线恢复、下一版本轮换、CRC/镜像/SHA 损坏、NVS 失败和供数与擦写互斥，
普通及 ASan/UBSan 运行通过。SDK 镜像解析和 NVS 使用宿主替身，不证明真实 flash 掉电行为，
也不模拟闭源库或无线传输。构建按 `AGENTS.md` 中指定的 ESP-IDF 命令执行。

三层实机顺序：

1. 三台节点都先安装含本实现的旧版本 A，再在服务器准备更高三段版本 B。不能只把 B 放服务器，
   而让接收命令的节点仍运行旧的 FIRMWARE/自动重启实现。
2. 测试期间冻结 `update.json` 与 BIN，记录文件字节数和 `sha256sum`；不发送 CanRestart。
3. 只升级根。等待 READY 与该设备的 OTACP，核对 running=A、target=B、长度/散列正确，无重启。
4. 给第二层 OTA 命令，核对根出现 PROVIDE；第二层 READY 后仍 running=A，长度/散列一致。
5. 确认第三层实际连接第二层后发送命令。必须看到第二层 PROVIDE；仅看到根供数不能认定
   “第二层不重启也可供第三层”通过。三台运行版本/启动计数均不应变化。
6. 再覆盖父节点未就绪、错误/超长文件名、中断、校验失败、重复命令和两子并发请求；不得误报
   READY、提供半成品或自动重启。若库仍拒绝/不调用回调，保留两端日志，停止扩展该路线。

恢复实机检查：

1. 根和第二层暂存 B 后分别重启，确认运行 B，出现 `source=running`，没有 HTTP 正文下载；
   旧子节点仍能获取 B，原 OTACP 使用原设备、AgingNumber 和 Seq 补报。
2. 在 NVS 提交前、提交后启动选择前、启动选择后登记前分别断电，核对无未校验供数、无错选槽、
   无假完成；提交前可能需要重新下载，已提交记录应恢复。
3. 双槽分别作为运行槽执行恢复，再从 B 升级 C；下载 C 中断后重启不能把 B 作为 C 提供。
4. 验证 NVS 空间不足、损坏记录、错误长度/SHA、恢复时无网络、并发子节点读取与新 OTA；
   NVS 故障不应导致清空其他命名空间，未通过校验不报 READY 或 OTACP。

实机进度：**已有根→第二层 BINARY 日志验证；第三层及本次恢复检查待执行**。
本阶段不宣称 21 节点升级已通过。

## 6. 历史 FIRMWARE 模式限制（保留依据）

此前仓库记录的 1 根 1 子测试，在父节点仍运行旧固件时观察到：

```text
The currently running firmware version is 0.0.0, the firmware version
requested by the child node is 0.0.1
Current running version is not same as the OTA firmware or different
categories. We will not continue the update.
```

原 FIRMWARE 路线据此采用“根先重启供数、子完成后重启”。该记录证明旧模式的限制，
不能用作 BINARY 模式成功依据；本次去除适配层的自动重启路径，单独验证 BINARY。
