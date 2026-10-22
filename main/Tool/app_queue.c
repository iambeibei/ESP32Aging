#include "app_queue.h"

#include <stdint.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"

static const char *TAG = "app_queue";

QueueHandle_t app_psram_queue_create(AppPsramQueue *queue, UBaseType_t length,
                                    UBaseType_t item_size, const char *name)
{
    if (queue == NULL || !esp_ptr_internal(queue) || length == 0 || item_size == 0 ||
        (size_t)length > SIZE_MAX / item_size) return NULL;
    if (queue->handle != NULL) {
        return queue->length == length && queue->item_size == item_size ? queue->handle : NULL;
    }
    size_t bytes = (size_t)length * item_size;
#if CONFIG_SPIRAM
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    uint8_t *storage = heap_caps_malloc(bytes, caps);
    if (storage == NULL) {
        ESP_LOGE(TAG, "Queue %s storage allocation failed: bytes=%u caps=0x%lx",
                 name != NULL ? name : "unnamed", (unsigned)bytes, (unsigned long)caps);
        return NULL;
    }
    QueueHandle_t handle = xQueueCreateStatic(length, item_size, storage, &queue->control);
    if (handle == NULL) {
        free(storage);
        return NULL;
    }
    queue->storage = storage;
    queue->length = length;
    queue->item_size = item_size;
    queue->handle = handle;
    ESP_LOGI(TAG, "Queue %s ready: items=%u item_bytes=%u storage=%u external=%d control_internal=1",
             name != NULL ? name : "unnamed", (unsigned)length, (unsigned)item_size,
             (unsigned)bytes, esp_ptr_external_ram(storage));
    return handle;
}

void app_psram_queue_delete(AppPsramQueue *queue)
{
    if (queue == NULL || queue->handle == NULL) return;
    vQueueDelete(queue->handle); /* SDK 知道这是静态队列，不释放 control/storage。 */
    queue->handle = NULL;
    free(queue->storage);
    queue->storage = NULL;
    queue->length = 0;
    queue->item_size = 0;
}
