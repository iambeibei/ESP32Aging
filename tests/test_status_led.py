#!/usr/bin/env python3
"""Check status_led.c GPIO polarity, boot order, fast/slow blink and fallback.

Run: python3 tests/test_status_led.py (requires cc).
Optional: ASAN_OPTIONS=detect_leaks=0 TEST_SANITIZE=1 python3 tests/test_status_led.py
This drives status_led_task one tick per vTaskDelay() call via setjmp/longjmp; it
checks the LED state machine, not ESP32 GPIO electrical behaviour.
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
#define ESP_ERR_NO_MEM 0x101

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdPASS 1
#define pdFAIL 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#define LG_APP_CPU_CORE 1
#define LG_STACK_STATUS_LED 2048
#define LG_PRIO_STATUS_LED 1

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
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                   void *arg, UBaseType_t prio, TaskHandle_t *handle,
                                   BaseType_t core);
void vTaskDelay(TickType_t ticks);

void mock_log(const char *tag, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define ESP_LOGI mock_log
#define ESP_LOGE mock_log
#define ESP_LOGW mock_log

const char *esp_err_to_name(esp_err_t err);

#endif
'''

HARNESS = r'''
#include <assert.h>
#include <setjmp.h>
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
static unsigned g_config_calls;
static unsigned g_set_before_config;
static int g_boot_levels[2];
static unsigned g_boot_level_count;

static TaskFunction_t g_task;
static jmp_buf g_task_exit;
static bool g_task_should_exit;
static TickType_t g_last_delay_ticks;
static unsigned g_delay_calls;

#if MOCK_TASK_CREATE_FAIL
static BaseType_t g_create_result = pdFAIL;
#else
static BaseType_t g_create_result = pdPASS;
#endif

void mock_enter_critical(portMUX_TYPE *lock)
{
    assert(lock != NULL);
}

void mock_exit_critical(portMUX_TYPE *lock)
{
    assert(lock != NULL);
}

int gpio_config(const gpio_config_t *cfg)
{
    assert(cfg != NULL);
    ++g_config_calls;
    g_config_mask = cfg->pin_bit_mask;
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

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                   void *arg, UBaseType_t prio, TaskHandle_t *handle,
                                   BaseType_t core)
{
    assert(fn != NULL && name != NULL && arg == NULL && handle == NULL);
    assert(stack == LG_STACK_STATUS_LED && prio == LG_PRIO_STATUS_LED);
    assert(core == LG_APP_CPU_CORE);
    if (g_create_result != pdPASS) {
        return g_create_result;
    }
    g_task = fn;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    ++g_delay_calls;
    g_last_delay_ticks = ticks;
    if (g_task_should_exit) {
        longjmp(g_task_exit, 1);
    }
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

#if !MOCK_TASK_CREATE_FAIL
/* 每次进入任务函数执行一轮循环体，vTaskDelay 处 longjmp 退出，等效一个 50 ms tick。 */
static void run_ticks(unsigned count)
{
    assert(g_task != NULL);
    for (unsigned i = 0; i < count; ++i) {
        g_task_should_exit = true;
        if (setjmp(g_task_exit) == 0) {
            g_task(NULL);
            assert(0 && "status_led_task must not return");
        }
        g_task_should_exit = false;
    }
}

static void expect_pn_level(unsigned ticks, int level)
{
    for (unsigned i = 0; i < ticks; ++i) {
        run_ticks(1);
        assert(g_level[PIN_PN] == level);
    }
}

static void expect_fast_blink(void)
{
    /* 快闪：亮 200 ms / 灭 200 ms 为一次，共 3 次（每 4 个 tick 翻转）。 */
    expect_pn_level(4, 0);
    expect_pn_level(4, 1);
    expect_pn_level(4, 0);
    expect_pn_level(4, 1);
    expect_pn_level(4, 0);
    expect_pn_level(4, 1);
}

static void test_status_led(void)
{
    assert(status_led_init() == ESP_OK);

    /* 上电防误亮：两次“灭”电平必须在 gpio_config 之前写入锁存器。 */
    assert(g_set_before_config == 2);
    assert(g_boot_levels[0] == 1 && g_boot_levels[1] == 1);
    assert(g_config_calls == 1);
    assert(g_config_mask == ((1ULL << PIN_AGING) | (1ULL << PIN_PN)));
    assert(g_configured[PIN_AGING] && g_configured[PIN_PN]);
    assert(g_level[PIN_AGING] == 1 && g_level[PIN_PN] == 1);
    assert(status_led_pn_mode() == STATUS_LED_PN_OFF);

    /* 重复 init 幂等，不再配置 GPIO。 */
    assert(status_led_init() == ESP_OK);
    assert(g_config_calls == 1);

    /* IO17：低电平点亮，幂等。 */
    status_led_aging_start();
    assert(g_level[PIN_AGING] == 0);
    status_led_aging_start();
    assert(g_level[PIN_AGING] == 0);
    status_led_aging_stop();
    assert(g_level[PIN_AGING] == 1);
    status_led_aging_stop();
    assert(g_level[PIN_AGING] == 1);

    /* IO18 常亮：最多一个 tick 生效。 */
    status_led_pn_ready();
    assert(status_led_pn_mode() == STATUS_LED_PN_ON);
    run_ticks(1);
    assert(g_level[PIN_PN] == 0);
    assert(g_last_delay_ticks == pdMS_TO_TICKS(50));

    /* 常亮时重复 set_pn：快闪 3 次后回到常亮。 */
    status_led_pn_reject();
    expect_fast_blink();
    run_ticks(1);
    assert(g_level[PIN_PN] == 0);
    assert(status_led_pn_mode() == STATUS_LED_PN_ON);

    /* 无 PN 时重复 set_pn：快闪 3 次后回到熄灭。 */
    status_led_pn_off();
    run_ticks(1);
    assert(g_level[PIN_PN] == 1);
    status_led_pn_reject();
    expect_fast_blink();
    run_ticks(1);
    assert(g_level[PIN_PN] == 1);
    assert(status_led_pn_mode() == STATUS_LED_PN_OFF);

    /* 老化失败：1 Hz 慢闪（亮 500 ms / 灭 500 ms）。 */
    status_led_pn_fail();
    assert(status_led_pn_mode() == STATUS_LED_PN_FAIL);
    expect_pn_level(10, 0);
    expect_pn_level(10, 1);
    expect_pn_level(10, 0);

    /* 慢闪期间重复 set_pn：快闪后回到慢闪（相位重新从亮半周开始）。 */
    status_led_pn_reject();
    expect_fast_blink();
    run_ticks(1);
    assert(g_level[PIN_PN] == 1); /* 快闪结束 tick 输出灭，慢闪下一 tick 起亮 */
    expect_pn_level(1, 0);

    /* set_pn 成功清除失败指示。 */
    status_led_pn_ready();
    run_ticks(1);
    assert(g_level[PIN_PN] == 0);
    expect_pn_level(5, 0);

    /* 老化失败后再次失败：保持慢闪；off 立即熄灭。 */
    status_led_pn_fail();
    run_ticks(1);
    assert(g_level[PIN_PN] == 0);
    status_led_pn_off();
    run_ticks(1);
    assert(g_level[PIN_PN] == 1);

    puts("status LED host checks passed");
}
#else
static void test_status_led_fallback(void)
{
    /* 任务创建失败：GPIO 仍可用作稳态指示，无闪烁能力。 */
    assert(status_led_init() == ESP_ERR_NO_MEM);
    assert(g_task == NULL);
    assert(g_level[PIN_AGING] == 1 && g_level[PIN_PN] == 1);

    status_led_pn_ready();
    assert(g_level[PIN_PN] == 0);
    status_led_pn_off();
    assert(g_level[PIN_PN] == 1);

    /* 老化失败降级为熄灭，避免误报“PN 就绪”。 */
    status_led_pn_fail();
    assert(g_level[PIN_PN] == 1);

    /* 重复 set_pn 不改变稳态电平（无任务可闪烁）。 */
    status_led_pn_ready();
    assert(g_level[PIN_PN] == 0);
    status_led_pn_reject();
    assert(g_level[PIN_PN] == 0);

    status_led_aging_start();
    assert(g_level[PIN_AGING] == 0);
    status_led_aging_stop();
    assert(g_level[PIN_AGING] == 1);
    assert(g_delay_calls == 0);

    puts("status LED fallback host checks passed");
}
#endif

int main(void)
{
#if MOCK_TASK_CREATE_FAIL
    test_status_led_fallback();
#else
    test_status_led();
#endif
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="status-led-test-") as directory:
    tmp = Path(directory)
    (tmp / "freertos").mkdir()
    (tmp / "driver").mkdir()
    (tmp / "mock.h").write_text(MOCK)
    for name in (
        "freertos/FreeRTOS.h",
        "freertos/task.h",
        "driver/gpio.h",
        "esp_log.h",
        "esp_err.h",
        "task_config.h",
    ):
        (tmp / name).write_text('#include "mock.h"\n')
    (tmp / "harness.c").write_text(HARNESS)

    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.getenv("TEST_SANITIZE") else []
    sources = [str(ROOT / "main/APP/Status/status_led.c"), str(tmp / "harness.c")]
    includes = ["-I", str(tmp), "-I", str(ROOT / "main/APP/Status")]

    for fail_mode, binary in ((0, "test_status_led"), (1, "test_status_led_fallback")):
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
            f"-DMOCK_TASK_CREATE_FAIL={fail_mode}",
            *includes, *sources, "-o", str(tmp / binary),
        ], check=True)
        subprocess.run([str(tmp / binary)], check=True)
