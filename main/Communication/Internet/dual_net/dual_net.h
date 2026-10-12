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
 * 能力门控：默认仅根节点执行有线优先、路由裁决和 STA 重连控制；
 * 子节点不初始化此模块。硬件仍由 iot_bridge 管理。
 */
#ifndef DUAL_NET_ENABLE
#define DUAL_NET_ENABLE 1 /**< 模块总开关；0 时 dual_net_init() 直接返回 */
#endif

#ifndef DUAL_NET_ROOT_ONLY
#define DUAL_NET_ROOT_ONLY 1 /**< 1=仅根节点(IsRoot==1)运行；子节点入口直接返回，零代码运行 */
#endif

#ifndef DUAL_NET_ETH_HW_ENABLE
#define DUAL_NET_ETH_HW_ENABLE 1 /**< 1=接管 iot_bridge 创建的 ETH_WAN netif 并参与上行裁决 */
#endif

#ifndef DUAL_NET_ENABLE_ROUTE_CONTROL
#define DUAL_NET_ENABLE_ROUTE_CONTROL 1 /**< 1=允许 esp_netif_set_default_netif() 并写 Network_Flag（以太网接入后需为 1） */
#endif

#ifndef DUAL_NET_ENABLE_WIFI_FALLBACK
#define DUAL_NET_ENABLE_WIFI_FALLBACK 0 /**< 1=允许 dual_net 主动调用 esp_wifi_connect() */
#endif

/* 默认探测/认证重试周期；事件唤醒不改变周期计数。 */
#ifndef LG_DUAL_NET_MONITOR_IDLE_PERIOD_MS
#define LG_DUAL_NET_MONITOR_IDLE_PERIOD_MS 60000
#endif

/* 允许主动连接时的探测/重试周期。 */
#ifndef LG_DUAL_NET_MONITOR_ACTIVE_PERIOD_MS
#define LG_DUAL_NET_MONITOR_ACTIVE_PERIOD_MS 10000
#endif

/* 事件响应和去抖检查的最大等待时间，不计入探测/认证重试次数。 */
#ifndef LG_DUAL_NET_EVENT_CHECK_PERIOD_MS
#define LG_DUAL_NET_EVENT_CHECK_PERIOD_MS 1000
#endif

/* 认证失败后的重试次数上限（每个探测/重试周期计一次） */
#ifndef LG_DUAL_NET_AUTH_RETRY_MAX_COUNT
#define LG_DUAL_NET_AUTH_RETRY_MAX_COUNT 30
#endif

/* 无网络时每 N 次轮询才尝试一次 WiFi 连接 */
#ifndef LG_DUAL_NET_WIFI_RETRY_INTERVAL
#define LG_DUAL_NET_WIFI_RETRY_INTERVAL 3
#endif

/*
 * 上行可用性判定参数。
 * WiFi 可用时，以太网必须通过 MQTT TCP 探测才抢占默认路由。
 * WiFi 不可用时，有线有效 IP 立即作为唯一出口；网络条件与服务可达性分开。
 */
#ifndef LG_DUAL_NET_ETH_LINK_UP_DEBOUNCE_MS
#define LG_DUAL_NET_ETH_LINK_UP_DEBOUNCE_MS 3000 /**< 网线插入后的去抖确认时间 */
#endif

#ifndef LG_DUAL_NET_ETH_LINK_DOWN_DEBOUNCE_MS
#define LG_DUAL_NET_ETH_LINK_DOWN_DEBOUNCE_MS 1000 /**< 网线拔出后的去抖确认时间 */
#endif

#ifndef LG_DUAL_NET_UPLINK_PROBE_TIMEOUT_MS
#define LG_DUAL_NET_UPLINK_PROBE_TIMEOUT_MS 2000 /**< 单次 MQTT TCP 建连等待超时 */
#endif

#ifndef LG_DUAL_NET_UPLINK_PROBE_OK_COUNT
#define LG_DUAL_NET_UPLINK_PROBE_OK_COUNT 2 /**< 连续成功次数达到后才认定可用 */
#endif

#ifndef LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT
#define LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT 3 /**< 连续失败次数达到后才认定不可用 */
#endif

