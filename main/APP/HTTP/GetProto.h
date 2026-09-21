#ifndef GETPROTO_H
#define GETPROTO_H

#include <stdint.h>

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
esp_err_t test_http_getproto_New(int64_t Id);

esp_err_t Http_init(void);
int app_read_HTTP_data(char *data_buffer, int buffer_len, uint32_t timeout);

#ifdef __cplusplus
}
#endif

#endif
