#include "app_mem.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "sdkconfig.h"

static const char *TAG = "app_mem";
/* 内部静态 RAM；分配失败回调只更新定宽原子值，不日志、不申请内存，避免递归。 */
static atomic_uint s_failed_attempts, s_last_failed_bytes, s_last_failed_caps, s_internal_fallbacks;
static bool s_memory_initialized;

static void memory_allocation_failed(size_t bytes, uint32_t caps, const char *function_name)
{
    (void)function_name;
    atomic_fetch_add(&s_failed_attempts, 1);
    atomic_store(&s_last_failed_bytes, bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes);
    atomic_store(&s_last_failed_caps, caps);
}

void app_memory_init(void)
{
    if (s_memory_initialized) return; /* 启动任务串行调用，不在运行中改 hooks。 */
    cJSON_Hooks hooks = {.malloc_fn = app_malloc_prefer_psram, .free_fn = free};
    cJSON_InitHooks(&hooks);
    esp_err_t err = heap_caps_register_failed_alloc_callback(memory_allocation_failed);
    if (err != ESP_OK) ESP_LOGW(TAG, "Failed to register allocation diagnostics: %d", err);
    s_memory_initialized = true;
    app_memory_log("startup");
}

void app_memory_log(const char *stage)
{
    multi_heap_info_t internal = {0}, dma = {0}, external = {0};
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_caps_get_info(&dma, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
#if CONFIG_SPIRAM
    heap_caps_get_info(&external, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t external_total = esp_psram_get_size();
#else
    size_t external_total = 0;
#endif
    ESP_LOGI(TAG, "MEM_STAGE=%s internal_free=%u min=%u largest=%u dma_free=%u dma_min=%u dma_largest=%u "
             "psram_total=%u free=%u min=%u largest=%u "
             "failed_attempts=%u last_bytes=%u last_caps=0x%x internal_fallbacks=%u",
             stage != NULL ? stage : "unknown", (unsigned)internal.total_free_bytes,
             (unsigned)internal.minimum_free_bytes, (unsigned)internal.largest_free_block,
             (unsigned)dma.total_free_bytes, (unsigned)dma.minimum_free_bytes, (unsigned)dma.largest_free_block,
             (unsigned)external_total, (unsigned)external.total_free_bytes,
             (unsigned)external.minimum_free_bytes, (unsigned)external.largest_free_block,
             atomic_load(&s_failed_attempts), atomic_load(&s_last_failed_bytes),
             atomic_load(&s_last_failed_caps), atomic_load(&s_internal_fallbacks));
}

static void *record_fallback(void *p)
{
#if CONFIG_SPIRAM
    if (p != NULL && esp_ptr_internal(p)) atomic_fetch_add(&s_internal_fallbacks, 1);
#endif
    return p;
}

void *app_malloc_prefer_psram(size_t size)
{
    if (size == 0) {
        return NULL;
    }

#if CONFIG_SPIRAM
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return p;
    }
#endif

    return record_fallback(heap_caps_malloc(size, MALLOC_CAP_8BIT));
}

void *app_realloc_prefer_psram(void *ptr, size_t size)
{
    if (size == 0) {
        free(ptr);
        return NULL;
    }

#if CONFIG_SPIRAM
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return p;
    }
#endif

    return record_fallback(heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT));
}

void *app_calloc_prefer_psram(size_t count, size_t size)
{
    if (count == 0 || size == 0) {
        return NULL;
    }

#if CONFIG_SPIRAM
    void *p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return p;
    }
#endif

    // 降级到内部 RAM
    return record_fallback(heap_caps_calloc(count, size, MALLOC_CAP_8BIT));
}
