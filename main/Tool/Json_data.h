#ifndef JSON_DATA_H
#define JSON_DATA_H

#include <stdbool.h>
#include "cJSON.h"
#include "app_crc.h"
#include "spiffs_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define BASE_CONFIG_JSON_FILE "baseconfig.json"
#define DEVICE_JSON_SUFFIX "_device.json"

    // 检查数据包是否是完整 JSON
    bool is_complete_packet(const char *buffer, int len);
    bool is_complete_json(const char *buffer, int len);

    // 检查数据包是否是 json;crc 格式
    bool is_complete_packet_with_crc(const char *buffer, int len);

    // ChangeConfig：Value 保存为 baseconfig.json，ExternalDevices 每个对象保存为 Name_device.json
    // 支持两种输入：纯 JSON，或者 json;crc
    void process_packet_AllConfig(const char *packet, int len);

    // 读取 baseconfig.json 和 *_device.json，重新组装为 ReadConfig 返回 JSON
    // 返回值需要调用 cJSON_free() 释放
    char *CreateReadConfigJsonFromFiles(void);

    // 生成数据上报JSON字符串，返回值需要调用 free() 释放
    char *CreateDataReportJson(int idnum,
                               int timestamp,
                               char *sn_buffer,
                               int current_step,
                               const double *values,
                               int value_count,
                               int decimal_places);

    esp_err_t process_packet_AllConfig_MQTT(const char *packet, int len);

    char *CreateReadConfigJsonFromFiles_MQTT(int seq, int cmd_seq, int code, const char *msg);

    int get_json_int64(const char *json, const char *key, int64_t *value);

    char *create_json_deviceMode(int seq, int mode, const char *aging_num);

    char *create_json_agingStage(int timestamp, const char *aging_stage, const char *aging_number);

    char *create_json_agingComplete(int timestamp, int is_complete, const char *aging_number);

    char *create_sensor_json(int idnum, const char *pn, int current_step, int timestamp, const char *value_json);

    char *create_aging_state_json(int seq, const char *aging_state);

    char *create_aging_state_json_ProgramId(int seq, const char *ProgramId);

    char *create_aging_state_json_StepId(int seq, const char *StepId);

    char *create_pn_response_json(int req, int cmd_seq, int code, const char *get_pn);

    char *create_device_response(int seq, int cmd_seq, int code, const char *msg);

    char *update_device_id_in_json(const char *json_input, const char *new_device_id);
#ifdef __cplusplus
}
#endif

#endif
