#ifndef CAN_EXTENDED_H
#define CAN_EXTENDED_H

#include "driver/twai.h"
#include "esp_err.h"
#include <inttypes.h>
#include <stdbool.h>
#include "can_protocol_ext.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define CAN_TX_PIN 1
#define CAN_RX_PIN 2

    /* 29 位扩展帧 ID: [28-26]P | [25]EDP | [24]DP | [23-16]PF | [15-8]PS | [7-0]SA */
    typedef union
    {
        uint32_t id;
        struct
        {
            uint32_t source_addr : 8;
            uint32_t pdu_specific : 8;
            uint32_t pdu_format : 8;
            uint32_t data_page : 1;
            uint32_t ext_data_page : 1;
            uint32_t priority : 3;
        } fields;
    } can_extended_id_t;

    typedef struct
    {
        can_extended_id_t ext_id;
        uint8_t data[CAN_EXT_FRAME_DATA_LEN];
        uint8_t data_len;
        bool is_remote;
    } can_message_ext_t;

    typedef void (*can_ext_receive_callback_t)(can_message_ext_t *msg);

    typedef struct
    {
        int tx_pin;
        int rx_pin;
        uint32_t baud_rate;
        twai_mode_t mode;
        uint32_t acceptance_code;
        uint32_t acceptance_mask;
    } can_ext_config_t;

    esp_err_t can_ext_driver_init(void);
    esp_err_t can_ext_driver_start(void);
    esp_err_t can_ext_driver_stop(void);
    esp_err_t can_ext_driver_uninstall(void);
    bool can_ext_is_started(void);

    esp_err_t can_ext_send_message(can_message_ext_t *msg, uint32_t timeout_ms);
    esp_err_t can_ext_receive_message(can_message_ext_t *msg, uint32_t timeout_ms);
    void can_ext_receive_task1(void *arg);
    void can_ext_print_id_info(uint32_t id);
    void send_control_command(uint8_t target_addr, uint8_t cmd, uint8_t value);

    /*
     * 当前工程统一通信函数指针接口：
     * int (*CurrentSendFunc)(void *pdata, int len, uint32_t timeout);
     * int (*CurrentRecvFunc)(void *pdata, int len, uint32_t timeout);
     *
     * 支持三种 pdata：
     * 1) can_ext_transfer_t：按 BluettiCAN 4.2 执行 READ/WRITE 高层传输；
     * 2) can_message_ext_t：发送/接收一帧扩展 CAN 消息；
     * 3) twai_message_t：兼容旧 appTask 的原始 TWAI 帧读取。
     */
    int app_write_Can_data(void *pdata, int len, uint32_t timeout);
    int app_read_Can_data(void *pdata, int max_len, uint32_t timeout);

    static inline uint32_t can_ext_create_id(uint8_t priority, uint8_t ext_data_page, uint8_t data_page, uint8_t pdu_format, uint8_t pdu_specific, uint8_t source_addr)
    {
        return ((uint32_t)(priority & 0x07u) << 26) |
               ((uint32_t)(ext_data_page & 0x01u) << 25) |
               ((uint32_t)(data_page & 0x01u) << 24) |
               ((uint32_t)(pdu_format & 0xFFu) << 16) |
               ((uint32_t)(pdu_specific & 0xFFu) << 8) |
               ((uint32_t)source_addr & 0xFFu);
    }

    static inline uint8_t can_ext_get_priority(uint32_t id)
    {
        return (uint8_t)((id >> 26) & 0x07u);
    }

    static inline uint8_t can_ext_get_ext_data_page(uint32_t id)
    {
        return (uint8_t)((id >> 25) & 0x01u);
    }

    static inline uint8_t can_ext_get_data_page(uint32_t id)
    {
        return (uint8_t)((id >> 24) & 0x01u);
    }

    static inline uint8_t can_ext_get_pdu_format(uint32_t id)
    {
        return (uint8_t)((id >> 16) & 0xFFu);
    }

    static inline uint8_t can_ext_get_pdu_specific(uint32_t id)
    {
        return (uint8_t)((id >> 8) & 0xFFu);
    }

    static inline uint8_t can_ext_get_source_addr(uint32_t id)
    {
        return (uint8_t)(id & 0xFFu);
    }

#ifdef __cplusplus
}
#endif

#endif /* CAN_EXTENDED_H */
