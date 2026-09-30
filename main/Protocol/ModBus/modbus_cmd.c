#include "modbus_cmd.h"
#include "app_crc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool modbus_alloc_frame(ModbusFrame *out_frame, size_t len)
{
    if (!out_frame || len == 0)
        return false;

    out_frame->data = (uint8_t *)malloc(len);
    if (!out_frame->data)
    {
        out_frame->len = 0;
        return false;
    }

    memset(out_frame->data, 0, len);
    out_frame->len = len;
    return true;
}

bool Modbus_Generate03(uint8_t slave_addr, uint16_t reg_addr, uint16_t quantity, ModbusFrame *out_frame)
{
    if (!out_frame || quantity == 0)
        return false;

    if (!modbus_alloc_frame(out_frame, 8))
        return false;

    size_t index = 0;

    out_frame->data[index++] = slave_addr;
    out_frame->data[index++] = 0x03;
    out_frame->data[index++] = (uint8_t)((reg_addr >> 8) & 0xFF);
    out_frame->data[index++] = (uint8_t)(reg_addr & 0xFF);
    out_frame->data[index++] = (uint8_t)((quantity >> 8) & 0xFF);
    out_frame->data[index++] = (uint8_t)(quantity & 0xFF);

    uint16_t crc = Modbus_Crc16Cal(out_frame->data, index);

    out_frame->data[index++] = (uint8_t)(crc & 0xFF);
    out_frame->data[index++] = (uint8_t)((crc >> 8) & 0xFF);

    return true;
}

bool Modbus_Generate06(uint8_t slave_addr, uint16_t reg_addr, uint16_t value, ModbusFrame *out_frame)
{
    if (!out_frame)
        return false;

    if (!modbus_alloc_frame(out_frame, 8))
        return false;

    size_t index = 0;

    out_frame->data[index++] = slave_addr;
    out_frame->data[index++] = 0x06;
    out_frame->data[index++] = (uint8_t)((reg_addr >> 8) & 0xFF);
    out_frame->data[index++] = (uint8_t)(reg_addr & 0xFF);
    out_frame->data[index++] = (uint8_t)((value >> 8) & 0xFF);
    out_frame->data[index++] = (uint8_t)(value & 0xFF);

    uint16_t crc = Modbus_Crc16Cal(out_frame->data, index);

    out_frame->data[index++] = (uint8_t)(crc & 0xFF);
    out_frame->data[index++] = (uint8_t)((crc >> 8) & 0xFF);

    return true;
}

bool Modbus_Generate10(uint8_t slave_addr, uint16_t reg_addr, uint16_t quantity, const uint16_t *data, ModbusFrame *out_frame)
{
    if (!out_frame || !data || quantity == 0)
        return false;

    if (quantity > 123)
        return false;

    size_t frame_len = 9 + quantity * 2;

    if (!modbus_alloc_frame(out_frame, frame_len))
        return false;

    size_t index = 0;

    out_frame->data[index++] = slave_addr;
    out_frame->data[index++] = 0x10;
    out_frame->data[index++] = (uint8_t)((reg_addr >> 8) & 0xFF);
    out_frame->data[index++] = (uint8_t)(reg_addr & 0xFF);
    out_frame->data[index++] = (uint8_t)((quantity >> 8) & 0xFF);
    out_frame->data[index++] = (uint8_t)(quantity & 0xFF);
    out_frame->data[index++] = (uint8_t)(quantity * 2);

    for (uint16_t i = 0; i < quantity; i++)
    {
        out_frame->data[index++] = (uint8_t)((data[i] >> 8) & 0xFF);
        out_frame->data[index++] = (uint8_t)(data[i] & 0xFF);
    }

    uint16_t crc = Modbus_Crc16Cal(out_frame->data, index);

    out_frame->data[index++] = (uint8_t)(crc & 0xFF);
    out_frame->data[index++] = (uint8_t)((crc >> 8) & 0xFF);

    return true;
}

void Modbus_FreeFrame(ModbusFrame *frame)
{
    if (!frame)
        return;

    if (frame->data)
    {
        free(frame->data);
        frame->data = NULL;
    }

    frame->len = 0;
}

bool Modbus_CheckCrc(const uint8_t *frame, size_t len)
{
    if (!frame || len < 4)
        return false;

    uint16_t crc_calc = Modbus_Crc16Cal(frame, len - 2);
    uint16_t crc_recv = (uint16_t)frame[len - 2] | ((uint16_t)frame[len - 1] << 8);

    return crc_calc == crc_recv;
}

void Modbus_PrintFrame(const uint8_t *frame, size_t len, const char *title)
{
    if (title)
        printf("%s: ", title);

    if (!frame || len == 0)
    {
        printf("NULL\n");
        return;
    }

    for (size_t i = 0; i < len; i++)
    {
        printf("%02X ", frame[i]);
    }

    printf("\n");
}

static int32_t sign_extend(uint32_t value, uint8_t bits)
{
    if (bits == 16)
    {
        if (value & 0x8000)
            return (int32_t)(value | 0xFFFF0000);
        return (int32_t)value;
    }

    if (bits == 32)
    {
        return (int32_t)value;
    }

    return (int32_t)value;
}

