#pragma once

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/** 由调用方放在内部静态 RAM；控制块内部、消息数据区外部。禁止 ISR 使用本封装。 */
typedef struct {
    StaticQueue_t control;
    QueueHandle_t handle;
    uint8_t *storage;
    UBaseType_t length;
    UBaseType_t item_size;
} AppPsramQueue;

/** 启动/初始化任务串行调用，成功后同参数调用幂等。name 仅用于即时串口诊断。
 * 启用 PSRAM 时数据区只使用 PSRAM，失败不挤占内部 RAM；不缩减容量。
 * queue 必须指向内部静态 RAM，生命周期覆盖所有使用者；失败返回 NULL 并清理本次分配。 */
QueueHandle_t app_psram_queue_create(AppPsramQueue *queue, UBaseType_t length,
                                    UBaseType_t item_size, const char *name);
/** 仅在没有发送/接收者时调用，例如初始化回滚；删除 RTOS 队列后释放自有存储区。
 * 不可用普通 vQueueDelete 代替本函数，否则外部存储会泄漏。非 ISR、不可并发。 */
void app_psram_queue_delete(AppPsramQueue *queue);
