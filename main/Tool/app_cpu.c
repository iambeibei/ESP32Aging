#include "app_cpu.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS || !CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER || \
    !CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64 || CONFIG_FREERTOS_SMP
#error "CPU usage requires IDF FreeRTOS with 64-bit ESP Timer run-time statistics"
#endif

#define APP_CPU_USAGE_CORE_COUNT 2
#define APP_CPU_USAGE_SAMPLE_INTERVAL_US INT64_C(1000000)
#define APP_CPU_USAGE_PERCENT_SCALE 1000u

typedef struct {
    bool baseline_valid;
    bool sample_valid;
    int64_t sampled_at_us;
    uint64_t idle_us[APP_CPU_USAGE_CORE_COUNT];
    uint64_t window_us;
    uint16_t busy_per_mille[APP_CPU_USAGE_CORE_COUNT];
} CpuUsageState;

/* 按键任务单写单读；统计窗口不依赖按键间隔，长按期间也继续采样。
 * 这是基于 Idle 任务调度时间的估算，存在中断计时归属及上下文切换粒度误差。 */
static CpuUsageState s_cpu_usage;

void app_cpu_usage_poll(void)
{
    int64_t now_us = esp_timer_get_time();
    if (s_cpu_usage.baseline_valid && now_us >= s_cpu_usage.sampled_at_us &&
        now_us - s_cpu_usage.sampled_at_us < APP_CPU_USAGE_SAMPLE_INTERVAL_US) return;

    uint64_t idle_us[APP_CPU_USAGE_CORE_COUNT];
    for (unsigned core = 0; core < APP_CPU_USAGE_CORE_COUNT; ++core) {
        idle_us[core] = ulTaskGetIdleRunTimeCounterForCore((BaseType_t)core);
    }
    now_us = esp_timer_get_time();
    s_cpu_usage.sample_valid = s_cpu_usage.baseline_valid && now_us > s_cpu_usage.sampled_at_us;
    if (s_cpu_usage.sample_valid) {
        uint64_t window_us = (uint64_t)(now_us - s_cpu_usage.sampled_at_us);
        for (unsigned core = 0; core < APP_CPU_USAGE_CORE_COUNT; ++core) {
            if (idle_us[core] < s_cpu_usage.idle_us[core]) {
                s_cpu_usage.sample_valid = false;
                break;
            }
            uint64_t idle_delta_us = idle_us[core] - s_cpu_usage.idle_us[core];
            /* Idle 计数在切换时结算，边界误差可能略大于采样窗；将占用率约束到 0..100%。 */
            if (idle_delta_us > window_us) idle_delta_us = window_us;
            s_cpu_usage.busy_per_mille[core] = (uint16_t)(APP_CPU_USAGE_PERCENT_SCALE -
                idle_delta_us * APP_CPU_USAGE_PERCENT_SCALE / window_us);
        }
        s_cpu_usage.window_us = window_us;
    }
    for (unsigned core = 0; core < APP_CPU_USAGE_CORE_COUNT; ++core) {
        s_cpu_usage.idle_us[core] = idle_us[core];
    }
    s_cpu_usage.sampled_at_us = now_us;
    s_cpu_usage.baseline_valid = true;
}

void app_cpu_usage_log(const char *tag)
{
    app_cpu_usage_poll();
    if (tag == NULL) tag = "CPU";
    if (!s_cpu_usage.sample_valid) {
        ESP_LOGI(tag, "CPU usage: sampling, need a valid window >= 1000 ms");
        return;
    }
    unsigned cpu0 = s_cpu_usage.busy_per_mille[0];
    unsigned cpu1 = s_cpu_usage.busy_per_mille[1];
    unsigned average = (cpu0 + cpu1) / APP_CPU_USAGE_CORE_COUNT;
    int64_t age_us = esp_timer_get_time() - s_cpu_usage.sampled_at_us;
    ESP_LOGI(tag, "CPU usage estimate: CPU0=%u.%u%%, CPU1=%u.%u%%, average=%u.%u%% "
             "(window=%" PRIu64 " ms, age=%" PRId64 " ms)",
             cpu0 / 10, cpu0 % 10, cpu1 / 10, cpu1 % 10, average / 10, average % 10,
             s_cpu_usage.window_us / 1000u, age_us / 1000);
}