bool Modbus_Parse03RawValue(const uint8_t *response,
                            size_t response_len,
                            uint16_t reg_len,
                            uint8_t is_unsigned,
                            int32_t *out_raw_value)
{
    if (!response || !out_raw_value || reg_len == 0)
        return false;

    if (response_len < 5)
        return false;

    if (!Modbus_CheckCrc(response, response_len))
    {
        printf("Modbus CRC check failed\n");
        return false;
    }

    uint8_t func = response[1];

    if (func & 0x80)
    {
        printf("Modbus exception response, func=0x%02X, exception=0x%02X\n", func, response[2]);
        return false;
    }

    if (func != 0x03)
        return false;

    uint8_t byte_count = response[2];
    size_t expected_len = 3 + byte_count + 2;

    if (response_len < expected_len)
        return false;

    if (byte_count < reg_len * 2)
        return false;

    if (reg_len == 1)
    {
        uint16_t raw16 = ((uint16_t)response[3] << 8) | response[4];

        if (is_unsigned)
            *out_raw_value = (int32_t)raw16;
        else
            *out_raw_value = sign_extend(raw16, 16);

        return true;
    }
    else if (reg_len == 2)
    {
        uint32_t raw32 = 0;

        raw32 |= ((uint32_t)response[3] << 24);
        raw32 |= ((uint32_t)response[4] << 16);
        raw32 |= ((uint32_t)response[5] << 8);
        raw32 |= ((uint32_t)response[6]);

        if (is_unsigned)
            *out_raw_value = (int32_t)raw32;
        else
            *out_raw_value = sign_extend(raw32, 32);

        return true;
    }

    printf("Unsupported register length: %u\n", reg_len);
    return false;
}

void Modbus_ParseResponseBasic(const uint8_t *response, size_t length)
{
    if (!response || length < 5)
    {
        printf("Invalid Modbus response\n");
        return;
    }

    if (!Modbus_CheckCrc(response, length))
    {
        printf("CRC error\n");
        return;
    }

    uint8_t slave_addr = response[0];
    uint8_t func_code = response[1];

    if (func_code & 0x80)
    {
        printf("Slave: %u, Exception Function: 0x%02X, Exception Code: 0x%02X\n",
               slave_addr,
               func_code,
               response[2]);
        return;
    }

    if (func_code == 0x03)
    {
        uint8_t byte_count = response[2];

        printf("Slave: %u, Function: 0x03, Byte Count: %u\n", slave_addr, byte_count);

        for (uint8_t i = 0; i < byte_count / 2; i++)
        {
            uint16_t reg_value = ((uint16_t)response[3 + i * 2] << 8) |
                                 response[4 + i * 2];

            printf("Register[%u] raw: %u, hex: 0x%04X\n", i, reg_value, reg_value);
        }
    }
    else if (func_code == 0x06 || func_code == 0x10)
    {
        if (length < 8)
        {
            printf("Invalid write response length\n");
            return;
        }

        uint16_t reg_addr = ((uint16_t)response[2] << 8) | response[3];
        uint16_t value_or_qty = ((uint16_t)response[4] << 8) | response[5];

        printf("Slave: %u, Function: 0x%02X, Register Address: %u, Value/Quantity: %u\n",
               slave_addr,
               func_code,
               reg_addr,
               value_or_qty);
    }
    else
    {
        printf("Unknown function code: 0x%02X\n", func_code);
    }
}


/**
 * @brief 从 Modbus 03 响应数据中解析出原始值（自动识别寄存器数量和符号）
 * 
 * @param response Modbus 响应数据（完整的帧，包含 CRC）
 * @param response_len 响应数据长度
 * @param out_raw_value 解析出的原始值（int32_t 类型）
 * @return true 解析成功
 * @return false 解析失败
 */
bool Modbus_Parse03Response_Simple(const uint8_t *response, size_t response_len, int32_t *out_raw_value)
{
    if (!response || !out_raw_value || response_len < 5)
        return false;

    // 1. CRC 校验
    if (!Modbus_CheckCrc(response, response_len))
    {
        printf("Modbus CRC check failed\n");
        return false;
    }

    uint8_t func = response[1];

    // 2. 检查异常响应
    if (func & 0x80)
    {
        printf("Modbus exception response, func=0x%02X, exception code=0x%02X\n", func, response[2]);
        return false;
    }

    // 3. 检查功能码
    if (func != 0x03)
    {
        printf("Invalid function code: 0x%02X, expected 0x03\n", func);
        return false;
    }

    // 4. 获取字节数
    uint8_t byte_count = response[2];
    size_t expected_len = 3 + byte_count + 2; // 地址+功能+字节数+数据+CRC

    if (response_len < expected_len)
    {
        printf("Response too short: %d < %d\n", (int)response_len, (int)expected_len);
        return false;
    }

    // 5. 根据字节数计算寄存器数量
    uint16_t reg_count = byte_count / 2;
    
    if (reg_count == 0 || byte_count % 2 != 0)
    {
        printf("Invalid byte count: %d\n", byte_count);
        return false;
    }

    // 6. 根据寄存器数量解析数据
    const uint8_t *data = response + 3; // 数据起始位置
    
    if (reg_count == 1)
    {
        // 16位数据
        uint16_t raw16 = ((uint16_t)data[0] << 8) | data[1];
        *out_raw_value = (int32_t)(int16_t)raw16; // 自动处理符号
        return true;
    }
    else if (reg_count == 2)
    {
        // 32位数据（大端模式）
        uint32_t raw32 = ((uint32_t)data[0] << 24) |
                         ((uint32_t)data[1] << 16) |
                         ((uint32_t)data[2] << 8)  |
                         ((uint32_t)data[3]);
        *out_raw_value = (int32_t)raw32;
        return true;
    }
    else
    {
        printf("Unsupported register count: %d (only 1 or 2 registers supported)\n", reg_count);
        return false;
    }
}

