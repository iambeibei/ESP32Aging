#include "GetProto.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "service_urls.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"

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
static esp_err_t http_request_execute(esp_http_client_config_t *config,
                                     const HttpProtocolTarget *target, const char *post_data)
{
    HttpRequestContext ctx = {0};
    if (target != NULL) ctx.response.target = *target;
    config->event_handler = http_event_handler;
    config->user_data = &ctx;
    esp_http_client_handle_t client = esp_http_client_init(config);
    if (client == NULL) return ESP_ERR_NO_MEM;
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
    if (err == ESP_OK && target != NULL) {
        if (ctx.response.len_bytes == 0 || HttpData_queue == NULL) err = ESP_ERR_INVALID_STATE;
        else if (xQueueSend(HttpData_queue, &ctx.response, pdMS_TO_TICKS(100)) != pdTRUE)
            err = ESP_ERR_TIMEOUT;
        else ctx.response.data = NULL; // 完整对象一次入队，消费者成为唯一所有者。
    }
    esp_err_t cleanup_err = esp_http_client_cleanup(client);
    if (cleanup_err != ESP_OK) ESP_LOGW(TAG, "HTTP cleanup failed: %s", esp_err_to_name(cleanup_err));
    http_protocol_response_release(&ctx.response);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "HTTP request rejected: status=%d error=%s", status, esp_err_to_name(err));
    return err;
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

esp_err_t test_http_post_record_query(int32_t seq, int32_t id_num, const char *record_id,char *out_response_buffer, int out_buffer_len)
{
    /*
     * 服务器 IP 运行时取自 baseconfig.json 的 SERVER_IP（与 MQTT broker 同一
     * 机制），端口/路径为编译期模板；换服务器 IP 无需重新编译 app。
     */
    char url[LG_URL_MAX_LEN];
    esp_err_t build_err = lg_url_build(LG_URL_GET_LOST_IDNUM_URL_FMT, url, sizeof(url));
    if (build_err != ESP_OK) {
        ESP_LOGE(TAG, "Build GetMissingIdNums URL failed: %s", esp_err_to_name(build_err));
        return build_err;
    }

    // ---- Build request JSON body: {"Seq":...,"Data":{"RecordId":"...","IdNum":...}} ----
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "Seq", cJSON_CreateNumber(seq));

    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "RecordId", cJSON_CreateString(record_id));
    cJSON_AddItemToObject(data, "IdNum", cJSON_CreateNumber(id_num));

    cJSON_AddItemToObject(root, "Data", data);

    char *post_data = cJSON_PrintUnformatted(root);
    if (post_data == NULL) {
        ESP_LOGE(TAG, "Failed to print JSON body");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
        //.crt_bundle_attach = esp_crt_bundle_attach,
        // 不再需要 .event_handler
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        free(post_data);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "CSharp-HttpClient/1.0");
    esp_http_client_set_header(client, "Accept", "application/json");

    // 走底层流程，而不是 esp_http_client_perform
    err = esp_http_client_open(client, strlen(post_data));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        goto cleanup;
    }

    int wlen = esp_http_client_write(client, post_data, strlen(post_data));
    if (wlen < 0) {
        ESP_LOGE(TAG, "Write failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "HTTP client fetch headers failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    // 直接把响应体读入调用者提供的 buffer
    int data_read = esp_http_client_read_response(client, out_response_buffer, out_buffer_len - 1);
    if (data_read < 0) {
        ESP_LOGE(TAG, "Failed to read response");
        err = ESP_FAIL;
        goto cleanup;
    }
    out_response_buffer[data_read] = '\0';

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP Status = %d, content_length = %d", status_code, content_length);
    ESP_LOGI(TAG, "Response Body: %s", out_response_buffer);

    err = ESP_OK;

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(post_data);
    cJSON_Delete(root);

    return err;
}

