/**
 * @file lg_log_config.h
 * @brief 串口日志开关宏：只提供编译期开关，不引入任何依赖。
 *
 * 本工程控制台日志与设备 SCPI 共用 UART0（GPIO43/44，115200），日志与仪表报文
 * 走同一条物理链路。因此所有"整帧/整报文"级别的 dump 默认关闭，需要离线排障
 * 时才临时改为 1 并重新编译。
 */
#ifndef LG_LOG_CONFIG_H
#define LG_LOG_CONFIG_H

/* 自包含：LG_PROTO_LOG 展开为 printf，调用方无需先包含 <stdio.h>。 */
#include <stdio.h>

/**
 * 协议层与加解密全量 dump：
 *   - Modbus 帧、寄存器逐条解析结果（modbus_cmd.c / modbus_json_parser.c）
 *   - SCPI 协议树全量打印（scpi_dynamic.c）
 *   - 老化步骤与设备清单全量打印（aging_config.c）
 *   - 蓝牙加解密报文十六进制内容（app_enc.c 的 printData）
 *
 * 0 = 关闭（默认，量产固件）；1 = 打开（仅限串口离线排障）。
 * 打开后启动与每次配置解析会输出上百行，且会与设备报文交织，禁止随固件出厂。
 */
#ifndef LG_LOG_PROTO_DUMP
#define LG_LOG_PROTO_DUMP 0
#endif

/**
 * 协议层裸打印宏（等价 printf，受 LG_LOG_PROTO_DUMP 控制）。
 * 这些打印不经过 esp_log，无法用 esp_log_level_set() 关闭，只能编译期裁剪。
 *
 * 注意：关闭时参数不会被求值，因此调用处不得在参数表达式里写副作用
 * （如 LG_PROTO_LOG("%d", i++)），否则调试版与量产版行为不一致。
 */
#if LG_LOG_PROTO_DUMP
#define LG_PROTO_LOG(...) printf(__VA_ARGS__)
#else
#define LG_PROTO_LOG(...) ((void)0)
#endif

#endif /* LG_LOG_CONFIG_H */
