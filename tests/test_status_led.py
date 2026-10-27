#!/usr/bin/env python3
"""Check status_led.c GPIO polarity, boot order and event-driven levels.

Run: python3 tests/test_status_led.py (requires cc).
Optional: ASAN_OPTIONS=detect_leaks=0 TEST_SANITIZE=1 python3 tests/test_status_led.py
This checks only the software GPIO levels (low = LED on); it does not model
ESP32 GPIO electrical behaviour.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

MOCK = r'''
#ifndef STATUS_LED_MOCK_H
#define STATUS_LED_MOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;

#define GPIO_MODE_OUTPUT 2
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0

typedef struct {
    uint32_t spinlock;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void mock_enter_critical(portMUX_TYPE *lock);
void mock_exit_critical(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) mock_enter_critical(lock)
#define portEXIT_CRITICAL(lock) mock_exit_critical(lock)

int gpio_config(const gpio_config_t *cfg);
int gpio_set_level(int gpio_num, int level);

void mock_log(const char *tag, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define ESP_LOGI mock_log
#define ESP_LOGE mock_log

const char *esp_err_to_name(esp_err_t err);

#endif
'''

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "mock.h"
#include "status_led.h"

#define PIN_AGING 17
#define PIN_PN 18
#define PIN_SPAN 64

static int g_level[PIN_SPAN];
static bool g_configured[PIN_SPAN];
static uint64_t g_config_mask;
static int g_config_mode;
static unsigned g_config_calls;
static unsigned g_set_before_config;
static int g_boot_levels[2];
static unsigned g_boot_level_count;

int gpio_config(const gpio_config_t *cfg)
{
    assert(cfg != NULL);
    ++g_config_calls;
    g_config_mask = cfg->pin_bit_mask;
    g_config_mode = cfg->mode;
    for (int pin = 0; pin < PIN_SPAN; ++pin) {
        if ((cfg->pin_bit_mask & (1ULL << pin)) != 0) {
            g_configured[pin] = true;
        }
    }
    return ESP_OK;
}

int gpio_set_level(int gpio_num, int level)
{
    assert(gpio_num >= 0 && gpio_num < PIN_SPAN);
    assert(level == 0 || level == 1);
    if (g_config_calls == 0) {
        ++g_set_before_config;
        if (g_boot_level_count < 2) {
            g_boot_levels[g_boot_level_count++] = level;
        }
    }
    g_level[gpio_num] = level;
    return ESP_OK;
}

void mock_enter_critical(portMUX_TYPE *lock)
{
    assert(lock != NULL);
}

void mock_exit_critical(portMUX_TYPE *lock)
{
    assert(lock != NULL);
}

void mock_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "mock_err";
}

int main(void)
{
    /* 上电防误亮：两次“灭”电平必须在 gpio_config 之前写入锁存器。 */
    assert(status_led_init() == ESP_OK);
    assert(g_set_before_config == 2);
    assert(g_boot_levels[0] == 1 && g_boot_levels[1] == 1);
    assert(g_config_calls == 1);
    assert(g_config_mask == ((1ULL << PIN_AGING) | (1ULL << PIN_PN)));
    assert(g_config_mode == GPIO_MODE_OUTPUT);
    assert(g_configured[PIN_AGING] && g_configured[PIN_PN]);
    assert(g_level[PIN_AGING] == 1 && g_level[PIN_PN] == 1);

    /* 重复 init 幂等：不重复配置 GPIO，也不改变灯态。 */
    assert(status_led_init() == ESP_OK);
    assert(g_config_calls == 1);
    assert(g_level[PIN_AGING] == 1 && g_level[PIN_PN] == 1);

    /* set_pn 成功：只点亮 IO18，IO17 不受影响。 */
    status_led_pn_ready();
    assert(g_level[PIN_PN] == 0);
    assert(g_level[PIN_AGING] == 1);
    status_led_pn_ready();
    assert(g_level[PIN_PN] == 0);

    /* 启动老化：点亮 IO17，IO18 保持点亮。 */
    status_led_aging_start();
    assert(g_level[PIN_AGING] == 0);
    assert(g_level[PIN_PN] == 0);
    status_led_aging_start();
    assert(g_level[PIN_AGING] == 0);

    /* 老化结束：两灯一起熄灭，重复调用幂等。 */
    status_led_all_off();
    assert(g_level[PIN_AGING] == 1 && g_level[PIN_PN] == 1);
    status_led_all_off();
    assert(g_level[PIN_AGING] == 1 && g_level[PIN_PN] == 1);

    /* 下一轮 set_pn：只点亮 IO18，IO17 保持熄灭。 */
    status_led_pn_ready();
    assert(g_level[PIN_PN] == 0 && g_level[PIN_AGING] == 1);

    /* 按键灯测试：双灯电平各自取反（当前 IO17 灭、IO18 亮）。 */
    status_led_toggle_both();
    assert(g_level[PIN_AGING] == 0 && g_level[PIN_PN] == 1);
    status_led_toggle_both();
    assert(g_level[PIN_AGING] == 1 && g_level[PIN_PN] == 0);

    /* 双亮后翻转到双灭，再翻转回双亮。 */
    status_led_pn_ready();
    status_led_aging_start();
    assert(g_level[PIN_AGING] == 0 && g_level[PIN_PN] == 0);
    status_led_toggle_both();
    assert(g_level[PIN_AGING] == 1 && g_level[PIN_PN] == 1);
    status_led_toggle_both();
    assert(g_level[PIN_AGING] == 0 && g_level[PIN_PN] == 0);

    /* 业务事件覆盖翻转结果；init 幂等不受 toggle 影响。 */
    status_led_all_off();
    assert(g_level[PIN_AGING] == 1 && g_level[PIN_PN] == 1);
    assert(status_led_init() == ESP_OK);
    assert(g_config_calls == 1);

    puts("status LED host checks passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="status-led-test-") as directory:
    tmp = Path(directory)
    (tmp / "driver").mkdir()
    (tmp / "freertos").mkdir()
    (tmp / "mock.h").write_text(MOCK)
    for name in ("driver/gpio.h", "freertos/FreeRTOS.h", "esp_log.h", "esp_err.h"):
        (tmp / name).write_text('#include "mock.h"\n')
    (tmp / "harness.c").write_text(HARNESS)

    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.getenv("TEST_SANITIZE") else []
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
        "-I", str(tmp), "-I", str(ROOT / "main/APP/Status"),
        str(ROOT / "main/APP/Status/status_led.c"), str(tmp / "harness.c"),
        "-o", str(tmp / "test_status_led"),
    ], check=True)
    subprocess.run([str(tmp / "test_status_led")], check=True)
