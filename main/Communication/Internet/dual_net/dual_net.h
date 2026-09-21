
#ifndef DUAL_NET_H
#define DUAL_NET_H

#include <stdbool.h>
#include "esp_netif.h"

#define SPI_MOSIPIN 15
#define SPI_MISOPIN 7
#define SPI_SCLKPIN 6
#define ETH_INT 4



extern bool wifi_up ;
extern bool wifi_auth_failed;
// 初始化双网卡
esp_err_t dual_net_init(void);

/// @brief 通知WiFi连接成功
/// @param  
void dual_net_notify_wifi_up(void);

/// @brief 通知WiFi断开连接
/// @param  
void dual_net_notify_wifi_down(void);

/// @brief 获取当前激活的网络接口
/// @param  
/// @return 
const char* dual_net_get_active_interface(void);

/// @brief 检查以太网是否激活
/// @param  
/// @return 
bool dual_net_is_ethernet_active(void);

/// @brief 检查WiFi是否激活
bool dual_net_is_wifi_active(void);

/// @brief 检查是否有任何网络连接可用
bool dual_net_is_any_network_up(void);

/// @brief 获取WiFi状态
/// @param  
/// @return 
const char* dual_net_get_wifi_state(void);

// 注册网络就绪回调
typedef void (*network_ready_cb_t)(void);

/// @brief 注册网络就绪回调函数
/// @param callback 
void dual_net_register_ready_callback(network_ready_cb_t callback);

void dual_net_set_mesh_switching(bool switching);
bool dual_net_is_mesh_switching(void);

#endif // DUAL_NET_H