#include "Json_data.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "appTask.h"
#include "time.h"

static const char *TAG = "JsonData";

#define DEVICE_NAME_SAFE_MAX_LEN 30
#define DEVICE_JSON_FILE_MAX_LEN 96

static bool str_ends_with(const char *str, const char *suffix)
{
    if (str == NULL || suffix == NULL)
    {
        return false;
    }

    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);

    if (str_len < suffix_len)
    {
        return false;
    }

    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

static bool make_safe_filename_part(const char *name, char *out, size_t out_size)
{
    if (name == NULL || out == NULL || out_size == 0)
    {
        return false;
    }

    size_t j = 0;

    for (size_t i = 0; name[i] != '\0' && j < out_size - 1; i++)
    {
        char c = name[i];

        if ((c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            c == '_' ||
            c == '-')
        {
            out[j++] = c;
        }
        else
        {
            out[j++] = '_';
        }
    }

    out[j] = '\0';
    return j > 0;
}

static bool build_device_json_filename(const char *device_name,
                                       char *filename,
                                       size_t filename_size)
{
    if (device_name == NULL || filename == NULL || filename_size == 0)
    {
        return false;
    }

    char safe_name[DEVICE_NAME_SAFE_MAX_LEN];
    if (!make_safe_filename_part(device_name, safe_name, sizeof(safe_name)))
    {
        return false;
    }

    size_t name_len = strlen(safe_name);
    size_t suffix_len = strlen(DEVICE_JSON_SUFFIX);

    if (name_len + suffix_len + 1 > filename_size)
    {
        ESP_LOGE(TAG, "Device json filename too long: %s", safe_name);
        return false;
    }

    memcpy(filename, safe_name, name_len);
    memcpy(filename + name_len, DEVICE_JSON_SUFFIX, suffix_len + 1);

    return true;
}

static void delete_old_device_json_files(void)
{
    char file_names[SPIFFS_MAX_FILE_LIST_COUNT][SPIFFS_FILE_NAME_MAX_LEN];
    size_t file_count = 0;

    if (spiffs_get_file_names(file_names, SPIFFS_MAX_FILE_LIST_COUNT, &file_count) != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to get SPIFFS file list while deleting device json files");
        return;
    }

    for (size_t i = 0; i < file_count; i++)
    {
        if (str_ends_with(file_names[i], DEVICE_JSON_SUFFIX))
        {
            esp_err_t ret = spiffs_file_delete(file_names[i]);
            ESP_LOGI(TAG, "Delete old device json: %s, ret=%s",
                     file_names[i],
                     esp_err_to_name(ret));
        }
    }
}

static esp_err_t save_cjson_object_to_spiffs(const char *filename, cJSON *object)
{
    if (filename == NULL || object == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char *json_str = cJSON_PrintUnformatted(object);
    if (json_str == NULL)
    {
        ESP_LOGE(TAG, "Failed to print json object: %s", filename);
        return ESP_FAIL;
    }

    esp_err_t ret = save_json_to_spiffs(filename, json_str);
    cJSON_free(json_str);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to save %s: %s", filename, esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "Saved json file: %s", filename);
    }

    return ret;
}

bool is_complete_packet(const char *buffer, int len)
{
    return is_complete_json(buffer, len);
}

bool is_complete_packet_with_crc(const char *buffer, int len)
{
    if (buffer == NULL || len <= 0)
    {
        return false;
    }

    const char *semicolon = memchr(buffer, ';', (size_t)len);
    if (semicolon == NULL)
    {
        return false;
    }

    int json_len = (int)(semicolon - buffer);
    if (json_len <= 0 || !is_complete_json(buffer, json_len))
    {
        return false;
    }

    for (int i = json_len + 1; i < len; i++)
    {
        if (isdigit((unsigned char)buffer[i]))
        {
            return true;
        }
    }

    return false;
}

bool is_complete_json(const char *buffer, int len)
{
    if (buffer == NULL || len <= 0)
    {
        return false;
    }

    int start = 0;
    while (start < len && isspace((unsigned char)buffer[start]))
    {
        start++;
    }

    if (start >= len || buffer[start] != '{')
    {
        return false;
    }

    int end = len - 1;
    while (end >= 0 && isspace((unsigned char)buffer[end]))
    {
        end--;
    }

    if (end < 0 || buffer[end] != '}')
    {
        return false;
    }

    int brace_count = 0;
    bool in_string = false;
    bool escape = false;

    for (int i = start; i <= end; i++)
    {
        char c = buffer[i];

        if (escape)
        {
            escape = false;
            continue;
        }

        if (c == '\\')
        {
            escape = true;
            continue;
        }

        if (c == '"')
        {
            in_string = !in_string;
            continue;
        }

        if (!in_string)
        {
            if (c == '{')
            {
                brace_count++;
            }
            else if (c == '}')
            {
                brace_count--;
                if (brace_count < 0)
                {
                    return false;
                }
            }
        }
    }

    return (!in_string) && (brace_count == 0);
}

// 串口版本的处理函数
void process_packet_AllConfig(const char *packet, int len)
{

    if (packet == NULL || len <= 0)
    {
        ESP_LOGE(TAG, "Invalid config packet");
        return;
    }

    cJSON *root = cJSON_Parse(packet);
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Failed to parse ChangeConfig json: %s",
                 cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "Cmd");
    if (!cJSON_IsString(cmd) || cmd->valuestring == NULL)
    {
        ESP_LOGE(TAG, "Invalid Cmd, expected ChangeConfig");
        cJSON_Delete(root);
        return;
    }

    cJSON *value = cJSON_GetObjectItem(root, "Value");
    if (!cJSON_IsObject(value))
    {
        ESP_LOGE(TAG, "No valid Value object");
        cJSON_Delete(root);
        return;
    }

    cJSON *external_devices = cJSON_GetObjectItem(root, "ExternalDevices");
    if (!cJSON_IsArray(external_devices))
    {
        ESP_LOGE(TAG, "No valid ExternalDevices array");
        cJSON_Delete(root);
        return;
    }

    delete_old_device_json_files();

    esp_err_t ret = save_cjson_object_to_spiffs(BASE_CONFIG_JSON_FILE, value);
    if (ret != ESP_OK)
    {
        cJSON_Delete(root);
        return;
    }

    int device_count = cJSON_GetArraySize(external_devices);
    int saved_count = 0;

    for (int i = 0; i < device_count; i++)
    {
        cJSON *device = cJSON_GetArrayItem(external_devices, i);
        if (!cJSON_IsObject(device))
        {
            ESP_LOGW(TAG, "Skip invalid ExternalDevices item, index=%d", i);
            continue;
        }

        cJSON *name = cJSON_GetObjectItem(device, "Name");
        if (!cJSON_IsString(name) || name->valuestring == NULL)
        {
            ESP_LOGW(TAG, "Skip device without Name, index=%d", i);
            continue;
        }

        char filename[DEVICE_JSON_FILE_MAX_LEN];
        if (!build_device_json_filename(name->valuestring, filename, sizeof(filename)))
        {
            ESP_LOGW(TAG, "Build device filename failed, name=%s", name->valuestring);
            continue;
        }

        ret = save_cjson_object_to_spiffs(filename, device);
        if (ret == ESP_OK)
        {
            saved_count++;
        }
    }

    ESP_LOGI(TAG, "ChangeConfig saved, device_count=%d, saved_count=%d", device_count, saved_count);

    cJSON_Delete(root);
}

