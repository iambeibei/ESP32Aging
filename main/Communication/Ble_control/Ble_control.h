// ble_control.h
#ifndef BLE_CONTROL_H
#define BLE_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_gatt_defs.h"
#include "esp_err.h"
#include "ble_target.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUFFER_MAX 1200
extern bool ble_Scan_complate;// 扫描完成标志

// 蓝牙设备信息结构体
typedef struct {
    char name[64];                 // 设备名称
    esp_bd_addr_t bda;              // 蓝牙设备地址
    uint8_t addr_type;               // 地址类型
    int8_t rssi;                     // 信号强度
    bool is_connected;                // 连接状态
} ble_device_info_t;

// 蓝牙MTU数据结构体
typedef struct
{
    uint8_t *data;
    uint16_t data_len;
    uint32_t wait_time;
    uint32_t session_id;           // 接收时固化的连接代次
} ble_mtu_data_t;

// 蓝牙数据结构体
typedef struct 
{
    uint8_t profile_id;
    // uint8_t remote[6];
    uint8_t *data;
    uint16_t data_len;
    uint32_t session_id;
}ble_data_t;

// BLE事件回调函数类型
typedef void (*ble_scan_cb_t)(ble_device_info_t *device, int device_count);  // 扫描回调
/* success=false 表示连接建立失败（enh_open 未被接受或建链失败，如 HCI rsn 0x3E），
 * 区别于已建立连接后的断开（走 disconnect_cb）。调用方据此重试而非中止业务。 */
typedef void (*ble_connect_cb_t)(bool success, const char *device_name);     // 连接回调
typedef void (*ble_data_cb_t)(uint8_t *data, uint16_t len);                  // 数据接收回调
typedef void (*ble_disconnect_cb_t)(void);                                   // 断开连接回调

/**
 * @brief 初始化BLE控制模块
 */
esp_err_t ble_control_init(void);

/**
 * @brief 设置BLE回调函数
 * @param scan_cb 扫描完成回调
 * @param connect_cb 连接结果回调
 * @param data_cb 数据接收回调
 * @param disconnect_cb 断开连接回调
 */
void ble_control_set_callbacks(ble_scan_cb_t scan_cb, 
                               ble_connect_cb_t connect_cb,
                               ble_disconnect_cb_t disconnect_cb);

/**
 * @brief 开始扫描BLE设备
 * @note 任务上下文，可能阻塞；每个控制步骤最多 5 秒，不可在 ISR/蓝牙回调调用。
 * @param scan_time 扫描时间(秒)
 * @return true: 成功, false: 失败
 */
bool ble_start_scan(uint8_t scan_time);

/* CPU1 任务上下文，非 ISR/蓝牙回调；串行配置白名单并启动扫描。
 * target 仅在调用期间借用；每个异步步骤最多等待 5 秒，失败不启动扫描。
 * 名称模式传 use_mac=false 的目标。初始化完成后可重复调用。 */
esp_err_t ble_start_target_scan(const BleTarget *target, uint8_t scan_time);

/* 任务上下文，非 ISR；可能短暂等待列表锁。按地址及类型匹配，不依赖名称。 */
bool ble_scanned_has_address(const uint8_t *bda, uint8_t addr_type);

/* 任务上下文，非 ISR/蓝牙回调；停止并等待确认，每步最多 5 秒。
 * 未确认停扫时返回错误，调用方不得发起连接。 */
esp_err_t ble_stop_scan_wait(void);

/* 命令任务删除老化任务前调用 pause；等待其退出控制锁，最多 5 秒。
 * pause 成功后禁止新扫描/建链；旧任务删除后、新会话初始化前调用 resume。
 * resume 在旧连接请求仍在途时返回错误并保持暂停，防止旧目标迟到连接进入新会话。
 * 任务上下文可用，不可在 ISR 或蓝牙回调调用。 */
esp_err_t ble_control_pause(void);
esp_err_t ble_control_resume(void);

/**
 * @brief 停止扫描
 * @note 任务上下文，等待停止确认；错误会记录日志。不可在 ISR/蓝牙回调调用。
 */
void ble_stop_scan(void);

/**
 * @brief 恢复 Wi-Fi/BLE 共存偏好为 BALANCE（幂等，任意任务上下文）
 *
 * 扫描/建链阶段会临时把共存偏好提升为蓝牙优先；GATT 设置结论、建链失败、
 * 断开、停扫会自动恢复。本接口供上层在老化中止/检查轮次边界兜底调用，
 * 避免异常路径遗留蓝牙优先长期压缩 Wi-Fi 上行。
 */
void ble_restore_coex_balance(void);

/**
 * @brief 连接BLE设备（通过名称）
 * @param device_name 设备名称
 * @return true: 连接请求已发送, false: 失败
 */
