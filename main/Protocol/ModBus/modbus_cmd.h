#ifndef MODBUS_CMD_H
#define MODBUS_CMD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct
{
    uint8_t *data;
    size_t len;
} ModbusFrame;

bool Modbus_Generate03(uint8_t slave_addr, uint16_t reg_addr, uint16_t quantity, ModbusFrame *out_frame);
bool Modbus_Generate06(uint8_t slave_addr, uint16_t reg_addr, uint16_t value, ModbusFrame *out_frame);
bool Modbus_Generate10(uint8_t slave_addr, uint16_t reg_addr, uint16_t quantity, const uint16_t *data, ModbusFrame *out_frame);

void Modbus_FreeFrame(ModbusFrame *frame);

bool Modbus_CheckCrc(const uint8_t *frame, size_t len);
void Modbus_PrintFrame(const uint8_t *frame, size_t len, const char *title);

bool Modbus_Parse03RawValue(const uint8_t *response,
                            size_t response_len,
                            uint16_t reg_len,
                            uint8_t is_unsigned,
                            int32_t *out_raw_value);

void Modbus_ParseResponseBasic(const uint8_t *response, size_t length);

bool Modbus_Parse03Response_Simple(const uint8_t *response, size_t response_len, int32_t *out_raw_value);

//点位自己的PN码
int Modbus_BuildFrame_PN(const char* str_data, uint64_t long_data, uint8_t* out_buf);
int Modbus_BuildFrame_Type(const char* str_data, uint8_t* out_buf);
int Modbus_BuildFrame_SafeCode(uint64_t value, uint8_t *out_buf);
uint64_t Modbus_ExtractU64(const uint8_t* buf);
int Modbus_BuildCmd_0110(uint8_t byte4, uint8_t *out_buf);
#endif