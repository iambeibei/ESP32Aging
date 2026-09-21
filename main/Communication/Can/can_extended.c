#include "can_extended.h"
#include "task_config.h"
#include "app_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "CAN_EXT";

static bool driver_installed = false;
static bool driver_started = false;
static can_ext_receive_callback_t receive_callback = NULL;
static QueueHandle_t rx_queue = NULL;
static SemaphoreHandle_t tx_mutex = NULL;
static TaskHandle_t rx_task_handle = NULL;
static char Use_Send_Flag = 0;

static can_ext_config_t can_config = {
    .tx_pin = CAN_TX_PIN,
    .rx_pin = CAN_RX_PIN,
    .baud_rate = 250000,
    .mode = TWAI_MODE_NORMAL,
    .acceptance_code = 0,
    .acceptance_mask = 0xFFFFFFFF,
};

static esp_err_t get_timing_config(uint32_t baud_rate, twai_timing_config_t *t_config)
{
    if (t_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (baud_rate) {
    case 25000:
        t_config->clk_src = TWAI_CLK_SRC_DEFAULT;
        t_config->quanta_resolution_hz = 625000;
        t_config->brp = 0;
        t_config->tseg_1 = 16;
        t_config->tseg_2 = 8;
        t_config->sjw = 3;
        t_config->triple_sampling = false;
        break;
    case 50000:
        t_config->clk_src = TWAI_CLK_SRC_DEFAULT;
        t_config->quanta_resolution_hz = 1000000;
        t_config->brp = 0;
        t_config->tseg_1 = 15;
        t_config->tseg_2 = 4;
        t_config->sjw = 3;
        t_config->triple_sampling = false;
        break;
    case 100000:
        t_config->clk_src = TWAI_CLK_SRC_DEFAULT;
        t_config->quanta_resolution_hz = 2000000;
        t_config->brp = 0;
        t_config->tseg_1 = 15;
        t_config->tseg_2 = 4;
        t_config->sjw = 3;
        t_config->triple_sampling = false;
        break;
    case 125000:
        t_config->clk_src = TWAI_CLK_SRC_DEFAULT;
        t_config->quanta_resolution_hz = 2000000;
        t_config->brp = 0;
        t_config->tseg_1 = 11;
        t_config->tseg_2 = 4;
        t_config->sjw = 3;
        t_config->triple_sampling = false;
        break;
    case 250000:
        t_config->clk_src = TWAI_CLK_SRC_DEFAULT;
        t_config->quanta_resolution_hz = 4000000;
        t_config->brp = 0;
        t_config->tseg_1 = 11;
        t_config->tseg_2 = 4;
        t_config->sjw = 2;
        t_config->triple_sampling = false;
        break;
    case 500000:
        t_config->clk_src = TWAI_CLK_SRC_DEFAULT;
        t_config->quanta_resolution_hz = 8000000;
        t_config->brp = 0;
        t_config->tseg_1 = 11;
        t_config->tseg_2 = 4;
        t_config->sjw = 2;
        t_config->triple_sampling = false;
        break;
    case 800000:
        t_config->clk_src = TWAI_CLK_SRC_DEFAULT;
        t_config->quanta_resolution_hz = 8000000;
        t_config->brp = 0;
        t_config->tseg_1 = 6;
        t_config->tseg_2 = 3;
        t_config->sjw = 1;
        t_config->triple_sampling = false;
        break;
    case 1000000:
        t_config->clk_src = TWAI_CLK_SRC_DEFAULT;
        t_config->quanta_resolution_hz = 8000000;
        t_config->brp = 0;
        t_config->tseg_1 = 5;
        t_config->tseg_2 = 2;
        t_config->sjw = 1;
        t_config->triple_sampling = false;
        break;
    default:
        ESP_LOGE(TAG, "Unsupported baud rate: %" PRIu32, baud_rate);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

static void can_msg_to_twai(const can_message_ext_t *src, twai_message_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->identifier = src->ext_id.id;
    dst->data_length_code = src->data_len > CAN_EXT_FRAME_DATA_LEN ? CAN_EXT_FRAME_DATA_LEN : src->data_len;
    dst->rtr = src->is_remote;
    dst->self = 0;
    dst->flags = TWAI_MSG_FLAG_EXTD;

    if (!src->is_remote && dst->data_length_code > 0) {
        memcpy(dst->data, src->data, dst->data_length_code);
    }
}

static void twai_to_can_msg(const twai_message_t *src, can_message_ext_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->ext_id.id = src->identifier;
    dst->data_len = src->data_length_code > CAN_EXT_FRAME_DATA_LEN ? CAN_EXT_FRAME_DATA_LEN : src->data_length_code;
    dst->is_remote = src->rtr;

    if (!src->rtr && dst->data_len > 0) {
        memcpy(dst->data, src->data, dst->data_len);
    }
}

static TickType_t can_timeout_ms_to_ticks(uint32_t timeout_ms)
{
    return timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
}

static esp_err_t can_send_twai_ticks(const twai_message_t *msg, TickType_t timeout_ticks)
{
    if (msg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!driver_installed || !driver_started) {
        ESP_LOGE(TAG, "Driver not ready");
        return ESP_ERR_INVALID_STATE;
    }

    if (tx_mutex != NULL) {
        if (xSemaphoreTake(tx_mutex, timeout_ticks) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }

    esp_err_t ret = twai_transmit(msg, timeout_ticks);

    if (tx_mutex != NULL) {
        xSemaphoreGive(tx_mutex);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CAN transmit failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t can_send_msg_ticks(const can_message_ext_t *msg, TickType_t timeout_ticks)
{
    if (msg == NULL || msg->data_len > CAN_EXT_FRAME_DATA_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    twai_message_t twai_msg;
    can_msg_to_twai(msg, &twai_msg);
    return can_send_twai_ticks(&twai_msg, timeout_ticks);
}

static int can_recv_twai_from_queue(twai_message_t *msg, TickType_t timeout_ticks)
{
    if (msg == NULL) {
        return -1;
    }

    if (!driver_installed || !driver_started || rx_queue == NULL) {
        ESP_LOGE(TAG, "CAN RX queue not ready");
        return -1;
    }

    if (xQueueReceive(rx_queue, msg, timeout_ticks) != pdTRUE) {
        if (Use_Send_Flag == 1) {
            Use_Send_Flag = 0;
            return -5;
        }
        return -1;
    }

    Use_Send_Flag = 0;
    return msg->data_length_code;
}

static int can_recv_msg_from_queue(can_message_ext_t *msg, TickType_t timeout_ticks)
{
    twai_message_t twai_msg;
    int ret = can_recv_twai_from_queue(&twai_msg, timeout_ticks);
    if (ret < 0) {
        return ret;
    }

    twai_to_can_msg(&twai_msg, msg);
    return msg->data_len;
}

static void can_ext_flush_rx_queue(void)
{
    if (rx_queue == NULL) {
        return;
    }

    twai_message_t dump;
    while (xQueueReceive(rx_queue, &dump, 0) == pdTRUE) {
    }
}

static TickType_t remaining_ticks(TickType_t deadline, TickType_t start_timeout)
{
    if (start_timeout == portMAX_DELAY) {
        return portMAX_DELAY;
    }

    TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - deadline) < (TickType_t)0x80000000u) {
        return 0;
    }
    return deadline - now;
}

static bool can_match_response_id(uint32_t id, const can_ext_transfer_t *transfer, uint8_t pf)
{
    uint8_t expected_master = can_proto_source_or_default(transfer->source_addr);
    uint8_t expected_slave = transfer->target_addr;

    if (can_ext_get_pdu_format(id) != pf) {
        return false;
    }

    if (can_ext_get_ext_data_page(id) != EDP || can_ext_get_data_page(id) != DP) {
        return false;
    }

    /* 响应方向：PS=主机地址，SA=从机地址。 */
    if (can_ext_get_pdu_specific(id) != expected_master) {
        return false;
    }

    if (expected_slave != 0 && can_ext_get_source_addr(id) != expected_slave) {
        return false;
    }

    return true;
}

static void can_fill_common_frame(can_message_ext_t *msg, uint8_t pf,
                                  uint8_t target_addr, uint8_t source_addr)
{
    memset(msg, 0, sizeof(*msg));
    msg->ext_id.id = can_ext_create_id(CAN_EXT_PRIORITY_PROTOCOL, EDP, DP,
                                       pf, target_addr,
                                       can_proto_source_or_default(source_addr));
    msg->data_len = CAN_EXT_FRAME_DATA_LEN;
    msg->is_remote = false;
}

static int can_send_read_start(const can_ext_transfer_t *transfer, TickType_t timeout_ticks)
{
    can_message_ext_t msg;
    can_fill_common_frame(&msg, READ_START, transfer->target_addr, transfer->source_addr);

    msg.data[0] = transfer->data_type;
    can_proto_put_u16_le(&msg.data[1], transfer->offset);
    can_proto_put_u16_le(&msg.data[3], transfer->length);
    msg.data[5] = 0;
    msg.data[6] = 0;
    msg.data[7] = 0;

    esp_err_t ret = can_send_msg_ticks(&msg, timeout_ticks);
    if (ret != ESP_OK) {
        return -1;
    }

    Use_Send_Flag = 1;
    return (int)msg.data_len;
}

static int can_send_write_sequence(can_ext_transfer_t *transfer, TickType_t timeout_ticks)
{
    if (transfer->tx_payload == NULL || transfer->tx_len == 0) {
        ESP_LOGE(TAG, "WRITE payload is empty");
        return -1;
    }

    uint16_t total_len = transfer->length ? transfer->length : transfer->tx_len;
    if (total_len > transfer->tx_len) {
        ESP_LOGE(TAG, "WRITE length exceeds tx_len: length=%u tx_len=%u", total_len, transfer->tx_len);
        return -1;
    }

    transfer->length = total_len;
    transfer->crc16 = Modbus_Crc16Cal(transfer->tx_payload, total_len);
    transfer->error_code = CAN_PROTO_ACK_OK;

    can_message_ext_t msg;
    can_fill_common_frame(&msg, WRITE_START, transfer->target_addr, transfer->source_addr);
    msg.data[0] = transfer->data_type;
    can_proto_put_u16_le(&msg.data[1], transfer->offset);
    can_proto_put_u16_le(&msg.data[3], total_len);
    can_proto_put_u16_le(&msg.data[5], transfer->crc16);
    msg.data[7] = 0;

    if (can_send_msg_ticks(&msg, timeout_ticks) != ESP_OK) {
        return -1;
    }

    int sent_bytes = msg.data_len;
    uint16_t copied = 0;
    uint8_t packet_no = 0;

    while (copied < total_len) {
        uint16_t chunk = total_len - copied;
        if (chunk > CAN_EXT_FRAME_PAYLOAD_LEN) {
            chunk = CAN_EXT_FRAME_PAYLOAD_LEN;
        }

        can_fill_common_frame(&msg, WRITE_DATA, transfer->target_addr, transfer->source_addr);
        msg.data[0] = transfer->data_type;
        msg.data[1] = packet_no++;
        memcpy(&msg.data[2], &transfer->tx_payload[copied], chunk);

        if (can_send_msg_ticks(&msg, timeout_ticks) != ESP_OK) {
            return -1;
        }

        copied += chunk;
        sent_bytes += msg.data_len;
    }

    Use_Send_Flag = 1;
    return sent_bytes;
}

static int can_receive_write_response(can_ext_transfer_t *transfer, TickType_t timeout_ticks)
{
    TickType_t deadline = xTaskGetTickCount() + timeout_ticks;

    while (1) {
        TickType_t wait_ticks = remaining_ticks(deadline, timeout_ticks);
        if (wait_ticks == 0 && timeout_ticks != portMAX_DELAY) {
            transfer->error_code = CAN_PROTO_ACK_TIMEOUT;
            Use_Send_Flag = 0;
            return -5;
        }

        can_message_ext_t msg;
        int ret = can_recv_msg_from_queue(&msg, wait_ticks);
        if (ret < 0) {
            transfer->error_code = CAN_PROTO_ACK_TIMEOUT;
            return ret;
        }

        if (!can_match_response_id(msg.ext_id.id, transfer, WRITE_RESPONSE)) {
            continue;
        }

        if (msg.data_len < CAN_EXT_FRAME_DATA_LEN || msg.data[0] != transfer->data_type) {
            continue;
        }

        uint16_t offset = can_proto_get_u16_le(&msg.data[1]);
        uint16_t length = can_proto_get_u16_le(&msg.data[3]);
        uint16_t err = can_proto_get_u16_le(&msg.data[5]);

        transfer->error_code = err;

        if (offset != transfer->offset || length != transfer->length) {
            ESP_LOGE(TAG, "WRITE_RESPONSE mismatch offset/len: resp_offset=%u resp_len=%u expect_offset=%u expect_len=%u",
                     offset, length, transfer->offset, transfer->length);
            return -1;
        }

        if (err != CAN_PROTO_ACK_OK) {
            ESP_LOGE(TAG, "WRITE_RESPONSE error code=%u", err);
            return -(int)err;
        }

        return CAN_EXT_FRAME_DATA_LEN;
    }
}

static int can_receive_read_payload(can_ext_transfer_t *transfer, TickType_t timeout_ticks)
{
    if (transfer->rx_payload == NULL || transfer->rx_capacity == 0) {
        ESP_LOGE(TAG, "READ rx buffer is invalid");
        return -1;
    }

    transfer->rx_len = 0;
    transfer->error_code = CAN_PROTO_ACK_OK;

    TickType_t deadline = xTaskGetTickCount() + timeout_ticks;
    uint16_t expected_len = 0;
    uint8_t expected_packet_no = 0;
    bool got_start = false;

    while (!got_start) {
        TickType_t wait_ticks = remaining_ticks(deadline, timeout_ticks);
        if (wait_ticks == 0 && timeout_ticks != portMAX_DELAY) {
            transfer->error_code = CAN_PROTO_ACK_TIMEOUT;
            Use_Send_Flag = 0;
            return -5;
        }

        can_message_ext_t msg;
        int ret = can_recv_msg_from_queue(&msg, wait_ticks);
        if (ret < 0) {
            transfer->error_code = CAN_PROTO_ACK_TIMEOUT;
            return ret;
        }

        if (!can_match_response_id(msg.ext_id.id, transfer, READ_DATA_START)) {
            continue;
        }

        if (msg.data_len < CAN_EXT_FRAME_DATA_LEN || msg.data[0] != transfer->data_type) {
            continue;
        }

        uint16_t offset = can_proto_get_u16_le(&msg.data[1]);
        expected_len = can_proto_get_u16_le(&msg.data[3]);
        transfer->crc16 = can_proto_get_u16_le(&msg.data[5]);

        if (offset != transfer->offset) {
            ESP_LOGE(TAG, "READ_DATA_START offset mismatch: resp=%u expect=%u", offset, transfer->offset);
            transfer->error_code = CAN_PROTO_ACK_PARAM_ERR;
            return -1;
        }

        if (expected_len > transfer->rx_capacity) {
            ESP_LOGE(TAG, "READ payload too large: len=%u cap=%u", expected_len, transfer->rx_capacity);
            transfer->error_code = CAN_PROTO_ACK_LEN_ERR;
            return -1;
        }

        transfer->length = expected_len;
        got_start = true;
    }

    while (transfer->rx_len < expected_len) {
        TickType_t wait_ticks = remaining_ticks(deadline, timeout_ticks);
        if (wait_ticks == 0 && timeout_ticks != portMAX_DELAY) {
            transfer->error_code = CAN_PROTO_ACK_TIMEOUT;
            Use_Send_Flag = 0;
            return -5;
        }

        can_message_ext_t msg;
        int ret = can_recv_msg_from_queue(&msg, wait_ticks);
        if (ret < 0) {
            transfer->error_code = CAN_PROTO_ACK_TIMEOUT;
            return ret;
        }

        if (!can_match_response_id(msg.ext_id.id, transfer, READ_DATA_LOAD)) {
            continue;
        }

        if (msg.data_len < 2 || msg.data[0] != transfer->data_type) {
            continue;
        }

        if (msg.data[1] != expected_packet_no) {
            ESP_LOGE(TAG, "READ packet_no mismatch: expect=%u got=%u", expected_packet_no, msg.data[1]);
            transfer->error_code = CAN_PROTO_ACK_DATA_LOST;
            return -1;
        }

        uint16_t remain = expected_len - transfer->rx_len;
        uint16_t chunk = remain > CAN_EXT_FRAME_PAYLOAD_LEN ? CAN_EXT_FRAME_PAYLOAD_LEN : remain;
        uint16_t frame_payload_len = msg.data_len > 2 ? (uint16_t)(msg.data_len - 2) : 0;

        if (frame_payload_len < chunk) {
            ESP_LOGE(TAG, "READ frame payload too short: frame=%u need=%u", frame_payload_len, chunk);
            transfer->error_code = CAN_PROTO_ACK_DATA_LOST;
            return -1;
        }

        memcpy(&transfer->rx_payload[transfer->rx_len], &msg.data[2], chunk);
        transfer->rx_len += chunk;
        expected_packet_no++;
    }

    uint16_t calc_crc = Modbus_Crc16Cal(transfer->rx_payload, transfer->rx_len);
    if (calc_crc != transfer->crc16) {
        ESP_LOGE(TAG, "READ CRC mismatch: calc=0x%04X resp=0x%04X", calc_crc, transfer->crc16);
        transfer->error_code = CAN_PROTO_ACK_CRC_ERR;
        return -1;
    }

    return transfer->rx_len;
}

void can_ext_print_id_info(uint32_t id)
{
    ESP_LOGI(TAG, "=== CAN Extended ID: 0x%08" PRIX32 " ===", id);
    ESP_LOGI(TAG, "  Priority:      %u (0=highest)", can_ext_get_priority(id));
    ESP_LOGI(TAG, "  Ext Data Page: %u", can_ext_get_ext_data_page(id));
    ESP_LOGI(TAG, "  Data Page:     %u", can_ext_get_data_page(id));
    ESP_LOGI(TAG, "  PDU Format:    0x%02X", can_ext_get_pdu_format(id));
    ESP_LOGI(TAG, "  PDU Specific:  0x%02X", can_ext_get_pdu_specific(id));
    ESP_LOGI(TAG, "  Source Addr:   0x%02X", can_ext_get_source_addr(id));
}

esp_err_t can_ext_driver_init(void)
{
    if (driver_installed) {
        ESP_LOGW(TAG, "Driver already installed");
        return ESP_OK;
    }

    twai_filter_config_t f_config = {
        .acceptance_code = can_config.acceptance_code,
        .acceptance_mask = can_config.acceptance_mask,
        .single_filter = true,
    };

    twai_timing_config_t t_config;
    esp_err_t ret = get_timing_config(can_config.baud_rate, &t_config);
    if (ret != ESP_OK) {
        return ret;
    }

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(can_config.tx_pin, can_config.rx_pin, can_config.mode);

    ret = twai_driver_install(&g_config, &t_config, &f_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install driver: %s", esp_err_to_name(ret));
        return ret;
    }

    rx_queue = xQueueCreate(20, sizeof(twai_message_t));
    if (rx_queue == NULL) {
        twai_driver_uninstall();
        ESP_LOGE(TAG, "RX queue create failed");
        return ESP_ERR_NO_MEM;
    }

    tx_mutex = xSemaphoreCreateMutex();
    if (tx_mutex == NULL) {
        vQueueDelete(rx_queue);
        rx_queue = NULL;
        twai_driver_uninstall();
        ESP_LOGE(TAG, "TX mutex create failed");
        return ESP_ERR_NO_MEM;
    }

    driver_installed = true;
    ESP_LOGI(TAG, "CAN Extended driver installed");
    ESP_LOGI(TAG, "  TX: GPIO%d, RX: GPIO%d", can_config.tx_pin, can_config.rx_pin);
    ESP_LOGI(TAG, "  Baud Rate: %" PRIu32 " bps", can_config.baud_rate);

    return ESP_OK;
}

esp_err_t can_ext_driver_start(void)
{
    if (!driver_installed) {
        ESP_LOGE(TAG, "Driver not installed");
        return ESP_ERR_INVALID_STATE;
    }

    if (driver_started) {
        ESP_LOGW(TAG, "Driver already started");
        return ESP_OK;
    }

    esp_err_t ret = twai_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start: %s", esp_err_to_name(ret));
        return ret;
    }

    driver_started = true;
    can_ext_flush_rx_queue();

    if (rx_task_handle == NULL) {
        BaseType_t task_ret = xTaskCreatePinnedToCore(can_ext_receive_task1,
                                                       "can_rx",
                                                       LG_STACK_CAN_RX,
                                                       NULL,
                                                       LG_PRIO_CAN_RX,
                                                       &rx_task_handle,
                                                       LG_APP_CPU_CORE);
        if (task_ret != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create CAN receive task");
            twai_stop();
            driver_started = false;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "CAN Extended driver started");
    return ESP_OK;
}

esp_err_t can_ext_driver_stop(void)
{
    if (!driver_installed) {
        ESP_LOGE(TAG, "Driver not installed");
        return ESP_ERR_INVALID_STATE;
    }

    if (!driver_started) {
        ESP_LOGW(TAG, "Driver already stopped");
        return ESP_OK;
    }

    if (rx_task_handle != NULL) {
        vTaskDelete(rx_task_handle);
        rx_task_handle = NULL;
    }

    esp_err_t ret = twai_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop: %s", esp_err_to_name(ret));
        return ret;
    }

    driver_started = false;
    ESP_LOGI(TAG, "CAN Extended driver stopped");
    return ESP_OK;
}

esp_err_t can_ext_driver_uninstall(void)
{
    if (!driver_installed) {
        ESP_LOGW(TAG, "Driver not installed");
        return ESP_OK;
    }

    if (driver_started) {
        can_ext_driver_stop();
    }

    if (rx_queue != NULL) {
        vQueueDelete(rx_queue);
        rx_queue = NULL;
    }

    if (tx_mutex != NULL) {
        vSemaphoreDelete(tx_mutex);
        tx_mutex = NULL;
    }

    esp_err_t ret = twai_driver_uninstall();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to uninstall: %s", esp_err_to_name(ret));
        return ret;
    }

    driver_installed = false;
    ESP_LOGI(TAG, "CAN Extended driver uninstalled");
    return ESP_OK;
}

bool can_ext_is_started(void)
{
    return driver_started;
}

esp_err_t can_ext_send_message(can_message_ext_t *msg, uint32_t timeout_ms)
{
    esp_err_t ret = can_send_msg_ticks(msg, can_timeout_ms_to_ticks(timeout_ms));
    if (ret == ESP_OK) {
        Use_Send_Flag = 1;
        ESP_LOGD(TAG, "Sent: ID=0x%08" PRIX32 ", DLC=%u", msg->ext_id.id, msg->data_len);
    }
    return ret;
}

esp_err_t can_ext_receive_message(can_message_ext_t *msg, uint32_t timeout_ms)
{
    int ret = can_recv_msg_from_queue(msg, can_timeout_ms_to_ticks(timeout_ms));
    return ret >= 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

int app_write_Can_data(void *pdata, int len, uint32_t timeout)
{
    if (pdata == NULL || len <= 0) {
        ESP_LOGE(TAG, "invalid CAN write args");
        return -1;
    }

    TickType_t timeout_ticks = (TickType_t)timeout;

    if (len == (int)sizeof(can_ext_transfer_t)) {
        can_ext_transfer_t *transfer = (can_ext_transfer_t *)pdata;
        can_ext_flush_rx_queue();

        if (transfer->op == CAN_EXT_OP_READ) {
            return can_send_read_start(transfer, timeout_ticks);
        }

        if (transfer->op == CAN_EXT_OP_WRITE) {
            return can_send_write_sequence(transfer, timeout_ticks);
        }

        ESP_LOGE(TAG, "unsupported CAN transfer op=%d", transfer->op);
        return -1;
    }

    if (len == (int)sizeof(can_message_ext_t)) {
        can_message_ext_t *msg = (can_message_ext_t *)pdata;
        esp_err_t ret = can_send_msg_ticks(msg, timeout_ticks);
        if (ret == ESP_OK) {
            Use_Send_Flag = 1;
            return msg->data_len;
        }
        return -1;
    }

    if (len == (int)sizeof(twai_message_t)) {
        twai_message_t *msg = (twai_message_t *)pdata;
        esp_err_t ret = can_send_twai_ticks(msg, timeout_ticks);
        if (ret == ESP_OK) {
            Use_Send_Flag = 1;
            return msg->data_length_code;
        }
        return -1;
    }

    ESP_LOGE(TAG, "CAN write requires can_ext_transfer_t/can_message_ext_t/twai_message_t, len=%d", len);
    return -1;
}

int app_read_Can_data(void *pdata, int max_len, uint32_t timeout)
{
    if (pdata == NULL || max_len <= 0) {
        ESP_LOGE(TAG, "invalid CAN read args");
        return -1;
    }

    TickType_t timeout_ticks = (TickType_t)timeout;

    if (max_len == (int)sizeof(can_ext_transfer_t)) {
        can_ext_transfer_t *transfer = (can_ext_transfer_t *)pdata;

        if (transfer->op == CAN_EXT_OP_READ) {
            return can_receive_read_payload(transfer, timeout_ticks);
        }

        if (transfer->op == CAN_EXT_OP_WRITE) {
            return can_receive_write_response(transfer, timeout_ticks);
        }

        ESP_LOGE(TAG, "unsupported CAN transfer op=%d", transfer->op);
        return -1;
    }

    if (max_len == (int)sizeof(can_message_ext_t)) {
        can_message_ext_t *msg = (can_message_ext_t *)pdata;
        return can_recv_msg_from_queue(msg, timeout_ticks);
    }

    if (max_len == (int)sizeof(twai_message_t)) {
        twai_message_t *msg = (twai_message_t *)pdata;
        return can_recv_twai_from_queue(msg, timeout_ticks);
    }

    ESP_LOGE(TAG, "CAN read requires can_ext_transfer_t/can_message_ext_t/twai_message_t, len=%d", max_len);
    return -1;
}

void can_ext_receive_task1(void *arg)
{
    twai_message_t twai_msg;
    ESP_LOGI(TAG, "CAN receive task started");

    while (1) {
        memset(&twai_msg, 0, sizeof(twai_msg));
        esp_err_t ret = twai_receive(&twai_msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "twai_receive failed: %s", esp_err_to_name(ret));
            continue;
        }

        if (rx_queue != NULL) {
            if (xQueueSend(rx_queue, &twai_msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "CAN RX queue full, message dropped: ID=0x%08" PRIX32, twai_msg.identifier);
            }
        }

        if (receive_callback != NULL) {
            can_message_ext_t can_msg;
            twai_to_can_msg(&twai_msg, &can_msg);
            receive_callback(&can_msg);
        }
    }
}

void send_control_command(uint8_t target_addr, uint8_t DataType, uint8_t value)
{
    (void)value;

    can_message_ext_t msg;
    can_fill_common_frame(&msg, READ_START, target_addr, ADDR_MASTER);

    msg.data[0] = DataType;
    can_proto_put_u16_le(&msg.data[1], 0x0004);
    can_proto_put_u16_le(&msg.data[3], 0x000C);
    msg.data[5] = 0;
    msg.data[6] = 0;
    msg.data[7] = 0;

    can_ext_send_message(&msg, 1000);
    ESP_LOGI(TAG, "Control command sent to 0x%02X: DataType=%u", target_addr, DataType);
}

esp_err_t can_ext_send_and_receive(uint8_t target_addr,
                                   uint8_t cmd,
                                   uint8_t *send_data,
                                   uint8_t send_len,
                                   uint8_t *recv_data,
                                   uint8_t *recv_len,
                                   uint32_t timeout_ms)
{
    if (send_data == NULL || recv_data == NULL || recv_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    can_ext_transfer_t transfer;
    memset(&transfer, 0, sizeof(transfer));
    transfer.op = CAN_EXT_OP_WRITE;
    transfer.target_addr = target_addr;
    transfer.source_addr = ADDR_MASTER;
    transfer.data_type = cmd;
    transfer.offset = 0;
    transfer.length = send_len;
    transfer.tx_payload = send_data;
    transfer.tx_len = send_len;

    int written = app_write_Can_data(&transfer, sizeof(transfer), pdMS_TO_TICKS(timeout_ms));
    if (written <= 0) {
        return ESP_FAIL;
    }

    int ack = app_read_Can_data(&transfer, sizeof(transfer), pdMS_TO_TICKS(timeout_ms));
    if (ack <= 0) {
        return ESP_ERR_TIMEOUT;
    }

    *recv_len = 0;
    return ESP_OK;
}
