#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    AgingIdle = 0,// 空闲状态
    AgingStartCheck, // 开始检查状态
    AgingDeviceCheck, // 老化设备检查阶段，有蓝牙功能就连接蓝牙
    AgingAction, // 执行老化
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

/*
 * 联网恢复后补发未送达的"已完成任务"响应（device/%s/event/TaskCP）。
 * 由 MQTT_EVENT_CONNECTED 触发；内部幂等，无待补发内容时直接返回。
 */
void aging_resend_pending_replies(void);

void Config_Report(int cmd_seq);

int publish_device_mode(int mode);



extern bool IsEnc;

extern uint8_t AgingCMode; // 0表示单控模式，1表示群控模式

extern uint8_t CycleIndex; //老化循环次数

extern char RecordId[28];
#ifdef __cplusplus
}
#endif