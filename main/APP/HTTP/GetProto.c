#include "GetProto.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_crt_bundle.h"

static const char *TAG = "HTTP";

/*
 * 原工程：xQueueCreate(10, 1024)，队列本身固定占用约 10KB internal SRAM。
 * 修改后：队列只保存指针和长度，HTTP 数据本体优先放 PSRAM。
 */
#define HTTP_DATA_QUEUE_LEN 3
#define HTTP_QUEUE_CHUNK_SIZE 1024
#define HTTP_RESPONSE_MAX_LEN (16 * 1024)

typedef struct
{
    char *data; /* 指向 PSRAM/internal heap 中的数据，消费者读取后必须释放 */
    size_t len; /* 不包含结尾 '\0' */
} http_data_msg_t;

static QueueHandle_t HttpData_queue = NULL;

static void *http_malloc_prefer_psram(size_t size)
{
    if (size == 0)
    {
        return NULL;
    }

    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL)
    {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

static void *http_realloc_prefer_psram(void *ptr, size_t size)
{
    if (size == 0)
    {
        free(ptr);
        return NULL;
    }

    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL)
    {
        p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    }
    return p;
}

static void http_response_reset(char **buf, int *len)
{
    if (buf && *buf)
    {
        free(*buf);
        *buf = NULL;
    }
    if (len)
    {
        *len = 0;
    }
}

static esp_err_t http_queue_send_copy(const char *data, size_t len, TickType_t wait_ticks)
{
    if (HttpData_queue == NULL || data == NULL || len == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    http_data_msg_t msg = {0};
    msg.data = (char *)http_malloc_prefer_psram(len + 1);
    if (msg.data == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate HTTP queue payload, len=%u", (unsigned int)len);
        return ESP_ERR_NO_MEM;
    }

    memcpy(msg.data, data, len);
    msg.data[len] = '\0';
    msg.len = len;

    if (xQueueSend(HttpData_queue, &msg, wait_ticks) != pdTRUE)
    {
        ESP_LOGE(TAG, "HTTP queue full, drop payload, len=%u", (unsigned int)len);
        free(msg.data);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

// HTTP 事件处理函数
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer = NULL;
    static int output_len = 0;

    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_CONNECTED:
        /* 防止上一次异常断开后残留缓存 */
        http_response_reset(&output_buffer, &output_len);
        break;

    case HTTP_EVENT_ON_DATA:
        if (evt->data == NULL || evt->data_len <= 0)
        {
            break;
        }

        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);

        if ((output_len + evt->data_len) > HTTP_RESPONSE_MAX_LEN)
        {
            ESP_LOGE(TAG, "HTTP response too large: %d > %d, drop", output_len + evt->data_len, HTTP_RESPONSE_MAX_LEN);
            http_response_reset(&output_buffer, &output_len);
            return ESP_ERR_NO_MEM;
        }

        char *new_buf = NULL;
        if (output_buffer == NULL)
        {
            new_buf = (char *)http_malloc_prefer_psram((size_t)evt->data_len + 1);
            output_len = 0;
        }
        else
        {
            new_buf = (char *)http_realloc_prefer_psram(output_buffer, (size_t)output_len + evt->data_len + 1);
        }

        if (new_buf == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate HTTP response buffer");
            http_response_reset(&output_buffer, &output_len);
            return ESP_ERR_NO_MEM;
        }

        output_buffer = new_buf;
        memcpy(output_buffer + output_len, evt->data, evt->data_len);
        output_len += evt->data_len;
        output_buffer[output_len] = '\0';
        break;

    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH, total received=%d", output_len);

        if (output_buffer && output_len > 0)
        {
            char *clean_data = (char *)http_malloc_prefer_psram((size_t)output_len + 1);
            if (clean_data == NULL)
            {
                ESP_LOGE(TAG, "Failed to allocate memory for clean data");
                http_response_reset(&output_buffer, &output_len);
                return ESP_ERR_NO_MEM;
            }

            /* 保持原逻辑：去掉空格、回车、换行、TAB。注意：如果 JSON 字符串值中本来需要空格，这里会被删除。 */
            int clean_len = 0;
            for (int i = 0; i < output_len; i++)
            {
                char c = output_buffer[i];
                if (c != ' ' && c != '\r' && c != '\n' && c != '\t')
                {
                    clean_data[clean_len++] = c;
                }
            }
            clean_data[clean_len] = '\0';

            int total_data_len = clean_len;
            int offset = 0;
            int send_count = 0;

            while (total_data_len > 0)
            {
                int chunk_size = (total_data_len > HTTP_QUEUE_CHUNK_SIZE) ? HTTP_QUEUE_CHUNK_SIZE : total_data_len;

                esp_err_t ret = http_queue_send_copy(clean_data + offset, (size_t)chunk_size, pdMS_TO_TICKS(100));
                if (ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to send HTTP fragment %d, err=%s", send_count + 1, esp_err_to_name(ret));
                    break;
                }

                total_data_len -= chunk_size;
                offset += chunk_size;
                send_count++;
            }

            free(clean_data);
        }

        http_response_reset(&output_buffer, &output_len);
        break;

    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;

    case HTTP_EVENT_DISCONNECTED:
    case HTTP_EVENT_ERROR:
        http_response_reset(&output_buffer, &output_len);
        break;

    default:
        break;
    }
    return ESP_OK;
}

