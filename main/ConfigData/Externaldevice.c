#include "Externaldevice.h"
#include "string.h"

DeviceProtocol string_to_device_protocol(const char* str) {
    if (strcmp(str, "MODBUS") == 0) return MODBUS;
    if (strcmp(str, "SCPI") == 0) return SCPI;
    return UNKNOWN_DEVICE_PROTOCOL;
}