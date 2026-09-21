#ifndef MODBUS_JSON_PARSER_H
#define MODBUS_JSON_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <modbus_cmd.h>

#define PROTOCOL_STR_LEN_SHORT   16
#define PROTOCOL_STR_LEN_MID     32
#define PROTOCOL_STR_LEN_LONG    128

typedef enum
{
    PROTOCOL_TYPE_SCPI   = 0,
    PROTOCOL_TYPE_MODBUS = 1,
    PROTOCOL_TYPE_UNKNOWN = 255
} ProtocolType;

typedef enum
{
    ITEM_RW_UNKNOWN = 0,
    ITEM_RW_READ,
    ITEM_RW_WRITE,
    ITEM_RW_READ_WRITE
} ProtocolItemRW;

typedef struct
{
    char id[PROTOCOL_STR_LEN_MID];
    char name[PROTOCOL_STR_LEN_MID];
    char rw_str[PROTOCOL_STR_LEN_SHORT];
    char unit[PROTOCOL_STR_LEN_SHORT];
    char data_type[PROTOCOL_STR_LEN_SHORT];

    uint16_t reg_addr;
    uint16_t reg_len;

    double precision;
    double compensation;

    uint8_t is_unsigned;

    ProtocolItemRW rw;
} ProtocolItem;

typedef struct
{
    char id[PROTOCOL_STR_LEN_MID];
    char uuid[PROTOCOL_STR_LEN_MID];
    char name[PROTOCOL_STR_LEN_LONG];
    char description[PROTOCOL_STR_LEN_LONG];
    char protocol_type_str[PROTOCOL_STR_LEN_SHORT];

    ProtocolType protocol_type;

    ProtocolItem *items;
    size_t item_count;
} ProtocolInfo;

bool ModbusJson_Parse(const char *json_text, ProtocolInfo *out_info);
void ModbusJson_Free(ProtocolInfo *info);
void ModbusJson_Print(const ProtocolInfo *info);

const ProtocolItem *ModbusJson_FindItemByName(const ProtocolInfo *info, const char *name);
const ProtocolItem *ModbusJson_FindItemByID(const ProtocolInfo *info, const char *id);
const ProtocolItem *ModbusJson_FindItemByRegAddr(const ProtocolInfo *info, uint16_t reg_addr);

double ModbusJson_RawToPhysical(const ProtocolItem *item, int32_t raw_value);
int32_t ModbusJson_PhysicalToRaw(const ProtocolItem *item, double physical_value);

bool Modbus_Generate03_GetFrame(ProtocolInfo *modbusprotocol, const char *item_name, uint8_t slave_addr,ModbusFrame *out_frame);

#endif