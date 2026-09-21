#pragma once

#include "stdint.h"

uint16_t Xmodem_Crc16Cal(const uint8_t *ptr, int len);
uint16_t Modbus_Crc16Cal(const uint8_t *ptr, int len);

uint32_t calcu_crc32(uint32_t icrc, const uint8_t *data, int len);


