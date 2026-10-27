---
name: simplify-status-led
overview: 将 status_led 从「轮询任务 + 快闪/慢闪状态机」简化为纯 GPIO 事件驱动：set_pn 成功点亮 IO18、启动老化成功点亮 IO17、老化结束（正常/异常/stop）双灯熄灭，并同步清理无用 API、测试与文档。
todos:
  - id: rewrite-status-led
    content: 重写 status_led.c/h 为纯 GPIO 事件接口，删除任务与快闪慢闪状态机
    status: completed
  - id: cleanup-hooks
    content: 删除 task_config.h 的 LED 任务常量并将 appTask.c 的 10 处钩子收敛为常亮/双灭
    status: completed
    dependencies:
      - rewrite-status-led
  - id: rewrite-test
    content: 重写 tests/test_status_led.py 为电平与上电顺序断言，移除任务 mock
    status: completed
    dependencies:
      - rewrite-status-led
  - id: update-docs
    content: 精简 STATUS_LED_DESIGN.md 并更新 AGENTS.md 索引描述为事件驱动
    status: completed
    dependencies:
      - rewrite-status-led
  - id: verify
    content: 用 [skill:embedded-coding-standard] 复核，执行残留 grep、编译与宿主回归
    status: completed
    dependencies:
      - cleanup-hooks
      - rewrite-test
      - update-docs
---

## 产品概述
将状态指示灯从「轮询任务 + 闪烁状态机」简化为纯 GPIO 事件驱动：`set_pn` 成功点亮 IO18，启动老化成功点亮 IO17，老化正常完成、异常退出或人工停止时两灯一起熄灭，其余状态不再处理。

## 核心功能
- `set_pn` 成功：IO18 拉低点亮，并在老化运行期间保持
- `start_aging` 成功（`agingStart` 回复点）：IO17 拉低点亮，此时两灯同亮
- 老化正常完成（`agingIdle`）、异常退出（`agingError`）、人工 `stop_aging`（`agingStop`）：两灯恢复高电平
- 掉电重启恢复：恢复 Ready PN 点亮 IO18，恢复运行中老化点亮 IO17；恢复失败双灭
- 删除 50 ms 轮询任务、快闪 3 次、1 Hz 慢闪、失败保持等全部状态机行为
- 重复/非法 `set_pn`、缺 PN 拒绝不改变灯态（保持现状）

## 各灯语义
| 事件 | IO18（GPIO18，低电平亮） | IO17（GPIO17，低电平亮） |
|---|---|---|
| `set_pn` 成功 | 点亮 | 不变 |
| `start_aging` 成功 | 保持点亮 | 点亮 |
| 恢复 Ready PN 成功 | 点亮 | 不变 |
| 恢复运行中老化成功 | 点亮 | 点亮 |
| 老化正常完成 / 异常退出 / stop | 熄灭 | 熄灭 |
| 上电默认、重复或非法 PN | 保持现状（默认熄灭） | 保持现状（默认熄灭） |

## PN 设置成功后的清除路径（本次已确认的分析结论）
- 老化启动成功消费 Ready PN（转当前 PN 后清零并写回 NVS）
- 老化正常完成、异常中止、人工 `stop_aging`
- 掉电重启且 `aging_valid=1` 恢复运行中老化时清 Ready PN
- 掉电重启时 NVS 无有效 Ready PN（标志为 0、读取失败或为空）
- 掉电恢复运行中老化失败
- 不消除：重复/非法 `set_pn`、老化启动初始化失败、缺 PN 拒绝、MQTT 重连、未掉电的一键重启


## 技术栈
- C（贴合工程风格）、ESP-IDF v5.4.4、FreeRTOS 双核、CMake
- 仅用 `driver/gpio.h` 直接输出，不新增任务、不使用 esp_timer/ledc/rmt

## 实现方案

### 核心决策
1. **删除轮询任务与状态机**：原 `status_led_task`、`portMUX` 共享状态、快闪/慢闪宏、任务句柄全部移除，`task_config.h` 的 `LG_STACK_STATUS_LED` / `LG_PRIO_STATUS_LED` 一并删除。理由：用户只要亮/灭两种稳态，任务与状态机是多余开销（每 50 ms 唤醒一次、2048 字节栈）。
2. **API 收敛为 4 个**：`status_led_init` / `status_led_pn_ready` / `status_led_aging_start` / `status_led_all_off`。删除 `status_led_pn_reject`、`status_led_pn_fail`、`status_led_pn_off`、`status_led_aging_stop`、`status_led_pn_mode` 及 `status_led_pn_mode_t` 枚举。
3. **事件点直接写 GPIO**：`gpio_set_level` 自身线程安全，调用方（mqtt_rx / aging_ctrl / net_init）均为任务上下文，无需临界区。
4. **保留上电防误亮顺序**：先向输出锁存器写高（灭）→ `gpio_config()` 配成输出 → 再锁存一次高电平，避免配置瞬间拉低点亮。
5. **保留返回值检查**：`status_led_output()` 封装 `gpio_set_level` 并 `ESP_LOGE`，遵循 CERT ERR 与本地规范。

