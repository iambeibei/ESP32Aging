#include "task_config.h"
/**
 * @file app_rs485.c
 * @brief RS485 驱动封装：固定大小队列版本，避免频繁 malloc/free
 */

#include <stdint.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"


#define TAG "RS485"

/* ---------------- 硬件配置 ---------------- */

#define APP_UART_PORT UART_NUM_2
#define APP_UART_TXD 1
#define APP_UART_RXD 2

// 测试使用
//  #define APP_UART_PORT UART_NUM_0
//  #define APP_UART_TXD 43
//  #define APP_UART_RXD 44

#define APP_UART_RTS UART_PIN_NO_CHANGE
#define APP_UART_CTS UART_PIN_NO_CHANGE

#define APP_UART_BAUD_RATE 115200

/* ---------------- 缓冲区与队列配置 ---------------- */

/**
 * UART 驱动内部 RX ring buffer。
 * 建议大于单个数据块大小，减少 UART_BUFFER_FULL 风险。
 */
#define APP_UART_RX_RING_BUF_SIZE 1024

/**
 * TX buffer 设为 0，表示不额外创建 UART TX ring buffer。
 * uart_write_bytes 会直接写入 FIFO，行为更接近阻塞发送。
 */
#define APP_UART_TX_RING_BUF_SIZE 0

/**
 * UART 驱动事件队列长度。
 */
#define APP_UART_EVENT_QUEUE_LEN 3

/**
 * 应用层数据队列长度。
 * 固定大小队列总占用约为：
 * APP_UART_DATA_QUEUE_LEN × sizeof(app_uart_data_t)
 */
#define APP_UART_DATA_QUEUE_LEN 3

/**
 * 单个应用层数据块最大长度。
 */
#define APP_UART_BLOCK_SIZE 1024

/**
 * ESP32 UART 硬件 FIFO 典型深度。
 * 这里用于估算等待时间，不直接用于数组大小。
 */
#define APP_UART_HW_FIFO_LEN 120

/**
 * 接收任务配置。
 */
#define APP_UART_TASK_STACK_SIZE LG_STACK_RS485_RX
#define APP_UART_TASK_PRIORITY LG_PRIO_RS485_RX

/* ---------------- 内部数据结构 ---------------- */

/**
 * 固定大小数据块。
 *
 * 注意：
 * 这里不再使用 uint8_t *data，也不再每次 malloc/free。
 * FreeRTOS 队列会直接复制整个 app_uart_data_t 结构体。
 */
typedef struct
{
    uint16_t data_len;
    uint8_t data[APP_UART_BLOCK_SIZE];
} app_uart_data_t;

/* ---------------- 静态变量 ---------------- */

static QueueHandle_t s_uart_event_queue = NULL;
static QueueHandle_t s_data_queue = NULL;

static SemaphoreHandle_t s_uart_write_mutex = NULL;
static SemaphoreHandle_t s_uart_read_mutex = NULL;

/**
 * 接收任务使用的累积缓冲区。
 * 放在静态区，避免占用任务栈。
 */
static uint8_t s_rx_acc_buf[APP_UART_BLOCK_SIZE];

/**
 * 入队缓存。
 * 放在静态区，避免 data_to_queue() 内部创建 1 KB 以上的局部变量。
 */
static app_uart_data_t s_queue_cache;

/**
 * 出队缓存。
 * 放在静态区，避免 app_read_UART0_data() 内部创建 1 KB 以上的局部变量。
 * 通过 s_uart_read_mutex 保护。
 */
static app_uart_data_t s_read_cache;

/* ---------------- 内部函数声明 ---------------- */

static esp_err_t app_UART_init_internal(void);
static void app_UART_rx_task(void *pvParameters);
static TickType_t recv_wait_time_ticks(uart_port_t port);
static esp_err_t data_to_queue(const uint8_t *data, uint32_t len);

/* ---------------- 内部函数实现 ---------------- */

