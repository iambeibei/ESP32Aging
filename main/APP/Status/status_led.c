/**
 * @file status_led.c
 * @brief 状态指示灯驱动：IO18 显示 PN 状态，IO17 显示老化运行状态，均低电平点亮。
 *
 * 设计要点（STATUS_LED_DESIGN.md）：
 * 1. 事件接口只更新稳态与快闪请求，GPIO 时序全部由 50 ms 轮询任务输出；
 * 2. IO17 为纯电平，事件点直接写，保证 agingStart 回复后立即点亮；
 * 3. 事件接口可被 mqtt_rx / aging_ctrl / net_init 并发调用，临界区只做赋值；
 * 4. 不新增 NVS 字段：失败指示保存在 RAM，重启后由恢复路径重新决定灯态。
 */
#include "status_led.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "task_config.h"

#define TAG "status_led"

/* 两灯均为低电平点亮。 */
#define STATUS_LED_LEVEL_ON 0
#define STATUS_LED_LEVEL_OFF 1

/* IO18 轮询周期：50 ms 足以分辨 200 ms 快闪与 1 Hz 慢闪，任务开销可忽略。 */
#define STATUS_LED_TICK_MS 50u

/* 快闪：亮 200 ms / 灭 200 ms 为一次，共 3 次。 */
#define STATUS_LED_FAST_HALF_MS 200u
#define STATUS_LED_FAST_COUNT 3u

/* 慢闪：1 Hz（亮 500 ms / 灭 500 ms）。 */
#define STATUS_LED_SLOW_PERIOD_MS 1000u
#define STATUS_LED_SLOW_ON_MS 500u

/* 事件侧与轮询任务共享：PN 稳态 + 快闪请求序号。临界区内只做赋值/自增。 */
static portMUX_TYPE s_led_lock = portMUX_INITIALIZER_UNLOCKED;
static status_led_pn_mode_t s_pn_mode = STATUS_LED_PN_OFF;
static uint32_t s_pn_reject_seq;

/*
 * 仅 status_led_init() 写一次，之后只读；事件接口据此选择任务驱动或降级直写。
 * 写入发生在轮询任务创建之前，读取发生在 init 返回之后（net_init/mqtt_rx 任务），
 * 任务创建本身提供 happens-before，因此无需额外同步。
 */
static bool s_led_task_running;

/*
 * 以下状态只由 status_led_task 访问。定义为文件级而非任务局部变量，使轮询逻辑
 * 可在宿主机测试中逐 tick 驱动（任务只有唯一实例，语义等价）。
 */
static uint32_t s_reject_seq_seen;
static uint32_t s_fast_elapsed_ms;
static uint32_t s_slow_elapsed_ms;
static bool s_fast_running;
static bool s_pn_output_on;

static void status_led_output(int gpio_num, bool on)
{
    esp_err_t err = gpio_set_level(gpio_num, on ? STATUS_LED_LEVEL_ON : STATUS_LED_LEVEL_OFF);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "gpio_set_level(%d) failed: %s", gpio_num, esp_err_to_name(err));
    }
}

/*
 * 轮询任务创建失败时的降级：常亮/熄灭直接写电平，快闪与慢闪无法表现。
 * 老化失败（FAIL）降级为熄灭，避免误导为“PN 就绪”。
 */
static void status_led_pn_apply_steady(status_led_pn_mode_t mode)
{
    if (s_led_task_running)
    {
        return;
    }
    status_led_output(STATUS_LED_PN_GPIO, mode == STATUS_LED_PN_ON);
}

/* IO18 时序状态机：快闪优先，结束后回到稳态；慢闪相位在模式切换时清零。 */
static void status_led_task(void *arg)
{
    (void)arg;

    while (1)
    {
        status_led_pn_mode_t mode;
        uint32_t reject_seq;

        portENTER_CRITICAL(&s_led_lock);
        mode = s_pn_mode;
        reject_seq = s_pn_reject_seq;
        portEXIT_CRITICAL(&s_led_lock);

        if (reject_seq != s_reject_seq_seen)
        {
            /* 新的重复 set_pn：覆盖仍在进行的上一轮快闪，从头再闪 3 次。 */
            s_reject_seq_seen = reject_seq;
            s_fast_running = true;
            s_fast_elapsed_ms = 0;
        }

        bool target_on;
        if (s_fast_running)
        {
            const uint32_t fast_total_ms = STATUS_LED_FAST_HALF_MS * 2u * STATUS_LED_FAST_COUNT;
            if (s_fast_elapsed_ms >= fast_total_ms)
            {
                s_fast_running = false;
                s_fast_elapsed_ms = 0;
                s_slow_elapsed_ms = 0;
                target_on = (mode == STATUS_LED_PN_ON);
            }
            else
            {
                target_on = ((s_fast_elapsed_ms / STATUS_LED_FAST_HALF_MS) % 2u) == 0;
                s_fast_elapsed_ms += STATUS_LED_TICK_MS;
            }
        }
        else if (mode == STATUS_LED_PN_ON)
        {
            target_on = true;
            s_slow_elapsed_ms = 0;
        }
        else if (mode == STATUS_LED_PN_FAIL)
        {
            target_on = (s_slow_elapsed_ms % STATUS_LED_SLOW_PERIOD_MS) < STATUS_LED_SLOW_ON_MS;
            s_slow_elapsed_ms = (s_slow_elapsed_ms + STATUS_LED_TICK_MS) % STATUS_LED_SLOW_PERIOD_MS;
        }
        else
        {
            target_on = false;
            s_slow_elapsed_ms = 0;
        }

        if (target_on != s_pn_output_on)
        {
            s_pn_output_on = target_on;
            status_led_output(STATUS_LED_PN_GPIO, s_pn_output_on);
        }

        vTaskDelay(pdMS_TO_TICKS(STATUS_LED_TICK_MS));
    }
}