/**
 * 生成Modbus数据帧
 * @param str_data   输入：12字节字符串数据
 * @param long_data  输入：long类型数据（8字节，低位在前存储）
 * @param out_buf    输出：生成的完整数据帧（至少25字节）
 * @return           生成的帧总长度（25字节）
 */
int Modbus_BuildFrame_PN(const char* str_data, uint64_t long_data, uint8_t* out_buf)
{
    int idx = 0;

    // 第1~3字节：固定头
    out_buf[idx++] = 0x01;
    out_buf[idx++] = 0x03;
    out_buf[idx++] = 0x14;

    // 第4~15字节：字符串数据（12字节）
    memcpy(&out_buf[idx], str_data, 12);
    idx += 12;

    // 第16~23字节：long数据，低位在前（小端序）
    uint8_t* p_long = (uint8_t*)&long_data;
    for (int i = 0; i < 8; i++)
    {
        out_buf[idx++] = p_long[i];
    }

    // 第24~25字节：CRC校验（低字节在前）
    uint16_t crc = Modbus_Crc16Cal(out_buf, idx);
    out_buf[idx++] = crc & 0xFF;         // CRC低字节
    out_buf[idx++] = (crc >> 8) & 0xFF;  // CRC高字节

    return idx; // 返回25
}

int Modbus_BuildFrame_Type(const char* str_data, uint8_t* out_buf)
{
    int idx = 0;

    // 第1~3字节：固定头
    out_buf[idx++] = 0x01;
    out_buf[idx++] = 0x03;
    out_buf[idx++] = 0x0C;

    // 第4~15字节：字符串数据（12字节）
    memcpy(&out_buf[idx], str_data, 12);
    idx += 12;

    // 第24~25字节：CRC校验（低字节在前）
    uint16_t crc = Modbus_Crc16Cal(out_buf, idx);
    out_buf[idx++] = crc & 0xFF;         // CRC低字节
    out_buf[idx++] = (crc >> 8) & 0xFF;  // CRC高字节

    return idx; // 返回25
}


/**
 * 生成Modbus数据帧
 * @param value    输入：uint64_t类型数据
 * @param out_buf  输出：生成的完整数据帧（至少13字节）
 * @return         生成的帧总长度（13字节）
 */
int Modbus_BuildFrame_SafeCode(uint64_t value, uint8_t *out_buf)
{
    int idx = 0;

    // 第1~3字节：固定头
    out_buf[idx++] = 0x01;
    out_buf[idx++] = 0x03;
    out_buf[idx++] = 0x08;

    // 第4~11字节：uint64数据，低位在前（小端序）
    for (int i = 0; i < 8; i++)
    {
        out_buf[idx++] = (uint8_t)(value & 0xFF);
        value >>= 8;
    }

    // 第12~13字节：CRC校验（低字节在前）
    uint16_t crc = Modbus_Crc16Cal(out_buf, idx);
    out_buf[idx++] = crc & 0xFF;         // CRC低字节
    out_buf[idx++] = (crc >> 8) & 0xFF;  // CRC高字节

    return idx; // 返回13
}


uint64_t Modbus_ExtractU64(const uint8_t* buf)
{
    uint64_t value = 0;

    // 从第8个字节（索引7）开始，低位在前
    for (int i = 0; i < 8; i++)
    {
        value |= ((uint64_t)buf[7 + i]) << (i * 8);
    }

    return value;
}

/**
 * 生成 Modbus 指令：01 10 00 XX 00 04 CRC16
 * @param byte4    第4个字节
 * @param out_buf  输出缓冲区，至少8字节
 * @return         生成的帧长度，固定为8
 */
int Modbus_BuildCmd_0110(uint8_t byte4, uint8_t *out_buf)
{
    out_buf[0] = 0x01;
    out_buf[1] = 0x10;
    out_buf[2] = 0x00;
    out_buf[3] = byte4;
    out_buf[4] = 0x00;
    out_buf[5] = 0x04;

    uint16_t crc = Modbus_Crc16Cal(out_buf, 6);

    out_buf[6] = crc & 0xFF;
    out_buf[7] = (crc >> 8) & 0xFF;

    return 8;
}

