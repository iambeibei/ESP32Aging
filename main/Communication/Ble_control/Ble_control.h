// ble_control.h
#ifndef BLE_CONTROL_H
#define BLE_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_gatt_defs.h"
#include "esp_err.h"

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
} ble_mtu_data_t;

// 蓝牙数据结构体
typedef struct 
{
    uint8_t profile_id;
    // uint8_t remote[6];
    uint8_t *data;
    uint16_t data_len;
}ble_data_t;

// BLE事件回调函数类型
typedef void (*ble_scan_cb_t)(ble_device_info_t *device, int device_count);  // 扫描回调
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
 * @param scan_time 扫描时间(秒)
 * @return true: 成功, false: 失败
 */
bool ble_start_scan(uint8_t scan_time);

/**
 * @brief 停止扫描
 */
void ble_stop_scan(void);

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
 * @param pdata 数据缓冲区
 * @param len 数据长度
 * @return true: 发送成功, false: 失败
 */
int ble_send_data(void *pdata, int len, uint32_t timeout);

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
 * @brief 清除扫描结果
 */
void ble_clear_scan_results(void);

/// @brief 接收来自远程设备的数据
/// @param data 
/// @param max_len 
/// @param wait_time 
/// @return 
int app_ble_recv_data_form_remote(uint8_t *data, int max_len, uint32_t wait_time);

#ifdef __cplusplus
}
#endif
#endif // BLE_CONTROL_H