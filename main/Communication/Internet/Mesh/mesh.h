// mesh_common.h
#ifndef MESH_COMMON_H
#define MESH_COMMON_H

#define ESP_BD_ADDR_STR "%02x:%02x:%02x:%02x:%02x:%02x"
#define ESP_BD_ADDR_HEX1(addr) \
    (addr)[0], (addr)[1], (addr)[2], (addr)[3], (addr)[4], (addr)[5]

#include <stdint.h>
#include <stdbool.h>
#include <esp_mesh.h>
#include "dual_net.h"
#include "log.h"


#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_MESH_MAX_LAYER 6
#define CONFIG_MESH_ROUTE_TABLE_SIZE 50
#define CONFIG_MESH_AP_AUTHMODE 3
#define CONFIG_MESH_AP_CONNECTIONS 5
#define CONFIG_MESH_NON_MESH_AP_CONNECTIONS 0

#define WiFiNums 2
// 命令定义
#define CMD_KEYPRESSED 0x55
#define CMD_ROUTE_TABLE 0x56
#define CMD_USER_MSG     0x57
#define CMD_MSG_ACK     0x58

// BLE控制命令
#define CMD_BLE_SCAN_START     0x60
#define CMD_BLE_SCAN_RESULT    0x61
#define CMD_BLE_CONNECT        0x62
#define CMD_BLE_CONNECT_RESULT 0x63
#define CMD_BLE_SEND_DATA      0x64
#define CMD_BLE_DATA_FORWARD   0x65
#define CMD_BLE_DISCONNECT     0x66
#define CMD_BLE_STATUS         0x67
#define CMD_BLE_STATUS_RESP    0x68

// 外部变量声明

extern mesh_addr_t s_route_table[];
extern int s_route_table_size;
extern SemaphoreHandle_t s_route_table_lock;
extern mesh_router_t list_router[];

// 函数声明

/// @brief 解析MAC地址字符串
/// @param str MAC地址字符串
/// @param mac 解析后的MAC地址
/// @return 解析是否成功
bool parse_mac_address(const char *str, uint8_t *mac);

/// @brief 发送Mesh消息
/// @param target_mac 目标MAC地址
/// @param data  要发送的数据
/// @param len  数据长度
void send_mesh_message(uint8_t *target_mac, uint8_t *data, int len);

// Mesh初始化
void mesh_init_Custom(void);

void Mesh_cmd_list(void);
void Mesh_cmd_info();

bool Mesh_parse_mac_address(const char *str, uint8_t *mac);

void cmd_send(uint8_t *target_mac, const char *message);
void cmd_broadcast(const char *message);

#ifdef __cplusplus
}
#endif
#endif // MESH_COMMON_H