/** 上行链路类型：为后续有线接入与并行分流预留 */
typedef enum
{
    DUAL_NET_UPLINK_NONE = 0, /**< 无可用上行 */
    DUAL_NET_UPLINK_ETH,      /**< 有线以太网 */
    DUAL_NET_UPLINK_WIFI,     /**< WiFi STA */
} dual_net_uplink_t;

/** 上行可用性状态（叠加连通性探测结果，而非仅有 IP） */
typedef enum
{
    DUAL_NET_UPLINK_STATE_NONE = 0, /**< 无可用上行 */
    DUAL_NET_UPLINK_STATE_PROBING,  /**< 已取得 IP，正在探测连通性 */
    DUAL_NET_UPLINK_STATE_READY,    /**< 有线已通过 MQTT TCP 探测，或 WiFi STA 已取得 IP */
    DUAL_NET_UPLINK_STATE_STALE,    /**< 探测连续失败达到阈值，恢复无线重连 */
} dual_net_uplink_state_t;

/** 上行变化回调；在 net_monitor 任务调用，回调内不得阻塞或删除该任务 */
typedef void (*dual_net_uplink_cb_t)(dual_net_uplink_t uplink);

/** 以下状态查询使用短临界区快照；仅任务/事件上下文调用，不可在 ISR 调用。 */
bool dual_net_is_initialized(void);

/** 物理链路在线且有有效 IP；不要求 MQTT 探测完成。供根节点重启守卫使用。 */
bool dual_net_has_eth_ip(void);

/// @brief 初始化双网卡。
/// @note 必须在 esp_bridge_create_all_netif() 之后调用，否则取不到 WIFI_STA_DEF 句柄。
///       非根节点(IsRoot!=1)时直接返回 ESP_OK，不注册任何事件处理器、不创建任务。
/// @note 仅启动任务串行调用；可能阻塞，不可在 ISR/事件回调调用，不支持并发初始化。
/// @return ESP_OK 成功或不适用；ESP_ERR_INVALID_STATE 缺少 STA netif；其他错误为初始化失败
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
const char *dual_net_get_active_interface(void);

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
const char *dual_net_get_wifi_state(void);

// 注册网络就绪回调
typedef void (*network_ready_cb_t)(void);

/// @brief 注册一次性网络就绪回调；由 net_monitor 任务调用，回调不得阻塞
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

/// @brief 请求选择上行链路（DUAL_NET_UPLINK_NONE 恢复自动）；任务/事件上下文，不阻塞
/// @note 由 net_monitor 串行执行；通过 active_uplink 查询或回调确认实际切换结果。
/// @return ESP_OK 请求已受理；ESP_ERR_NOT_SUPPORTED 当前门控下不支持切换
esp_err_t dual_net_select_uplink(dual_net_uplink_t uplink);

/// @brief 预留：注册上行变化回调
/// @param callback 回调，NULL 表示取消注册
void dual_net_register_uplink_change_cb(dual_net_uplink_cb_t callback);

/// @brief 获取上行可用性状态（已叠加连通性探测结果）
/// @return 见 dual_net_uplink_state_t
dual_net_uplink_state_t dual_net_get_uplink_state(void);

/// @brief 以太网是否已确认为可用上行（取得 IP + 连通性探测通过）
/// @note WiFi 可用时以此决定有线抢占；重启守卫应使用 dual_net_has_eth_ip()。
/// @return true=以太网可承担上行
bool dual_net_is_eth_uplink_ready(void);

/// @brief 根节点是否具备链路/IP条件（有线或 WiFi STA 有有效 IP；不保证服务器可达）
/// @note 供 Network_Flag 裁决使用；可在任务上下文调用。
/// @return true=有可用上行
bool dual_net_is_uplink_ready(void);

/// @brief 兼容入口：请求监控任务重新检查网络；初始化后根节点标志仅由监控任务写入
/// @note 任务/事件上下文调用，不阻塞，不可在 ISR 调用。
void dual_net_arbitrate_network_flag(void);

/// @brief 将实际选中的上行 DNS 下发给数据转发网卡（含 SoftAP）
/// @note 在 net_monitor 任务串行调用，可能阻塞，不可在 ISR/事件回调调用。
/// @return ESP_OK 成功；ESP_ERR_INVALID_STATE 当前无有效上行
esp_err_t dual_net_refresh_dns_to_softap(void);

#endif // DUAL_NET_H