esp_err_t process_packet_AllConfig_MQTT(const char *packet, int len)
{
    if (packet == NULL || len <= 0)
    {
        ESP_LOGE(TAG, "Invalid MQTT config packet");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(packet);
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Failed to parse MQTT config json: %s",
                 cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");
        return ESP_FAIL;
    }

    cJSON *seq = cJSON_GetObjectItem(root, "Seq");
    if (!cJSON_IsNumber(seq))
    {
        ESP_LOGE(TAG, "No valid seq in MQTT config json");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *data = cJSON_GetObjectItem(root, "Data");
    if (!cJSON_IsObject(data))
    {
        ESP_LOGE(TAG, "No valid Data object in MQTT config json");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *basic_config = cJSON_GetObjectItem(data, "BasicConfig");
    if (!cJSON_IsObject(basic_config))
    {
        ESP_LOGE(TAG, "No valid Data.BasicConfig object");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *external_devices = cJSON_GetObjectItem(data, "ExternalDevices");
    if (!cJSON_IsArray(external_devices))
    {
        ESP_LOGE(TAG, "No valid Data.ExternalDevices array");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    delete_old_device_json_files();

    esp_err_t ret = save_cjson_object_to_spiffs(BASE_CONFIG_JSON_FILE, basic_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Save MQTT BasicConfig failed: %s", esp_err_to_name(ret));
        cJSON_Delete(root);
        return ret;
    }

    int device_count = cJSON_GetArraySize(external_devices);
    int saved_count = 0;

    for (int i = 0; i < device_count; i++)
    {
        cJSON *device = cJSON_GetArrayItem(external_devices, i);
        if (!cJSON_IsObject(device))
        {
            ESP_LOGW(TAG, "Skip invalid MQTT ExternalDevices item, index=%d", i);
            continue;
        }

        cJSON *name = cJSON_GetObjectItem(device, "Name");
        if (!cJSON_IsString(name) || name->valuestring == NULL || strlen(name->valuestring) == 0)
        {
            ESP_LOGW(TAG, "Skip MQTT device without valid Name, index=%d", i);
            continue;
        }

        cJSON *proto_id = cJSON_GetObjectItem(device, "ProtoID");
        if (cJSON_IsNumber(proto_id) && proto_id->valueint == 0)
        {
            ESP_LOGW(TAG, "Skip MQTT placeholder device, name=%s, ProtoID=0", name->valuestring);
            continue;
        }

        char filename[31];

        sprintf(filename, "Device%d%s", i, DEVICE_JSON_SUFFIX);

        // if (!build_device_json_filename(name->valuestring, filename, sizeof(filename)))
        // {
        //     ESP_LOGW(TAG, "Build MQTT device filename failed, name=%s", name->valuestring);
        //     continue;
        // }

        ret = save_cjson_object_to_spiffs(filename, device);
        if (ret == ESP_OK)
        {
            saved_count++;
        }
        else
        {
            ESP_LOGW(TAG, "Save MQTT device failed, name=%s, ret=%s", name->valuestring, esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG,
             "MQTT ChangeConfig saved, Req=%d, device_count=%d, saved_count=%d",
             seq->valueint,
             device_count,
             saved_count);

    cJSON_Delete(root);

    return ESP_OK;
}

// 串口版本的处理函数
char *CreateReadConfigJsonFromFiles(void)
{
    char *base_json = read_json_from_spiffs(BASE_CONFIG_JSON_FILE);
    if (base_json == NULL)
    {
        ESP_LOGE(TAG, "Failed to read %s", BASE_CONFIG_JSON_FILE);
        return NULL;
    }

    cJSON *base_obj = cJSON_Parse(base_json);
    free(base_json);

    if (!cJSON_IsObject(base_obj))
    {
        ESP_LOGE(TAG, "Invalid %s", BASE_CONFIG_JSON_FILE);
        cJSON_Delete(base_obj);
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *devices = cJSON_CreateArray();
    if (root == NULL || devices == NULL)
    {
        cJSON_Delete(base_obj);
        cJSON_Delete(root);
        cJSON_Delete(devices);
        return NULL;
    }

    cJSON_AddStringToObject(root, "Cmd", "ReadConfig");
    cJSON_AddItemToObject(root, "Value", base_obj);
    cJSON_AddItemToObject(root, "ExternalDevices", devices);

    char file_names[SPIFFS_MAX_FILE_LIST_COUNT][SPIFFS_FILE_NAME_MAX_LEN];
    size_t file_count = 0;

    if (spiffs_get_file_names(file_names, SPIFFS_MAX_FILE_LIST_COUNT, &file_count) == ESP_OK)
    {
        for (size_t i = 0; i < file_count; i++)
        {
            if (!str_ends_with(file_names[i], DEVICE_JSON_SUFFIX))
            {
                continue;
            }

            char *device_json = read_json_from_spiffs(file_names[i]);
            if (device_json == NULL)
            {
                continue;
            }

            cJSON *device_obj = cJSON_Parse(device_json);
            free(device_json);

            if (cJSON_IsObject(device_obj))
            {
                cJSON_AddItemToArray(devices, device_obj);
            }
            else
            {
                cJSON_Delete(device_obj);
            }
        }
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

// MQTT版本的读取配置返回函数
char *CreateReadConfigJsonFromFiles_MQTT(int seq, int cmd_seq, int code, const char *msg)
{
    char *base_json = read_json_from_spiffs(BASE_CONFIG_JSON_FILE);
    if (base_json == NULL)
    {
        ESP_LOGE(TAG, "Failed to read %s", BASE_CONFIG_JSON_FILE);
        return NULL;
    }

    cJSON *base_obj = cJSON_Parse(base_json);
    free(base_json);

    if (!cJSON_IsObject(base_obj))
    {
        ESP_LOGE(TAG, "Invalid %s", BASE_CONFIG_JSON_FILE);
        cJSON_Delete(base_obj);
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *cmd_data = cJSON_CreateObject();
    cJSON *devices = cJSON_CreateArray();

    if (root == NULL || data == NULL || cmd_data == NULL || devices == NULL)
    {
        cJSON_Delete(base_obj);
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(cmd_data);
        cJSON_Delete(devices);
        return NULL;
    }

    cJSON_AddNumberToObject(root, "Seq", seq);

    cJSON_AddItemToObject(root, "Data", data);
    cJSON_AddNumberToObject(data, "CmdSeq", cmd_seq);
    cJSON_AddNumberToObject(data, "Code", code);

    if (msg == NULL)
    {
        cJSON_AddNullToObject(data, "Msg");
    }
    else
    {
        cJSON_AddStringToObject(data, "Msg", msg);
    }

    cJSON_AddItemToObject(data, "CmdData", cmd_data);
    cJSON_AddItemToObject(cmd_data, "BasicConfig", base_obj);
    cJSON_AddItemToObject(cmd_data, "ExternalDevices", devices);

    char file_names[SPIFFS_MAX_FILE_LIST_COUNT][SPIFFS_FILE_NAME_MAX_LEN];
    size_t file_count = 0;

    if (spiffs_get_file_names(file_names, SPIFFS_MAX_FILE_LIST_COUNT, &file_count) == ESP_OK)
    {
        for (size_t i = 0; i < file_count; i++)
        {
            if (!str_ends_with(file_names[i], DEVICE_JSON_SUFFIX))
            {
                continue;
            }

            char *device_json = read_json_from_spiffs(file_names[i]);
            if (device_json == NULL)
            {
                ESP_LOGW(TAG, "Failed to read device json file: %s", file_names[i]);
                continue;
            }

            cJSON *device_obj = cJSON_Parse(device_json);
            free(device_json);

            if (cJSON_IsObject(device_obj))
            {
                cJSON_AddItemToArray(devices, device_obj);
            }
            else
            {
                ESP_LOGW(TAG, "Invalid device json file: %s", file_names[i]);
                cJSON_Delete(device_obj);
            }
        }
    }
    else
    {
        ESP_LOGW(TAG, "Failed to get SPIFFS file list");
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return out;
}

char *CreateDataReportJson(int idnum, int timestamp, char *sn_buffer, int current_step, const double *values, int value_count, int decimal_places)
{
    if (values == NULL || value_count <= 0)
    {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return NULL;
    }

    cJSON_AddStringToObject(root, "Cmd", "DataReport");
    cJSON_AddNumberToObject(root, "CurrentStep", current_step);
    cJSON_AddStringToObject(root, "SN", sn_buffer);
    cJSON_AddNumberToObject(root, "IDNUM", idnum);
    cJSON_AddNumberToObject(root, "TIMESTAMP", timestamp);

    cJSON *value_array = cJSON_CreateArray();
    if (value_array == NULL)
    {
        cJSON_Delete(root);
        return NULL;
    }

    char buffer[64];
    for (int i = 0; i < value_count; i++)
    {
        if (decimal_places >= 0)
        {
            snprintf(buffer, sizeof(buffer), "%.*f", decimal_places, values[i]);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "%g", values[i]);
        }
        cJSON_AddItemToArray(value_array, cJSON_CreateString(buffer));
    }

    cJSON_AddItemToObject(root, "Value", value_array);

    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_string;
}

// 解析 JSON 字符串中的 int64_t 值
int get_json_int64(const char *json, const char *key, int64_t *value)
{
    char key_buf[64];

    snprintf(key_buf, sizeof(key_buf), "\"%s\"", key);

    // 找到 "ID"
    const char *p = strstr(json, key_buf);
    if (p == NULL)
    {
        return -1;
    }

    // 找到 :
    p = strchr(p + strlen(key_buf), ':');
    if (p == NULL)
    {
        return -1;
    }

    p++;

    // 跳过空格
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    {
        p++;
    }

    char *endptr;

    // 直接从原始 JSON 字符串转换成 int64_t
    long long temp = strtoll(p, &endptr, 10);

    if (p == endptr)
    {
        return -1;
    }

    *value = (int64_t)temp;

    return 0;
}

// 生成设备模式的JSON字符串
char *create_json_deviceMode(int seq, int mode, const char *aging_num)
{
    cJSON *root = NULL;
    cJSON *data = NULL;
    char *json_str = NULL;

    // 1. 创建根对象
    root = cJSON_CreateObject();
    if (root == NULL)
        goto fail;

    // 2. 添加 Seq 字段
    if (!cJSON_AddNumberToObject(root, "Seq", seq))
        goto fail;

    // 3. 创建 Data 嵌套对象
    data = cJSON_CreateObject();
    if (data == NULL)
        goto fail;

    // 4. 向 Data 中添加字段
    if (!cJSON_AddNumberToObject(data, "Mode", mode))
        goto fail;
    if (!cJSON_AddStringToObject(data, "AgingNumber", aging_num))
        goto fail;

    // 5. 将 Data 挂载到根对象（注意：成功后 root 接管 data 的生命周期）
    if (!cJSON_AddItemToObject(root, "Data", data))
        goto fail;
    data = NULL; // 防止 fail 标签中重复释放

    // 6. 打印为紧凑JSON字符串（不带换行和空格）
    json_str = cJSON_PrintUnformatted(root);

fail:
    // 7. 清理根对象（会递归释放所有子节点）
    if (root != NULL)
    {
        cJSON_Delete(root);
    }
    else if (data != NULL)
    {
        // 仅当 data 未成功挂载到 root 时才单独释放
        cJSON_Delete(data);
    }

    return json_str;
}

/**
 * @brief 构造老化阶段JSON字符串
 * @return malloc分配的JSON字符串，调用方负责free；失败返回NULL
 */
char *create_json_agingStage(int timestamp, const char *aging_stage, const char *aging_number)
{
    // 预估最大长度，留足余量
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
                       "{\"Seq\":%d,"
                       "\"Data\":{"
                       "\"AgingStage\":\"%s\","
                       "\"AgingNumber\":\"%s\""
                       "}}",
                       timestamp, aging_stage, aging_number);

    if (len < 0 || (size_t)len >= sizeof(buf))
    {
        return NULL; // 截断或编码错误
    }

    char *json_str = malloc(len + 1);
    if (json_str == NULL)
    {
        return NULL;
    }
    memcpy(json_str, buf, len + 1);

    return json_str;
}

/**
 * @brief 构造老化完成状态JSON字符串
 * @return malloc分配的JSON字符串，调用方负责free；失败返回NULL
 */
char *create_json_agingComplete(int timestamp, int is_complete, const char *aging_number)
{
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
                       "{\"Seq\":%d,"
                       "\"Data\":{"
                       "\"IsComplete\":%d,"
                       "\"AgingNumber\":\"%s\""
                       "}}",
                       timestamp, is_complete, aging_number);

    if (len < 0 || (size_t)len >= sizeof(buf))
    {
        return NULL;
    }

    char *json_str = malloc(len + 1);
    if (json_str == NULL)
    {
        return NULL;
    }
    memcpy(json_str, buf, len + 1);

    return json_str;
}

/**
 * @brief 使用已经固化的结构化 Value JSON 生成完整老化数据上报包。
 *
 * Value JSON 在采样时生成并同时用于 SQLite，因此实时上报与断网补发
 * 不再维护两套不同的数据名称/数据结构逻辑。
 */
char *create_sensor_json(int idnum, const char *pn, int current_step, int timestamp, const char *value_json)
{
    if (pn == NULL || pn[0] == '\0' || value_json == NULL || value_json[0] == '\0')
    {
        return NULL;
    }

    cJSON *value_array = cJSON_Parse(value_json);
    if (!cJSON_IsArray(value_array))
    {
        cJSON_Delete(value_array);
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL)
    {
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(value_array);
        return NULL;
    }

    /* 实时上报时 Seq 为发送时刻；TIMESTAMP 保持原始采样时刻。 */
    cJSON_AddNumberToObject(root, "Seq", time(NULL));
    cJSON_AddItemToObject(root, "Data", data);
    cJSON_AddNumberToObject(data, "IDNUM", idnum);
    cJSON_AddNumberToObject(data, "TIMESTAMP", timestamp);
    cJSON_AddStringToObject(data, "PN", pn);
    cJSON_AddStringToObject(data, "RecordId", RecordId);
    cJSON_AddNumberToObject(data, "CurrentStep", current_step);
    cJSON_AddItemToObject(data, "Value", value_array);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

/**
 * @brief 生成包含 Seq 和 AgingState 的 JSON 字符串
 *
 * 输出格式：
 * {
 *   "Seq": seq,
 *   "Data": {
 *     "AgingState": "agingStart"
 *   }
 * }
 *
 * @param seq          Seq 整数值
 * @param aging_state  AgingState 字符串（例如 "agingStart"）
 * @return char*       成功返回 JSON 字符串（需 free），失败返回 NULL
 */
char *create_aging_state_json(int seq, const char *aging_state)
{
    if (aging_state == NULL)
    {
        return NULL;
    }

    // 1. 创建根对象
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return NULL;
    }

    // 2. 添加 Seq
    cJSON_AddNumberToObject(root, "Seq", seq);

    // 3. 创建 Data 对象
    cJSON *data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "Data", data);

    // 4. 添加 AgingState 字符串
    cJSON_AddStringToObject(data, "AgingState", aging_state);

    // 5. 生成紧凑格式 JSON（无换行空格）
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root); // 释放 cJSON 对象树

    return json_str; // 调用者需 free()
}

char *create_aging_state_json_ProgramId(int seq, const char *ProgramId)
{
    if (ProgramId == NULL)
    {
        return NULL;
    }

    // 1. 创建根对象
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return NULL;
    }

    // 2. 添加 Seq
    cJSON_AddNumberToObject(root, "Seq", seq);

    // 3. 创建 Data 对象
    cJSON *data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "Data", data);

    // 4. 添加 ProgramId 字符串
    cJSON_AddStringToObject(data, "ProgramId", ProgramId);

    // 5. 生成紧凑格式 JSON（无换行空格）
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root); // 释放 cJSON 对象树

    return json_str; // 调用者需 free()
}

/**
 * @brief 生成如下格式的JSON:
 * {
 *   "Seq": Seq,
 *   "Data": {
 *     "CmdSeq": cmd_seq,
 *     "Code": code,
 *     "Msg": null,
 *     "CmdData": {
 *       "GetPN": get_pn
 *     }
 *   }
 * }
 * @param seq        Seq字段整数值
 * @param cmd_seq    CmdSeq字段整数值
 * @param code       Code字段整数值
 * @param get_pn     GetPN字符串
 * @return char*     成功返回JSON字符串（需free），失败返回NULL
 */
char *create_pn_response_json(int req, int cmd_seq, int code, const char *get_pn)
{
    if (get_pn == NULL)
    {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return NULL;
    }

    // 添加 Seq
    cJSON_AddNumberToObject(root, "Seq", req);

    // 创建 Data 对象
    cJSON *data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "Data", data);

    // 添加 Data 内部字段
    cJSON_AddNumberToObject(data, "CmdSeq", cmd_seq);
    cJSON_AddNumberToObject(data, "Code", code);
    cJSON_AddNullToObject(data, "Msg"); // null 值

    // 创建 CmdData 子对象
    cJSON *cmd_data = cJSON_CreateObject();
    if (cmd_data == NULL)
    {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(data, "CmdData", cmd_data);

    // 添加 GetPN
    cJSON_AddStringToObject(cmd_data, "GetPN", get_pn);

    // 生成紧凑JSON
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

/**
 * @brief 生成包含 Seq、Data（内含 CmdSeq, Code, Msg, CmdData: null）的 JSON
 *
 * @param seq       Seq 整数值
 * @param cmd_seq   CmdSeq 整数值
 * @param code      Code 整数值
 * @param msg       Msg 字符串（若为 NULL，则设为空字符串，但本例固定为 "指令重复，老化已启动"）
 * @return char*    成功返回 JSON 字符串（需 free），失败返回 NULL
 */
char *create_device_response(int seq, int cmd_seq, int code, const char *msg)
{
    if (msg == NULL)
    {
        return NULL;
    }

    // 1. 创建根对象
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return NULL;
    }

    // 2. 添加 Seq
    cJSON_AddNumberToObject(root, "Seq", seq);

    // 3. 创建 Data 对象
    cJSON *data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "Data", data);

    // 4. 添加 Data 内部字段
    cJSON_AddNumberToObject(data, "CmdSeq", cmd_seq);
    cJSON_AddNumberToObject(data, "Code", code);
    cJSON_AddStringToObject(data, "Msg", msg);
    cJSON_AddNullToObject(data, "CmdData"); // 固定为 null

    // 5. 生成紧凑格式 JSON
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str; // 调用者 free()
}

/**
 * @brief 修改JSON中的DEVICE_ID字段，其余内容保持不变
 * @param json_input  原始JSON字符串
 * @param new_device_id 新的设备ID字符串
 * @return malloc分配的新JSON字符串，调用方负责free；失败返回NULL
 */
char *update_device_id_in_json(const char *json_input, const char *new_device_id)
{
    // 1. 参数校验
    if (json_input == NULL || new_device_id == NULL) {
        return NULL;
    }

    // 2. 解析原始JSON
    cJSON *root = cJSON_Parse(json_input);
    if (root == NULL) {
        return NULL;
    }

    // 3. 定位到 Value 对象
    cJSON *value_obj = cJSON_GetObjectItem(root, "Value");
    if (value_obj == NULL || !cJSON_IsObject(value_obj)) {
        cJSON_Delete(root);
        return NULL;
    }

    // 4. 修改 DEVICE_ID
    cJSON *device_id_item = cJSON_GetObjectItem(value_obj, "DEVICE_ID");
    if (device_id_item == NULL) {
        // DEVICE_ID 不存在，新增一个
        cJSON_AddStringToObject(value_obj, "DEVICE_ID", new_device_id);
    } else {
        // DEVICE_ID 已存在，替换值
        cJSON_SetValuestring(device_id_item, "DEVICE_ID");  // 保持key不变
        cJSON_ReplaceItemInObject(value_obj, "DEVICE_ID",
                                  cJSON_CreateString(new_device_id));
    }

    // 5. 序列化为JSON字符串
    char *json_str = cJSON_PrintUnformatted(root);  // 紧凑格式，无换行缩进
    // 如果需要可读格式，改用 cJSON_Print(root)

    // 6. 释放cJSON树
    cJSON_Delete(root);

    if (json_str == NULL) {
        return NULL;
    }

    return json_str;
}
