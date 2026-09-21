#ifndef CAN_JSON_PARSER_H
#define CAN_JSON_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "can_extended.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_PROTOCOL_STR_LEN_SHORT 16
#define CAN_PROTOCOL_STR_LEN_MID   64
#define CAN_PROTOCOL_STR_LEN_LONG  128

typedef enum
{
    CAN_ITEM_RW_UNKNOWN = 0,
    CAN_ITEM_RW_READ,
    CAN_ITEM_RW_WRITE,
    CAN_ITEM_RW_READ_WRITE
} CanItemRW;

typedef enum
{
    CAN_ITEM_TYPE_UNKNOWN = 0,
    CAN_ITEM_TYPE_NUM,
    CAN_ITEM_TYPE_STRING
} CanItemValueType;

typedef struct
{
    char id[CAN_PROTOCOL_STR_LEN_MID];
    char name[CAN_PROTOCOL_STR_LEN_MID];
    char rw_str[CAN_PROTOCOL_STR_LEN_SHORT];
    char unit[CAN_PROTOCOL_STR_LEN_SHORT];
    char type_str[CAN_PROTOCOL_STR_LEN_SHORT];

    uint16_t data_type;      // JSON字段：数据类型
    uint16_t start;          // JSON字段：起始
    uint16_t len;            // JSON字段：长度

    double precision;        // JSON字段：精度
    double compensation;     // JSON字段：补偿
    uint8_t is_unsigned;     // JSON字段：无符号
    uint8_t bit_state;       // JSON字段：位状态，可选

    CanItemRW rw;
    CanItemValueType value_type;
} CanProtocolItem;

typedef struct
{
    char id[CAN_PROTOCOL_STR_LEN_MID];
    char uuid[CAN_PROTOCOL_STR_LEN_MID];
    char name[CAN_PROTOCOL_STR_LEN_LONG];
    char description[CAN_PROTOCOL_STR_LEN_LONG];
    char protocol_type_str[CAN_PROTOCOL_STR_LEN_SHORT];

    int protocol_type;

    CanProtocolItem *items;
    size_t item_count;
} CanProtocolInfo;

bool CanJson_Parse(const char *json_text, CanProtocolInfo *out_info);
void CanJson_Free(CanProtocolInfo *info);
void CanJson_Print(const CanProtocolInfo *info);

const CanProtocolItem *CanJson_FindItemByName(const CanProtocolInfo *info, const char *name);
const CanProtocolItem *CanJson_FindItemByID(const CanProtocolInfo *info, const char *id);

bool CanJson_ItemReadable(const CanProtocolItem *item);
bool CanJson_ItemWritable(const CanProtocolItem *item);

double CanJson_RawToPhysical(const CanProtocolItem *item, int64_t raw_value);
int64_t CanJson_PhysicalToRaw(const CanProtocolItem *item, double physical_value);

esp_err_t CanProtocol_BuildReadStart(const CanProtocolItem *item,
                                     uint8_t target_addr,
                                     can_message_ext_t *out_msg);

esp_err_t CanProtocol_BuildWriteStart(const CanProtocolItem *item,
                                      uint8_t target_addr,
                                      can_message_ext_t *out_msg);

esp_err_t CanProtocol_BuildWriteData(const CanProtocolItem *item,
                                     uint8_t target_addr,
                                     const char *param,
                                     can_message_ext_t *out_msg);

bool CanProtocol_DecodeNumValue(const CanProtocolItem *item,
                                const uint8_t *payload,
                                size_t payload_len,
                                double *out_value);

#ifdef __cplusplus
}
#endif

#endif // CAN_JSON_PARSER_H
