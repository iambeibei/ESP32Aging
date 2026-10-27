---
name: fix-led-toggle-readback
overview: 修复按键翻转灯无效：GPIO_MODE_OUTPUT 下 gpio_get_level 恒为 0 导致 toggle 总写灭，改为模块内电平镜像 + 短临界区实现翻转。
todos:
  - id: rewrite-led-mirror
    content: 重写 status_led.c 为电平镜像加短临界区，拆分 gpio_write 与 output
    status: completed
  - id: update-headers-doc
    content: 更新 status_led.h 注释与 STATUS_LED_DESIGN.md 的镜像与临界区说明
    status: completed
    dependencies:
      - rewrite-led-mirror
  - id: clean-test-mock
    content: 删除测试中 gpio_get_level 的 mock 声明与实现并保留翻转用例
    status: completed
    dependencies:
      - rewrite-led-mirror
  - id: review-build
    content: 用 [skill:embedded-coding-standard] 复核，运行宿主测试、回归与完整编译
    status: completed
    dependencies:
      - update-headers-doc
      - clean-test-mock
---

## 问题现象
按键诊断已接入 `status_led_toggle_both()`，按下 GPIO40 时 `show_free_heap` 与任务栈打印正常，但 IO17/IO18 指示灯电平无任何变化。

## 根因
`status_led_toggle_both()` 依赖 `gpio_get_level()` 回读当前电平：

```c
status_led_output(STATUS_LED_PN_GPIO, gpio_get_level(STATUS_LED_PN_GPIO) != 0);
```

- `gpio_get_level()` 读的是输入寄存器 `GPIO_IN`；
- `status_led_init()` 使用 `GPIO_MODE_OUTPUT`，`gpio_config()` 会关闭该引脚输入使能；
- 输入使能关闭后回读恒为 0，`0 != 0` 为假，于是每次 toggle 都写高电平（灭），初始也是灭，表现为“灯完全不动”；
- 宿主机测试未暴露该问题，因为 mock 直接返回 `g_level[]`，模拟了本不存在的回读能力。

## 修复目标
- 每次按下按键，IO17 与 IO18 各自取反一次，不依赖 GPIO 回读，不受 LED 接法/输入阈值影响；
- 业务事件（`set_pn` 成功、`start_aging` 成功、老化结束双灭、掉电恢复）行为保持不变；
- 保持纯事件驱动：不新增任务、不使用定时器；
- 头文件对外接口不变，只调整内部实现与注释。

## 保持不变
- 低电平点亮（0=亮，1=灭）；
- `status_led_init` 上电防误亮顺序（先写高、再配方向、再锁存）；
- `button_task` 每次按下调用一次 toggle，`click_count` 既有诊断分支不动。


## 技术方案

### 核心思路
在 `status_led.c` 内维护两灯电平镜像，所有写灯路径统一更新镜像；`toggle_both()` 在短临界区内对镜像取反后写 GPIO，彻底去掉 `gpio_get_level()` 依赖。

```c
static portMUX_TYPE s_led_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_aging_on;   /* true = 点亮（输出低） */
static bool s_pn_on;

/* 仅写 GPIO 并检查返回值，不触碰镜像 */
static void status_led_gpio_write(int gpio_num, bool on);

/* 写 GPIO + 更新镜像：init 与业务事件统一入口 */
static void status_led_output(int gpio_num, bool on)
{
    status_led_gpio_write(gpio_num, on);
    portENTER_CRITICAL(&s_led_lock);
    if (gpio_num == STATUS_LED_AGING_GPIO) {
        s_aging_on = on;
    } else if (gpio_num == STATUS_LED_PN_GPIO) {
        s_pn_on = on;
    }
    portEXIT_CRITICAL(&s_led_lock);
}

void status_led_toggle_both(void)
{
    bool aging_on;
    bool pn_on;
    portENTER_CRITICAL(&s_led_lock);
    s_aging_on = !s_aging_on;
    s_pn_on = !s_pn_on;
    aging_on = s_aging_on;
    pn_on = s_pn_on;
    portEXIT_CRITICAL(&s_led_lock);
    status_led_gpio_write(STATUS_LED_AGING_GPIO, aging_on);
    status_led_gpio_write(STATUS_LED_PN_GPIO, pn_on);
}
```

