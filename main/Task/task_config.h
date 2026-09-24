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

/*
 * 群控模式等待上位机下发"下一步放行"(CanNextstep)的最长时间。
 *
 * 上位机是以"收到设备的放行响应"作为放行成功的判据；根节点断网时指令与响应
 * 都到不了，若无超时兜底，老化任务会永久卡在步骤之间（既不推进也不采样）。
 * 超时后本地自动放行继续下一步，并留下"离线自动推进"记录，联网后补偿上报对账。
 *
 * 单位毫秒，默认 10 分钟；需要更长/更短时只改这里。
 */
#define LG_NEXTSTEP_WAIT_TIMEOUT_MS (10 * 60 * 1000)

/* 群控等待期间轮询间隔（毫秒）：留出中止/状态检查的机会，不要死睡。 */
#define LG_NEXTSTEP_POLL_INTERVAL_MS 1000

#endif /* LIGHT_GATEWAY_TASK_CONFIG_H */
