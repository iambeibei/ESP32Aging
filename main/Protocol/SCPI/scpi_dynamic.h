#ifndef SCPI_DYNAMIC_H
#define SCPI_DYNAMIC_H

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SCPI_DYNAMIC_LINE_ENDING
#define SCPI_DYNAMIC_LINE_ENDING "\n"
#endif

typedef struct {
    char *key;          // 例如 "0"、"1"、"swit"
    char *desc;         // 例如 "关闭"、"开启"、"立即切换"
} scpi_limit_item_t;

typedef struct {
    char *name;         // 例如 "动作"
    char *unit;
    char *default_value;

    scpi_limit_item_t *limits;
    size_t limit_count;
} scpi_param_def_t;

typedef struct {
    char *id;
    char *name;
    char *rw;               // "R" / "W" / "RW"

    char *cmd_template;     // 例如 "CURR <NRf>"
    char *rsp_template;     // 例如 "<NRf>" 或 "<NRf>,<NRf>"

    bool readable;
    bool writable;

    scpi_param_def_t param;
    bool has_param_def;
} scpi_cmd_item_t;

typedef struct {
    char *id;
    char *uuid;
    char *name;
    char *description;
    int protocol_type;
    char *protocol_type_str;
    int version;

    scpi_cmd_item_t *items;
    size_t item_count;
} scpi_protocol_t;

/**
 * @brief 从 JSON 字符串导入 SCPI 协议点表
 *
 * @param json_text 收到的完整 JSON 字符串
 * @param out_protocol 输出协议对象，使用完后必须调用 scpi_dynamic_free()
 */
esp_err_t scpi_dynamic_import_json(const char *json_text, scpi_protocol_t **out_protocol);

/**
 * @brief 释放协议对象
 */
void scpi_dynamic_free(scpi_protocol_t *protocol);

/**
 * @brief 获取命令数量
 */
size_t scpi_dynamic_get_count(const scpi_protocol_t *protocol);

/**
 * @brief 根据名称查找命令
 */
const scpi_cmd_item_t *scpi_dynamic_find_by_name(const scpi_protocol_t *protocol, const char *name);

/**
 * @brief 根据 id 查找命令
 */
const scpi_cmd_item_t *scpi_dynamic_find_by_id(const scpi_protocol_t *protocol, const char *id);

/**
 * @brief 根据命令对象构建 SCPI 指令
 *
 * 例如：
 * 模板 "CURR <NRf>" + 参数 "2.5" => "CURR 2.5\n"
 * 模板 "MEAS:VOLT?" + 参数 NULL => "MEAS:VOLT?\n"
 */
esp_err_t scpi_dynamic_build_command(const scpi_cmd_item_t *cmd,
                                     const char *param,
                                     char *out_buf,
                                     size_t out_size);

/**
 * @brief 根据名称构建 SCPI 指令
 */
esp_err_t scpi_dynamic_build_by_name(const scpi_protocol_t *protocol,
                                     const char *name,
                                     const char *param,
                                     char *out_buf,
                                     size_t out_size);

/**
 * @brief 根据 id 构建 SCPI 指令
 */
esp_err_t scpi_dynamic_build_by_id(const scpi_protocol_t *protocol,
                                   const char *id,
                                   const char *param,
                                   char *out_buf,
                                   size_t out_size);

/**
 * @brief 解析 SCPI 响应
 *
 * 例如：
 * 响应模板 "<NRf>"，设备返回 "12.35\n" => values[0] = 12.35
 * 响应模板 "<NRf>,<NRf>"，设备返回 "0.31,0.42\n" => values[0] = 0.31, values[1] = 0.42
 */
esp_err_t scpi_dynamic_parse_response(const scpi_cmd_item_t *cmd,
                                      const char *response,
                                      double *values,
                                      size_t max_values,
                                      size_t *out_value_count);

/**
 * @brief 打印导入的协议表，调试用
 */
void scpi_dynamic_dump(const scpi_protocol_t *protocol);

#ifdef __cplusplus
}
#endif

#endif