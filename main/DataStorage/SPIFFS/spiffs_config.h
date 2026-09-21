#ifndef SPIFFS_CONFIG_H
#define SPIFFS_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPIFFS_BASE_PATH              "/spiffs"
#define SPIFFS_PATH_MAX_LEN           128
#define SPIFFS_FILE_NAME_MAX_LEN      64
#define SPIFFS_MAX_FILE_LIST_COUNT    16

// 初始化SPIFFS
esp_err_t spiffs_init(void);

// 反初始化SPIFFS
esp_err_t spiffs_deinit(void);

// 写入文件，filename 为相对 /spiffs 的文件名，例如 "baseconfig.json"
esp_err_t spiffs_file_write(const char *filename, const void *data, size_t len);

// 读取文件到调用方提供的缓冲区
esp_err_t spiffs_file_read(const char *filename, void *buffer, size_t *len, size_t max_len);

// 删除文件
esp_err_t spiffs_file_delete(const char *filename);

// 检查文件是否存在
bool spiffs_file_exists(const char *filename);

// 保存 JSON 字符串到 SPIFFS
esp_err_t save_json_to_spiffs(const char *filename, const char *json_str);

// 从 SPIFFS 读取 JSON 字符串，返回值需要调用 free() 释放
char *read_json_from_spiffs(const char *filename);

// 打印 SPIFFS 分区中的所有文件名称和大小
esp_err_t spiffs_list_files(void);

// 获取 SPIFFS 分区中的所有文件名
esp_err_t spiffs_get_file_names(char file_names[][SPIFFS_FILE_NAME_MAX_LEN],
                                size_t max_files,
                                size_t *file_count);



// 删除 SPIFFS 分区中的全部文件
esp_err_t spiffs_delete_all_files(void);
#ifdef __cplusplus
}
#endif

#endif
