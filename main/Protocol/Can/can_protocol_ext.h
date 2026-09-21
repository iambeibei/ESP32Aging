#ifndef CAN_PROTOCOL_EXT_H
#define CAN_PROTOCOL_EXT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BluettiCAN 4.2 读写协议常量
 * 29 bit 扩展帧 ID: [28:26]P | [25]EDP | [24]DP | [23:16]PF | [15:8]PS | [7:0]SA
 * 4.2 节示例 0x086xXXYY 中，P=2、EDP=0、DP=0。
 */
#define CAN_EXT_PRIORITY_HIGHEST      0u
#define CAN_EXT_PRIORITY_HIGH         1u
#define CAN_EXT_PRIORITY_PROTOCOL     2u
#define CAN_EXT_PRIORITY_LOW          4u
#define CAN_EXT_PRIORITY_LOWEST       7u

/* 兼容旧代码命名：CAN 仲裁规则中数值越小优先级越高。 */
#define PRIORITY_CRITICAL             CAN_EXT_PRIORITY_HIGHEST
#define PRIORITY_HIGH                 CAN_EXT_PRIORITY_HIGH
#define PRIORITY_NORMAL               CAN_EXT_PRIORITY_PROTOCOL
#define PRIORITY_LOW                  CAN_EXT_PRIORITY_LOW
#define PRIORITY_IDLE                 CAN_EXT_PRIORITY_LOWEST

#define EDP                           0u
#define DP                            0u

#define CAN_EXT_FRAME_DATA_LEN        8u
#define CAN_EXT_FRAME_PAYLOAD_LEN     6u
#define CAN_EXT_DEFAULT_SOURCE_ADDR   0x10u
#define ADDR_MASTER                   CAN_EXT_DEFAULT_SOURCE_ADDR

/* PDU Format / PF */
#define WRITE_START                   0x60u
#define WRITE_DATA                    0x61u
#define WRITE_RESPONSE                0x62u
#define READ_START                    0x63u
#define READ_DATA_START               0x64u
#define READ_DATA_LOAD                0x65u
#define READ_DATA_RESPONSE            0x66u   /* 4.2.5 标注为“不发送”，默认不主动发送。 */

/* 数据类型示例 */
#define Factory_calibration_area      0xFFu
#define Certificate_Information       0xFEu

#define Sensor1                       0x01u
#define Sensor2                       0x02u

/* 写入应答错误码，Byte5-6，小端 */
typedef enum {
    CAN_PROTO_ACK_OK          = 0,
    CAN_PROTO_ACK_TYPE_ERR    = 1,
    CAN_PROTO_ACK_LEN_ERR     = 2,
    CAN_PROTO_ACK_DATA_LOST   = 3,
    CAN_PROTO_ACK_CRC_ERR     = 4,
    CAN_PROTO_ACK_PARAM_ERR   = 5,
    CAN_PROTO_ACK_TIMEOUT     = 6,
} can_proto_ack_code_t;

typedef enum {
    CAN_EXT_OP_NONE = 0,
    CAN_EXT_OP_READ,
    CAN_EXT_OP_WRITE,
} can_ext_operation_t;

/*
 * 给 app_write_Can_data/app_read_Can_data 使用的高层传输描述。
 * - READ：app_write_Can_data 发送 0x63；app_read_Can_data 接收 0x64+0x65 并重组 rx_payload。
 * - WRITE：app_write_Can_data 发送 0x60+0x61；app_read_Can_data 接收 0x62 写入应答。
 * 函数指针类型保持 int (*)(void *pdata, int len, uint32_t timeout)。
 */
typedef struct {
    can_ext_operation_t op;

    uint8_t target_addr;       /* 目标设备地址；读响应时也用于匹配 SA。 */
    uint8_t source_addr;       /* 主机地址；填 0 时默认 ADDR_MASTER。 */
    uint8_t data_type;         /* Byte0 数据类型。 */
    uint16_t offset;           /* Byte1-2，小端。 */
    uint16_t length;           /* 读请求长度；写入时为有效数据总长度。读长度为 0 表示从 offset 到末尾。 */

    const uint8_t *tx_payload; /* WRITE 有效负载，CRC 只计算该区域。 */
    uint16_t tx_len;

    uint8_t *rx_payload;       /* READ 接收缓冲区。 */
    uint16_t rx_capacity;
    uint16_t rx_len;

    uint16_t crc16;            /* WRITE：发送 CRC；READ：响应起始帧 CRC。 */
    uint16_t error_code;       /* WRITE_RESPONSE 错误码或本地解析错误。 */
} can_ext_transfer_t;

/* 旧代码仍在使用的 8 字节命令格式。 */
typedef struct {
    uint8_t DataType;
    uint8_t param[7];
} __attribute__((packed)) system_cmd_t;

static inline uint16_t can_proto_get_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline void can_proto_put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint8_t can_proto_source_or_default(uint8_t source_addr)
{
    return source_addr ? source_addr : ADDR_MASTER;
}

#ifdef __cplusplus
}
#endif

#endif /* CAN_PROTOCOL_EXT_H */
