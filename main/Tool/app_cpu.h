#ifndef APP_CPU_H
#define APP_CPU_H

#ifdef __cplusplus
extern "C" {
#endif

/** 按键任务专用，非 ISR、不可重入。周期调用，约每秒采样两核空闲任务累计时间。
 * 无分配、无延时，不遍历任务列表；只在同一个按键任务中调用本模块两个接口。 */
void app_cpu_usage_poll(void);

/** 同一按键任务打印最近采样窗的 CPU0/CPU1 及双核平均占用率估算。
 * tag 可为 NULL；采样尚不足一秒时明确打印等待状态，不伪报 0%。日志输出可能阻塞。 */
void app_cpu_usage_log(const char *tag);

#ifdef __cplusplus
}
#endif

#endif