bool ble_connect_by_name(const char *device_name);

/**
 * @brief 连接BLE设备（通过地址）
 * @param bda 设备地址
 * @param addr_type 地址类型
 * @return true: 连接请求已发送, false: 失败
 */
bool ble_connect_by_address(uint8_t *bda, uint8_t addr_type);

/**
 * @brief 断开当前BLE连接
 */
void ble_disconnect(void);

/**
 * @brief 发送数据到BLE设备
 * @note 任务上下文，异步提交；新连接默认明文，仅当前连接鉴权成功后加密。
 * @param pdata 数据缓冲区
 * @param len 数据长度
 * @param timeout 兼容参数，不在此等待对端回复
 * @return 1: SDK已受理, 0: 参数、连接或提交失败；不代表对端收到或解析成功
 */
int ble_send_data(void *pdata, int len, uint32_t timeout);

/* 无锁、非阻塞；任务及蓝牙回调可用，不在 ISR 调用。
 * session_id 是连接代次，不是 SDK conn_id。鉴权结果只能改变原连接的状态。 */
uint32_t ble_session_id(void);
bool ble_session_is_current(uint32_t session_id);
bool ble_session_is_encrypted(void);
bool ble_session_set_encrypted(uint32_t session_id, bool encrypted);

/* 任务上下文；鉴权帧始终原样提交，仅接收仍有效的连接代次。
 * data 在调用期间借用；返回 1 仅表示 SDK 已受理，不代表对端业务确认。 */
int ble_send_auth_data(uint32_t session_id, const uint8_t *data, int len);

/**
 * @brief 获取当前连接的设备名称
 * @return 设备名称，如果没有连接返回NULL
 */
const char* ble_get_connected_device_name(void);

/**
 * @brief 获取当前连接的设备地址
 * @param bda 输出缓冲区(6字节)
 * @return true: 已连接, false: 未连接
 */
bool ble_get_connected_device_addr(uint8_t *bda);

/**
 * @brief 检查BLE是否已连接
 * @return true: 已连接, false: 未连接
 */
bool ble_is_connected(void);

/**
 * @brief GATT 设置阶段是否已得出结论
 *
 * 连接建立成功后，本模块依次完成服务发现、notify 特征注册与 CCCD 写入。
 * 本函数反映该阶段是否已得出结论：CCCD 写结果到达（ESP_GATTC_WRITE_DESCR_EVT，
 * 成败均算结论），或降级结论（描述符/特征缺失），或连接断开。
 * 老化任务应在收到连接成功回调后轮询本函数并带超时：对端建链后停止响应时
 * 快速失败重试，避免傻等固定延时。断开后自动复位为 false。
 * @return true: 设置阶段已得出结论, false: 进行中/未开始
 */
bool ble_gatt_setup_done(void);

/**
 * @brief 检查是否正在扫描
 * @return true: 扫描中, false: 未扫描
 */
bool ble_is_scanning(void);

/**
 * @brief 获取扫描到的设备列表
 * @param devices 输出设备数组
 * @param max_count 最大设备数
 * @return 实际设备数量
 */
int ble_get_scanned_devices(ble_device_info_t *devices, int max_count);

/**
 * @brief 目标名称是否已出现在本轮扫描结果中（精确匹配，不打印日志）
 *
 * 调用上下文：任务上下文可用；内部持设备列表互斥锁，可能短暂阻塞（持锁
 * 仅做一次线性比较），**不可在 ISR 调用**。GAP 回调负责写列表、调用方读。
 * 仅供扫描期间提前命中判断（命中后由调用方停扫并调用 ble_connect_by_name），
 * 本接口不发起连接、不打日志，可安全地周期性轮询。
 * @param name 目标设备名称（PN 全名），可为 NULL（直接返回 false）
 * @return true: 已出现在列表中, false: 尚未出现或 name 非法
 */
bool ble_scanned_has_name(const char *name);

/**
 * @brief 清除扫描结果
 */
void ble_clear_scan_results(void);

/* 任务上下文；接收当前连接的报文，最多等待 wait_time ticks。
 * data 由调用方持有，容量 max_len 字节；成功返回实际长度，失败返回 -1，超长不截断。 */
int app_ble_recv_data_form_remote(uint8_t *data, int max_len, uint32_t wait_time);

/* 任务上下文；最多等待 wait_time ticks，输出缓冲由调用方持有。
 * 成功同时返回报文所属连接代次；旧连接、超长报文拒绝，返回 -1。 */
int app_ble_recv_data_for_session(uint8_t *data, int max_len, uint32_t wait_time,
                                  uint32_t *session_id);

#ifdef __cplusplus
}
#endif
#endif // BLE_CONTROL_H
