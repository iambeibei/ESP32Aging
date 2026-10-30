#include "GetProto.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "service_urls.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "app_mem.h"

static const char *TAG = "HTTP";
#define HTTP_DATA_QUEUE_LEN 3
static QueueHandle_t HttpData_queue;

typedef struct {
    HttpProtocolResponse response;
    esp_err_t error;
    bool finished;
} HttpRequestContext;

static void *http_realloc_prefer_psram(void *ptr, size_t size)
{
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != NULL ? p : heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
}

void http_protocol_response_release(HttpProtocolResponse *response)
{
    if (response == NULL) return;
    free(response->data);
    memset(response, 0, sizeof(*response));
}

/* esp_http_client_perform 同步执行回调；上下文仅由该请求的调用任务访问。
 * callback 不释放/移交缓冲，所有出口由 http_request_execute 统一处理所有权。 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    HttpRequestContext *ctx = evt->user_data;
    if (ctx == NULL) return ESP_ERR_INVALID_ARG;
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        free(ctx->response.data);
        ctx->response.data = NULL;
        ctx->response.len_bytes = 0;
        ctx->error = ESP_OK;
        ctx->finished = false;
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && ctx->error == ESP_OK) {
        if (evt->data_len <= 0) return ESP_OK;
        if (evt->data == NULL || (size_t)evt->data_len >
            HTTP_PROTOCOL_RESPONSE_MAX_BYTES - ctx->response.len_bytes) {
            ctx->error = ESP_ERR_INVALID_SIZE;
        } else {
            size_t length = ctx->response.len_bytes + (size_t)evt->data_len;
            char *buffer = http_realloc_prefer_psram(ctx->response.data, length + 1U);
            if (buffer == NULL) ctx->error = ESP_ERR_NO_MEM;
            else {
                ctx->response.data = buffer;
                memcpy(buffer + ctx->response.len_bytes, evt->data, (size_t)evt->data_len);
                buffer[length] = '\0';
                ctx->response.len_bytes = length;
            }
        }
    } else if (evt->event_id == HTTP_EVENT_ON_FINISH) {
        ctx->finished = true;
    } else if (evt->event_id == HTTP_EVENT_ERROR) {
        ctx->error = ESP_FAIL;
    }
    return ctx->error;
}

/* 诊断请求不指定 target，不会将任意网页响应装入设备协议队列。 */
static esp_err_t http_request_execute_full(esp_http_client_config_t *config,
                                          const HttpProtocolTarget *target, const char *post_data,
                                          char *out, size_t out_size)
{
    HttpRequestContext ctx = {0};
    if (target != NULL) ctx.response.target = *target;
    config->event_handler = http_event_handler;
    config->user_data = &ctx;
    esp_http_client_handle_t client = esp_http_client_init(config);
    if (client == NULL) return ESP_ERR_NO_MEM;
    if (target != NULL) app_memory_log("protocol_http_start");
    esp_err_t err = ESP_OK;
    if (post_data != NULL) {
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (err == ESP_OK)
            err = esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }
    if (err == ESP_OK) err = esp_http_client_perform(client);
    if (err == ESP_OK && ctx.error != ESP_OK) err = ctx.error;
    int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && (status < 200 || status >= 300 || !ctx.finished ||
                          !esp_http_client_is_complete_data_received(client)))
        err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && out != NULL) {
        if (ctx.response.len_bytes == 0) err = ESP_ERR_INVALID_RESPONSE;
        else if (ctx.response.len_bytes >= out_size) err = ESP_ERR_INVALID_SIZE;
        else memcpy(out, ctx.response.data, ctx.response.len_bytes + 1U);
    }
    if (err == ESP_OK && target != NULL) {
        if (ctx.response.len_bytes == 0 || HttpData_queue == NULL) err = ESP_ERR_INVALID_STATE;
        else if (xQueueSend(HttpData_queue, &ctx.response, pdMS_TO_TICKS(100)) != pdTRUE)
            err = ESP_ERR_TIMEOUT;
        else ctx.response.data = NULL; // 完整对象一次入队，消费者成为唯一所有者。
    }
    esp_err_t cleanup_err = esp_http_client_cleanup(client);
    if (cleanup_err != ESP_OK) ESP_LOGW(TAG, "HTTP cleanup failed: %s", esp_err_to_name(cleanup_err));
    http_protocol_response_release(&ctx.response);
    if (target != NULL) app_memory_log("protocol_http_done");
    if (err != ESP_OK)
        ESP_LOGW(TAG, "HTTP request rejected: status=%d error=%s", status, esp_err_to_name(err));
    return err;
}

