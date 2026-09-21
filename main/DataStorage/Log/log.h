#ifndef LOG_H
#define LOG_H
#include <stdint.h>
#include <stdbool.h>


#define RECORD_SIZE 128  // 每条记录最大128字节（包含字符串内容）
#define MAX_STR_LEN  127 // 字符串最大长度（留1字节给结束符）
#define LOG_PARTITION_LABEL "log_storage"

#define MAX_RECORDS  3000 // 最大记录数

/// @brief 挂载文件系统
/// @return ESP_OK on success, ESP_FAIL on failure.
esp_err_t log_storage_mount(void);

/// @brief 卸载文件系统
void storage_unmount(void);

/// @brief 写入字符串记录
/// @param index 索引
/// @param str 要写入的字符串内容
/// @return ESP_OK on success, ESP_FAIL on failure.
esp_err_t storage_write_record(uint16_t index, const char *str);

/// @brief 读取字符串记录
/// @param index 索引
/// @param buffer 存放读取到的字符串
/// @param buffer_size 缓冲区大小
/// @return ESP_OK on success, ESP_FAIL on failure.
esp_err_t storage_read_record(uint16_t index, char *buffer, size_t buffer_size);

/// @brief 循环写入字符串记录（自动管理写入位置，覆盖最旧数据）
/// @param pn pn码
/// @param str 要写入的字符串内容
/// @return ESP_OK on success, ESP_FAIL on failure.
esp_err_t storage_write_record_cyclic(const char *pn,const char *str);

/// @brief 获取当前循环写入位置（用于调试）
/// @return 当前写入索引
uint16_t storage_get_current_index(void);

/// @brief 重置循环写入位置到0
void storage_reset_cyclic_position(void);

void print_logfatfs_usage(void);

uint16_t log_get_offset(uint16_t index);

void Get_Index_From_Flash(uint16_t index);//从flash中获取当前日志写入位置

#endif //DATA_RECORD_H
