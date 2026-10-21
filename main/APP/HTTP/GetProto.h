#ifndef GETPROTO_H
#define GETPROTO_H

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

void test_http_connection(void);
void test_http_connection_simple(void);
esp_err_t test_http_getproto(int id);
#define HTTP_PROTOCOL_RESPONSE_MAX_BYTES (16U * 1024U)

typedef enum {
    HTTP_PROTOCOL_EXTERNAL = 1,
    HTTP_PROTOCOL_AGING = 2,
} HttpProtocolTargetKind;

typedef struct {
    HttpProtocolTargetKind kind;
    uint8_t device_index;
    int64_t proto_id;
    uint32_t generation;
} HttpProtocolTarget;

typedef struct {
    char *data; // 出队者拥有，必须经 http_protocol_response_release() 释放。
    size_t len_bytes;
    HttpProtocolTarget target;
} HttpProtocolResponse;

/** 同步 HTTP 请求，最多16KiB；只在完整2xx响应入队后返回 ESP_OK。任务上下文。 */
esp_err_t http_get_protocol(const HttpProtocolTarget *target);
/** 完整响应出队并转移所有权，不返回分片；失败时不产生待释放对象。 */
esp_err_t http_receive_protocol(HttpProtocolResponse *response, TickType_t timeout_ticks);
/** 释放响应并清零，可重复调用；不可在 ISR 调用。 */
void http_protocol_response_release(HttpProtocolResponse *response);

esp_err_t Http_init(void);


esp_err_t test_http_post_record_query(int32_t seq, int32_t id_num, const char *record_id,char *out_response_buffer, int out_buffer_len);


#ifdef __cplusplus
}
#endif

#endif
