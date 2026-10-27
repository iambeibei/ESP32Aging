---
name: btn-led-toggle-test
overview: 在按键处理中增加灯测试：每次按键按下翻转一次 IO17/IO18 电平，status_led 模块提供 toggle_both 接口，配套测试与文档更新。
todos:
  - id: add-toggle-api
    content: 在 status_led.h/c 新增 status_led_toggle_both 读电平取反接口
    status: completed
  - id: hook-button
    content: 在 button_task 按下检测处调用 status_led_toggle_both
    status: completed
    dependencies:
      - add-toggle-api
  - id: update-test
    content: tests/test_status_led.py 增加 gpio_get_level mock 与翻转用例
    status: completed
    dependencies:
      - add-toggle-api
  - id: update-doc
    content: STATUS_LED_DESIGN.md 补充翻转接口说明与实机验收行
    status: completed
    dependencies:
      - add-toggle-api
  - id: verify
    content: 用 [skill:embedded-coding-standard] 复核，运行测试、回归与编译验证
    status: completed
    dependencies:
      - hook-button
      - update-test
      - update-doc
---

## 用户需求
在按键测试任务中新增灯测试功能：每按一次按键（GPIO40，低电平有效），把 IO17 与 IO18 两路指示灯的输出电平各自取反（0→1、1→0），用于实机确认灯珠与接线是否正常。

## 产品概述
诊断测试功能。在现有 `button_task` 按键按下处理处接入一次电平翻转调用，翻转能力由 `status_led` 模块提供统一接口，与既有 `click_count` 堆/栈诊断分支并存。

## 核心功能
- 每次按键按下，IO17 与 IO18 各自取反一次（亮↔灭），与按键第几次无关
- 翻转接口收敛在 `status_led` 模块，不破坏 GPIO 封装边界
- 保留 `click_count` 四分支既有诊断行为（free heap、任务栈水位打印）
- 翻转结果不持久化；后续 `set_pn` / `start_aging` / 老化结束业务事件仍按业务规则覆盖灯态


## 技术栈
沿用现有工程：ESP32-S3、ESP-IDF v5.4.4、C、FreeRTOS 双核；改动集中在 `status_led` 模块与 `button_task` 一处调用点。

## 实现方案
### 核心决策
1. **翻转能力放 `status_led` 模块**，新增 `status_led_toggle_both()`：读回两脚输出寄存器后各自取反。理由：GPIO 电平语义（低=亮）集中在模块内，`button_task` 不直接操作引脚，与现有 `status_led_pn_ready` / `status_led_aging_start` / `status_led_all_off` 同层，保持封装一致。
2. **读输出寄存器而非 RAM 镜像**：`status_led.c` 不跟踪当前电平，ESP32 输出模式下 `gpio_get_level()` 读取的是输出锁存器，可安全用于反转；无需新增静态状态变量，消除并发写状态。
3. **每次按下翻转一次**：调用点放在 `button_task` 按下检测之后、`click_count` 分支之前，与点击计数无关，保证任何一次按下都触发翻转。
4. **翻转语义与 `status_led_output(gpio, on)` 对齐**：`on` 为 `true` 表示亮（低电平），因此传 `gpio_get_level(gpio) != 0`——当前高电平（灭）时传 `true` 转亮，当前低电平（亮）时传 `false` 转灭。

### 性能与可靠性
- 事件接口 O(1)，两次 `gpio_get_level` + 两次 `gpio_set_level`，无阻塞、无动态内存
- 任意任务上下文可调用（`gpio_get_level`/`gpio_set_level` 线程安全），非 ISR
- 爆炸半径：`status_led.c/h` 新增接口、`button_task` 一行调用、测试与文档；不触碰 PN/老化业务逻辑

## 实现要点
### 1. `main/APP/Status/status_led.h`
在 `status_led_all_off()` 之后新增声明：
```c
/** 两灯电平各自取反（按键诊断测试用）。任意任务上下文、无阻塞、非 ISR。 */
void status_led_toggle_both(void);
```

### 2. `main/APP/Status/status_led.c`
新增实现（放在 `status_led_all_off()` 之后）：
```c
void status_led_toggle_both(void)
{
    /* 低电平点亮：当前高（灭）→ 点亮，当前低（亮）→ 熄灭。 */
    status_led_output(STATUS_LED_AGING_GPIO, gpio_get_level(STATUS_LED_AGING_GPIO) != 0);
    status_led_output(STATUS_LED_PN_GPIO, gpio_get_level(STATUS_LED_PN_GPIO) != 0);
}
```
依赖 `driver/gpio.h`（已 include），`status_led_output()` 已封装返回值检查。

### 3. `main/Task/appTask.c` `button_task()`
在 `if (gpio_get_level(BUTTON_PIN) == 0) {` 之后、`click_count` 分支之前插入：
```c
            /* 灯测试：每按一次反转 IO17/IO18 电平，用于实机确认灯与接线。 */
            status_led_toggle_both();
```
`click_count` 四分支保持不变。

### 4. `tests/test_status_led.py`
mock 增加 `gpio_get_level`（返回 `g_level[gpio_num]`），新增用例：
- 上电双灭（17=1、18=1）→ toggle → 双亮（17=0、18=0）→ toggle → 双灭
- `status_led_pn_ready()` + `status_led_aging_start()`（双亮）→ toggle → 双灭 → toggle → 双亮
- `status_led_init()` 幂等断言不受 toggle 影响

### 5. `STATUS_LED_DESIGN.md`
- 接口清单增加 `status_led_toggle_both()`
- 边界约定补充：诊断用翻转，不持久化；后续业务事件会覆盖其结果
- 7.2 实机验收增加一行「连按按键，IO17/IO18 电平逐次反转」

## 验证方案
1. `python3 tests/test_status_led.py`，再 `ASAN_OPTIONS=detect_leaks=0 TEST_SANITIZE=1 python3 tests/test_status_led.py`
2. 既有回归：`test_root_uplink` / `test_mqtt_uplink` / `test_system_restart` / `test_http_protocol` / `test_psram_memory` 全部 exit=0
3. 编译：`cd /home/wbb/espidf/esp-idf-mesh-lite && . ./export.sh`，再执行
   `IDF_SKIP_CHECK_SUBMODULES=1 idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig build`，要求 exit=0
4. 实机：每按一次 GPIO40 按键，IO17 与 IO18 各自取反；`click_count` 堆/栈打印仍工作


## Agent Extensions

### Skill
- **embedded-coding-standard**
  - Purpose: 按 `.em_skill.json` 已配置的 CERT C + FreeRTOS + Local coding standard 复核新增 `status_led_toggle_both()` 与 `button_task` 调用点，重点确认 GPIO 读写返回值处理、线程安全声明与调用上下文注释合规
  - Expected outcome: 输出逐条审查结论，确认无阻塞调用、无未检查返回值、无悬空声明，给出可交付结论
