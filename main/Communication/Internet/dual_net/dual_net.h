#ifndef DUAL_NET_H
#define DUAL_NET_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

/*
 * 以太网硬件（KSZ8851SNL over SPI）由 iot_bridge 组件负责初始化，
 * 引脚通过 menuconfig 配置，不在本模块内定义：
 *   CONFIG_BRIDGE_ETH_SPI_HOST / _SCLK_GPIO / _MOSI_GPIO / _MISO_GPIO / _CS0_GPIO / _INT0_GPIO
 * 本模块只接管 iot_bridge 创建好的 "ETH_WAN" netif，不再自行 spi_bus_initialize()，
 * 以避免与 bridge_eth.c 中的 ESP_ERROR_CHECK(spi_bus_initialize(...)) 争用 SPI2_HOST 而 abort。
 * 详见 DUAL_NET_DESIGN.md。
 */

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
#define DUAL_NET_ETH_HW_ENABLE          1   /**< 1=接管 iot_bridge 创建的 ETH_WAN netif 并参与上行裁决 */
#endif

#ifndef DUAL_NET_ENABLE_ROUTE_CONTROL
#define DUAL_NET_ENABLE_ROUTE_CONTROL   1   /**< 1=允许 esp_netif_set_default_netif() 并写 Network_Flag（以太网接入后需为 1） */
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

/*
 * 上行可用性判定参数。
 * 只拿到 IP 不足以认定上行可用（网线可能接到一台不通的设备），
 * 必须叠加「业务服务器可达」探测，避免锁死在「有 IP 却上不了网」的僵局。
 */
#ifndef LG_DUAL_NET_ETH_LINK_UP_DEBOUNCE_MS
#define LG_DUAL_NET_ETH_LINK_UP_DEBOUNCE_MS      3000  /**< 网线插入后的去抖确认时间 */
#endif

#ifndef LG_DUAL_NET_ETH_LINK_DOWN_DEBOUNCE_MS
#define LG_DUAL_NET_ETH_LINK_DOWN_DEBOUNCE_MS    1000  /**< 网线拔出后的去抖确认时间 */
#endif

#ifndef LG_DUAL_NET_UPLINK_PROBE_TIMEOUT_MS
#define LG_DUAL_NET_UPLINK_PROBE_TIMEOUT_MS      2000  /**< 单次连通性探测超时 */
#endif

#ifndef LG_DUAL_NET_UPLINK_PROBE_OK_COUNT
#define LG_DUAL_NET_UPLINK_PROBE_OK_COUNT        2     /**< 连续成功次数达到后才认定可用 */
#endif

#ifndef LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT
#define LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT      3     /**< 连续失败次数达到后才认定不可用 */
#endif

/** 上行链路类型：为后续有线接入与并行分流预留 */
typedef enum {
    DUAL_NET_UPLINK_NONE = 0,   /**< 无可用上行 */
    DUAL_NET_UPLINK_ETH,        /**< 有线以太网 */
    DUAL_NET_UPLINK_WIFI,       /**< WiFi STA */
} dual_net_uplink_t;

/** 上行可用性状态（叠加连通性探测结果，而非仅有 IP） */
typedef enum {
    DUAL_NET_UPLINK_STATE_NONE = 0,  /**< 无可用上行 */
    DUAL_NET_UPLINK_STATE_PROBING,   /**< 已取得 IP，正在探测连通性 */
    DUAL_NET_UPLINK_STATE_READY,     /**< IP 与业务服务器均可达 */
    DUAL_NET_UPLINK_STATE_STALE,     /**< 曾可用但探测连续失败，等待回落 */
} dual_net_uplink_state_t;

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

/// @brief 获取上行可用性状态（已叠加连通性探测结果）
/// @return 见 dual_net_uplink_state_t
dual_net_uplink_state_t dual_net_get_uplink_state(void);

/// @brief 以太网是否已确认为可用上行（取得 IP + 连通性探测通过）
/// @note 这是「以太网优先」的唯一依据，也是抑制 root_failover 重启的判据。
/// @return true=以太网可承担上行
bool dual_net_is_eth_uplink_ready(void);

/// @brief 根节点上行是否真正可用（以太网已确认，或 WiFi STA 已取得 IP）
/// @note 供 Network_Flag 裁决使用；可在任务上下文调用。
/// @return true=有可用上行
bool dual_net_is_uplink_ready(void);

/// @brief Network_Flag 的统一裁决入口（mesh.c 的两处写点已改为调用本函数）
/// @note 在事件上下文调用，内部只做轻量赋值与日志，不阻塞。
void dual_net_arbitrate_network_flag(void);

/// @brief 以太网成为出口后，把它的 DNS 重新下发给 SoftAP（修复 iot_bridge 固定用 STA 覆盖的问题）
/// @note 内部使用 esp_bridge_update_dns_info()，对 NULL 参数安全，不要求 netif 已 up。
/// @return ESP_OK 成功；ESP_ERR_INVALID_STATE 当前无可用以太网 netif
esp_err_t dual_net_refresh_dns_to_softap(void);

#endif // DUAL_NET_H
