#include "app_mem.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "sdkconfig.h"

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

    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
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

    return heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
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
    return heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
}