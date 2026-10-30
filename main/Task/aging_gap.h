#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* 以下业务接口仅限任务上下文，非 ISR/SDK 回调；内部用互斥锁保护会话。
 * init 在启动任务串行调用，不创建任务；内存保留到整机重启。 */
esp_err_t aging_gap_init(void);
bool aging_gap_session_begin(const char *pn, const char *record_id, bool resume);
/* close 关闭采样入口；idle 在资源清理完成后调用，允许补查。均幂等。 */
void aging_gap_session_close(void);
void aging_gap_idle(void);
/* 快照在采样开始时取得；返回0表示当前没有可采样会话。字符串写入调用者缓冲。 */
uint32_t aging_gap_sample_snapshot(char *pn, size_t pn_bytes, char *record_id, size_t record_bytes);
/* 队列元素由队列复制；成功后消费者拥有报文。失败时仍由生产者释放。
 * 在同一互斥区内验证会话、入队并计数，避免关闭与队尾提交竞态。
 * 入队最多等待50ms，沿用原采样队列行为。 */
bool aging_gap_sample_submit(uint32_t token, QueueHandle_t queue, const void *item);
/* 仅实时上传消费者调用：取编号后必须调用 done，即使落库失败。 */
int aging_gap_sample_next_id(uint32_t token);
void aging_gap_sample_done(uint32_t token, bool persisted);
/* SDK 回调也可调用：只写原子标志，不等待锁。true 表示订阅及上线PUBACK已完成。 */
void aging_gap_mqtt_ready(bool ready);
/* 仅已有 http_auto 任务调用；每次至多一个HTTP请求或一条MQTT补发，网络不持会话锁。 */
void aging_gap_poll(void);
