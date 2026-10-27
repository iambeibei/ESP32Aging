# 状态指示灯设计（IO18 PN 灯 / IO17 老化运行灯）

## 1. 需求与结论

两路低电平点亮的单色指示灯，**纯事件驱动**：不创建任务、不使用软件定时器、不做快闪/慢闪。
只在业务状态迁移点直接写 GPIO。

| 引脚 | 语义 | 灯亮（低） | 灯灭（高） |
|---|---|---|---|
| IO18 | PN 灯 | `set_pn` 成功 / 恢复 Ready PN | 老化结束 / 上电默认 |
| IO17 | 老化运行灯 | 老化已启动（`agingStart` 已回复）/ 恢复运行中老化 | 老化结束 / 上电默认 |

GPIO17/18 与现有 UART（43/44）、RS485/CAN（1/2）、以太网 SPI（6/15/7/16/4/5）、按键（40）
均不冲突。

## 2. 引脚与模块

- 驱动模块：`main/APP/Status/status_led.c` / `.h`
- 接口 5 个：`status_led_init()`、`status_led_pn_ready()`、`status_led_aging_start()`、
  `status_led_all_off()`、`status_led_toggle_both()`（按键诊断：两灯电平各自取反）
- 无任务、无动态内存；翻转与写镜像使用极短临界区（只保护两灯电平镜像，不调用 SDK/日志），
  `gpio_set_level()` 本身线程安全，事件接口幂等、无阻塞、非 ISR
- 初始化：`app_task_init()` 在 `app_uart_init()` 之前调用 `status_led_init()`，失败只写日志不阻断启动
- 编译：`main/CMakeLists.txt` 的 `SRCS`/`INCLUDE_DIRS` 已包含 `APP/Status`

## 3. 行为规则

| 事件 | IO18 | IO17 |
|---|---|---|
| `set_pn` 成功（`publish_pn_reply(…,1,…)` 之后） | 点亮 | 不变 |
| `start_aging` 成功（`aging_state_publish("agingStart")` 之后） | 保持点亮 | 点亮 |
| 掉电恢复 Ready PN 成功 | 点亮 | 不变 |
| 掉电恢复运行中老化成功（`aging_valid=1`） | 点亮 | 点亮 |
| 老化正常完成（`agingIdle`） | 熄灭 | 熄灭 |
| 老化异常退出（`agingError` → `Aging_Task_Abort_And_Delete`） | 熄灭 | 熄灭 |
| 人工 `stop_aging`（`agingStop`） | 熄灭 | 熄灭 |
| 掉电恢复失败 / 无有效 Ready PN | 熄灭 | 熄灭 |
| 重复或非法 `set_pn`、`start_aging` 缺 PN 被拒 | 不变 | 不变 |
| 上电默认 | 熄灭 | 熄灭 |

老化运行期间 IO18 保持点亮（PN 仍在用），因此「老化结束双灭」有明确的可见变化。

## 4. 代码注入点（`main/Task/appTask.c`）

| 位置 | 调用 |
|---|---|
| `app_task_init()`，`app_uart_init()` 之前 | `status_led_init()` |
| `start_aging_with_ready_pn()`，`aging_state_publish("agingStart")` 之后 | `status_led_aging_start()` |
| `set_pn` 成功，`publish_pn_reply(…,1,…)` 之后 | `status_led_pn_ready()` |
| `stop_aging` 分支，`aging_state_publish("agingStop")` 之后 | `status_led_all_off()` |
| `Aging_Task_Abort_And_Delete()` | `status_led_all_off()` |
| `case AgingComplete`，`aging_state_publish("agingIdle")` 之后 | `status_led_all_off()` |
| `Init_ByNetwork_Flag`：恢复 Ready PN 成功 / 失败 | `status_led_pn_ready()` / `status_led_all_off()` |
| `Init_ByNetwork_Flag`：恢复运行中老化成功 / 失败 | `status_led_aging_start()` + `status_led_pn_ready()` / `status_led_all_off()` |

## 5. 上电与掉电恢复

1. `status_led_init()` 先向输出锁存器写高（灭），再 `gpio_config()` 切输出方向，最后再次锁存
   高电平，避免配置瞬间误亮。
2. 子节点 `Init_ByNetwork_Flag` 恢复 Ready PN 成功 → IO18 点亮；无有效 Ready PN → 双灭。
3. `aging_valid == 1` 且 `aging_command_json_parse()` 成功 → IO17 与 IO18 同时点亮
   （当前 PN 仍在使用）。
4. 恢复失败（Aging.json / 当前 PN 缺失或解析失败）→ 双灭。
5. 根节点按既有逻辑跳过 Ready PN / 运行中老化恢复，其上电为双灯熄灭。
6. 灯态不写 NVS：重启后由上述恢复路径重新决定。

## 6. 边界约定

| # | 约定 |
|---|---|
| 1 | 两灯均低电平点亮（0=亮，1=灭） |
| 2 | 事件接口幂等、无阻塞、非 ISR；可从 mqtt_rx / aging_ctrl / net_init 调用 |
| 3 | 灯态与 MQTT 状态上报在同一代码点，顺序为「先上报、后写灯」 |
| 4 | 重复/非法 `set_pn` 与缺 PN 拒绝**不改变**灯态（不再有闪烁提示） |
| 5 | 老化运行期间不清 IO18；只有老化结束才双灭 |
| 6 | 灯态只存 RAM，不新增 NVS 字段 |
| 7 | `status_led_toggle_both()`（`button_task` 每次按键调用）基于模块内电平镜像取反，不依赖 `gpio_get_level()` 回读；仅作灯/接线诊断，不持久化，后续业务事件会覆盖其结果 |

## 7. 测试与验收

### 7.1 宿主机回归

```bash
python3 tests/test_status_led.py
ASAN_OPTIONS=detect_leaks=0 TEST_SANITIZE=1 python3 tests/test_status_led.py
```

覆盖：上电两次「灭」电平早于 `gpio_config`、引脚掩码与输出模式、`init` 幂等、
`pn_ready` 只点亮 IO18、`aging_start` 点亮 IO17 且 IO18 保持、`all_off` 双灭与幂等、
下一轮 `set_pn` 不误点亮 IO17。

### 7.2 实机验收

| # | 操作 | 期望 |
|---|---|---|
| 1 | 上电 | 双灯熄灭（无闪亮） |
| 2 | 下发 `set_pn` | IO18 点亮，IO17 保持熄灭 |
| 3 | 下发 `start_aging` | IO17 点亮，IO18 保持点亮 |
| 4 | 老化正常跑完 | 两灯同时熄灭，`agingIdle` |
| 5 | 制造 `agingError`（如拔掉老化设备） | 两灯同时熄灭 |
| 6 | 运行中 `stop_aging` | 两灯同时熄灭，`agingStop` |
| 7 | 老化运行中掉电重启 | 恢复成功后 IO17 与 IO18 同时点亮 |
| 8 | 仅 `set_pn` 后掉电重启 | 恢复后 IO18 点亮，IO17 熄灭 |
| 9 | 运行中重复 `set_pn` | 回 Code=0，灯态不变 |
| 10 | 连按按键（GPIO40） | IO17/IO18 电平逐次反转；原有 `click_count` 堆/栈打印仍工作 |