void test_http_connection(void)
{
    char post_data[256];
    snprintf(post_data, sizeof(post_data), "{\"Name\":\"wbb\",\"Password\":\"123456\"}");

    esp_http_client_config_t config = {
        .url = "http://10.16.160.158:5000/Login/Login",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 4000,
        .event_handler = http_event_handler,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
        
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "*/*");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Status = %d", status_code);
    }
    else
    {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

esp_err_t test_http_getproto(int Id)
{
    char url[256];
    snprintf(url, sizeof(url), "http://10.16.160.51:5000/Login/GetJson?ID=%d", Id);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 4000,
        .event_handler = http_event_handler,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,

    };
    

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");

    err = esp_http_client_set_header(client, "Accept", "application/json");

    err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Status = %d", status_code);
    }
    else
    {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    err =esp_http_client_cleanup(client);
    return err;
}


esp_err_t test_http_getproto_New(int64_t Id)
{
    const char *url ="https://ipc.poweroak.ltd:29009/DataCenter/V1/Protocol/GetWholeProtocol";

    char post_data[32];
    snprintf(post_data, sizeof(post_data), "%" PRId64, Id);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .event_handler = http_event_handler,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,

        // HTTPS服务器证书验证
        .crt_bundle_attach = esp_crt_bundle_attach,
        
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err;

    // 对应 C#：
    // request.ContentType = "application/json";
    err = esp_http_client_set_header(
        client,
        "Content-Type",
        "application/json");

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set Content-Type failed");
        esp_http_client_cleanup(client);
        return err;
    }

    // 对应 C#：
    // request.UserAgent = "CSharp-HttpClient/1.0";
    esp_http_client_set_header(
        client,
        "User-Agent",
        "CSharp-HttpClient/1.0");

    esp_http_client_set_header(
        client,
        "Accept",
        "application/json");

    // 关键：把 ID 作为 POST Body 发送
    err = esp_http_client_set_post_field(
        client,
        post_data,
        strlen(post_data));

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set post field failed");
        esp_http_client_cleanup(client);
        return err;
    }

    ESP_LOGI(TAG, "POST URL: %s", url);
    ESP_LOGI(TAG, "POST Body: %s", post_data);

    err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Status = %d", status_code);
    }
    else
    {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);

    return err;
}

// 简单的HTTP GET测试（不需要认证）
void test_http_connection_simple(void)
{
    esp_http_client_config_t config = {
        .url = "http://httpbin.org/get",
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .timeout_ms = 5000,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32S3-Device");

    ESP_LOGI(TAG, "Sending GET request to httpbin.org...");
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP GET Success, Status = %d", status_code);
    }
    else
    {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

esp_err_t Http_init(void)
{
    if (HttpData_queue != NULL)
    {
        return ESP_OK;
    }

    HttpData_queue = xQueueCreate(HTTP_DATA_QUEUE_LEN, sizeof(http_data_msg_t));
    if (HttpData_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create HTTP data queue");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HTTP queue created: len=%d, item_size=%u", HTTP_DATA_QUEUE_LEN, (unsigned int)sizeof(http_data_msg_t));
    return ESP_OK;
}

int app_read_HTTP_data(char *data_buffer, int buffer_len, uint32_t timeout)
{
    if (data_buffer == NULL || buffer_len <= 1)
    {
        ESP_LOGE(TAG, "Invalid buffer or buffer length");
        return -1;
    }
    if (HttpData_queue == NULL)
    {
        ESP_LOGE(TAG, "HTTP data queue not initialized");
        return -1;
    }

    http_data_msg_t msg = {0};
    if (xQueueReceive(HttpData_queue, &msg, timeout) != pdTRUE)
    {
        return -1;
    }

    int copy_len = (msg.len > (size_t)(buffer_len - 1)) ? (buffer_len - 1) : (int)msg.len;
    if (msg.data != NULL && copy_len > 0)
    {
        memcpy(data_buffer, msg.data, (size_t)copy_len);
    }
    data_buffer[copy_len] = '\0';

    if (msg.len > (size_t)copy_len)
    {
        ESP_LOGW(TAG, "HTTP data truncated: src=%u, dst=%d", (unsigned int)msg.len, copy_len);
    }

    free(msg.data);
    return copy_len;
}