static esp_err_t http_request_execute(esp_http_client_config_t *config,
                                     const HttpProtocolTarget *target, const char *post_data)
{
    return http_request_execute_full(config, target, post_data, NULL, 0);
}

esp_err_t http_get_protocol(const HttpProtocolTarget *target)
{
    if (target == NULL || target->proto_id == 0 || target->generation == 0 ||
        (target->kind != HTTP_PROTOCOL_EXTERNAL && target->kind != HTTP_PROTOCOL_AGING))
        return ESP_ERR_INVALID_ARG;
    char post_data[32];
    snprintf(post_data, sizeof(post_data), "%" PRId64, target->proto_id);
    esp_http_client_config_t config = {
        .url = LG_URL_HTTP_PROTOCOL_WHOLE_URL, .method = HTTP_METHOD_POST,
        .timeout_ms = 30000, .buffer_size = 1024, .buffer_size_tx = 1024,
        .keep_alive_enable = false, .crt_bundle_attach = esp_crt_bundle_attach,
    };
    return http_request_execute(&config, target, post_data);
}

/* 启动任务串行调用；创建后队列终生有效。请求函数不懒初始化，避免并发首次创建。 */
esp_err_t Http_init(void)
{
    if (HttpData_queue != NULL) return ESP_OK;
    HttpData_queue = xQueueCreate(HTTP_DATA_QUEUE_LEN, sizeof(HttpProtocolResponse));
    return HttpData_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t http_receive_protocol(HttpProtocolResponse *response, TickType_t timeout_ticks)
{
    if (response == NULL) return ESP_ERR_INVALID_ARG;
    memset(response, 0, sizeof(*response));
    if (HttpData_queue == NULL) return ESP_ERR_INVALID_STATE;
    return xQueueReceive(HttpData_queue, response, timeout_ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void test_http_connection(void)
{
    esp_http_client_config_t config = {
        .url = "http://10.16.160.158:5000/Login/Login", .method = HTTP_METHOD_POST,
        .timeout_ms = 4000, .buffer_size = 1024, .buffer_size_tx = 1024,
    };
    esp_err_t err = http_request_execute(&config, NULL, "{\"Name\":\"wbb\",\"Password\":\"123456\"}");
    if (err != ESP_OK) ESP_LOGW(TAG, "HTTP diagnostic failed: %s", esp_err_to_name(err));
}

esp_err_t test_http_getproto(int id)
{
    char url[256];
    snprintf(url, sizeof(url), "http://10.16.160.51:5000/Login/GetJson?ID=%d", id);
    esp_http_client_config_t config = {.url = url, .method = HTTP_METHOD_POST,
        .timeout_ms = 4000, .buffer_size = 1024};
    return http_request_execute(&config, NULL, NULL);
}

void test_http_connection_simple(void)
{
    esp_http_client_config_t config = {.url = "http://httpbin.org/get", .method = HTTP_METHOD_GET,
        .timeout_ms = 5000, .buffer_size = 1024};
    esp_err_t err = http_request_execute(&config, NULL, NULL);
    if (err != ESP_OK) ESP_LOGW(TAG, "HTTP diagnostic failed: %s", esp_err_to_name(err));
}

/* 复用完整响应累积器，perform 负责完整发送请求体；不接受短写/截断响应。 */
esp_err_t test_http_post_record_query(int32_t seq, int32_t id_num, const char *record_id,
                                      char *out_response_buffer, int out_buffer_len)
{
    if (record_id == NULL || record_id[0] == '\0' || id_num < 0 ||
        out_response_buffer == NULL || out_buffer_len < 2) return ESP_ERR_INVALID_ARG;
    out_response_buffer[0] = '\0';
    char url[LG_URL_MAX_LEN];
    esp_err_t err = lg_url_build(LG_URL_GET_LOST_IDNUM_URL_FMT, url, sizeof(url));
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    cJSON *data = cJSON_AddObjectToObject(root, "Data");
    bool valid = data != NULL && cJSON_AddNumberToObject(root, "Seq", seq) != NULL &&
                 cJSON_AddStringToObject(data, "RecordId", record_id) != NULL &&
                 cJSON_AddNumberToObject(data, "IdNum", id_num) != NULL;
    char *post_data = valid ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (post_data == NULL) return ESP_ERR_NO_MEM;
    esp_http_client_config_t config = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 10000,
        .buffer_size = 1024, .buffer_size_tx = 1024, .keep_alive_enable = false,
    };
    err = http_request_execute_full(&config, NULL, post_data, out_response_buffer,
                                    (size_t)out_buffer_len);
    free(post_data);
    return err;
}
