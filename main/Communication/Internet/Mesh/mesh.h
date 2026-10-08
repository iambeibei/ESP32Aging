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

/*
 * ===== 50个子节点规模的组网参数 =====
 * 注意：这里的 CONFIG_MESH_ROUTE_TABLE_SIZE 与 mesh_netif.h 中同名宏重复定义，
 * 两者必须保持一致，否则不同编译单元看到的路由表容量不同，
 * 会导致 memcpy 越界或路由表静默截断。
 *
 * CONFIG_MESH_AP_CONNECTIONS 是"每个父节点最多能直接带几个子节点"：
 *   原值5意味着根节点只能直连5个子节点，50个子节点必须形成3层以上的树，
 *   每一跳都会把带宽减半、延迟翻倍，并且任一中间节点掉线整棵子树失联。
 *   这里提到ESP-MESH允许的上限10，把树尽量压扁(Root 10 + 第2层各10)。
 * 同时必须把sdkconfig里的 CONFIG_LWIP_DHCPS_MAX_STATION_NUM 调大(>=64)，
 * 否则Root的DHCP server根本发不出足够的IP。
 */
#define CONFIG_MESH_MAX_LAYER 6
#define CONFIG_MESH_ROUTE_TABLE_SIZE 64
#define CONFIG_MESH_AP_AUTHMODE 3
#define CONFIG_MESH_AP_CONNECTIONS 10
#define CONFIG_MESH_NON_MESH_AP_CONNECTIONS 0

/*
 * Root外网链路抖动的恢复阈值：
 * 前 ROOT_UPLINK_FAIL_BEFORE_SWITCH 次先让 esp-mesh 自组网自行重连，
 * 不立刻关自组网/换SSID，避免单次 beacon timeout 就把全网子节点踢下去。
 */
#define ROOT_UPLINK_FAIL_BEFORE_SWITCH 3

/*
 * 是否允许非根节点作为"备份Root"参与竞选。
 * 目前所有非根节点都调用了 esp_mesh_fix_root(true)，它们永远不会成为Root，
 * 整个网络只有唯一一台候选Root：它一旦断电/死机，全网没有任何自愈手段。
 * 子节点规模到50台时这个单点风险必须解决，建议置1(并确保各节点都配了外网SSID)。
 * 默认保留0，是为了不改变现有现场行为。
 */
#define MESH_ALLOW_BACKUP_ROOT 0

/*
 * Root日志里每几百毫秒就有成对的 [TXQ]/[RXQ] 队列打印(来自ESP-MESH内部)，
 * 50个子节点时这会持续占用串口和CPU。需要调试时把该宏置1即可恢复。
 */
#define MESH_QUEUE_LOG_ENABLE 0

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