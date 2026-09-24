#ifndef LIGHT_GATEWAY_TASK_CONFIG_H
#define LIGHT_GATEWAY_TASK_CONFIG_H

#include "sdkconfig.h"

/*
 * ESP32-S3 双核任务分配：
 * CPU0：ESP-IDF Wi-Fi、Bluedroid、系统网络任务为主。
 * CPU1：本项目自定义通信接收、老化控制、协议解析和数据处理任务。
 *
 * 当前工程 sdkconfig 已关闭 CONFIG_FREERTOS_UNICORE。
 * 若以后误改为单核配置，直接在编译阶段报错，避免把 Core 1 写死后运行异常。
 */
#if CONFIG_FREERTOS_UNICORE
#error "Light_Gateway CPU1 task layout requires dual-core FreeRTOS. Disable CONFIG_FREERTOS_UNICORE."
#endif

#define LG_APP_CPU_CORE 1

/* ---------------- 底层通信接收任务 ---------------- */
#define LG_STACK_UART0_RX          2560
#define LG_PRIO_UART0_RX           12

#define LG_STACK_RS485_RX          3072
#define LG_PRIO_RS485_RX           12

#define LG_STACK_BLE_RECV          3072
#define LG_PRIO_BLE_RECV           11

#define LG_STACK_CAN_RX            3072
#define LG_PRIO_CAN_RX             11

#define LG_STACK_MESH_RX           3072
#define LG_PRIO_MESH_RX            11

/* ---------------- 老化及应用业务任务 ---------------- */
#define LG_STACK_AGING_CTRL        6144
#define LG_PRIO_AGING_CTRL         8

#define LG_STACK_BLE_DATA          6144
#define LG_PRIO_BLE_DATA           8

#define LG_STACK_MQTT_RX           8192
#define LG_PRIO_MQTT_RX            7

#define LG_STACK_HTTP_RX           3072
#define LG_PRIO_HTTP_RX            7

#define LG_STACK_AGING_GET         6144
#define LG_PRIO_AGING_GET          7

#define LG_STACK_UART_JSON         4096
#define LG_PRIO_UART_JSON          6

#define LG_STACK_NET_INIT          8192
#define LG_PRIO_NET_INIT           6

#define LG_STACK_UDP_RX            3072
#define LG_PRIO_UDP_RX             6

#define LG_STACK_HTTP_AUTO         3072
#define LG_PRIO_HTTP_AUTO          4

#define LG_STACK_AGING_UPLOAD      8192
#define LG_PRIO_AGING_UPLOAD       4

#define LG_STACK_SNTP              4096
#define LG_PRIO_SNTP               4

#define LG_STACK_NET_MONITOR       3072
#define LG_PRIO_NET_MONITOR        3

#define LG_STACK_BUTTON            3072
#define LG_PRIO_BUTTON             1

#endif /* LIGHT_GATEWAY_TASK_CONFIG_H */