/**
 * @brief 估算接收一个硬件 FIFO 深度数据所需时间
 *
 * UART 通常按 1 起始位 + 8 数据位 + 1 停止位计算。
 * 这里保守按 11 bit 估算，并增加 15 ms 冗余。
 */
static TickType_t recv_wait_time_ticks(uart_port_t port)
{
    uint32_t baud = 0;

    if (uart_get_baudrate(port, &baud) == ESP_OK && baud > 0)
    {
        uint32_t time_ms = ((1000UL * 11UL * APP_UART_HW_FIFO_LEN) / baud) + 15UL;
        if (time_ms == 0)
        {
            time_ms = 1;
        }
        return pdMS_TO_TICKS(time_ms);
    }

    return pdMS_TO_TICKS(150);
}

/**
 * @brief 将接收数据复制到固定大小队列
 *
 * 这里不再 malloc。
 * 队列发送时，FreeRTOS 会把整个 app_uart_data_t 复制到队列内部。
 */
static esp_err_t data_to_queue(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0 || len > APP_UART_BLOCK_SIZE)
    {
        ESP_LOGW(TAG, "invalid queue data, len=%lu", (unsigned long)len);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_data_queue == NULL)
    {
        ESP_LOGE(TAG, "data queue not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_queue_cache.data_len = (uint16_t)len;
    memcpy(s_queue_cache.data, data, len);

    if (xQueueSend(s_data_queue, &s_queue_cache, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        ESP_LOGW(TAG, "data queue full, drop %lu bytes", (unsigned long)len);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

/**
 * @brief UART0 初始化
 */
static esp_err_t app_UART_init_internal(void)
{
    esp_err_t ret = ESP_OK;
    bool driver_installed = false;

    if (s_data_queue != NULL || s_uart_event_queue != NULL || s_uart_write_mutex != NULL)
    {
        ESP_LOGW(TAG, "UART already initialized");
        return ESP_OK;
    }

    uart_config_t uart_config = {
        .baud_rate = APP_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    /*
     * 注意：
     * intr_alloc_flags 这里使用 0。
     * 不建议给 uart_driver_install 传 ESP_INTR_FLAG_IRAM。
     */
    ret = uart_driver_install(APP_UART_PORT, APP_UART_RX_RING_BUF_SIZE, APP_UART_TX_RING_BUF_SIZE, APP_UART_EVENT_QUEUE_LEN, &s_uart_event_queue, 0);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    driver_installed = true;

    ret = uart_param_config(APP_UART_PORT, &uart_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = uart_set_pin(
        APP_UART_PORT,
        APP_UART_TXD,
        APP_UART_RXD,
        APP_UART_RTS,
        APP_UART_CTS);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    uart_flush_input(APP_UART_PORT);

    if (s_uart_event_queue != NULL)
    {
        xQueueReset(s_uart_event_queue);
    }

    /*
     * 固定大小数据队列。
     *
     * 每个队列元素包含：
     * uint16_t data_len + uint8_t data[1024]
     *
     * 队列长度为 10 时，队列本身大约占用 10 KB 以上 RAM。
     * 优点是不再频繁 malloc/free，运行稳定性更好。
     */
    s_data_queue = xQueueCreate(APP_UART_DATA_QUEUE_LEN, sizeof(app_uart_data_t));
    if (s_data_queue == NULL)
    {
        ESP_LOGE(TAG, "data queue create failed");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_uart_write_mutex = xSemaphoreCreateMutex();
    if (s_uart_write_mutex == NULL)
    {
        ESP_LOGE(TAG, "write mutex create failed");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_uart_read_mutex = xSemaphoreCreateMutex();
    if (s_uart_read_mutex == NULL)
    {
        ESP_LOGE(TAG, "read mutex create failed");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    if (xTaskCreatePinnedToCore(app_UART_rx_task,
                                "rs485_rx",
                                APP_UART_TASK_STACK_SIZE,
                                NULL,
                                APP_UART_TASK_PRIORITY,
                                NULL,
                                LG_APP_CPU_CORE) != pdTRUE)
    {
        ESP_LOGE(TAG, "UART0 rx task create failed");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    ESP_LOGI(TAG, "UART0 initialized successfully");
    return ESP_OK;

fail:
    if (s_uart_read_mutex != NULL)
    {
        vSemaphoreDelete(s_uart_read_mutex);
        s_uart_read_mutex = NULL;
    }

    if (s_uart_write_mutex != NULL)
    {
        vSemaphoreDelete(s_uart_write_mutex);
        s_uart_write_mutex = NULL;
    }

    if (s_data_queue != NULL)
    {
        vQueueDelete(s_data_queue);
        s_data_queue = NULL;
    }

    if (driver_installed)
    {
        uart_driver_delete(APP_UART_PORT);
    }

    s_uart_event_queue = NULL;

    return ret;
}

/**
 * @brief UART0 接收任务
 *
 * 逻辑：
 * 1. 等待 UART 驱动事件；
 * 2. 收到 UART_DATA 后，从 UART ring buffer 读取数据；
 * 3. 数据先累积到 s_rx_acc_buf；
 * 4. 满 APP_UART_BLOCK_SIZE 时立即入队；
 * 5. 短包或等待超时时，将已有数据作为一个数据块入队；
 * 6. FIFO 溢出、ring buffer 满、帧错误、校验错误时清空当前缓存。
 */
static void app_UART_rx_task(void *pvParameters)
{
    (void)pvParameters;

    uint32_t rx_bytes_num = 0;
    TickType_t wait_time = portMAX_DELAY;
    TickType_t fifo_wait_ticks = recv_wait_time_ticks(APP_UART_PORT);

    while (1)
    {
        uart_event_t event;

        if (xQueueReceive(s_uart_event_queue, &event, wait_time) == pdTRUE)
        {
            switch (event.type)
            {
            case UART_DATA:
            {
                size_t remain = event.size;

                while (remain > 0)
                {
                    size_t free_space = APP_UART_BLOCK_SIZE - rx_bytes_num;

                    if (free_space == 0)
                    {
                        if (data_to_queue(s_rx_acc_buf, rx_bytes_num) != ESP_OK)
                        {
                            ESP_LOGW(TAG, "queue full block failed");
                        }
                        rx_bytes_num = 0;
                        free_space = APP_UART_BLOCK_SIZE;
                    }

                    size_t to_read = remain < free_space ? remain : free_space;

                    int len = uart_read_bytes(
                        APP_UART_PORT,
                        s_rx_acc_buf + rx_bytes_num,
                        to_read,
                        pdMS_TO_TICKS(20));

                    if (len <= 0)
                    {
                        break;
                    }

                    rx_bytes_num += (uint32_t)len;
                    remain -= (size_t)len;

                    if (rx_bytes_num >= APP_UART_BLOCK_SIZE)
                    {
                        if (data_to_queue(s_rx_acc_buf, rx_bytes_num) != ESP_OK)
                        {
                            ESP_LOGW(TAG, "queue full block failed");
                        }
                        rx_bytes_num = 0;
                    }
                }

                /*
                 * 短包判断：
                 * 如果本次 UART_DATA 事件数据量小于硬件 FIFO 深度，
                 * 认为这很可能是一个短包，下一轮立即超时并提交。
                 */
                if (event.size < APP_UART_HW_FIFO_LEN)
                {
                    wait_time = 0;
                }
                else
                {
                    wait_time = fifo_wait_ticks;
                }

                break;
            }

            case UART_FIFO_OVF:
                ESP_LOGW(TAG, "UART hw fifo overflow");
                uart_flush_input(APP_UART_PORT);
                if (s_uart_event_queue != NULL)
                {
                    xQueueReset(s_uart_event_queue);
                }
                rx_bytes_num = 0;
                wait_time = portMAX_DELAY;
                break;

            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "UART ring buffer full");
                uart_flush_input(APP_UART_PORT);
                if (s_uart_event_queue != NULL)
                {
                    xQueueReset(s_uart_event_queue);
                }
                rx_bytes_num = 0;
                wait_time = portMAX_DELAY;
                break;

            case UART_BREAK:
                ESP_LOGW(TAG, "UART rx break");
                rx_bytes_num = 0;
                wait_time = portMAX_DELAY;
                break;

            case UART_PARITY_ERR:
                ESP_LOGW(TAG, "UART parity error");
                rx_bytes_num = 0;
                wait_time = portMAX_DELAY;
                break;

            case UART_FRAME_ERR:
                ESP_LOGW(TAG, "UART frame error");
                rx_bytes_num = 0;
                wait_time = portMAX_DELAY;
                break;

            case UART_PATTERN_DET:
                ESP_LOGI(TAG, "UART pattern detected");
                break;

#if defined(UART_DATA_BREAK)
            case UART_DATA_BREAK:
                ESP_LOGW(TAG, "UART data break");
                rx_bytes_num = 0;
                wait_time = portMAX_DELAY;
                break;
#endif

            default:
                ESP_LOGI(TAG, "UART event type: %d", event.type);
                break;
            }
        }
        else
        {
            /*
             * 事件等待超时：
             * 说明一段时间内没有新数据到来，将当前累积数据作为一包提交。
             */
            if (rx_bytes_num > 0)
            {
                if (data_to_queue(s_rx_acc_buf, rx_bytes_num) != ESP_OK)
                {
                    ESP_LOGW(TAG, "queue timeout block failed");
                }
                rx_bytes_num = 0;
            }

            wait_time = portMAX_DELAY;
        }
    }
}

/* ---------------- 对外 API ---------------- */

static bool IsInited = false;
esp_err_t app_RS485_init(void)
{
    if(IsInited)
    {
        ESP_LOGW(TAG, "RS485 module already initialized");
        return ESP_OK;
    }
    esp_err_t ret = app_UART_init_internal();

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "app_uart_init failed: %s", esp_err_to_name(ret));
        return -1;
    }
    IsInited=true;
    return 0;
}

static char Use_Write_Flag = 0; // 写标志
int app_write_RS485_data(void *pdata1, int len, uint32_t timeout)
{
    uint8_t *pdata = (uint8_t *)pdata1;
    if (pdata == NULL || len <= 0)
    {
        ESP_LOGE(TAG, "invalid write args");
        return -1;
    }

    if (s_uart_write_mutex == NULL)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return -1;
    }

    if (xSemaphoreTake(s_uart_write_mutex, timeout) != pdTRUE)
    {
        return -1;
    }

    int ret = uart_write_bytes(APP_UART_PORT, (const char *)pdata, (size_t)len);

    xSemaphoreGive(s_uart_write_mutex);

    Use_Write_Flag = 1;
    return ret;
}

int app_read_RS485_data(void *pdata1, int max_len, uint32_t timeout)
{
    uint8_t *pdata = (uint8_t *)pdata1;
    if (pdata == NULL || max_len <= 0)
    {
        ESP_LOGE(TAG, "invalid read args");
        return -1;
    }
    memset(pdata, 0, max_len); // 清空接收缓冲区

    if (s_data_queue == NULL || s_uart_read_mutex == NULL)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return -1;
    }

    /*
     * s_read_cache 是静态缓存，因此这里需要互斥保护。
     * 一般情况下只有一个任务读 UART 数据，但加锁后更安全。
     */
    if (xSemaphoreTake(s_uart_read_mutex, timeout) != pdTRUE)
    {
        return -1;
    }

    if (xQueueReceive(s_data_queue, &s_read_cache, timeout) != pdTRUE)
    {
        xSemaphoreGive(s_uart_read_mutex);
        if (Use_Write_Flag == 1)
        {
            Use_Write_Flag = 0;
            return -5;//特殊错误码表示发送成功但未收到响应
        }
        return -1;
    }
    else{
        Use_Write_Flag = 0;
    }

    int copy_len = s_read_cache.data_len;

    if (copy_len > max_len)
    {
        copy_len = max_len;
    }

    memcpy(pdata, s_read_cache.data, (size_t)copy_len);

    xSemaphoreGive(s_uart_read_mutex);

    return copy_len;
}