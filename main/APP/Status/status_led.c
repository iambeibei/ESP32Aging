/**
 * @file status_led.c
 * @brief 状态指示灯驱动：IO18 为 PN 灯、IO17 为老化运行灯，均低电平点亮。
 *
 * 纯事件驱动，不创建任务、不使用软件定时器：
 * - set_pn 成功 / 掉电恢复 Ready PN 成功 -> status_led_pn_ready()
 * - start_aging 成功 / 掉电恢复运行中老化 -> status_led_aging_start()
 * - 老化正常完成 / 异常中止 / 人工 stop_aging -> status_led_all_off()
 * 重复或非法 set_pn、start_aging 缺 PN 被拒，均不改变灯态。
 *
 * 两灯电平在模块内维护镜像：GPIO_MODE_OUTPUT 下 gpio_get_level() 读不到输出电平
 * （输入使能已关闭，恒返回 0），因此翻转一律基于镜像，不依赖 GPIO 回读，也不受
 * LED 接法（灌电流/拉电流、是否经三极管）影响。
 */
#include "status_led.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define TAG "status_led"

/* 两灯均为低电平点亮。 */
#define STATUS_LED_LEVEL_ON 0
#define STATUS_LED_LEVEL_OFF 1

/*
 * 仅 status_led_init() 写一次，之后只读；调用发生在 app_task_init() 的启动阶段
 * （任何 set_pn/start_aging 事件之前），无需额外同步。
 */
static bool s_led_initialized;

/* 两灯当前输出电平镜像（true = 点亮/输出低）。锁内只做赋值，不调用 SDK/日志。 */
static portMUX_TYPE s_led_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_aging_on;
static bool s_pn_on;

/* 仅写 GPIO 并检查返回值，不更新镜像（镜像由调用方在临界区内维护）。 */
static void status_led_gpio_write(int gpio_num, bool on)
{
    esp_err_t err = gpio_set_level(gpio_num, on ? STATUS_LED_LEVEL_ON : STATUS_LED_LEVEL_OFF);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "gpio_set_level(%d) failed: %s", gpio_num, esp_err_to_name(err));
    }
}

/* 写 GPIO + 更新镜像：init 与业务事件的统一入口。 */
static void status_led_output(int gpio_num, bool on)
{
    status_led_gpio_write(gpio_num, on);

    portENTER_CRITICAL(&s_led_lock);
    if (gpio_num == STATUS_LED_AGING_GPIO)
    {
        s_aging_on = on;
    }
    else if (gpio_num == STATUS_LED_PN_GPIO)
    {
        s_pn_on = on;
    }
    portEXIT_CRITICAL(&s_led_lock);
}

esp_err_t status_led_init(void)
{
    if (s_led_initialized)
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

    /* 配置完成后再锁存一次高电平，并把镜像初始化为“双灭”。 */
    status_led_output(STATUS_LED_AGING_GPIO, false);
    status_led_output(STATUS_LED_PN_GPIO, false);

    s_led_initialized = true;
    ESP_LOGI(TAG, "Status LED ready: aging=GPIO%d pn=GPIO%d, active low",
             STATUS_LED_AGING_GPIO, STATUS_LED_PN_GPIO);
    return ESP_OK;
}

void status_led_pn_ready(void)
{
    status_led_output(STATUS_LED_PN_GPIO, true);
}

void status_led_aging_start(void)
{
    status_led_output(STATUS_LED_AGING_GPIO, true);
}

void status_led_all_off(void)
{
    status_led_output(STATUS_LED_AGING_GPIO, false);
    status_led_output(STATUS_LED_PN_GPIO, false);
}

void status_led_toggle_both(void)
{
    bool aging_on;
    bool pn_on;

    /* 临界区内只翻转镜像；GPIO 写入与日志留在临界区外。 */
    portENTER_CRITICAL(&s_led_lock);
    s_aging_on = !s_aging_on;
    s_pn_on = !s_pn_on;
    aging_on = s_aging_on;
    pn_on = s_pn_on;
    portEXIT_CRITICAL(&s_led_lock);

    status_led_gpio_write(STATUS_LED_AGING_GPIO, aging_on);
    status_led_gpio_write(STATUS_LED_PN_GPIO, pn_on);
}
