#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 RS485 驱动及相关的任务、队列和信号量
 *
 * - 配置 RS485 参数：115200，8N1，无流控
 * - 安装 RS485 驱动，创建接收事件队列
 * - 创建固定大小数据队列，避免频繁 malloc/free
 * - 创建发送互斥锁和读取互斥锁
 * - 启动接收任务
 *
 * @return 初始化成功返回0
 */
esp_err_t app_RS485_init(void);

/**
 * @brief 向 RS485 发送数据
 * @param pdata   待发送数据缓冲区
 * @param len     待发送数据长度
 * @param timeout 等待发送互斥锁的超时时间，单位为 FreeRTOS tick
 *
 * @return 成功时返回实际写入字节数；
 *         参数错误、未初始化或获取互斥锁失败时返回 -1
 */
int app_write_RS485_data(void *pdata1, int len, uint32_t timeout);

/**
 * @brief 从 RS485 接收数据队列中读取一个数据块
 *
 * 该函数不会直接操作 RS485 硬件，而是从内部固定大小队列中读取
 * 已经组装好的数据块。
 *
 * @param pdata   用户接收缓冲区
 * @param max_len 用户缓冲区最大长度
 * @param timeout 等待数据块的超时时间，单位为 FreeRTOS tick
 *
 * @return 成功时返回实际复制字节数；
 *         参数错误、未初始化、超时或队列为空时返回 -1
 */
int app_read_RS485_data(void *pdata1, int max_len, uint32_t timeout);

#ifdef __cplusplus
}
#endif