#pragma once

#include <stdint.h>
#include "stdbool.h"

#include "scpi_dynamic.h"
#include "modbus_json_parser.h"
#include "aging_config.h"
#include "can_json_parser.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // 设备状态枚举
    typedef enum
    {
        DeviceStatus_Online,
        DeviceStatus_Offline,
        DeviceStatus_Unknown

    } DeviceStatus;

    // 设备协议枚举
    typedef enum
    {
        MODBUS,
        SCPI,
        UNKNOWN_DEVICE_PROTOCOL
    } DeviceProtocol;

    typedef struct
    {
        char name[32];   // 设备名称
        int64_t ProtoID;     // 协议ID
        int LastProtoID; // 根据老化协议读取的协议ID

        char CommunicationMode[16]; // 通信方式
        char Protocol[16];          // 协议类型
        bool online;                // 在线状态,默认离线，后续通过在线检测更新状态
        bool hardware_initialized;  // 默认未初始化，后续根据实际情况更新状态

        bool HaveProtocol; // 默认未配置协议，后续根据实际情况更新状态

        scpi_protocol_t *scpi_protocol; // SCPI协议,
        ProtocolInfo modbusprotocol;    // Modbus协议
        CanProtocolInfo canprotocol;    // CAN 协议
        

        uint8_t ProtoType; // 协议类型,0表示SCPI协议，1表示Modbus协议，2表示Can协议，9表示未知

        int (*CurrentSendFunc)(void *pdata, int len, uint32_t timeout); // 设置当前发送函数的指针
        int (*CurrentRecvFunc)(void *pdata, int len, uint32_t timeout); // 设置当前接收函数的指针

        AgingStep *steps; // 老化协议步骤
        size_t step_count;// 老化协议步骤数量
    } Externaldevice;

    DeviceProtocol string_to_device_protocol(const char *str);

#ifdef __cplusplus
}
#endif