esp_err_t status_led_init(void)
{
    if (s_led_task_running)
    {
        return ESP_OK;
    }

    /*
     * 先向输出锁存器写“灭”，再切换为输出方向：GPIO 复位后输出寄存器为 0，
     * 若先配置方向会瞬间输出低电平导致两灯误亮一拍。
     */
    esp_err_t err = gpio_set_level(STATUS_LED_AGING_GPIO, STATUS_LED_LEVEL_OFF);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "preset aging LED failed: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_set_level(STATUS_LED_PN_GPIO, STATUS_LED_LEVEL_OFF);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "preset PN LED failed: %s", esp_err_to_name(err));
        return err;
    }

    const gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << STATUS_LED_AGING_GPIO) | (1ULL << STATUS_LED_PN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io_conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 配置完成后再锁存一次高电平，兼容驱动内部的方向/电平切换顺序差异。 */
    status_led_output(STATUS_LED_AGING_GPIO, false);
    status_led_output(STATUS_LED_PN_GPIO, false);

    /* 复位状态机：首次 init 才走到这里，保证起始灯态与事件序号确定。 */
    portENTER_CRITICAL(&s_led_lock);
    s_pn_mode = STATUS_LED_PN_OFF;
    s_pn_reject_seq = 0;
    portEXIT_CRITICAL(&s_led_lock);

    s_reject_seq_seen = 0;
    s_fast_elapsed_ms = 0;
    s_slow_elapsed_ms = 0;
    s_fast_running = false;
    s_pn_output_on = false;

    BaseType_t task_ret = xTaskCreatePinnedToCore(status_led_task,
                                                 "status_led",
                                                 LG_STACK_STATUS_LED,
                                                 NULL,
                                                 LG_PRIO_STATUS_LED,
                                                 NULL,
                                                 LG_APP_CPU_CORE);
    if (task_ret != pdPASS)
    {
        /* GPIO 已配置：IO17 与 IO18 稳态指示仍可用，仅失去闪烁能力。 */
        ESP_LOGE(TAG, "status_led task create failed; steady output only");
        return ESP_ERR_NO_MEM;
    }

    s_led_task_running = true;
    ESP_LOGI(TAG, "Status LED ready: aging=GPIO%d pn=GPIO%d, active low",
             STATUS_LED_AGING_GPIO, STATUS_LED_PN_GPIO);
    return ESP_OK;
}

void status_led_aging_start(void)
{
    status_led_output(STATUS_LED_AGING_GPIO, true);
}

void status_led_aging_stop(void)
{
    status_led_output(STATUS_LED_AGING_GPIO, false);
}

void status_led_pn_ready(void)
{
    portENTER_CRITICAL(&s_led_lock);
    s_pn_mode = STATUS_LED_PN_ON;
    portEXIT_CRITICAL(&s_led_lock);
    status_led_pn_apply_steady(STATUS_LED_PN_ON);
}

void status_led_pn_reject(void)
{
    status_led_pn_mode_t mode;

    portENTER_CRITICAL(&s_led_lock);
    ++s_pn_reject_seq;
    mode = s_pn_mode;
    portEXIT_CRITICAL(&s_led_lock);

    /* 稳态不变：无任务时保持原输出（快闪只是提示，不改变 PN 有效性）。 */
    status_led_pn_apply_steady(mode);
}

void status_led_pn_fail(void)
{
    portENTER_CRITICAL(&s_led_lock);
    s_pn_mode = STATUS_LED_PN_FAIL;
    portEXIT_CRITICAL(&s_led_lock);
    status_led_pn_apply_steady(STATUS_LED_PN_FAIL);
}

void status_led_pn_off(void)
{
    portENTER_CRITICAL(&s_led_lock);
    s_pn_mode = STATUS_LED_PN_OFF;
    portEXIT_CRITICAL(&s_led_lock);
    status_led_pn_apply_steady(STATUS_LED_PN_OFF);
}

status_led_pn_mode_t status_led_pn_mode(void)
{
    status_led_pn_mode_t mode;

    portENTER_CRITICAL(&s_led_lock);
    mode = s_pn_mode;
    portEXIT_CRITICAL(&s_led_lock);

    return mode;
}
