#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 状态指示灯（两灯均为低电平点亮）：
 * - IO18：PN 灯。set_pn 成功、掉电恢复 Ready PN 成功时点亮。
 * - IO17：老化运行灯。start_aging 成功（agingStart 回复点）、掉电恢复运行中老化时点亮。
 * - 老化结束（agingIdle / agingError / stop_aging）两灯一起熄灭。
 * - 重复或非法 set_pn、start_aging 缺 PN 被拒，均不改变灯态。
 *
 * 事件点直接写 GPIO，不创建任务、不使用软件定时器。
 *
 * 引脚来源：硬件接线约定（2026-10-27 用户确认），与现有 UART/RS485/CAN/SPI/按键
 * 引脚（1/2/4/5/6/7/15/16/40/43/44）不冲突。
 */
#define STATUS_LED_AGING_GPIO 8
#define STATUS_LED_PN_GPIO 18

/** 配置两路 GPIO 为输出并置为熄灭（高电平）；幂等。
 * 仅启动阶段调用（app_task_init 内、app_uart_init 之前），非 ISR。 */
esp_err_t status_led_init(void);

/** IO18 拉低点亮（PN 就绪）。任意任务上下文、无阻塞、幂等，非 ISR。 */
void status_led_pn_ready(void);

/** IO17 拉低点亮（老化已启动）。任意任务上下文、无阻塞、幂等，非 ISR。 */
void status_led_aging_start(void);

/** 两灯恢复高电平熄灭（老化结束）。任意任务上下文、无阻塞、幂等，非 ISR。 */
void status_led_all_off(void);

/** 两灯电平各自取反（按键诊断测试用）。基于模块内电平镜像，不依赖 GPIO 回读。
 * 任意任务上下文、无阻塞、非 ISR；不持久化，后续业务事件会覆盖其结果。 */
void status_led_toggle_both(void);

#ifdef __cplusplus
}
#endif
