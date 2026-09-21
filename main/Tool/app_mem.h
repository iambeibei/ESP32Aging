#ifndef APP_MEM_H
#define APP_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *app_malloc_prefer_psram(size_t size);
void *app_realloc_prefer_psram(void *ptr, size_t size);
void *app_calloc_prefer_psram(size_t count, size_t size);

#ifdef __cplusplus
}
#endif

#endif