### 关键设计决策
1. **镜像而非回读**：`GPIO_MODE_OUTPUT` 下 `gpio_get_level()` 不可用，镜像方案与具体 LED 接法（灌电流/拉电流、是否经三极管）无关，行为确定。
2. **临界区只保护镜像**：`portENTER_CRITICAL` 内仅做 bool 赋值/取反，`gpio_set_level` 与 `ESP_LOGE` 放在临界区外，满足 FreeRTOS 临界区短小、不调用可能阻塞 API 的约束。
3. **镜像随业务事件同步**：`pn_ready`/`aging_start`/`all_off` 均经 `status_led_output()`，因此 toggle 始终基于最新实际电平“各自取反”，不会被并发业务写入带偏到同一极性。
4. **允许诊断场景的轻微竞态**：toggle 与 mqtt_rx/aging_ctrl 并发时，最终电平取决于执行顺序，但每次写入都是合法电平，无未定义行为；下一业务事件会纠正，与现有文档约定一致。
5. **保留 `GPIO_MODE_OUTPUT`**：不改为 `GPIO_MODE_INPUT_OUTPUT`，不需要输入使能，避免外部电平阈值影响。

### 文件改动
```
main/APP/Status/status_led.c   # [MODIFY] 增加 freertos/FreeRTOS.h；新增镜像与锁；
                               #          拆分 status_led_gpio_write/status_led_output；
                               #          toggle 改为镜像取反；文件头注释去掉“回读”描述
main/APP/Status/status_led.h   # [MODIFY] toggle 注释补充“基于模块内电平镜像，不依赖 GPIO 回读”
tests/test_status_led.py       # [MODIFY] 删除 gpio_get_level 的 mock 声明与实现（实现不再调用）；
                               #          翻转用例断言保持不变
STATUS_LED_DESIGN.md           # [MODIFY] 第 2 节“无临界区”改为“短临界区仅保护电平镜像”；
                               #          第 6 节第 7 条补充镜像/不回读说明
```

### 实现要点
- 镜像初值 `false/false`（上电灭）；`status_led_init()` 中两次 `status_led_output(..., false)` 会把镜像写成 false，与 GPIO 一致。
- `status_led_gpio_write()` 保留 `gpio_set_level` 返回值检查与 `ESP_LOGE`（CERT ERR）。
- `s_led_initialized` 仍只在启动阶段写一次，无需同步。
- 删除 `status_led_toggle_both()` 中“gpio_get_level 读取输出寄存器”的错误注释。

### 验证方案
1. `python3 tests/test_status_led.py` 与 `ASAN_OPTIONS=detect_leaks=0 TEST_SANITIZE=1 python3 tests/test_status_led.py` 通过（翻转用例覆盖“IO17 灭/IO18 亮”与“双亮/双灭”两种起点）。
2. 既有回归 `test_root_uplink` / `test_mqtt_uplink` / `test_system_restart` / `test_http_protocol` / `test_psram_memory` 全部 exit=0。
3. `cd /home/wbb/espidf/esp-idf-mesh-lite && . ./export.sh` 后执行
   `IDF_SKIP_CHECK_SUBMODULES=1 idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig build`，要求 exit=0。
4. 实机：`idf.py -p /dev/ttyUSB0 app-flash`（必须是 app-flash，只 build 不烧录不会生效），连按 GPIO40，IO17/IO18 应逐次翻转；随后 `set_pn` 点亮 IO18、`start_aging` 点亮 IO17、老化结束双灭仍正常。
5. 若烧录后仍无变化，需核对 LED 接线极性（是否存在反相驱动），该情况不在代码修复范围。
  

## Agent Extensions
### Skill
- **embedded-coding-standard**
  - Purpose: 按工程 `.em_skill.json` 已配置的 CERT C + FreeRTOS + Local coding standard 复核新增的电平镜像与临界区改动，重点确认临界区内只访问共享 bool、无阻塞 API、无未检查返回值、注释与调用上下文声明合规
  - Expected outcome: 输出逐条审查结论（临界区长度、并发语义、返回值处理、命名与注释），确认可交付后再执行编译与宿主回归
