#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 状态指示灯（两灯均为低电平点亮）：
 * - IO17：老化运行灯。agingStart 回复后点亮，agingIdle/agingError/agingStop 后熄灭。
 * - IO18：PN 状态灯。set_pn 成功常亮；重复 set_pn 快闪 3 次后回原状态；
 *         老化失败 1Hz 慢闪并保持到下次 set_pn 成功；老化正常结束熄灭。
 *
 * 引脚来源：硬件接线约定（2026-10-27 用户确认），与现有 UART/RS485/CAN/SPI/按键
 * 引脚（1/2/4/5/6/7/15/16/40/43/44）不冲突。
 */
#define STATUS_LED_AGING_GPIO 17
#define STATUS_LED_PN_GPIO 18

/** IO18 稳态语义。快闪只是临时覆盖，结束后必定回到这里的稳态。 */
typedef enum
{
    STATUS_LED_PN_OFF = 0, /* 无 PN / 老化正常结束：熄灭 */
    STATUS_LED_PN_ON,      /* PN 已就绪（含老化运行期）：常亮 */
    STATUS_LED_PN_FAIL,    /* 老化失败：1Hz 慢闪，保持到下次 set_pn 成功 */
} status_led_pn_mode_t;

/** 配置两路 GPIO 为输出（默认熄灭）并创建 CPU1 轮询任务；幂等。
 * 仅启动阶段调用，非 ISR。任务创建失败返回 ESP_ERR_NO_MEM，此时 GPIO 仍可用作
 * 稳态指示（IO18 无闪烁、失败退化为熄灭）。 */
esp_err_t status_led_init(void);

/** IO17 拉低点亮。任意任务上下文、无阻塞、幂等，非 ISR。 */
void status_led_aging_start(void);
/** IO17 恢复高电平熄灭。任意任务上下文、无阻塞、幂等，非 ISR。 */
void status_led_aging_stop(void);

/** IO18 转常亮（PN 就绪/老化运行期保持）。任意任务上下文、无阻塞、幂等。 */
void status_led_pn_ready(void);
/** IO18 快闪 3 次（亮 200 ms / 灭 200 ms）后回到当前稳态。任意任务上下文、无阻塞。 */
void status_led_pn_reject(void);
/** IO18 转 1Hz 慢闪（老化失败），保持到下次 set_pn 成功。任意任务上下文、无阻塞。 */
void status_led_pn_fail(void);
/** IO18 熄灭（无 PN 或老化正常结束）。任意任务上下文、无阻塞、幂等。 */
void status_led_pn_off(void);

/** 查询 IO18 当前稳态（不改变输出）。任意任务上下文、无阻塞，供诊断/测试使用。 */
status_led_pn_mode_t status_led_pn_mode(void);

#ifdef __cplusplus
}
#endif