### 行为注入点（`main/Task/appTask.c`）
| 位置 | 现状 | 调整 |
|---|---|---|
| `start_aging_with_ready_pn()` 的 `aging_state_publish("agingStart")` 后 | `status_led_aging_start()` | 保留 |
| `set_pn` 成功 `publish_pn_reply(...1...)` 后 | `status_led_pn_ready()` | 保留 |
| `set_pn` RUNNING / 已有 Ready PN / 空 / 超长 四个拒绝分支 | 4 处 `status_led_pn_reject()` | 全部删除 |
| `stop_aging` 分支 `agingStop` 后 | `aging_stop()` + `pn_off()` | 改 `status_led_all_off()` |
| `Aging_Task_Abort_And_Delete()` | `aging_stop()` + `pn_fail()` | 改 `status_led_all_off()` |
| `case AgingComplete` | `aging_stop()` + `pn_off()` | 改 `status_led_all_off()` |
| 恢复 Ready PN 成功 / 无 Ready PN | `pn_ready()` / `pn_off()` | 保留 / 改 `status_led_all_off()` |
| 恢复运行中老化成功 | `aging_start()` + `pn_ready()` | 保留 |
| 恢复失败两个分支 | `aging_stop()` + `pn_off()` | 改 `status_led_all_off()` |

### 性能与可靠性
- 事件接口 O(1)，无动态内存、无阻塞、无定时器；删除任务后省 2048 字节栈与任务控制块
- 灯态与 MQTT 状态上报在同一代码点完成，顺序为「先上报、后写灯」与原实现一致
- 爆炸半径限定在 `status_led.*`、`task_config.h` 两行、`appTask.c` 钩子行、测试与文档；不触碰 PN/老化业务逻辑

## 目录结构
```
main/
├── APP/Status/
│   ├── status_led.h      # [MODIFY] 仅保留 2 个引脚宏 + 4 个接口；删除枚举与 5 个旧接口
│   └── status_led.c      # [MODIFY] 重写为纯 GPIO 封装：init + pn_ready + aging_start + all_off
├── Task/
│   ├── appTask.c         # [MODIFY] 删 4 处 reject，6 处 stop/off 组合改 all_off，其余保留
│   └── task_config.h     # [MODIFY] 删除 LG_STACK_STATUS_LED / LG_PRIO_STATUS_LED 整块
tests/
└── test_status_led.py    # [MODIFY] 重写为纯电平断言，移除任务/Delay/longjmp mock
STATUS_LED_DESIGN.md      # [MODIFY] 精简为行为表、注入点、上电恢复与验收表
AGENTS.md                 # [MODIFY] 文档索引描述改为事件驱动（去掉快闪/慢闪/时序参数）
```

## 关键接口
```c
#define STATUS_LED_AGING_GPIO 17   /* 低电平点亮 */
#define STATUS_LED_PN_GPIO    18   /* 低电平点亮 */

esp_err_t status_led_init(void);   /* 配置两路输出，默认熄灭，幂等 */
void status_led_pn_ready(void);    /* IO18 拉低点亮，幂等 */
void status_led_aging_start(void); /* IO17 拉低点亮，幂等 */
void status_led_all_off(void);     /* 两灯恢复高电平，幂等 */
```

## 验证方案
1. 残留检查：`grep -rn "status_led_pn_reject\|status_led_pn_fail\|status_led_aging_stop\|status_led_pn_off\|status_led_pn_mode\|LG_STACK_STATUS_LED\|LG_PRIO_STATUS_LED\|status_led_task" main/ tests/` 无匹配
2. 宿主测试：`python3 tests/test_status_led.py`，覆盖上电电平顺序、位掩码、点亮/双灭、幂等；`TEST_SANITIZE=1` 再跑一遍
3. 回归：`test_root_uplink` / `test_mqtt_uplink` / `test_system_restart` / `test_http_protocol` / `test_psram_memory` 全部 exit=0
4. 编译：`cd /home/wbb/espidf/esp-idf-mesh-lite && . ./export.sh`，再执行 `IDF_SKIP_CHECK_SUBMODULES=1 idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig build`，要求 exit=0
5. 实机：`set_pn` 亮 IO18；`start_aging` 亮 IO17 且 IO18 仍亮；正常完成/异常/stop 双灭；运行中掉电重启恢复后两灯均亮


## Agent Extensions
### Skill
- **embedded-coding-standard**
  - Purpose: 改动完成后按工程 `.em_skill.json` 已配置的 CERT C + FreeRTOS + Local coding standard 复核 `status_led.c/h` 与 `appTask.c` 钩子，重点确认无残留符号引用、无悬空声明、无任务/临界区残留、GPIO 初始化顺序与返回值处理合规
  - Expected outcome: 输出逐条审查结论，确认无残留 API 引用、无阻塞调用，并给出可交付结论后再执行编译与宿主回归
