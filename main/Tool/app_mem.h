#ifndef APP_MEM_H
#define APP_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** app_main 首先串行调用一次，早于所有 JSON 使用/业务任务。
 * 安装进程全局 cJSON 分配器并登记无分配的失败计数回调；不可运行中切换或在 ISR 调用。 */
void app_memory_init(void);
/** 任务上下文输出内外堆当前/最低/最大块及失败计数；无 MQTT 发布，不在 ISR 调用。 */
void app_memory_log(const char *stage);

/** 任务上下文优先 PSRAM，保留既有回退语义；返回值由调用者用 free() 释放。
 * 失败返回 NULL；realloc 失败不改变原指针。不可在 ISR/缓存关闭期间使用。 */
void *app_malloc_prefer_psram(size_t size);
void *app_realloc_prefer_psram(void *ptr, size_t size);
void *app_calloc_prefer_psram(size_t count, size_t size);

#ifdef __cplusplus
}
#endif

#endif
