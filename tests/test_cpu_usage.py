#!/usr/bin/env python3
"""Check app_cpu.c sampling and percentages with host timer/Idle counter mocks.

Run: python3 tests/test_cpu_usage.py (requires cc).
Optional: ASAN_OPTIONS=detect_leaks=0 TEST_SANITIZE=1 python3 tests/test_cpu_usage.py
This checks arithmetic and sampling, not ESP32 interrupt accounting accuracy.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

MOCK = r'''
#ifndef CPU_USAGE_MOCK_H
#define CPU_USAGE_MOCK_H
#include <stdint.h>
#define CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS 1
#define CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER 1
#define CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64 1
#define CONFIG_FREERTOS_SMP 0
typedef int BaseType_t;
uint64_t ulTaskGetIdleRunTimeCounterForCore(BaseType_t core);
int64_t esp_timer_get_time(void);
void mock_log(const char *tag, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define ESP_LOGI mock_log
#endif
'''

HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "mock.h"
#include "app_cpu.h"

/* Start after 32-bit microsecond counters would have wrapped. */
static int64_t now_us = INT64_C(10000000000);
static uint64_t idle_us[2] = {UINT64_C(5000000000), UINT64_C(7000000000)};
static unsigned counter_queries;
static char line[512];
static const char *last_tag;

uint64_t ulTaskGetIdleRunTimeCounterForCore(BaseType_t core)
{
    assert(core >= 0 && core < 2);
    ++counter_queries;
    return idle_us[core];
}

int64_t esp_timer_get_time(void) { return now_us; }

void mock_log(const char *tag, const char *format, ...)
{
    last_tag = tag;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    assert(n >= 0 && (size_t)n < sizeof(line));
}

static void expect(const char *text) { assert(strstr(line, text) != NULL); }

int main(void)
{
    app_cpu_usage_log(NULL);
    expect("sampling");
    assert(!strcmp(last_tag, "CPU") && counter_queries == 2);
    now_us += 500000;
    app_cpu_usage_poll();
    app_cpu_usage_log("test");
    expect("sampling");
    assert(counter_queries == 2); /* No counter reads before one second. */

    now_us += 500000;
    idle_us[0] += 750000;
    idle_us[1] += 250000;
    app_cpu_usage_log("test");
    expect("CPU0=25.0%, CPU1=75.0%, average=50.0%");
    expect("window=1000 ms, age=0 ms");
    assert(counter_queries == 4 && !strcmp(last_tag, "test"));

    now_us += 250000;
    app_cpu_usage_log("test");
    expect("age=250 ms");
    assert(counter_queries == 4);

    /* Poll without printing: a later press reports the latest window. */
    now_us += 750000;
    idle_us[0] += 1000000;
    app_cpu_usage_poll();
    now_us += 1000000;
    idle_us[0] += 666000;
    idle_us[1] += 333000;
    app_cpu_usage_log("test");
    expect("CPU0=33.4%, CPU1=66.7%, average=50.0%");
    expect("window=1000 ms");

    /* A delayed low-priority sampler uses actual elapsed time. */
    now_us += 10000000;
    idle_us[0] += 2500000;
    idle_us[1] += 7500000;
    app_cpu_usage_log("test");
    expect("CPU0=75.0%, CPU1=25.0%, average=50.0%");
    expect("window=10000 ms");

    /* Clamp scheduling-boundary Idle overcount rather than underflow. */
    now_us += 1000000;
    idle_us[0] += 1000010;
    app_cpu_usage_log("test");
    expect("CPU0=0.0%, CPU1=100.0%, average=50.0%");

    /* Invalid counters/time discard the sample and establish a new baseline. */
    now_us += 1000000;
    idle_us[1] = 0;
    app_cpu_usage_log("test");
    expect("sampling");
    now_us += 1000000;
    app_cpu_usage_log("test");
    expect("CPU0=100.0%, CPU1=100.0%, average=100.0%");
    now_us -= 10000000;
    app_cpu_usage_log("test");
    expect("sampling");
    now_us += 1000000;
    idle_us[0] += 1000000;
    idle_us[1] += 1000000;
    app_cpu_usage_log("test");
    expect("CPU0=0.0%, CPU1=0.0%, average=0.0%");
    puts("CPU usage host checks passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="cpu-usage-test-") as directory:
    tmp = Path(directory)
    (tmp / "freertos").mkdir()
    (tmp / "mock.h").write_text(MOCK)
    for name in ("freertos/FreeRTOS.h", "freertos/task.h", "esp_log.h", "esp_timer.h"):
        (tmp / name).write_text('#include "mock.h"\n')
    (tmp / "harness.c").write_text(HARNESS)
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.getenv("TEST_SANITIZE") else []
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
        "-I", str(tmp), "-I", str(ROOT / "main/Tool"),
        str(ROOT / "main/Tool/app_cpu.c"), str(tmp / "harness.c"),
        "-o", str(tmp / "test_cpu_usage"),
    ], check=True)
    subprocess.run([str(tmp / "test_cpu_usage")], check=True)
