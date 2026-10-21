#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    AgingIdle = 0,// 空闲状态
    AgingStartCheck, // 开始检查状态
    AgingDeviceCheck, // 老化设备检查阶段，有蓝牙功能就连接蓝牙
    AgingAction, // 执行老化
    AgingDataCheck,//老化数据检测
    AgingComplete // 老化完成

} AgingProcessState;


typedef enum
{
    IdleState = 0, //空闲状态
    StandingState = 1, //静置状态
    RechargeState = 2, //充电
    DischargeState = 3 //放电
} AgingDataAcquisitionMode;


void app_task_init();

/** 任务上下文的 MQTT 服务建任务封装；复用现有句柄，不可在 ISR 调用。 */
BaseType_t app_create_mqtt_service_task(TaskFunction_t task_fn, TaskHandle_t *task_handle);

void Config_Report(int cmd_seq);

int publish_device_mode(int mode);



extern bool IsEnc;

extern uint8_t AgingCMode; // 0表示单控模式，1表示群控模式

extern uint8_t CycleIndex; //老化循环次数

extern char RecordId[28];
#ifdef __cplusplus
}
#endif