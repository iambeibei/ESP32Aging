#ifndef DUAL_NET_H
#define DUAL_NET_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

/*
 * 以太网（KSZ8851SNL，SPI 接口）引脚定义。
 * 只在打开 DUAL_NET_ETH_HW_ENABLE 时才会真正占用这些引脚与 SPI2_HOST。
 * 注意：iot_bridge 组件自带的 SPI 以太网路径同样使用 SPI2_HOST 与 GPIO4 中断脚，
 * 与本模块硬件路径互斥，详见 DUAL_NET_DESIGN.md。
 */
#define SPI_MOSIPIN 15
#define SPI_MISOPIN 7
#define SPI_SCLKPIN 6
#define SPI_CSPIN   16
#define ETH_INT     4

/*
 * 能力门控：默认只保留「状态观察」，所有会产生副作用的动作都默认关闭，
 * 以保证现有 Mesh-Lite 子节点入网链路不受影响。
 */
#ifndef DUAL_NET_ENABLE
#define DUAL_NET_ENABLE                 1   /**< 模块总开关；0 时 dual_net_init() 直接返回 */
#endif

#ifndef DUAL_NET_ROOT_ONLY
#define DUAL_NET_ROOT_ONLY              1   /**< 1=仅根节点(IsRoot==1)运行；子节点入口直接返回，零代码运行 */
#endif

#ifndef DUAL_NET_ETH_HW_ENABLE
#define DUAL_NET_ETH_HW_ENABLE          0   /**< 1=初始化 KSZ8851SNL 硬件并 esp_eth_start() */
#endif

#ifndef DUAL_NET_ENABLE_ROUTE_CONTROL
#define DUAL_NET_ENABLE_ROUTE_CONTROL   0   /**< 1=允许 esp_netif_set_default_netif() 并写 Network_Flag */
#endif

#ifndef DUAL_NET_ENABLE_WIFI_FALLBACK
#define DUAL_NET_ENABLE_WIFI_FALLBACK   0   /**< 1=允许 dual_net 主动调用 esp_wifi_connect() */
#endif

/* 监控任务在「仅观察」模式下的轮询周期；带单位，避免魔法数 */
#ifndef LG_DUAL_NET_MONITOR_IDLE_PERIOD_MS
#define LG_DUAL_NET_MONITOR_IDLE_PERIOD_MS  60000
#endif

/* 监控任务在「允许主动连接」模式下的轮询周期 */
#ifndef LG_DUAL_NET_MONITOR_ACTIVE_PERIOD_MS
#define LG_DUAL_NET_MONITOR_ACTIVE_PERIOD_MS 10000
#endif

/* 认证失败后的重试次数上限（每次轮询周期计一次） */
#ifndef LG_DUAL_NET_AUTH_RETRY_MAX_COUNT
#define LG_DUAL_NET_AUTH_RETRY_MAX_COUNT    30
#endif

/* 无网络时每 N 次轮询才尝试一次 WiFi 连接 */
#ifndef LG_DUAL_NET_WIFI_RETRY_INTERVAL
#define LG_DUAL_NET_WIFI_RETRY_INTERVAL     3
#endif

/* KSZ8851SNL 的 SPI 时钟频率与传输队列长度（仅在 DUAL_NET_ETH_HW_ENABLE=1 时使用） */
#ifndef LG_DUAL_NET_ETH_SPI_CLK_HZ
#define LG_DUAL_NET_ETH_SPI_CLK_HZ          5000000
#endif

#ifndef LG_DUAL_NET_ETH_SPI_QUEUE_SIZE
#define LG_DUAL_NET_ETH_SPI_QUEUE_SIZE      20
#endif

/** 上行链路类型：为后续有线接入与并行分流预留 */
typedef enum {
    DUAL_NET_UPLINK_NONE = 0,   /**< 无可用上行 */
    DUAL_NET_UPLINK_ETH,        /**< 有线以太网 */
    DUAL_NET_UPLINK_WIFI,       /**< WiFi STA */
} dual_net_uplink_t;

/** 上行变化回调；在事件上下文调用，回调内不得阻塞 */
typedef void (*dual_net_uplink_cb_t)(dual_net_uplink_t uplink);

extern bool wifi_up;
extern bool wifi_auth_failed;

/// @brief 初始化双网卡。
/// @note 必须在 esp_bridge_create_all_netif() 之后调用，否则取不到 WIFI_STA_DEF 句柄。
///       非根节点(IsRoot!=1)时直接返回 ESP_OK，不注册任何事件处理器、不创建任务。
/// @return ESP_OK 成功或不适用；ESP_FAIL 取 netif 句柄失败或建任务失败
esp_err_t dual_net_init(void);

/// @brief 通知WiFi连接成功
/// @param  
void dual_net_notify_wifi_up(void);

/// @brief 通知WiFi断开连接
/// @param  
void dual_net_notify_wifi_down(void);

/// @brief 获取当前激活的网络接口
/// @param  
/// @return "ETHERNET" / "WIFI" / "NONE"
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

/// @brief 获取当前生效的上行链路类型（以太网优先、WiFi 兜底）
/// @return 上行类型；无上行时返回 DUAL_NET_UPLINK_NONE
dual_net_uplink_t dual_net_get_active_uplink(void);

/// @brief 获取指定上行对应的 netif 句柄，未就绪或类型非法时返回 NULL
/// @param uplink 上行类型
/// @return netif 句柄或 NULL
esp_netif_t *dual_net_get_uplink_netif(dual_net_uplink_t uplink);

/// @brief 预留：手动选择上行链路（DUAL_NET_UPLINK_NONE 表示恢复自动）
/// @return ESP_OK 成功；ESP_ERR_NOT_SUPPORTED 当前门控下不支持切换
esp_err_t dual_net_select_uplink(dual_net_uplink_t uplink);

/// @brief 预留：注册上行变化回调
/// @param callback 回调，NULL 表示取消注册
void dual_net_register_uplink_change_cb(dual_net_uplink_cb_t callback);

#endif // DUAL_NET_H
