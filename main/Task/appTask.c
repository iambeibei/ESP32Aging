#include <stdio.h>
#include <inttypes.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "Uart.h"
#include "spiffs_config.h"
#include "log.h"
#include "SqLite.h"
#include "Json_data.h"
#include "RS485.h"
#include "Ble_control.h"
#include "app_enc.h"
#include "ConfigData.h"
#include "mesh.h"
#include "esp_mac.h"
#include "can_extended.h"
#include "can_protocol_ext.h"
#include "Externaldevice.h"
#include "mqtt_app.h"
#include "modbus_json_parser.h"
#include "modbus_cmd.h"
#include "app_crc.h"
#include "scpi_dynamic.h"
#include "aging_config.h"
#include "GetProto.h"
#include "appTask.h"
#include "modbus_cmd.h"
#include "esp_heap_caps.h"
#include "app_mem.h"
#include "SelfRecovery.h"
#include "task_config.h"
#include "ota.h"

#pragma region 全局状态与任务辅助

// 单次老化采样最多包含的参数总数（老化设备 + 0~N 个外接设备）
#define AGING_UPLOAD_VALUE_COUNT 32
// 最多支持6个外接设备
#define MAX_EXTERNAL_DEVICE_COUNT 6
#define AGING_UPLOAD_SN_MAX_LEN 64

#define NVS_KEY_CURRENT_PN "bt_name"
#define NVS_KEY_READY_PN "bt_name_Ready"
#define NVS_KEY_NEW_PN_READY "new_pn_ready"
#define NVS_KEY_AGING_VALID "aging_valid"
#define NVS_KEY_START_PENDING "start_pending"
#define NVS_KEY_START_SEQ "start_seq"
#define NVS_KEY_DEVICE_NUMBER "device_number"
#define NVS_KEY_SAFE_CODE "safe_code"

// 型号直接代码写死,现在待定
const char DEVICE_TYPE[12] = "Point";

uint64_t DeviceNumber = 0; // 设备编号
uint64_t SafeCode = 0;     // 安全码

static void show_free_heap(const char *tag)
{
    size_t free_internal_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t min_internal_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(tag, "Internal SRAM free: %u bytes (%.2f KB)",
             (unsigned)free_internal_heap, free_internal_heap / 1024.0);
    ESP_LOGI(tag, "Internal SRAM minimum: %u bytes (%.2f KB)",
             (unsigned)min_internal_heap, min_internal_heap / 1024.0);
    ESP_LOGI(tag, "Internal SRAM largest block: %u bytes (%.2f KB)",
             (unsigned)largest_internal_block, largest_internal_block / 1024.0);
    ESP_LOGI(tag, "PSRAM free: %u bytes (%.2f KB)",
             (unsigned)free_spiram, free_spiram / 1024.0);
    ESP_LOGI(tag, "Free heap: %" PRId32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(tag, "Minimum free heap: %" PRId32 " bytes", esp_get_minimum_free_heap_size());
}

static const char *TAG = "app_task";

static Externaldevice *Aging_device = NULL; // 待老化设备
static Externaldevice *read_devices = NULL; // 外接设备数组，检测设备
static int read_device_count = 0;           // 外接设备数量

typedef struct
{
    char sn[AGING_UPLOAD_SN_MAX_LEN];
    int current_step;
    int timestamp;
    uint8_t value_count;
    /* 唯一结构化 Value；实时 MQTT 与 SQLite 直接共用。 */
    char *value_json;
} AgingUploadPacket;

long Programld = 0;      // 老化任务ID
uint8_t Cannextstep = 0; // 默认为0，群控模式下，收到下一步操作指令后，设置为1，表示可以执行下一步老化操作
uint8_t AgingCMode = 0;
uint8_t CycleIndex = 0;
AgingConfig agingcfg = {0}; // 定义全局老化配置结构体
// 老化设备的当前老化步骤
uint8_t CurrentAgingStep = 0;

AgingProcessState agingState = AgingIdle; // 定义全局老化状态变量

// 当前正在执行老化任务所使用的PN；一旦进入RUNNING，本轮结束前禁止修改。
char PN_Code[64] = {0};
// 老化使用到的任务记录ID
char RecordId[28] = {0};

// 已收到但尚未被下一条start_aging消费的新PN。
char PN_Code_Ready[64] = {0};
uint16_t s_new_pn_ready = 0;

/*
 * MQTT老化启动协调状态：
 * AGING_CMD_IDLE    : 当前没有待启动任务，也没有正在运行的老化任务；
 * AGING_CMD_WAIT_PN : 已收到start_aging并缓存，但尚未收到本轮新的set_pn；
 * AGING_CMD_RUNNING : PN和start_aging均已就绪，老化任务已经真正启动。
 *
 * 注意：PN_Code中即使残留旧PN，也不能据此判断下一轮是否可以启动；
 * 只有s_new_pn_ready == 1才表示PN_Code_Ready中存在“本轮新PN”。
 */
typedef enum
{
    AGING_CMD_IDLE = 0,
    AGING_CMD_WAIT_PN,
    AGING_CMD_RUNNING
} AgingCommandState;

static AgingCommandState s_aging_cmd_state = AGING_CMD_IDLE;
static int s_pending_start_seq = 0;

int8_t Mqtt_Log_Mode = -1; // MQTT日志上报模式，-1表示不启动，0为启动运行日志，1为启动错误日志

AgingDataAcquisitionMode agingDataAMode = IdleState;
double D_SOC = 100; // 待老化设备的SOC

TaskHandle_t BleData_task_handle = NULL; // 蓝牙任务句柄，用于删除任务使用

TaskHandle_t Aging_test_task_handle = NULL; // 老化工艺执行任务句柄，用于删除任务使用

TaskHandle_t AgingData_task_handle = NULL; // 老化数据获取处理任务句柄，用于删除任务使用

/* 长期任务保存句柄，防止按键调试或网络重连时重复创建同一任务。 */
static TaskHandle_t s_uart_json_task_handle = NULL;
static TaskHandle_t s_network_init_task_handle = NULL;
static TaskHandle_t s_mqtt_rx_task_handle = NULL;
static TaskHandle_t s_http_auto_task_handle = NULL;
static TaskHandle_t s_http_rx_task_handle = NULL;
static TaskHandle_t s_aging_upload_task_handle = NULL;
static TaskHandle_t s_button_task_handle = NULL;

static QueueHandle_t Upload_data_queue = NULL; // 上传数据队列

AgingResumeState agingResumeState = {0}; // 定义全局老化恢复状态变量

uint8_t BTDisConnect = 0; // 蓝牙断开标志位，为1是主动去断开蓝牙触发的，为0是被动断开蓝牙触发的

void Aging_Test_Task(void *arg);
void app_AgingData_Get_handle(void *arg);
void app_AgingData_Upload_handle(void *arg);
static bool aging_command_json_parse(const char *packet_copy);
static bool Data_Get_Method(Externaldevice *device, const char *id, uint8_t slave_addr, double *value, uint8_t is_name);
static void Device_Init(Externaldevice **device1, int devicecount);
static void AgingDevice_RuntimeFree(void);
static bool ExDevice_Check(void);
const char *AgingDataName(const char *id);

/*
 * 所有本项目业务任务统一固定到 CPU1。
 * ESP32-S3 双核调度器会自动启动 CPU1，不需要单独“开启”CPU1。
 */
static BaseType_t create_cpu1_task(TaskFunction_t task_fn, const char *task_name, uint32_t stack_size, UBaseType_t priority, TaskHandle_t *task_handle)
{
    if (task_handle != NULL && *task_handle != NULL)
    {
        ESP_LOGW(TAG, "Task %s already exists; skip duplicate creation", task_name);
        return pdPASS;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(task_fn, task_name, stack_size, NULL, priority, task_handle, LG_APP_CPU_CORE);
    if (ret == pdPASS)
    {
        ESP_LOGI(TAG, "Task %s created on CPU%d, stack=%u, priority=%u", task_name, LG_APP_CPU_CORE, (unsigned)stack_size, (unsigned)priority);
    }
    else
    {
        ESP_LOGE(TAG, "Task %s creation failed, internal_free=%u, largest_block=%u", task_name, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    return ret;
}

static void print_task_stack_watermark(const char *name, TaskHandle_t task_handle)
{
    if (task_handle == NULL)
    {
        ESP_LOGI("STACK", "%s: not running", name);
        return;
    }

    UBaseType_t free_stack = uxTaskGetStackHighWaterMark(task_handle);
    ESP_LOGI("STACK", "%s: minimum free stack=%u bytes",
             name,
             (unsigned)free_stack);
}

static void print_application_task_stacks(void)
{
    print_task_stack_watermark("uart_json", s_uart_json_task_handle);
    print_task_stack_watermark("net_init", s_network_init_task_handle);
    print_task_stack_watermark("mqtt_rx", s_mqtt_rx_task_handle);
    print_task_stack_watermark("http_auto", s_http_auto_task_handle);
    print_task_stack_watermark("http_rx", s_http_rx_task_handle);
    print_task_stack_watermark("aging_upload", s_aging_upload_task_handle);
    print_task_stack_watermark("ble_data", BleData_task_handle);
    print_task_stack_watermark("aging_ctrl", Aging_test_task_handle);
    print_task_stack_watermark("aging_get", AgingData_task_handle);
    print_task_stack_watermark("button", s_button_task_handle);
}

#pragma endregion

#pragma region 运行日志与错误记录

#define APP_LOG_MESSAGE_SIZE 256
#define DATA_READ_ERROR_REPORT_INTERVAL_MS 60000

/*
 * 日志会话从收到 start_aging 指令开始，直到老化完成、停止或启动失败。
 * 所有错误均写入本地；只有日志会话激活后才通过 MQTT 上报运行日志和错误日志。
 */
static volatile bool AgingLogActive = false;
static uint32_t s_consecutive_data_read_failures = 0;
static TickType_t s_last_data_read_error_report_tick = 0;
static uint32_t s_consecutive_data_upload_failures = 0;
static TickType_t s_last_data_upload_error_report_tick = 0;

static const char *current_log_pn(void)
{
    return PN_Code[0] != '\0' ? PN_Code : "";
}

static void mqtt_run_log(const char *publish_string)
{
    if (publish_string == NULL || Mqtt_Log_Mode == -1 || Mqtt_Log_Mode == 1)
    {
        return;
    }

    app_mqtt_publish("device/%s/log/runtime", (char *)publish_string, DEVICE_ID);
}

static void mqtt_Error_log(const char *publish_string)
{
    if (publish_string == NULL || Mqtt_Log_Mode == -1 || Mqtt_Log_Mode == 0)
    {
        return;
    }

    app_mqtt_publish("device/%s/log/error", (char *)publish_string, DEVICE_ID);
}

static void format_log_message(char *buffer, size_t buffer_size, const char *format, va_list args)
{
    if (buffer == NULL || buffer_size == 0)
    {
        return;
    }

    buffer[0] = '\0';
    if (format != NULL)
    {
        vsnprintf(buffer, buffer_size, format, args);
    }
}

static void build_mqtt_log_message(char *buffer, size_t buffer_size, const char *message)
{
    if (buffer == NULL || buffer_size == 0)
    {
        return;
    }

    if (current_log_pn()[0] != '\0')
    {
        snprintf(buffer, buffer_size, "PN=%s, %s", current_log_pn(), message != NULL ? message : "");
    }
    else
    {
        snprintf(buffer, buffer_size, "%s", message != NULL ? message : "");
    }
}

/* 所有错误均写入本地；PN 已设置时自动带上 PN。 */
static void local_error_log(const char *format, ...)
{
    char message[APP_LOG_MESSAGE_SIZE];
    va_list args;

    va_start(args, format);
    format_log_message(message, sizeof(message), format, args);
    va_end(args);

    storage_write_record_cyclic(current_log_pn(), message);
}

/* 只在收到老化开始指令后的日志会话中上报关键运行阶段。 */
static void aging_runtime_log(const char *format, ...)
{
    if (!AgingLogActive)
    {
        return;
    }

    char message[APP_LOG_MESSAGE_SIZE];
    char mqtt_message[APP_LOG_MESSAGE_SIZE];
    va_list args;

    va_start(args, format);
    format_log_message(message, sizeof(message), format, args);
    va_end(args);

    build_mqtt_log_message(mqtt_message, sizeof(mqtt_message), message);
    mqtt_run_log(mqtt_message);
}

/*
 * 老化相关错误：无条件写本地；日志会话激活后，同时交给 runtime 和 error 接口。
 * 最终由 Mqtt_Log_Mode 决定实际发送哪个 MQTT 主题。
 */
static void aging_error_log(const char *format, ...)
{
    char message[APP_LOG_MESSAGE_SIZE];
    char mqtt_message[APP_LOG_MESSAGE_SIZE];
    va_list args;

    va_start(args, format);
    format_log_message(message, sizeof(message), format, args);
    va_end(args);

    storage_write_record_cyclic(current_log_pn(), message);

    if (!AgingLogActive)
    {
        return;
    }

    build_mqtt_log_message(mqtt_message, sizeof(mqtt_message), message);
    mqtt_run_log(mqtt_message);
    mqtt_Error_log(mqtt_message);
}

/* 数据采集是高频路径：每次失败写本地，MQTT 仅首次和每隔 60 秒上报一次。 */
static void aging_data_read_result(bool success, const Externaldevice *device, const char *parameter)
{
    const char *device_name = "unknown";
    if (device != NULL && device->name[0] != '\0')
    {
        device_name = device->name;
    }

    if (success)
    {
        return;
    }

    s_consecutive_data_read_failures++;

    char message[APP_LOG_MESSAGE_SIZE];
    snprintf(message,
             sizeof(message),
             "Data read failed, device=%s, parameter=%s, failure_count=%" PRIu32,
             device_name,
             parameter != NULL ? parameter : "unknown",
             s_consecutive_data_read_failures);

    storage_write_record_cyclic(current_log_pn(), message);

    if (!AgingLogActive)
    {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    bool should_report = (s_consecutive_data_read_failures == 1) ||
                         (s_last_data_read_error_report_tick == 0) ||
                         ((now - s_last_data_read_error_report_tick) >=
                          pdMS_TO_TICKS(DATA_READ_ERROR_REPORT_INTERVAL_MS));

    if (should_report)
    {
        char mqtt_message[APP_LOG_MESSAGE_SIZE];
        build_mqtt_log_message(mqtt_message, sizeof(mqtt_message), message);
        mqtt_run_log(mqtt_message);
        mqtt_Error_log(mqtt_message);
        s_last_data_read_error_report_tick = now;
    }
}

/* 数据上传失败同样每次写本地，MQTT 告警最多每 60 秒尝试一次。 */
static void aging_data_upload_result(bool success, int idnum)
{
    if (success)
    {
        if (s_consecutive_data_upload_failures > 0)
        {
            aging_runtime_log("Aging-data upload recovered, previous_failures=%" PRIu32,
                              s_consecutive_data_upload_failures);
        }
        s_consecutive_data_upload_failures = 0;
        s_last_data_upload_error_report_tick = 0;
        return;
    }

    s_consecutive_data_upload_failures++;

    char message[APP_LOG_MESSAGE_SIZE];
    snprintf(message,
             sizeof(message),
             "Aging-data upload failed and cached locally, id=%d, consecutive=%" PRIu32,
             idnum,
             s_consecutive_data_upload_failures);

    storage_write_record_cyclic(current_log_pn(), message);

    if (!AgingLogActive)
    {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    bool should_report = (s_consecutive_data_upload_failures == 1) ||
                         (s_last_data_upload_error_report_tick == 0) ||
                         ((now - s_last_data_upload_error_report_tick) >=
                          pdMS_TO_TICKS(DATA_READ_ERROR_REPORT_INTERVAL_MS));
    if (should_report)
    {
        char mqtt_message[APP_LOG_MESSAGE_SIZE];
        build_mqtt_log_message(mqtt_message, sizeof(mqtt_message), message);
        mqtt_run_log(mqtt_message);
        mqtt_Error_log(mqtt_message);
        s_last_data_upload_error_report_tick = now;
    }
}

esp_err_t storage_print_all_records(void)
{
    FILE *f = fopen("/log/records.dat", "rb");
    if (f == NULL)
    {
        printf("Failed to open file for reading");
        return ESP_FAIL;
    }

    // 获取文件大小
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // 计算实际记录数
    int total_records = file_size / RECORD_SIZE;
    if (total_records > MAX_RECORDS)
    {
        total_records = MAX_RECORDS;
    }

    printf("\n========== All Records (%d/%d) ==========\n", total_records, MAX_RECORDS);

    char buffer[RECORD_SIZE];
    int valid_count = 0;

    for (int i = 0; i < total_records; i++)
    {
        fseek(f, log_get_offset(i), SEEK_SET);
        size_t read = fread(buffer, 1, RECORD_SIZE, f);

        if (read == RECORD_SIZE)
        {
            // 检查记录是否为空（全0或首字节为0）
            if (buffer[0] != '\0')
            {
                // printf("[%d] %s\n", i, buffer);
                app_mqtt_publish("device/%s/log/local", buffer, DEVICE_ID);
                valid_count++;
            }
        }
        vTaskDelay(100);
    }

    // printf("=========================================\n");
    // printf("Total: %d valid records\n", valid_count);

    fclose(f);
    return ESP_OK;
}

#pragma endregion

#pragma region MQTT响应与状态上报

static void device_response_publish_point(int cmd_seq, int code, const char *msg)
{
    char *response = create_device_response(time(NULL), cmd_seq, code, msg);
    if (response)
    {
        if (app_mqtt_publish("device/%s/cmd/reply/point", response, DEVICE_ID) < 0)
        {

            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg), "MQTT response Publish Failed: %s", msg);
            storage_write_record_cyclic(current_log_pn(), log_msg);
        }
        free(response);
    }
}

static void device_response_publish_state(int cmd_seq, int code, const char *msg)
{
    char *response = create_device_response(time(NULL), cmd_seq, code, msg);
    if (response)
    {
        if (app_mqtt_publish("device/%s/cmd/reply/state", response, DEVICE_ID) < 0)
        {

            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg), "MQTT response Publish Failed: %s", msg);
            storage_write_record_cyclic(current_log_pn(), log_msg);
        }
        free(response);
    }
}

static void device_response_publish_setlog(int cmd_seq, int code, const char *msg)
{
    char *response = create_device_response(time(NULL), cmd_seq, code, msg);
    if (response)
    {
        if (app_mqtt_publish("device/%s/cmd/reply/setlog", response, DEVICE_ID) < 0)
        {

            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg), "MQTT response Publish Failed: %s", msg);
            storage_write_record_cyclic(current_log_pn(), log_msg);
        }
        free(response);
    }
}

// 配置上报
void Config_Report(int cmd_seq)
{
    char *json_str = CreateReadConfigJsonFromFiles_MQTT(time(NULL), cmd_seq, 1, NULL);
    if (json_str)
    {
        if (app_mqtt_publish("device/%s/cmd/reply/config", json_str, DEVICE_ID) < 0)
        {
            ESP_LOGE(TAG, "MQTT ReadConfig reply publish failed");
        }
        free(json_str);
    }
}

// MQTT版本的状态上报
static char *CreateStateUPJsonFromFiles_MQTT(int seq, int cmd_seq, int code, const char *msg)
{

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *cmd_data = cJSON_CreateObject();
    cJSON *devices = cJSON_CreateArray();

    if (root == NULL || data == NULL || cmd_data == NULL || devices == NULL)
    {
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
    cJSON_AddItemToObject(cmd_data, "ExternalDevices", devices);

    for (int i = 0; i < read_device_count; i++)
    {
        cJSON *device_obj = cJSON_CreateObject();
        if (device_obj == NULL)
        {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddStringToObject(device_obj, "Name", read_devices[i].name);
        cJSON_AddBoolToObject(device_obj, "Online", read_devices[i].online);
        cJSON_AddItemToArray(devices, device_obj);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return out;
}

/**
 * @brief 从 JSON 中提取 Ids 数组，返回动态分配的 int 数组
 * @param json_str  输入的 JSON 字符串
 * @param out_count 输出参数，返回数组元素个数
 * @return int*     成功返回动态分配的 int 数组（需 free），失败返回 NULL
 */
int *parse_ids_array(const char *json_str, int *out_count)
{
    if (json_str == NULL || out_count == NULL)
    {
        return NULL;
    }

    // 1. 解析 JSON
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL)
    {
        printf("JSON parse error: %s\n", cJSON_GetErrorPtr());
        return NULL;
    }

    // 2. 获取 Data 对象
    cJSON *data = cJSON_GetObjectItem(root, "Data");
    if (!cJSON_IsObject(data))
    {
        cJSON_Delete(root);
        return NULL;
    }

    // 3. 获取 Ids 数组
    cJSON *ids = cJSON_GetObjectItem(data, "Ids");
    if (!cJSON_IsArray(ids))
    {
        cJSON_Delete(root);
        return NULL;
    }

    // 4. 获取数组长度
    int count = cJSON_GetArraySize(ids);
    if (count <= 0)
    {
        cJSON_Delete(root);
        return NULL;
    }

    // 5. 分配 int 数组
    int *array = (int *)malloc(count * sizeof(int));
    if (array == NULL)
    {
        cJSON_Delete(root);
        return NULL;
    }

    // 6. 遍历数组，将每个元素转为 int 存入
    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(ids, i);
        if (cJSON_IsNumber(item))
        {
            array[i] = item->valueint; // 或 (int)item->valuedouble
        }
        else
        {
            // 如果遇到非数字，可以忽略或设为默认值，这里简单处理为 0
            array[i] = 0;
        }
    }

    *out_count = count;
    cJSON_Delete(root);
    return array;
}

// 状态主动上报
static void aging_state_publish(const char *aging_state)
{
    char *response = create_aging_state_json(time(NULL), aging_state);
    if (response)
    {
        if (app_mqtt_publish("device/%s/event/state", response, DEVICE_ID) < 0)
        {
            storage_write_record_cyclic(current_log_pn(), "MQTT state Publish Failed");
        }
        free(response);
    }
}
// 利用状态来发送当前老化任务ID
static void aging_state_publish_ProgramId(const char *ProgramId)
{
    char *response = create_aging_state_json_ProgramId(time(NULL), ProgramId);
    if (response)
    {
        if (app_mqtt_publish("device/%s/event/state", response, DEVICE_ID) < 0)
        {
            storage_write_record_cyclic(current_log_pn(), "MQTT state Publish Failed");
        }
        free(response);
    }
}

/**
 * @brief 发布老化阶段信息到MQTT
 * @param aging_stage  老化阶段字符串，如 "Standing"
 * @param aging_number 老化编号字符串，如 "1"
 * @return 0:成功, -1:失败
 */
int publish_aging_stage(const char *aging_stage, const char *aging_number)
{
    // 1. 参数合法性校验
    if (aging_stage == NULL || aging_number == NULL)
    {
        storage_write_record_cyclic(current_log_pn(),
                                    "publish_aging_stage: NULL param");
        return -1;
    }

    // 2. 获取时间戳并生成JSON
    char *json_str = create_json_agingStage(time(NULL), aging_stage, aging_number);
    if (json_str == NULL)
    {
        storage_write_record_cyclic(current_log_pn(),
                                    "Create agingStage JSON Failed");
        return -1;
    }

    // 3. MQTT发布
    int ret = app_mqtt_publish("device/public/agingStage", json_str, NULL);
    if (ret < 0)
    {
        storage_write_record_cyclic(current_log_pn(),
                                    "MQTT agingStage Publish Failed");
    }

    // 4. 释放JSON内存
    free(json_str);

    return ret;
}

/**
 * @brief 发布老化完成状态到MQTT
 * @param is_complete   是否完成，1:完成, 0:未完成
 * @param aging_number  老化编号字符串，如 "1"
 * @return 0:成功, -1:失败
 */
int publish_aging_complete(int is_complete, const char *aging_number)
{
    // 1. 参数合法性校验
    if (aging_number == NULL)
    {
        storage_write_record_cyclic(current_log_pn(),
                                    "publish_aging_complete: NULL aging_number");
        return -1;
    }

    if (is_complete != 0 && is_complete != 1)
    {
        storage_write_record_cyclic(current_log_pn(), "publish_aging_complete: invalid IsComplete");
        return -1;
    }

    // 2. 生成JSON
    char *json_str = create_json_agingComplete(time(NULL), is_complete, aging_number);
    if (json_str == NULL)
    {
        storage_write_record_cyclic(current_log_pn(),
                                    "Create agingComplete JSON Failed");
        return -1;
    }

    // 3. MQTT发布
    int ret = app_mqtt_publish("device/%s/event/TaskCP", json_str, DEVICE_ID);
    if (ret < 0)
    {
        storage_write_record_cyclic(current_log_pn(),
                                    "MQTT agingComplete Publish Failed");
    }

    // 4. 释放JSON内存
    free(json_str);

    return ret;
}
/**
 * @brief 发布设备模式(单控/群控)到MQTT
 * @param mode 1:单控, 2:群控 (对应 ClMode->valueint)
 * @param aging_number 老化编号/数量
 * @return 0:成功, -1:失败
 */
int publish_device_mode(int mode)
{
    // 1. 参数合法性校验
    if (mode != 1 && mode != 2)
    {
        storage_write_record_cyclic(current_log_pn(), "Invalid deviceMode param");
        return -1;
    }

    char *json_str = create_json_deviceMode(time(NULL), mode, AgingNumber);
    if (json_str == NULL)
    {
        storage_write_record_cyclic(current_log_pn(),
                                    "Create deviceMode JSON Failed");
        return -1;
    }

    // 3. MQTT发布
    int ret = app_mqtt_publish("device/public/deviceMode", json_str, NULL);
    if (ret < 0)
    {
        storage_write_record_cyclic(current_log_pn(),
                                    "MQTT deviceMode Publish Failed");
    }

    // 4. 释放JSON内存（无论发布成功与否都必须释放）
    free(json_str);

    return ret;
}

#pragma endregion

#pragma region 配置读取与解析
#define JSON_CONFIG_VALUE_MAX_LEN 64

static char cfg_IsRoot[10] = {0};
static char cfg_SSID[JSON_CONFIG_VALUE_MAX_LEN] = {0};
static char cfg_WIFI_PS[JSON_CONFIG_VALUE_MAX_LEN] = {0};
static char cfg_SSID1[JSON_CONFIG_VALUE_MAX_LEN] = {0};
static char cfg_WIFI_PS1[JSON_CONFIG_VALUE_MAX_LEN] = {0};
static char cfg_Chanel[JSON_CONFIG_VALUE_MAX_LEN] = {0};
static char cfg_Mesh_ID[JSON_CONFIG_VALUE_MAX_LEN] = {0};
static char cfg_Mesh_PS[JSON_CONFIG_VALUE_MAX_LEN] = {0};
static char cfg_DEVICE_ID[32] = {0};
static char cfg_UDP_Port[10] = {0};
static char cfg_SERVER_IP[JSON_CONFIG_VALUE_MAX_LEN] = {0};
static char cfg_SERVER_UDP_Port[10] = {0};
static char cfg_AgingNumber[10] = {0};

static bool str_ends_with_local(const char *str, const char *suffix)
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

static bool copy_string_checked(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0 || src == NULL)
    {
        return false;
    }

    size_t src_len = strlen(src);
    if (src_len >= dst_size)
    {
        return false;
    }

    memcpy(dst, src, src_len + 1);
    return true;
}

/**
 * 将MAC地址字符串转换为字节数组
 * @param mac_str 输入字符串，格式如 "11:22:33:44:55:66" 或 "11-22-33-44-55-66"
 * @param mac_bytes 输出字节数组，至少6字节
 * @return true=成功, false=失败
 */
int mac_string_to_bytes(const char *mac_str)
{
    if (mac_str == NULL)
    {

        return 0;
    }
    // 临时存储解析的值
    unsigned int bytes[6];
    // 使用sscanf解析，支持冒号或短横线分隔
    int result = sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                        &bytes[0], &bytes[1], &bytes[2],
                        &bytes[3], &bytes[4], &bytes[5]);
    if (result != 6)
    {
        // 尝试用短横线格式
        result = sscanf(mac_str, "%02x-%02x-%02x-%02x-%02x-%02x",
                        &bytes[0], &bytes[1], &bytes[2],
                        &bytes[3], &bytes[4], &bytes[5]);
    }
    if (result != 6)
    {
        printf("Failed to parse MAC address: %s", mac_str);
        return 0;
    }
    // 转换为uint8_t
    for (int i = 0; i < 6; i++)
    {
        MESH_ID[i] = (uint8_t)bytes[i];
    }
    return 1;
}

static void json_get_string_to_buf(cJSON *root,
                                   const char *key,
                                   char *buf,
                                   size_t buf_size,
                                   const char *default_value)
{
    if (root == NULL || key == NULL || buf == NULL || buf_size == 0)
    {
        return;
    }

    cJSON *item = cJSON_GetObjectItem(root, key);

    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        if (!copy_string_checked(buf, buf_size, item->valuestring))
        {
            ESP_LOGW(TAG, "Config value too long, key=%s", key);
            copy_string_checked(buf, buf_size, default_value ? default_value : "");
        }
    }
    else if (cJSON_IsNumber(item))
    {
        snprintf(buf, buf_size, "%d", item->valueint);
    }
    else
    {
        copy_string_checked(buf, buf_size, default_value ? default_value : "");
    }
}

static esp_err_t load_baseconfig_json(void)
{
    char *json_str = read_json_from_spiffs(BASE_CONFIG_JSON_FILE);
    if (json_str == NULL)
    {
        ESP_LOGE(TAG, "Failed to read %s", BASE_CONFIG_JSON_FILE);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (!cJSON_IsObject(root))
    {
        ESP_LOGE(TAG, "Failed to parse %s", BASE_CONFIG_JSON_FILE);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    json_get_string_to_buf(root, "IsRoot", cfg_IsRoot, sizeof(cfg_IsRoot), "0");
    json_get_string_to_buf(root, "SSID", cfg_SSID, sizeof(cfg_SSID), "default");
    json_get_string_to_buf(root, "WIFI_PS", cfg_WIFI_PS, sizeof(cfg_WIFI_PS), "default");
    json_get_string_to_buf(root, "SSID1", cfg_SSID1, sizeof(cfg_SSID1), "default");
    json_get_string_to_buf(root, "WIFI_PS1", cfg_WIFI_PS1, sizeof(cfg_WIFI_PS1), "default");
    json_get_string_to_buf(root, "Chanel", cfg_Chanel, sizeof(cfg_Chanel), "0");
    json_get_string_to_buf(root, "Mesh_ID", cfg_Mesh_ID, sizeof(cfg_Mesh_ID), "11:22:33:44:55:66");
    json_get_string_to_buf(root, "Mesh_PS", cfg_Mesh_PS, sizeof(cfg_Mesh_PS), "123456789");

    // 新配置字段叫 DEVICE_ID，兼容读取旧字段 UDP_ID 作为兜底。
    cJSON *device_id = cJSON_GetObjectItem(root, "DEVICE_ID");
    if (device_id != NULL)
    {
        json_get_string_to_buf(root, "DEVICE_ID", cfg_DEVICE_ID, sizeof(cfg_DEVICE_ID), "100000001");
    }
    else
    {
        json_get_string_to_buf(root, "UDP_ID", cfg_DEVICE_ID, sizeof(cfg_DEVICE_ID), "100000001");
    }

    json_get_string_to_buf(root, "UDP_Port", cfg_UDP_Port, sizeof(cfg_UDP_Port), "0");
    json_get_string_to_buf(root, "SERVER_IP", cfg_SERVER_IP, sizeof(cfg_SERVER_IP), "default");
    json_get_string_to_buf(root, "SERVER_UDP_Port", cfg_SERVER_UDP_Port, sizeof(cfg_SERVER_UDP_Port), "0");
    json_get_string_to_buf(root, "AgingNumber", cfg_AgingNumber, sizeof(cfg_AgingNumber), "-1");

    IsRoot = (uint8_t)atoi(cfg_IsRoot);
    IsRoot_R = cfg_IsRoot;

    SSID = cfg_SSID;
    WIFI_PS = cfg_WIFI_PS;
    SSID1 = cfg_SSID1;
    WIFI_PS1 = cfg_WIFI_PS1;

    Chanel = (uint8_t)atoi(cfg_Chanel);
    Chanel_R = cfg_Chanel;

    Mesh_ID = cfg_Mesh_ID;
    mac_string_to_bytes(Mesh_ID);

    Mesh_PS = cfg_Mesh_PS;
    DEVICE_ID = cfg_DEVICE_ID;

    UDP_Port = atoi(cfg_UDP_Port);
    UDP_Port_R = cfg_UDP_Port;

    SERVER_IP = cfg_SERVER_IP;
    SERVER_UDP_Port = atoi(cfg_SERVER_UDP_Port);
    SERVER_UDP_Port_R = cfg_SERVER_UDP_Port;
    AgingNumber = cfg_AgingNumber;

    cJSON_Delete(root);

    ESP_LOGI(TAG, "%s loaded", BASE_CONFIG_JSON_FILE);
    return ESP_OK;
}

static esp_err_t parse_device_json_to_externaldevice(const char *filename, Externaldevice *device)
{
    if (filename == NULL || device == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char *json_str = read_json_from_spiffs(filename);
    if (json_str == NULL)
    {
        ESP_LOGE(TAG, "Failed to read device json: %s", filename);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(json_str);

    // 解析 ProtoID 字段为 int64_t,提前解析，cjson解析后可能会丢失精度
    int64_t protoid = 0;
    get_json_int64(json_str, "ProtoID", &protoid);

    free(json_str);

    if (!cJSON_IsObject(root))
    {
        ESP_LOGE(TAG, "Failed to parse device json: %s", filename);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *name = cJSON_GetObjectItem(root, "Name");
    cJSON *proto_id = cJSON_GetObjectItem(root, "ProtoID");
    cJSON *communication = cJSON_GetObjectItem(root, "Communication");
    cJSON *proto = cJSON_GetObjectItem(root, "Proto");
    cJSON *aging_steps = cJSON_GetObjectItemCaseSensitive(root, "AgingSteps");

    if (!cJSON_IsString(name) || name->valuestring == NULL ||
        !cJSON_IsNumber(proto_id) ||
        !cJSON_IsString(communication) || communication->valuestring == NULL ||
        !cJSON_IsString(proto) || proto->valuestring == NULL)
    {
        ESP_LOGE(TAG, "Invalid device json fields: %s", filename);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    memset(device, 0, sizeof(Externaldevice));

    if (!copy_string_checked(device->name, sizeof(device->name), name->valuestring) ||
        !copy_string_checked(device->CommunicationMode, sizeof(device->CommunicationMode), communication->valuestring) ||
        !copy_string_checked(device->Protocol, sizeof(device->Protocol), proto->valuestring))
    {
        ESP_LOGE(TAG, "Device json string too long: %s", filename);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    if (!cJSON_IsObject(aging_steps))
    {
        device->steps = NULL;
        device->step_count = 0;
    }
    else
    {
        // printf("Json中有老化协议步骤，开始解析...\n");

        AgingErr err = parse_steps_Ex(aging_steps, &device->steps, &device->step_count);
        if (err != AGING_OK)
        {
            ESP_LOGE(TAG, "Failed to parse aging steps: %s", filename);
            device->steps = NULL;
            device->step_count = 0;
        }
    }

    device->ProtoID = protoid;
    device->LastProtoID = 0;
    device->online = false;
    device->hardware_initialized = false;
    device->HaveProtocol = false;
    device->scpi_protocol = NULL;
    device->ProtoType = 9;

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Loaded device: file=%s, name=%s, ProtoID=%lld, Communication=%s, Proto=%s",
             filename,
             device->name,
             device->ProtoID,
             device->CommunicationMode,
             device->Protocol);

    return ESP_OK;
}

// 读取配置文件
static esp_err_t readconfig()
{
    read_device_count = 0;

    if (read_devices == NULL)
    {
        read_devices = (Externaldevice *)app_malloc_prefer_psram(sizeof(Externaldevice) * MAX_EXTERNAL_DEVICE_COUNT);
        if (read_devices == NULL)
        {
            ESP_LOGE(TAG, "read_devices malloc failed");
        }

        memset(read_devices, 0, sizeof(Externaldevice) * MAX_EXTERNAL_DEVICE_COUNT);
    }

    char file_names[MAX_EXTERNAL_DEVICE_COUNT][64];
    size_t file_count = 0;

    esp_err_t ret = spiffs_get_file_names(file_names, 10, &file_count);

    if (ret == ESP_OK)
    {
        for (size_t i = 0; i < file_count; i++)
        {
            if (strstr(file_names[i], "config") != NULL) // 基本配置加载
            {
                load_baseconfig_json();
            }
            else if (strstr(file_names[i], "device") != NULL) // 外接设备加载
            {
                Externaldevice *dev = &read_devices[read_device_count];

                ret = parse_device_json_to_externaldevice(file_names[i], dev);
                if (ret == ESP_OK)
                {
                    read_device_count++;
                }
            }
            vTaskDelay(10);
        }
    }

    return ret;
}

#pragma endregion

#pragma region UART配置命令与接收任务
// 命令处理任务，现在不使用
static void mesh_UartCommand(char *line)
{

    uint8_t target_mac[6];

    esp_mesh_get_routing_table((mesh_addr_t *)&s_route_table, CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &s_route_table_size);

    // 1. 创建本地缓冲区，复制原始数据
    char local_line[256]; // 根据最大命令长度调整
    if (strlen(line) >= sizeof(local_line))
    {
        printf("错误: 命令太长\n");

        return;
    }
    strcpy(local_line, line); // 复制到本地缓冲区

    // 解析命令
    char *cmd = strtok(local_line, " ");
    char *arg1 = strtok(NULL, " ");
    char *arg2 = strtok(NULL, "");

    if (cmd == NULL)
    {
        // 空命令
    }
    // === 原有命令 ===
    else if (strcmp(cmd, "list") == 0)
    {
        Mesh_cmd_list();
    }
    else if (strcmp(cmd, "info") == 0)
    {
        Mesh_cmd_info();
    }
    else if (strcmp(cmd, "send") == 0)
    {
        if (arg1 && arg2)
        {
            if (Mesh_parse_mac_address(arg1, target_mac))
            {
                cmd_send(target_mac, arg2);
            }
            else
            {
                printf("错误: MAC地址格式无效\n");
            }
        }
        else
        {
            printf("用法: send <MAC> <消息>\n");
            printf("示例: send 64:e8:33:46:20:2c hello\n");
        }
    }
    else if (strcmp(cmd, "broadcast") == 0)
    {
        if (arg1)
        {
            cmd_broadcast(arg1);
        }
        else
        {
            printf("用法: broadcast <消息>\n");
        }
    }

    // === 蓝牙本地命令 ===
    else if (strcmp(cmd, "ble_scan") == 0)
    {
        int scan_time = 10; // 默认10秒
        if (arg1 != NULL)
        {
            scan_time = atoi(arg1);
            if (scan_time <= 0 || scan_time > 30)
            {
                scan_time = 10;
            }
        }
        printf("开始BLE扫描 %d 秒...\n", scan_time);
        ble_start_scan(scan_time);
    }
    else if (strcmp(cmd, "ble_list") == 0)
    {
        ble_device_info_t devices[20];
        int count = ble_get_scanned_devices(devices, 20);

        if (count == 0)
        {
            printf("没有扫描到BLE设备，请先执行 ble_scan\n");
        }
        else
        {
            printf("\n=== BLE设备列表 (%d个) ===\n", count);
            for (int i = 0; i < count; i++)
            {
                printf("[%d] %s\n", i + 1, devices[i].name);
                printf("    地址: " ESP_BD_ADDR_STR "\n",
                       ESP_BD_ADDR_HEX(devices[i].bda));
                printf("    信号: %d dBm\n", devices[i].rssi);
                printf("    状态: %s\n\n",
                       devices[i].is_connected ? "已连接" : "可用");
            }
        }
    }
    else if (strcmp(cmd, "ble_connect") == 0)
    {
        if (arg1 == NULL)
        {
            printf("用法: ble_connect <设备名称>\n");
            printf("示例: ble_connect TemperatureSensor\n");
        }
        else
        {
            printf("正在连接BLE设备: %s\n", arg1);
            ble_connect_by_name(arg1);
        }
    }
    else if (strcmp(cmd, "ble_send") == 0)
    {
        if (arg1 == NULL)
        {
            printf("用法: ble_send_hex <十六进制数据>\n");
            printf("示例: ble_send_hex 010607DB00013945\n");
            printf("       ble_send_hex 0102030405\n");
        }
        else if (!ble_is_connected())
        {
            printf("错误: 没有连接到任何BLE设备\n");
        }
        else
        {
            // 将十六进制字符串转换为二进制数据
            uint8_t hex_data[128];
            int hex_len = 0;
            char *hex_str = arg1;
            char full_hex[256];

            // 如果还有arg2，组合完整的十六进制字符串
            if (arg2 != NULL)
            {
                snprintf(full_hex, sizeof(full_hex), "%s%s", arg1, arg2);
                hex_str = full_hex;
            }

            // 移除空格
            char clean_hex[256];
            int clean_idx = 0;
            for (int i = 0; hex_str[i] != '\0' && i < 256; i++)
            {
                if (hex_str[i] != ' ' && hex_str[i] != '-')
                {
                    clean_hex[clean_idx++] = hex_str[i];
                }
            }
            clean_hex[clean_idx] = '\0';

            // 检查长度是否为偶数
            int str_len = strlen(clean_hex);
            if (str_len % 2 != 0)

            {
                printf("错误: 十六进制字符串长度必须为偶数\n");
            }
            else
            {
                // 转换十六进制字符串到字节数组
                for (int i = 0; i < str_len && hex_len < sizeof(hex_data); i += 2)
                {
                    char byte_str[3] = {clean_hex[i], clean_hex[i + 1], 0};
                    hex_data[hex_len++] = strtol(byte_str, NULL, 16);
                }

                if (hex_len > 0)
                {
                    printf("发送十六进制数据 (%d 字节): ", hex_len);
                    for (int i = 0; i < hex_len; i++)
                    {
                        printf("%02x ", hex_data[i]);
                    }
                    printf("\n");
                    ble_send_data(hex_data, hex_len, 0);
                }
            }
        }
    }
    else if (strcmp(cmd, "ble_status") == 0)
    {
        printf("\n=== BLE状态 ===\n");
        printf("扫描状态: %s\n", ble_is_scanning() ? "扫描中" : "空闲");
        printf("连接状态: %s\n", ble_is_connected() ? "已连接" : "未连接");

        if (ble_is_connected())
        {
            printf("连接的设备: %s\n", ble_get_connected_device_name());
            uint8_t bda[6];
            if (ble_get_connected_device_addr(bda))
            {
                printf("设备地址: " ESP_BD_ADDR_STR "\n",
                       ESP_BD_ADDR_HEX(bda));
            }
        }

        ble_device_info_t devices[20];
        int count = ble_get_scanned_devices(devices, 20);
        printf("已扫描设备: %d个\n", count);
    }
    else if (strcmp(cmd, "ble_disconnect") == 0)
    {
        if (ble_is_connected())
        {
            printf("正在断开BLE连接...\n");
            ble_disconnect();
        }
        else
        {
            printf("当前没有连接的BLE设备\n");
        }
    }
    // === 远程蓝牙命令 ===
    else if (strcmp(cmd, "rble_scan") == 0)
    {
        if (arg1 == NULL)
        {
            printf("用法: rble_scan <目标MAC>\n");
            printf("示例: rble_scan 64:e8:33:46:20:78\n");
        }
        else if (parse_mac_address(arg1, target_mac))
        {
            uint8_t msg[2] = {CMD_BLE_SCAN_START, 10};
            send_mesh_message(target_mac, msg, sizeof(msg));
            printf("已发送BLE扫描命令到 " MACSTR "\n", MAC2STR(target_mac));
        }
        else
        {
            printf("错误: MAC地址格式无效\n");
        }
    }
    else if (strcmp(cmd, "rble_connect") == 0)
    {
        if (arg1 == NULL || arg2 == NULL)
        {
            printf("用法: rble_connect <目标MAC> <设备名称>\n");
            printf("示例: rble_connect 64:e8:33:46:20:78 TemperatureSensor\n");
        }
        else
        {
            // arg1是MAC，arg2是设备名
            char *mac_str = arg1;
            char *dev_name = arg2;

            if (parse_mac_address(mac_str, target_mac))
            {
                uint8_t msg[64];
                int name_len = strlen(dev_name);
                msg[0] = CMD_BLE_CONNECT;
                msg[1] = name_len;
                memcpy(msg + 2, dev_name, name_len);

                send_mesh_message(target_mac, msg, 2 + name_len);
                printf("已发送BLE连接命令到 " MACSTR " 设备: %s\n",
                       MAC2STR(target_mac), dev_name);
            }
            else
            {
                printf("错误: MAC地址格式无效\n");
            }
        }
    }
    else if (strcmp(cmd, "rble_send") == 0)
    {
        if (arg1 == NULL || arg2 == NULL)
        {
            printf("用法: rble_send_hex <目标MAC> <十六进制数据>\n");
            printf("示例: rble_send_hex 64:e8:33:46:20:78 010607DB00013945\n");
        }
        else
        {
            // arg1是MAC，arg2是十六进制数据
            char *mac_str = arg1;
            char *hex_str = arg2;

            if (parse_mac_address(mac_str, target_mac))
            {
                // 转换十六进制字符串为二进制数据
                uint8_t hex_data[128];
                int hex_len = 0;
                int str_len = strlen(hex_str);

                // 移除空格
                char clean_hex[256];
                int clean_idx = 0;
                for (int i = 0; hex_str[i] != '\0' && i < 256; i++)
                {
                    if (hex_str[i] != ' ' && hex_str[i] != '-')
                    {
                        clean_hex[clean_idx++] = hex_str[i];
                    }
                }
                clean_hex[clean_idx] = '\0';

                // 转换
                str_len = strlen(clean_hex);
                if (str_len % 2 == 0)
                {
                    for (int i = 0; i < str_len && hex_len < sizeof(hex_data); i += 2)
                    {
                        char byte_str[3] = {clean_hex[i], clean_hex[i + 1], 0};
                        hex_data[hex_len++] = strtol(byte_str, NULL, 16);
                    }

                    uint8_t msg[256];
                    msg[0] = CMD_BLE_SEND_DATA;
                    msg[1] = hex_len;
                    memcpy(msg + 2, hex_data, hex_len);

                    send_mesh_message(target_mac, msg, 2 + hex_len);
                    printf("已发送BLE十六进制命令到 " MACSTR ", %d 字节\n",
                           MAC2STR(target_mac), hex_len);
                }
                else
                {
                    printf("错误: 十六进制字符串长度必须为偶数\n");
                }
            }
            else
            {
                printf("错误: MAC地址格式无效\n");
            }
        }
    }
    else if (strcmp(cmd, "rble_status") == 0)
    {
        if (arg1 == NULL)
        {
            printf("用法: rble_status <目标MAC>\n");
            printf("示例: rble_status 64:e8:33:46:20:78\n");
        }
        else if (parse_mac_address(arg1, target_mac))
        {
            uint8_t msg[1] = {CMD_BLE_STATUS};
            send_mesh_message(target_mac, msg, sizeof(msg));
            printf("已发送BLE状态查询命令到 " MACSTR "\n", MAC2STR(target_mac));
        }
        else
        {
            printf("错误: MAC地址格式无效\n");
        }
    }
    // === 帮助命令 ===
    else if (strcmp(cmd, "help") == 0)
    {
        printf("\n╔══════════════════════════════════════════════════════════════════╗\n");
        printf("║                    Mesh网络控制台命令手册                          ║\n");
        printf("╚══════════════════════════════════════════════════════════════════╝\n");

        printf("\n📡 【Mesh网络命令】\n");
        printf("  ┌─────────────────────────────────────────────────────────────────┐\n");
        printf("  │ list                    - 显示Mesh路由表                        │\n");
        printf("  │ info                    - 显示本机Mesh节点信息                  │\n");
        printf("  │ send <MAC> <消息>       - 向指定MAC地址节点发送消息              │\n");
        printf("  │ broadcast <消息>        - 向所有Mesh节点广播消息                │\n");
        printf("  │ netstat                 - 显示网络状态（以太网/WiFi）           │\n");
        printf("  └─────────────────────────────────────────────────────────────────┘\n");

        printf("\n🔵 【本地蓝牙(BLE)命令】\n");
        printf("  ┌─────────────────────────────────────────────────────────────────┐\n");
        printf("  │ ble_scan [秒数]         - 扫描BLE设备（默认10秒，最大30秒）     │\n");
        printf("  │ ble_list                - 列出已扫描到的BLE设备                 │\n");
        printf("  │ ble_connect <设备名>    - 连接指定名称的BLE设备                 │\n");
        printf("  │ ble_send <十六进制数据> - 向已连接的BLE设备发送数据              │\n");
        printf("  │ ble_status              - 显示本地BLE状态（扫描/连接/设备）      │\n");
        printf("  │ ble_disconnect          - 断开当前BLE连接                       │\n");
        printf("  └─────────────────────────────────────────────────────────────────┘\n");

        printf("\n🌐 【远程蓝牙命令（通过Mesh网络控制其他节点）】\n");
        printf("  ┌─────────────────────────────────────────────────────────────────┐\n");
        printf("  │ rble_scan <目标MAC>     - 让指定节点扫描BLE设备                 │\n");
        printf("  │ rble_connect <目标MAC> <设备名> - 让指定节点连接BLE设备         │\n");
        printf("  │ rble_send <目标MAC> <十六进制数据> - 让指定节点发送BLE数据      │\n");
        printf("  │ rble_status <目标MAC>   - 查询指定节点的BLE状态                 │\n");
        printf("  └─────────────────────────────────────────────────────────────────┘\n");

        printf("\n⚙️ 【配置命令（通过串口JSON）】\n");
        printf("  ┌─────────────────────────────────────────────────────────────────┐\n");
        printf("  │ Readconfig              - 读取并返回当前配置（JSON格式）        │\n");
        printf("  │ Change{...}             - 修改配置（JSON格式，需完整包）        │\n");
        printf("  └─────────────────────────────────────────────────────────────────┘\n");

        printf("\n💡 【使用示例】\n");
        printf("  ┌─────────────────────────────────────────────────────────────────┐\n");
        printf("  │ 1. 查看路由表:              list                                │\n");
        printf("  │ 2. 广播消息:                broadcast hello                     │\n");
        printf("  │ 3. 单播消息:                send 64:e8:33:46:20:2c hello        │\n");
        printf("  │ 4. 本地扫描BLE:             ble_scan 15                         │\n");
        printf("  │ 5. 列出BLE设备:             ble_list                            │\n");
        printf("  │ 6. 连接BLE设备:             ble_connect TempSensor              │\n");
        printf("  │ 7. 发送BLE数据:             ble_send 010607DB00013945           │\n");
        printf("  │ 8. 远程控制扫描:            rble_scan 64:e8:33:46:20:78        │\n");
        printf("  │ 9. 远程查询状态:            rble_status 64:e8:33:46:20:78      │\n");
        printf("  │10. 查看网络状态:            netstat                             │\n");
        printf("  └─────────────────────────────────────────────────────────────────┘\n");

        printf("\n📝 【注意事项】\n");
        printf("  • MAC地址格式: XX:XX:XX:XX:XX:XX (十六进制，不区分大小写)\n");
        printf("  • 十六进制数据: 连续字符串如 010203AABB，支持空格分隔\n");
        printf("  • 远程命令需要目标节点在线且Mesh网络连通\n");
        printf("  • BLE扫描时间建议5-15秒，过长会占用较多资源\n");
        printf("  • 发送JSON配置时需保证数据包完整\n");

        printf("\n═══════════════════════════════════════════════════════════════════\n");
    }

    else if (strcmp(cmd, "netstat") == 0)
    {
        printf("\n=== 网络状态 ===\n");
        printf("活跃接口: %s\n", dual_net_get_active_interface());
        printf("以太网: %s\n", dual_net_is_ethernet_active() ? "已连接" : "断开");
        printf("WiFi: %s\n", wifi_up ? "已连接" : "断开");
        printf("WiFi认证失败: %s\n", wifi_auth_failed ? "是" : "否");
    }
    else
    {
        printf("未知命令: %s (输入 help 查看可用命令)\n", cmd);
    }
}

static void send_read_config_to_uart(void)
{
    char *json_str = CreateReadConfigJsonFromFiles();
    if (json_str == NULL)
    {
        ESP_LOGE(TAG, "CreateReadConfigJsonFromFiles failed");
        return;
    }

    app_write_UART_data((uint8_t *)json_str, strlen(json_str), 10);
    cJSON_free(json_str);
}

static bool Onely_Set_DeviceId(char *device_id)
{
    char *json_str = CreateReadConfigJsonFromFiles();
    if (json_str == NULL)
    {
        ESP_LOGE(TAG, "CreateReadConfigJsonFromFiles failed");
        return false;
    }

    char *updated_json = update_device_id_in_json(json_str, device_id);
    if (updated_json == NULL)
    {
        ESP_LOGE(TAG, "update_device_id_in_json failed");
        cJSON_free(json_str);
        return false;
    }

    process_packet_AllConfig(updated_json, strlen(updated_json));

    cJSON_free(json_str);
    return true;
}
// 串口接收数据任务处理，主要修改配置和读取配置，通过配置修改上位机
#define UART_JSON_CACHE_SIZE 3048
#define UART_READ_BLOCK_SIZE 1024

static void app_uart_data_handle(void *arg)
{
    char *uart_buf = (char *)app_malloc_prefer_psram(UART_READ_BLOCK_SIZE + 1);
    char *json_cache = (char *)app_malloc_prefer_psram(UART_JSON_CACHE_SIZE);

    if (uart_buf == NULL || json_cache == NULL)
    {
        ESP_LOGE(TAG, "UART json buffer malloc failed");
        free(uart_buf);
        free(json_cache);
        s_uart_json_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    int json_len = 0;
    uint8_t IsReset = 0; // 是否重启
    memset(json_cache, 0, UART_JSON_CACHE_SIZE);

    while (1)
    {
        memset(uart_buf, 0, UART_READ_BLOCK_SIZE + 1);

        int len = app_read_UART_data(uart_buf, UART_READ_BLOCK_SIZE, portMAX_DELAY);
        if (len <= 0)
        {
            continue;
        }

        uart_buf[len] = '\0';

        if (uart_buf[0] == 0x01 && uart_buf[1] == 0x03 && uart_buf[2] == 0x00 && uart_buf[3] == 0x02 && uart_buf[5] == 0x0A) // 查询设备型号
        {
            uint8_t frame[25];
            Modbus_BuildFrame_PN(DEVICE_TYPE, DeviceNumber, frame);
            app_write_UART_data(frame, 25, 10);
            // printf("查询设备型号，返回设备型号：%s\r\n", DEVICE_TYPE);
            // printf("查询设备型号，返回设备编号：%llu\r\n", DeviceNumber);
            continue;
        }
        else if (uart_buf[0] == 0x01 && uart_buf[1] == 0x03 && uart_buf[2] == 0x00 && uart_buf[3] == 0x02 && uart_buf[5] == 0x06) // 查类型
        {
            uint8_t frame[17];
            Modbus_BuildFrame_Type(DEVICE_TYPE, frame);
            app_write_UART_data(frame, 17, 10);
            continue;
        }
        else if (uart_buf[0] == 0x01 && uart_buf[1] == 0x03 && uart_buf[2] == 0x00 && uart_buf[3] == 0x1E) // 查询安全码
        {
            uint8_t frame[13];
            Modbus_BuildFrame_SafeCode(SafeCode, frame);
            app_write_UART_data(frame, 13, 10);
            continue;
        }
        else if (uart_buf[0] == 0x01 && uart_buf[1] == 0x03 && uart_buf[2] == 0x00 && uart_buf[3] == 0x08) // 纯数字
        {
            uint8_t frame[13];
            Modbus_BuildFrame_SafeCode(DeviceNumber, frame);
            app_write_UART_data(frame, 13, 10);
            continue;
        }
        else if (uart_buf[0] == 0x01 && uart_buf[1] == 0x10 && uart_buf[2] == 0x00 && uart_buf[3] == 0x08) // 纯数字写入
        {

            uint8_t frame[8];
            Modbus_BuildCmd_0110(0x08, frame);
            app_write_UART_data(frame, 8, 10);

            DeviceNumber = Modbus_ExtractU64((uint8_t *)uart_buf);
            SelfRecovery_Write_uint64(NVS_KEY_DEVICE_NUMBER, DeviceNumber);
            char newDeId[32];
            snprintf(newDeId, sizeof(newDeId), "%lld", DeviceNumber);
            Onely_Set_DeviceId(newDeId); // 将标定的PN写入配置json中去

            if (IsReset == 1)
            {
                vTaskDelay(1000);
                esp_restart();
            }
            IsReset = 1;
            continue;
        }
        else if (uart_buf[0] == 0x01 && uart_buf[1] == 0x10 && uart_buf[2] == 0x00 && uart_buf[3] == 0x1E) // 安全码写入
        {
            uint8_t frame[8];
            Modbus_BuildCmd_0110(0x1E, frame);
            app_write_UART_data(frame, 8, 10);

            SafeCode = Modbus_ExtractU64((uint8_t *)uart_buf);
            SelfRecovery_Write_uint64(NVS_KEY_SAFE_CODE, SafeCode);

            if (IsReset == 1)
            {
                vTaskDelay(1000);
                esp_restart();
            }
            IsReset = 1;
            continue;
        }

        if (json_len + len >= UART_JSON_CACHE_SIZE)
        {
            ESP_LOGE(TAG, "UART JSON cache overflow, discard %d bytes", json_len);
            json_len = 0;
            memset(json_cache, 0, UART_JSON_CACHE_SIZE);
        }

        memcpy(json_cache + json_len, uart_buf, (size_t)len);
        json_len += len;
        json_cache[json_len] = '\0';

        if (!(is_complete_json(json_cache, json_len)))
        {
            continue;
        }

        cJSON *pRoot = cJSON_Parse(json_cache);
        if (pRoot == NULL)
        {
            ESP_LOGE(TAG, "Failed to parse Uart_JSON");
            memset(json_cache, 0, UART_JSON_CACHE_SIZE);
            continue;
        }
        cJSON *cmd = cJSON_GetObjectItem(pRoot, "Cmd");
        if (!cJSON_IsString(cmd) || cmd->valuestring == NULL)
        {
            ESP_LOGE(TAG, "Invalid Uart_JSON: missing Cmd");
            cJSON_Delete(pRoot);
            memset(json_cache, 0, UART_JSON_CACHE_SIZE);
            continue;
        }
        char cmd_buf[32];
        memcpy(cmd_buf, cmd->valuestring, sizeof(cmd_buf) - 1);
        cmd_buf[sizeof(cmd_buf) - 1] = '\0';
        cJSON_Delete(pRoot);

        if (strncmp(cmd_buf, "ChangeConfig", 12) == 0)
        {
            // 先删除所有配置，再重新建立
            esp_err_t ret = spiffs_delete_all_files();
            if (ret == ESP_OK)
            {
                ESP_LOGI(TAG, "All SPIFFS files deleted");
            }
            else
            {
                ESP_LOGW(TAG, "Some SPIFFS files delete failed");
            }
            process_packet_AllConfig(json_cache, json_len);
            esp_restart();
        }
        else if (strncmp(cmd_buf, "ReadConfig", 10) == 0)
        {
            send_read_config_to_uart();
        }
        else
        {
            ESP_LOGW(TAG, "Unsupported UART JSON command");
        }

        json_len = 0;
        memset(json_cache, 0, UART_JSON_CACHE_SIZE);
    }
}

#pragma endregion

#pragma region RS485接收任务

// 串口接收数据任务处理，主要修改配置和读取配置，通过配置修改上位机
static void app_RS485_data_handle(void *arg)
{
    uint8_t RS485rbuf[1024] = {0};
    while (1)
    {
        memset(RS485rbuf, 0, sizeof(RS485rbuf));                             // 接收数据前先清0
        int len = app_read_RS485_data(RS485rbuf, 1023, pdMS_TO_TICKS(5000)); // 等待5秒接收数据
        if (len > 0)
        {
            RS485rbuf[len] = '\0'; // 确保字符串结束
            ESP_LOGI(TAG, "Received RS485 data: %s\n", RS485rbuf);

            // 这里可以添加对RS485rbuf内容的解析和处理逻辑，例如根据特定命令修改配置
        }
        else if (len == -5)
        {

            ESP_LOGI(TAG, "RS485 receive timeout, no data received within 5 seconds.\n");
        }
    }
}

#pragma endregion

#pragma region CAN协议读写封装
// 外接设备采用name，老化设别使用id，is_name=1表示name，is_name=0表示id
static int make_can_read_transfer_by_nameorid(Externaldevice *device, const char *item_name_id, uint8_t target_addr, uint8_t *rx_buf, uint16_t rx_capacity, can_ext_transfer_t *transfer, const CanProtocolItem **out_item, uint8_t is_name)
{
    if (device == NULL || item_name_id == NULL || rx_buf == NULL ||
        transfer == NULL || out_item == NULL)
    {
        return -1;
    }

    const CanProtocolItem *item = NULL;
    if (is_name == 1)
    {
        item = CanJson_FindItemByName(&device->canprotocol, item_name_id);
    }
    else
    {
        item = CanJson_FindItemByID(&device->canprotocol, item_name_id);
    }
    if (item == NULL)
    {
        printf("找不到CAN协议项: %s\r\n", item_name_id);
        return -1;
    }

    if (!CanJson_ItemReadable(item))
    {
        printf("该CAN协议项不可读: %s\r\n", item->name);
        return -1;
    }

    memset(transfer, 0, sizeof(can_ext_transfer_t));

    transfer->op = CAN_EXT_OP_READ;
    transfer->target_addr = target_addr;
    transfer->source_addr = ADDR_MASTER;

    transfer->data_type = (uint8_t)item->data_type;
    transfer->offset = item->start;
    transfer->length = item->len;

    transfer->rx_payload = rx_buf;
    transfer->rx_capacity = rx_capacity;
    transfer->rx_len = 0;

    *out_item = item;

    return 0;
}

static int encode_can_item_write_payload(const CanProtocolItem *item,
                                         const char *value_str,
                                         uint8_t *payload,
                                         uint16_t payload_capacity,
                                         uint16_t *payload_len)
{
    if (item == NULL || value_str == NULL ||
        payload == NULL || payload_len == NULL)
    {
        return -1;
    }

    if (item->len == 0 || item->len > payload_capacity)
    {
        return -1;
    }

    memset(payload, 0, payload_capacity);

    if (item->value_type == CAN_ITEM_TYPE_STRING)
    {
        size_t str_len = strlen(value_str);

        if (str_len > item->len)
        {
            str_len = item->len;
        }

        memcpy(payload, value_str, str_len);
        *payload_len = item->len;
        return 0;
    }

    if (item->value_type == CAN_ITEM_TYPE_NUM)
    {
        int64_t raw = CanJson_PhysicalToRaw(item, atof(value_str));

        for (uint16_t i = 0; i < item->len; i++)
        {
            payload[i] = (uint8_t)((raw >> (8 * i)) & 0xFF);
        }

        *payload_len = item->len;
        return 0;
    }

    return -1;
}

static int make_can_write_transfer_by_nameorid(Externaldevice *device,
                                               const char *item_nameorid,
                                               const char *value_str,
                                               uint8_t target_addr,
                                               uint8_t *tx_buf,
                                               uint16_t tx_capacity,
                                               can_ext_transfer_t *transfer,
                                               const CanProtocolItem **out_item, uint8_t is_name)
{
    if (device == NULL || item_nameorid == NULL || value_str == NULL || tx_buf == NULL || transfer == NULL || out_item == NULL)
    {
        return -1;
    }

    const CanProtocolItem *item = NULL;
    if (is_name == 1)
    {
        item = CanJson_FindItemByName(&device->canprotocol, item_nameorid);
    }
    else
    {
        item = CanJson_FindItemByID(&device->canprotocol, item_nameorid);
    }

    if (item == NULL)
    {
        printf("找不到CAN协议项: %s\r\n", item_nameorid);
        return -1;
    }

    if (!CanJson_ItemWritable(item))
    {
        printf("该CAN协议项不可写: %s\r\n", item->name);
        return -1;
    }

    uint16_t tx_len = 0;

    if (encode_can_item_write_payload(item,
                                      value_str,
                                      tx_buf,
                                      tx_capacity,
                                      &tx_len) != 0)
    {
        printf("CAN写入负载编码失败: %s\r\n", item->name);
        return -1;
    }

    memset(transfer, 0, sizeof(can_ext_transfer_t));

    transfer->op = CAN_EXT_OP_WRITE;
    transfer->target_addr = target_addr;
    transfer->source_addr = ADDR_MASTER;

    transfer->data_type = (uint8_t)item->data_type;
    transfer->offset = item->start;
    transfer->length = item->len;

    transfer->tx_payload = tx_buf;
    transfer->tx_len = tx_len;

    *out_item = item;

    return 0;
}
#pragma endregion

#pragma region BLE连接与接收任务
// 是否加密，暂时只有待老化设备(Modbus协议)
bool IsEnc = false;
static bool ScanComplete = false; // 用于判断是否扫描完成
// BLE扫描完成回调
void ble_scan_complete_cb(ble_device_info_t *devices, int device_count)
{
    ESP_LOGI(TAG, "BLE scan completed, found %d devices", device_count);

    ScanComplete = true;
    // 打印扫描结果到控制台
    printf("\n=== BLE Scan Results ===\n");
    for (int i = 0; i < device_count; i++)
    {

        printf("[%d] %s (RSSI: %d dBm) %s\n",
               i + 1,
               devices[i].name,
               devices[i].rssi,
               devices[i].is_connected ? "[Connected]" : "");
    }
}
// BLE连接结果回调
void ble_connect_cb(bool success, const char *device_name)
{
    if (success)
    {
        ESP_LOGI(TAG, "BLE connected to: %s\n", device_name);
    }
    else
    {
        ESP_LOGE(TAG, "BLE connection failed to: %s\n", device_name);
    }
}
// BLE断开连接回调
void ble_disconnect_cb(void)
{
    if (BTDisConnect == 0) // 异常断开
    {
        ESP_LOGW(TAG, "BLE disconnected,Restarting...\n");

        aging_error_log("BLE abnormal disconnection; gateway is restarting");

        // 这里暂时重启，后面重新修改
        esp_restart();
    }
    else if (BTDisConnect == 1) // 主动断开
    {
        ESP_LOGW(TAG, "BLE disconnected by user\n");
        BTDisConnect = 0;
    }
}

static int app_ble_recv_data_Ack(void *BleRecBuf1, int max_len, uint32_t wait_time)
{
    uint8_t *BleRecBuf = (uint8_t *)BleRecBuf1;
    int len = app_ble_recv_data_form_remote(BleRecBuf, max_len, wait_time);
    if (len > 0)
    {
        // 会话解密

        if (IsEnc)
        {
            uint8_t data[1024] = {0};
            uint16_t outLen = 0;
            app_enc_process_Decrypt(BleRecBuf, len, data, &outLen);
            memcpy(BleRecBuf, data, outLen);
            len = outLen;
            // 打印解密之后的数据
            ESP_LOGI(TAG, "Decrypted BLE data: ");
            for (int i = 0; i < len; i++)
            {
                printf("%02X ", data[i]);
            }
        }
    }
    return len;
}

void app_ble_data_handle(void *arg)
{
    uint8_t BleRecBuf[1024] = {0};
    uint8_t flag_one = 1;
    while (1)
    {
        // BleRecBuf数据先清0
        // memset(BleRecBuf, 0, sizeof(BleRecBuf));
        int len = app_ble_recv_data_form_remote(BleRecBuf, sizeof(BleRecBuf), portMAX_DELAY);
        if (len > 0)
        {
            // printData(BleRecBuf,len,"------------Get Ble Data-------------");
            bool needSendToUart = true;
            // 认证鉴权信息
            if ((BleRecBuf[0] == '*') && (BleRecBuf[1] == '*')) // 鉴权数据
            {
                if (len == 10)
                {
                    ESP_LOGI(TAG, "01 Pack Recive............");
                    IsEnc = false;
                    uint8_t resend[10];
                    // if (RandomSendPKey() != ESP_OK)
                    // {
                    //     ESP_LOGE(TAG, "Failed to initialize ECDH handshake");
                    //     ble_disconnect();
                    //     continue;
                    // }
                    RandomSendPKey();
                    if (app_enc_process_data(BleRecBuf, len, resend) == ESP_OK)
                    {
                        if (ble_send_data(resend, 10, 0) != 1)
                        {
                            ESP_LOGE(TAG, "Failed to re Ble data (len: %d)", 10);
                        }
                        else
                        {
                            ESP_LOGI(TAG, "02 Pack ReSend............");
                        }
                    }
                }
                if (len == 7)
                {
                    ESP_LOGI(TAG, "03 Pack Recive............");
                    if (BleRecBuf[0] != '*' || BleRecBuf[1] != '*' || BleRecBuf[4] != 0)
                    {
                        ESP_LOGI(TAG, "03 Pack Fail.....");
                        // 断开连接
                        ble_disconnect();
                    }
                    else
                    {
                        // app_write_UART_data((uint8_t *)BleRecBuf, len, pdMS_TO_TICKS(250)); // 验证是否成功，发送鉴权数据
                    }
                }
                needSendToUart = false;
            }

            // 认证鉴权信息2
            if ((BleRecBuf[0] == 0) && (BleRecBuf[1] == 0x86) && flag_one) // 鉴权数据
            {
                flag_one = 0;
                ESP_LOGI(TAG, "04 Pack Recive............");
                uint8_t resend[146];
                if (app_enc_process_data4(BleRecBuf, len, resend) == ESP_OK) // 可能出问题
                {
                    ESP_LOGI(TAG, "05 Pack ReSending............");
                    if (ble_send_data(resend, sizeof(resend), 0) != 1)
                    {
                        ESP_LOGE(TAG, "Failed to re Ble data (len: %d) forword to ble", len);
                    }
                    else
                    {
                        flag_one = 1;
                        // app_write_UART_data((uint8_t *)resend, sizeof(resend), pdMS_TO_TICKS(250)); // 验证是否成功，发送鉴权数据
                    }
                }
                needSendToUart = false;
            }

            if ((BleRecBuf[0] == 0) && (len == 18)) // 鉴权最终结果
            {
                ESP_LOGI(TAG, "06 Pack Recive............");
                uint8_t resend[2];
                if (app_enc_process_data6(BleRecBuf, len, resend) == ESP_OK)
                {
                    if (ble_send_data(resend, sizeof(resend), 0) != 1)
                    {
                        ESP_LOGE(TAG, "Failed to re Ble data (len: %d) forword to ble", len);
                    }
                    else
                    {
                        app_write_UART_data((uint8_t *)resend, sizeof(resend), pdMS_TO_TICKS(250)); // 验证是否成功，发送鉴权数据
                    }
                }
                IsEnc = true;
                needSendToUart = false;
            }
            // 会话解密
            if (IsEnc)
            {
                uint8_t data[1024] = {0};
                uint16_t outLen = 0;
                app_enc_process_Decrypt(BleRecBuf, len, data, &outLen);
                memcpy(BleRecBuf, data, outLen);
                len = outLen;
            }

            if (needSendToUart)
            {
                if (app_write_UART_data((uint8_t *)BleRecBuf, len, pdMS_TO_TICKS(250)) != len)
                {
                    ESP_LOGE(TAG, "Failed to ble data (len %d) to uart", len);
                }
            }
        }
    }
}

#pragma endregion

#pragma region HTTP协议导入与接收任务
uint8_t CurentDevicenum = 0;
uint8_t IsAgingDevice = 0; // 0表示不是老化设备，1表示是老化设备

#define HTTP_DATA_Cache 1024 * 8

static esp_err_t import_scpi_protocol_to_device(Externaldevice *device, const char *json_text)
{
    if (device == NULL || json_text == NULL)
    {
        ESP_LOGE(TAG, "Invalid SCPI import param");
        return ESP_ERR_INVALID_ARG;
    }

    scpi_protocol_t *new_protocol = NULL;

    esp_err_t ret = scpi_dynamic_import_json(json_text, &new_protocol);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SCPI JSON import failed: %s", esp_err_to_name(ret));
        /*
         * 这里不释放旧协议。
         * 原因：新协议导入失败时，保留旧协议更安全，避免设备直接丢失原有协议。
         */
        return ret;
    }

    if (device->scpi_protocol != NULL)
    {
        scpi_dynamic_free(device->scpi_protocol);
        device->scpi_protocol = NULL;
    }

    device->scpi_protocol = new_protocol;
    device->HaveProtocol = true;
    device->ProtoType = 0; // 0 表示 SCPI，保持你原来的定义

    ESP_LOGI(TAG, "SCPI protocol imported successfully");

    return ESP_OK;
}

static esp_err_t import_modbus_protocol_to_device(Externaldevice *device, const char *json_text)
{
    if (device == NULL || json_text == NULL)
    {
        ESP_LOGE(TAG, "Invalid MODBUS import param");
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 保持你原来的逻辑：
     * 先释放旧 MODBUS 协议，再把新协议解析到 device->modbusprotocol 中。
     */
    ModbusJson_Free(&device->modbusprotocol);

    if (!ModbusJson_Parse(json_text, &device->modbusprotocol))
    {
        ESP_LOGE(TAG, "MODBUS protocol JSON parse failed");

        /*
         * 防止 ModbusJson_Parse 解析到一半失败后，
         * device->modbusprotocol 里面残留半初始化数据。
         */
        ModbusJson_Free(&device->modbusprotocol);

        device->HaveProtocol = false;
        device->ProtoType = 1;

        return ESP_FAIL;
    }

    device->HaveProtocol = true;
    device->ProtoType = 1; // 1 表示 MODBUS，保持你原来的定义

    ESP_LOGI(TAG, "MODBUS protocol imported successfully");

    return ESP_OK;
}

static esp_err_t import_can_protocol_to_device(Externaldevice *device, const char *json_text)
{
    if (device == NULL || json_text == NULL)
    {
        ESP_LOGE(TAG, "Invalid CAN import param");
        return ESP_ERR_INVALID_ARG;
    }

    CanJson_Free(&device->canprotocol);

    if (!CanJson_Parse(json_text, &device->canprotocol))
    {
        ESP_LOGE(TAG, "CAN protocol JSON parse failed");

        CanJson_Free(&device->canprotocol);

        device->HaveProtocol = false;
        device->ProtoType = 2;

        return ESP_FAIL;
    }

    device->HaveProtocol = true;
    device->ProtoType = 2; // 2 表示 CAN

    ESP_LOGI(TAG, "CAN protocol imported successfully");
    CanJson_Print(&device->canprotocol);

    return ESP_OK;
}

void parse_jsonCommand_HTTP(const char *packet, int len)
{
    cJSON *pRoot = NULL;
    cJSON *data_obj = NULL;
    char *protocol_json = NULL;

    if (packet == NULL || len <= 0)
    {
        ESP_LOGE(TAG, "Invalid HTTP packet");
        return;
    }

    // 如果 packet 已经保证以 \0 结尾，可以继续使用 cJSON_Parse
    pRoot = cJSON_Parse(packet);

    if (pRoot == NULL)
    {
        ESP_LOGE(TAG, "Failed to parse HTTP JSON");
        goto cleanup;
    }

    /*
     * 当前接口格式：
     *
     * {
     *   "code":1,
     *   "message":"成功",
     *   "data":{
     *       ...
     *   }
     * }
     *
     * 所以 data 是 Object，不是 String
     */
    data_obj = cJSON_GetObjectItem(pRoot, "data");

    if (!cJSON_IsObject(data_obj))
    {
        ESP_LOGE(TAG, "data field is not a valid object");
        goto cleanup;
    }

    cJSON *proto = cJSON_GetObjectItem(data_obj, "protocolTypeStr");

    if (!cJSON_IsString(proto) || proto->valuestring == NULL)
    {
        ESP_LOGE(TAG, "No valid protocolTypeStr found in JSON");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Parsed protocolTypeStr: %s", proto->valuestring);

    /*
     * 因为下面的 import_xxx_protocol_to_device()
     * 原本接收 JSON 字符串，
     * 所以把 data 对象重新转换成 JSON 字符串
     */
    protocol_json = cJSON_PrintUnformatted(data_obj);

    if (protocol_json == NULL)
    {
        ESP_LOGE(TAG, "Failed to serialize protocol data");
        goto cleanup;
    }

    if (strcmp(proto->valuestring, "SCPI") == 0)
    {
        if (IsAgingDevice == 0)
        {
            esp_err_t ret =
                import_scpi_protocol_to_device(
                    &read_devices[CurentDevicenum],
                    protocol_json);

            if (ret != ESP_OK)
            {
                read_devices[CurentDevicenum].HaveProtocol = false;
                goto cleanup;
            }
        }
        else if (IsAgingDevice == 1)
        {
            if (Aging_device == NULL)
            {
                ESP_LOGE(TAG,
                         "Aging_device is NULL when SCPI protocol received");

                IsAgingDevice = 0;
                goto cleanup;
            }

            esp_err_t ret =
                import_scpi_protocol_to_device(
                    &Aging_device[0],
                    protocol_json);

            if (ret != ESP_OK)
            {
                Aging_device[0].HaveProtocol = false;
                IsAgingDevice = 0;
                goto cleanup;
            }

            IsAgingDevice = 0;
        }
    }
    else if (strcmp(proto->valuestring, "MODBUS") == 0)
    {
        if (IsAgingDevice == 0)
        {
            esp_err_t ret =
                import_modbus_protocol_to_device(
                    &read_devices[CurentDevicenum],
                    protocol_json);

            if (ret != ESP_OK)
            {
                goto cleanup;
            }
        }
        else if (IsAgingDevice == 1)
        {
            if (Aging_device == NULL)
            {
                ESP_LOGE(TAG,
                         "Aging_device is NULL when MODBUS protocol received");

                IsAgingDevice = 0;
                goto cleanup;
            }

            esp_err_t ret =
                import_modbus_protocol_to_device(
                    &Aging_device[0],
                    protocol_json);

            if (ret != ESP_OK)
            {
                Aging_device[0].HaveProtocol = false;
                IsAgingDevice = 0;
                goto cleanup;
            }

            IsAgingDevice = 0;
        }
    }
    else if (strcmp(proto->valuestring, "BLUETTICAN") == 0)
    {
        if (IsAgingDevice == 0)
        {
            esp_err_t ret =
                import_can_protocol_to_device(
                    &read_devices[CurentDevicenum],
                    protocol_json);

            if (ret != ESP_OK)
            {
                goto cleanup;
            }
        }
        else if (IsAgingDevice == 1)
        {
            if (Aging_device == NULL)
            {
                ESP_LOGE(TAG,
                         "Aging_device is NULL when CAN protocol received");

                IsAgingDevice = 0;
                goto cleanup;
            }

            esp_err_t ret =
                import_can_protocol_to_device(
                    &Aging_device[0],
                    protocol_json);

            if (ret != ESP_OK)
            {
                Aging_device[0].HaveProtocol = false;
                IsAgingDevice = 0;
                goto cleanup;
            }

            IsAgingDevice = 0;
        }
    }
    else
    {
        ESP_LOGW(TAG,
                 "Unknown protocolTypeStr: %s",
                 proto->valuestring);
    }

cleanup:

    if (protocol_json != NULL)
    {
        cJSON_free(protocol_json);
    }

    if (pRoot != NULL)
    {
        cJSON_Delete(pRoot);
    }
}

// 数据接收处理
static void app_HTTP_Rdata_handle(void *arg)
{
    char *json_str = app_malloc_prefer_psram(HTTP_DATA_Cache);
    if (json_str == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for HTTP json_str");
        s_http_rx_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    int json_str_len = 0;
    char Databuf[1025] = {0};
    TickType_t last_recv_time = xTaskGetTickCount();       // 记录上次收到数据的时间
    const TickType_t timeout_ticks = pdMS_TO_TICKS(10000); // 累积超时时间：10秒

    while (1)
    {
        memset(Databuf, 0, sizeof(Databuf));
        int datalen = app_read_HTTP_data(Databuf, sizeof(Databuf), pdMS_TO_TICKS(5000)); // 等待5秒接收HTTP数据

        if (datalen > 0)
        {
            // 收到有效数据，更新时间戳
            last_recv_time = xTaskGetTickCount();
            // 检查是否是完整包
            if (is_complete_packet(Databuf, datalen))
            {
                // 如果累积缓冲区有数据，说明之前的数据不完整
                if (json_str_len > 0)
                {
                    ESP_LOGW(TAG, "Previous incomplete data discarded, new complete packet received");
                    json_str_len = 0;
                    memset(json_str, 0, HTTP_DATA_Cache);
                }

                ESP_LOGI(TAG, "Complete JSON packet: %s", Databuf);
                parse_jsonCommand_HTTP(Databuf, datalen);
            }
            else
            {
                // 检查累积后是否溢出
                if (json_str_len + datalen >= HTTP_DATA_Cache)
                {
                    ESP_LOGE(TAG, "Buffer overflow! Discarding accumulated %d bytes", json_str_len);
                    json_str_len = 0;
                    memset(json_str, 0, HTTP_DATA_Cache);

                    // 当前数据可能已经是完整包
                    if (is_complete_packet(Databuf, datalen))
                    {
                        ESP_LOGI(TAG, "Current data is a complete packet");
                        parse_jsonCommand_HTTP(Databuf, datalen);
                    }
                    else
                    {
                        // 重新开始累积
                        strncpy(json_str, Databuf, datalen);
                        json_str_len = datalen;
                        json_str[json_str_len] = '\0';
                        ESP_LOGI(TAG, "Start new accumulation: %d bytes", json_str_len);
                    }
                }
                else
                {
                    // 追加到累积缓冲区
                    strncpy(json_str + json_str_len, Databuf, datalen);
                    json_str_len += datalen;
                    json_str[json_str_len] = '\0';

                    // 检查累积后是否完整
                    if (is_complete_packet(json_str, json_str_len))
                    {
                        ESP_LOGI(TAG, "Accumulated complete JSON: %s", json_str);
                        parse_jsonCommand_HTTP(json_str, json_str_len);
                        json_str_len = 0;
                        memset(json_str, 0, HTTP_DATA_Cache);
                    }
                    else
                    {
                        ESP_LOGD(TAG, "Still incomplete, accumulated %d bytes", json_str_len);
                    }
                }
            }
        }
        else if (datalen <= 0)
        {
            // 读取超时，没有新数据
            if (json_str_len > 0)
            {
                // 检查累积缓冲区的数据是否已超时（长时间未完成）
                TickType_t now = xTaskGetTickCount();
                if ((now - last_recv_time) >= timeout_ticks)
                {

                    ESP_LOGI(TAG, "Timeout waiting for complete JSON, discarding %d bytes", json_str_len);

                    json_str_len = 0;

                    memset(json_str, 0, HTTP_DATA_Cache);
                }
            }
        }
        if (datalen >= 0 && datalen < HTTP_DATA_Cache)
        {
            Databuf[datalen] = '\0';
        }
    }
    free(json_str);
}

// 根据外接设备的协议ID自动获取协议
static void app_HTTP_Auto_GetProtocol(void *arg)
{
    uint8_t checkflag = 0;
    while (1)
    {
        for (int i = 0; i < read_device_count; i++)
        {
            if (read_devices[i].ProtoID != 0 && read_devices[i].HaveProtocol == false) // 非老化设备的协议获取
            {
                esp_err_t err = test_http_getproto_New(read_devices[i].ProtoID);
                if (err != ESP_OK)
                {
                    storage_write_record_cyclic(current_log_pn(), "Failed to get protocol from server");
                }
                else if (err == ESP_OK)
                {
                    checkflag = 1;
                    read_devices[i].LastProtoID = read_devices[i].ProtoID;
                    CurentDevicenum = i;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000)); // 每秒检查一次
        }
        if (checkflag == 1)
        {
            uint8_t retry_count = 0;
            while (!ExDevice_Check())
            {
                retry_count++;
                if (retry_count > 5)
                {
                    retry_count = 0;
                    storage_write_record_cyclic(current_log_pn(), "Failed to ExDevice_Check online and protocol match");
                    if (app_mqtt_publish("device/%s/event/warn", "Failed to ExDevice_Check online and protocol match", DEVICE_ID) < 0)
                    {
                        storage_write_record_cyclic(current_log_pn(), "MQTT AgingStartCheck Error Publish Failed");
                    }
                    ESP_LOGW(TAG, "Failed to get protocol after 5 retries, skipping");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            checkflag = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(4000)); // 每秒检查一次
    }
}
#pragma endregion

#pragma region MQTT命令与接收任务

#define MQTT_DATA_Cache 1024 * 5
#define MQTT_TOPIC_LEN 60

static const char *aging_cmd_state_name(AgingCommandState state)
{
    switch (state)
    {
    case AGING_CMD_IDLE:
        return "IDLE";
    case AGING_CMD_WAIT_PN:
        return "WAIT_PN";
    case AGING_CMD_RUNNING:
        return "RUNNING";
    default:
        return "UNKNOWN";
    }
}

static void publish_pn_reply(int cmd_seq, int code, const char *pn)
{
    const char *reply_pn = pn != NULL ? pn : "";
    char *json = create_pn_response_json(time(NULL), cmd_seq, code, reply_pn);
    if (json == NULL)
    {
        return;
    }

    if (app_mqtt_publish("device/%s/cmd/reply/pn", json, DEVICE_ID) < 0)
    {
        storage_write_record_cyclic(reply_pn, "MQTT PN reply Publish Failed");
    }
    free(json);
}

static void clear_ready_pn_runtime(void)
{
    memset(PN_Code_Ready, 0, sizeof(PN_Code_Ready));
    s_new_pn_ready = 0;
}

static void clear_aging_command_runtime(void)
{
    s_aging_cmd_state = AGING_CMD_IDLE;
    s_pending_start_seq = 0;
    memset(PN_Code, 0, sizeof(PN_Code));
    clear_ready_pn_runtime();
}

static void persist_pending_start_state(bool pending, int start_seq)
{
    esp_err_t ret = SelfRecovery_Write_uint16(NVS_KEY_START_PENDING, pending ? 1 : 0);
    if (ret != ESP_OK)
    {
        local_error_log("Write start_pending failed, error=%s", esp_err_to_name(ret));
    }

    ret = SelfRecovery_Write_uint64(NVS_KEY_START_SEQ, pending ? (uint64_t)start_seq : 0);
    if (ret != ESP_OK)
    {
        local_error_log("Write start_seq failed, error=%s", esp_err_to_name(ret));
    }
}

static void persist_ready_pn_state(void)
{
    esp_err_t ret = SelfRecovery_Write_str(NVS_KEY_READY_PN,
                                           s_new_pn_ready ? PN_Code_Ready : "");
    if (ret != ESP_OK)
    {
        local_error_log("Write ready PN failed, error=%s", esp_err_to_name(ret));
    }

    ret = SelfRecovery_Write_uint16(NVS_KEY_NEW_PN_READY,
                                    s_new_pn_ready ? 1 : 0);
    if (ret != ESP_OK)
    {
        local_error_log("Write new_pn_ready failed, error=%s", esp_err_to_name(ret));
    }
}

/*
 * 缓存一条“已经收到start_aging但还没有本轮新PN”的启动命令。
 * 这里只建立WAIT_PN状态，不把aging_valid写成1，也不主动上报agingStart。
 */
static bool cache_pending_start_command(const char *aging_json, int start_seq)
{
    if (aging_json == NULL)
    {
        return false;
    }

    esp_err_t ret = save_json_to_spiffs("Aging.json", aging_json);
    if (ret != ESP_OK)
    {
        aging_error_log("Cache pending Aging.json failed, error=%s", esp_err_to_name(ret));
        device_response_publish_point(start_seq, 0, "Cache start aging command failed");
        return false;
    }

    /* WAIT_PN不是正在老化，掉电后不能按老化断点恢复处理。 */
    ret = SelfRecovery_Write_uint16(NVS_KEY_AGING_VALID, 0);
    if (ret != ESP_OK)
    {
        local_error_log("Clear aging_valid while waiting PN failed, error=%s", esp_err_to_name(ret));
    }

    memset(PN_Code, 0, sizeof(PN_Code));
    ret = SelfRecovery_Write_str(NVS_KEY_CURRENT_PN, "");
    if (ret != ESP_OK)
    {
        local_error_log("Clear current PN while waiting PN failed, error=%s", esp_err_to_name(ret));
    }

    s_pending_start_seq = start_seq;
    s_aging_cmd_state = AGING_CMD_WAIT_PN;
    persist_pending_start_state(true, start_seq);

    AgingLogActive = true;
    aging_runtime_log("Start aging command cached; waiting for a new PN, cmd_seq=%d", start_seq);
    ESP_LOGI(TAG, "start_aging cached, waiting for set_pn, seq=%d", start_seq);
    return true;
}

/*
 * 使用PN_Code_Ready启动老化。
 * PN只有在aging_command_json_parse()成功创建老化任务后才被“消费”。
 * 若初始化失败，PN_Code_Ready保持有效，服务器可以直接重发start_aging而无需重新扫码。
 */
static bool start_aging_with_ready_pn(const char *aging_json, int start_seq)
{
    if (aging_json == NULL)
    {
        device_response_publish_point(start_seq, 0, "Aging command is empty");
        return false;
    }

    if (s_aging_cmd_state == AGING_CMD_RUNNING)
    {
        device_response_publish_point(start_seq, 0, "Aging already started, ignoring new command");
        aging_error_log("Duplicate start aging command rejected: aging is already running");
        return false;
    }

    if (s_new_pn_ready == 0 || PN_Code_Ready[0] == '\0')
    {
        ESP_LOGW(TAG, "Cannot start aging: no new PN is ready");
        return false;
    }

    /* 在真正创建老化任务前先保证完整工艺JSON已经落盘。 */
    esp_err_t ret = save_json_to_spiffs("Aging.json", aging_json);
    if (ret != ESP_OK)
    {
        aging_error_log("Save Aging.json before starting failed, error=%s", esp_err_to_name(ret));
        device_response_publish_point(start_seq, 0, "Save aging command failed");
        return false;
    }

    /* 临时把Ready PN复制为当前PN，但此时还不消费Ready令牌。 */
    memset(PN_Code, 0, sizeof(PN_Code));
    if (!copy_string_checked(PN_Code, sizeof(PN_Code), PN_Code_Ready))
    {
        aging_error_log("Copy ready PN to current PN failed");
        device_response_publish_point(start_seq, 0, "PN copy failed");
        return false;
    }

    ret = SelfRecovery_Write_str(NVS_KEY_CURRENT_PN, PN_Code);
    if (ret != ESP_OK)
    {
        local_error_log("Write current PN before starting failed, error=%s", esp_err_to_name(ret));
    }

    /* 新任务不能继承上一次运行时的恢复游标。 */
    memset(&agingResumeState, 0, sizeof(agingResumeState));

    AgingLogActive = true;
    aging_runtime_log("Start aging command accepted; initializing aging task, cmd_seq=%d", start_seq);

    if (!aging_command_json_parse(aging_json))
    {
        /*
         * 初始化失败：本次没有进入RUNNING。
         * 当前PN清空，但Ready PN和new_pn_ready保留，允许直接重试start_aging。
         */
        s_aging_cmd_state = AGING_CMD_IDLE;
        s_pending_start_seq = 0;
        memset(PN_Code, 0, sizeof(PN_Code));
        SelfRecovery_Write_str(NVS_KEY_CURRENT_PN, "");
        SelfRecovery_Write_uint16(NVS_KEY_AGING_VALID, 0);
        persist_pending_start_state(false, 0);

        device_response_publish_point(start_seq, 0, "Aging start failed during initialization");
        AgingLogActive = false;
        return false;
    }

    /* aging_command_json_parse成功后，才正式进入RUNNING并消费本轮新PN。 */
    s_aging_cmd_state = AGING_CMD_RUNNING;

    /* 1. 先保存当前PN */
    ret = SelfRecovery_Write_str(NVS_KEY_CURRENT_PN, PN_Code);
    if (ret != ESP_OK)
    {
        aging_error_log("Persist current PN failed after aging started, error=%s", esp_err_to_name(ret));
    }

    /* 2. 再保存RUNNING标志 */
    ret = SelfRecovery_Write_uint16(NVS_KEY_AGING_VALID, 1);
    if (ret != ESP_OK)
    {
        aging_error_log("Write aging_valid failed after aging started, error=%s", esp_err_to_name(ret));
    }

    /* 3. 再清除Ready PN */
    clear_ready_pn_runtime();
    persist_ready_pn_state();

    /* 4. 最后清除WAIT_PN */
    s_pending_start_seq = 0;
    persist_pending_start_state(false, 0);

    device_response_publish_point(start_seq, 1, "Aging started, command received and processed");
    aging_state_publish("agingStart");
    aging_runtime_log("Aging task started successfully, state=%s", aging_cmd_state_name(s_aging_cmd_state));
    return true;
}

// Json数据解析
// 不带crc

void parse_jsonCommand_MQTT(const char *packet, int len)
{
    char *packet_copy = app_malloc_prefer_psram(len + 1);
    if (!packet_copy)
    {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return;
    }
    memcpy(packet_copy, packet, len);
    packet_copy[len] = '\0';

    cJSON *pRoot = cJSON_Parse(packet_copy);
    if (pRoot == NULL)
    {
        ESP_LOGE(TAG, "Failed to parse JSON");
        free(packet_copy);
        return;
    }

    cJSON *Seq = cJSON_GetObjectItem(pRoot, "Seq");
    if (cJSON_IsNumber(Seq) && (Seq->valueint >= 0))
    {
        char TopicBuf[MQTT_TOPIC_LEN] = {0};
        int len1 = app_read_MQTT_topic(TopicBuf, MQTT_TOPIC_LEN, portMAX_DELAY);
        if (len1 > 0)
        {
            if (strstr(TopicBuf, "public/cmd/ota"))
            {
                // 处理OTA升级
                ESP_LOGI(TAG, "publicOTA升级命令收到");
                esp_err_t err = simple_ota_check_and_update_default();
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "OTA check/update failed: %s", esp_err_to_name(err));
                }
            }
            else if (strstr(TopicBuf, "ota"))
            {
                // 处理OTA升级
                ESP_LOGI(TAG, "OTA升级命令收到");
                esp_err_t err = simple_ota_check_and_update_default();
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "OTA check/update failed: %s", esp_err_to_name(err));
                }
            }
            else if (strstr(TopicBuf, "start_aging"))
            {
                const int start_seq = Seq->valueint;
                cJSON *Data_obj = cJSON_GetObjectItem(pRoot, "Data");
                if (Data_obj == NULL || !cJSON_IsObject(Data_obj))
                {
                    device_response_publish_point(start_seq, 0, "Missing or invalid Data object in start_aging command");
                    ESP_LOGW(TAG, "start_aging command missing Data object, seq=%d", start_seq);
                }
                cJSON *programId = cJSON_GetObjectItem(Data_obj, "ProgramId");
                if (programId == NULL)
                {
                    device_response_publish_point(start_seq, 0, "Missing or invalid programId in start_aging command");
                    ESP_LOGW(TAG, "start_aging command missing programId, seq=%d", start_seq);
                }
                aging_state_publish_ProgramId(programId->valuestring);
                cJSON *recordId = cJSON_GetObjectItem(pRoot, "RecordId");
                if (recordId)
                {
                    strncpy(RecordId, recordId->valuestring, sizeof(RecordId) - 1);
                    RecordId[sizeof(RecordId) - 1] = '\0';
                    // 存nvs
                    esp_err_t ret = SelfRecovery_Write_str("RecordId", RecordId);
                    if (ret != ESP_OK)
                    {
                        local_error_log("Write RecordId failed, error=%s", esp_err_to_name(ret));
                    }
                }

                /*
                 * RUNNING时必须先拒绝，不能先修改PN_Code、Ready PN或Aging.json，
                 * 否则可能把正在运行的上一台设备错误切换到下一台PN。
                 */
                if (s_aging_cmd_state == AGING_CMD_RUNNING)
                {
                    device_response_publish_point(start_seq, 0, "Aging already started, ignoring new command");
                    ESP_LOGW(TAG, "Duplicate start_aging rejected while RUNNING, seq=%d", start_seq);
                    aging_error_log("Duplicate start aging command rejected: aging is already running");
                }
                else if (s_aging_cmd_state == AGING_CMD_WAIT_PN)
                {
                    if (start_seq == s_pending_start_seq)
                    {
                        /*
                         * QoS1或服务器重发可能带来相同Seq的重复消息。
                         * 该命令仍处于WAIT_PN，不发送失败回复，避免服务器把“等待PN”误判为启动失败。
                         * 真正启动后仍使用原始Seq只回复一次最终结果。
                         */
                        ESP_LOGW(TAG, "Duplicate pending start_aging ignored, seq=%d", start_seq);
                    }
                    else
                    {
                        device_response_publish_point(start_seq, 0, "Another start aging command is already waiting for PN");
                        ESP_LOGW(TAG,
                                 "New start_aging rejected because another command is waiting for PN, old_seq=%d, new_seq=%d",
                                 s_pending_start_seq,
                                 start_seq);
                    }
                }
                else if (s_new_pn_ready == 1 && PN_Code_Ready[0] != '\0')
                {
                    /* 正常顺序：set_pn先到，start_aging后到。 */
                    start_aging_with_ready_pn(packet_copy, start_seq);
                }
                else
                {
                    /*
                     * start_aging先到：不能使用PN_Code中可能残留的上一轮PN。
                     * 缓存完整JSON和原始Seq，等待新的set_pn到达后再真正启动。
                     */
                    cache_pending_start_command(packet_copy, start_seq);
                }
            }
            else if (strstr(TopicBuf, "stop_aging"))
            {
                aging_runtime_log("Stop aging command received, command_state=%s",
                                  aging_cmd_state_name(s_aging_cmd_state));

                if (Aging_test_task_handle != NULL)
                {
                    vTaskDelete(Aging_test_task_handle);
                    Aging_test_task_handle = NULL;
                }
                if (AgingData_task_handle != NULL)
                {
                    vTaskDelete(AgingData_task_handle);
                    AgingData_task_handle = NULL;
                }
                if (ble_is_connected())
                {
                    BTDisConnect = 1;
                    ble_disconnect();
                }

                SelfRecovery_Erase_aging_valid();
                AgingDevice_RuntimeFree();
                agingState = AgingIdle;
                agingDataAMode = IdleState;
                memset(&agingResumeState, 0, sizeof(agingResumeState));

                if (agingcfg.device_count != 0 || agingcfg.step_count != 0)
                {
                    aging_config_free(&agingcfg);
                    memset(&agingcfg, 0, sizeof(agingcfg));
                }

                /* WAIT_PN时agingcfg尚未解析，因此Aging.json必须无条件删除。 */
                spiffs_file_delete("Aging.json");

                aging_state_publish("agingStop");
                aging_runtime_log("Aging stopped and resources released");

                AgingLogActive = false;
                s_consecutive_data_read_failures = 0;
                s_last_data_read_error_report_tick = 0;
                s_consecutive_data_upload_failures = 0;
                s_last_data_upload_error_report_tick = 0;

                /* stop_aging定义为彻底取消当前、Ready和Pending三类上下文。 */
                clear_aging_command_runtime();
            }
            else if (strstr(TopicBuf, "set_pn"))
            {
                /* 老化运行期间禁止提前扫描/设置下一台设备PN。 */
                if (s_aging_cmd_state == AGING_CMD_RUNNING)
                {
                    publish_pn_reply(Seq->valueint, 0, PN_Code);
                    ESP_LOGW(TAG, "set_pn rejected while aging is RUNNING");
                    aging_error_log("Set PN command rejected while aging is running");
                }
                else if (s_new_pn_ready == 1)
                {
                    /* 已有一个尚未被start_aging消费的新PN，不允许覆盖。 */
                    publish_pn_reply(Seq->valueint, 0, PN_Code_Ready);
                    ESP_LOGW(TAG, "set_pn rejected: a new PN is already ready, PN=%s", PN_Code_Ready);
                    local_error_log("Set PN rejected: previous ready PN has not been consumed");
                }
                else
                {
                    char new_pn[sizeof(PN_Code_Ready)] = {0};
                    cJSON *Data_obj = cJSON_GetObjectItem(pRoot, "Data");
                    cJSON *pn_obj = cJSON_IsObject(Data_obj) ? cJSON_GetObjectItem(Data_obj, "PN") : NULL;

                    if (!cJSON_IsString(pn_obj) || pn_obj->valuestring == NULL || pn_obj->valuestring[0] == '\0')
                    {
                        publish_pn_reply(Seq->valueint, 0, "");
                        local_error_log("Invalid or empty PN code was rejected");
                    }
                    else if (!copy_string_checked(new_pn, sizeof(new_pn), pn_obj->valuestring))
                    {
                        publish_pn_reply(Seq->valueint, 0, "");
                        local_error_log("PN code is too long and was rejected");
                    }
                    else
                    {
                        /* 先完整校验到局部变量，再覆盖Ready PN，避免异常JSON破坏旧状态。 */
                        memset(PN_Code_Ready, 0, sizeof(PN_Code_Ready));
                        copy_string_checked(PN_Code_Ready, sizeof(PN_Code_Ready), new_pn);
                        s_new_pn_ready = 1;
                        persist_ready_pn_state();

                        publish_pn_reply(Seq->valueint, 1, PN_Code_Ready);
                        ESP_LOGI(TAG, "New PN ready for next aging: %s", PN_Code_Ready);

                        if (s_aging_cmd_state == AGING_CMD_WAIT_PN)
                        {
                            /*
                             * start_aging先到：此时读取之前缓存的完整JSON，
                             * 使用原始start_aging Seq回复启动结果。
                             */
                            char *pending_json = read_json_from_spiffs("Aging.json");
                            int pending_seq = s_pending_start_seq;

                            if (pending_json == NULL)
                            {
                                device_response_publish_point(pending_seq, 0, "Pending aging command is missing");
                                local_error_log("Pending Aging.json is missing while PN arrived");
                                s_aging_cmd_state = AGING_CMD_IDLE;
                                s_pending_start_seq = 0;
                                persist_pending_start_state(false, 0);
                            }
                            else
                            {
                                start_aging_with_ready_pn(pending_json, pending_seq);
                                free(pending_json);
                            }
                        }
                        else
                        {
                            /* 正常顺序：PN已准备好，但老化尚未启动。 */
                            esp_err_t ret = SelfRecovery_Write_uint16(NVS_KEY_AGING_VALID, 0);
                            if (ret != ESP_OK)
                            {
                                local_error_log("Clear aging_valid after setting PN failed, error=%s", esp_err_to_name(ret));
                            }
                        }
                    }
                }
            }
            else if (strstr(TopicBuf, "get_state"))
            {
                char *json_str = CreateStateUPJsonFromFiles_MQTT(time(NULL), Seq->valueint, 1, NULL);
                if (json_str)
                {
                    if (app_mqtt_publish("device/%s/cmd/reply/state", json_str, DEVICE_ID) < 0)
                    {
                        ESP_LOGE(TAG, "MQTT State reply publish failed");
                    }
                    free(json_str);
                }
            }
            else if (strstr(TopicBuf, "set_config"))
            {
                // 先删除所有配置，再重新建立
                esp_err_t ret = spiffs_delete_all_files();
                if (ret == ESP_OK)
                {
                    ESP_LOGI(TAG, "All SPIFFS files deleted");
                }
                else
                {
                    ESP_LOGW(TAG, "Some SPIFFS files delete failed");
                }
                ret = process_packet_AllConfig_MQTT(packet_copy, len);
                if (ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "process_packet_AllConfig_MQTT failed");
                }
                mqtt_app_stop();
                vTaskDelay(pdMS_TO_TICKS(1000));

                esp_restart();
            }
            else if (strstr(TopicBuf, "get_config"))
            {
                Config_Report(Seq->valueint);
            }
            else if (strstr(TopicBuf, "get_lost_aging"))
            {
                int count = 0;
                int *ids = parse_ids_array(packet_copy, &count);

                if (ids != NULL && count > 0)
                {
                    printf("Parsed %d IDs:\n", count);
                    for (int i = 0; i < count; i++)
                    {
                        printf("ids[%d] = %d\n", i, ids[i]);
                        query_db1_to_global(ids[i]);
                        if (g_db1_result.json_data[0] != '\0')
                        {
                            if (app_mqtt_publish("device/%s/data/aging", g_db1_result.json_data, DEVICE_ID) > 0)
                            {
                            }
                        }
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    free(ids);
                }
                else
                {
                    printf("Failed to parse Ids array\n");
                }
            }
            else if (strstr(TopicBuf, "remote_log"))
            {
                cJSON *data_item = cJSON_GetObjectItem(pRoot, "Data");
                if (cJSON_IsNumber(data_item))
                {
                    Mqtt_Log_Mode = (int8_t)data_item->valueint;
                }
                device_response_publish_setlog(Seq->valueint, 1, "Remote log mode set");
            }
            else if (strstr(TopicBuf, "local_log"))
            {
                storage_print_all_records();
            }
            else if (strstr(TopicBuf, "Cannextstep"))
            {
                cJSON *Data_obj = cJSON_GetObjectItem(pRoot, "Data");
                cJSON *NextStep = cJSON_IsObject(Data_obj) ? cJSON_GetObjectItem(Data_obj, "IsCanNext") : NULL;
                if (cJSON_IsNumber(NextStep))
                {
                    Cannextstep = (uint8_t)NextStep->valueint;
                }
            }
            else if (strstr(TopicBuf, "CPower")) // 只有根节点才会触发,控制开关相关代码待完善
            {
                cJSON *Data_obj = cJSON_GetObjectItem(pRoot, "Data");
                cJSON *PowerState = cJSON_IsObject(Data_obj) ? cJSON_GetObjectItem(Data_obj, "PowerState") : NULL;
                if (cJSON_IsNumber(PowerState))
                {
                    int power_state = PowerState->valueint;
                    if (power_state == 0)
                    {
                        ESP_LOGI(TAG, "Received command to power off the device");
                        device_response_publish_point(Seq->valueint, 1, "Device powered off");
                    }
                    else if (power_state == 1)
                    {
                        ESP_LOGI(TAG, "Received command to power on the device");
                        device_response_publish_point(Seq->valueint, 1, "Device powered on");
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Invalid PowerState value: %d", power_state);
                        device_response_publish_point(Seq->valueint, 0, "Invalid PowerState value");
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "PowerState is missing or not a number");
                    device_response_publish_point(Seq->valueint, 0, "PowerState is missing or invalid");
                }
            }
        }
    }
    else
    {
        ESP_LOGW(TAG, "Invalid packet format");
    }

    cJSON_Delete(pRoot);
    free(packet_copy);
}

// MQTT数据处理任务，主要处理MQTT消息的接收和解析
static void app_MQTT_Rdata_handle(void *arg)
{
    char *json_str = app_malloc_prefer_psram(MQTT_DATA_Cache);
    if (json_str == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for MQTT json_str");
        s_mqtt_rx_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    int json_str_len = 0; // 累积缓冲区长度
    char Databuf[1025] = {0};
    TickType_t last_recv_time = xTaskGetTickCount();      // 记录上次收到数据的时间
    const TickType_t timeout_ticks = pdMS_TO_TICKS(2000); // 累积超时时间：2秒

    while (1)
    {

        memset(Databuf, 0, sizeof(Databuf));

        int datalen = app_read_MQTT_data(Databuf, sizeof(Databuf), pdMS_TO_TICKS(5000)); // 等待5秒接收MQTT数据
        if (datalen > 0)
        {
            // 收到有效数据，更新时间戳
            last_recv_time = xTaskGetTickCount();
            // 检查是否是完整包
            if (is_complete_packet(Databuf, datalen))
            {

                // 如果累积缓冲区有数据，说明之前的数据不完整
                if (json_str_len > 0)
                {
                    ESP_LOGW(TAG, "Previous incomplete data discarded, new complete packet received");
                    json_str_len = 0;
                    memset(json_str, 0, MQTT_DATA_Cache);
                }

                ESP_LOGI(TAG, "Complete JSON packet: %s", Databuf);
                parse_jsonCommand_MQTT(Databuf, datalen);
            }
            else
            {
                // 检查累积后是否溢出
                if (json_str_len + datalen >= MQTT_DATA_Cache)
                {
                    ESP_LOGE(TAG, "Buffer overflow! Discarding accumulated %d bytes", json_str_len);
                    json_str_len = 0;
                    memset(json_str, 0, MQTT_DATA_Cache);

                    // 当前数据可能已经是完整包
                    if (is_complete_packet(Databuf, datalen))
                    {
                        ESP_LOGI(TAG, "Current data is a complete packet");
                        parse_jsonCommand_MQTT(Databuf, datalen);
                    }
                    else
                    {
                        // 重新开始累积
                        strncpy(json_str, Databuf, datalen);
                        json_str_len = datalen;
                        json_str[json_str_len] = '\0';
                        ESP_LOGI(TAG, "Start new accumulation: %d bytes", json_str_len);
                    }
                }
                else
                {
                    // 追加到累积缓冲区
                    strncpy(json_str + json_str_len, Databuf, datalen);
                    json_str_len += datalen;
                    json_str[json_str_len] = '\0';

                    // 检查累积后是否完整
                    if (is_complete_packet(json_str, json_str_len))
                    {

                        ESP_LOGI(TAG, "Accumulated complete JSON: %s", json_str);
                        parse_jsonCommand_MQTT(json_str, json_str_len);
                        json_str_len = 0;
                        memset(json_str, 0, MQTT_DATA_Cache);
                    }
                    else
                    {

                        ESP_LOGD(TAG, "Still incomplete, accumulated %d bytes", json_str_len);
                    }
                }
            }
        }
        else if (datalen <= 0)
        {
            // 读取超时，没有新数据
            if (json_str_len > 0)
            {
                // 检查累积缓冲区的数据是否已超时（长时间未完成）
                TickType_t now = xTaskGetTickCount();
                if ((now - last_recv_time) >= timeout_ticks)
                {

                    ESP_LOGW(TAG, "Timeout waiting for complete JSON, discarding %d bytes", json_str_len);
                    storage_write_record_cyclic(current_log_pn(), "Timeout waiting for complete JSON, discarding accumulated data");

                    json_str_len = 0;

                    memset(json_str, 0, MQTT_DATA_Cache);
                }
            }
        }
        // Databuf[datalen] = '\0';
    }
    free(json_str);
}

#pragma endregion

#pragma region 网络就绪与断电恢复

// 根据网络状态初始化MQTT和HTTP数据处理任务(子节点启动)
void Init_ByNetwork_Flag(void *arg)
{
    while (1)
    {
        if (Network_Flag == 1)
        {
            esp_err_t err = mqtt_init();
            if (err == ESP_ERR_NO_MEM)
            {
                storage_write_record_cyclic(current_log_pn(), "Failed to initialize MQTT");
            }
            else
            {
                err = mqtt_app_start();
                if (err != ESP_OK)
                {
                    storage_write_record_cyclic(current_log_pn(), "Failed to start MQTT");
                }
                else
                {
                    create_cpu1_task(app_MQTT_Rdata_handle,
                                     "mqtt_rx",
                                     LG_STACK_MQTT_RX,
                                     LG_PRIO_MQTT_RX,
                                     &s_mqtt_rx_task_handle);
                }
            }

            err = Http_init();
            if (err == ESP_ERR_NO_MEM)
            {
                storage_write_record_cyclic(current_log_pn(), "Failed to initialize HTTP");
            }
            else
            {
                create_cpu1_task(app_HTTP_Auto_GetProtocol,
                                 "http_auto",
                                 LG_STACK_HTTP_AUTO,
                                 LG_PRIO_HTTP_AUTO,
                                 &s_http_auto_task_handle);
                create_cpu1_task(app_HTTP_Rdata_handle,
                                 "http_rx",
                                 LG_STACK_HTTP_RX,
                                 LG_PRIO_HTTP_RX,
                                 &s_http_rx_task_handle);

                /*
                 * Ready PN与aging_valid是两个独立状态：
                 * 即使aging_valid==0，也可能已经先收到set_pn，掉电后必须恢复Ready PN。
                 */
                uint16_t persisted_new_pn_ready = 0;
                if (SelfRecovery_Read_uint16(NVS_KEY_NEW_PN_READY, &persisted_new_pn_ready) == ESP_OK &&
                    persisted_new_pn_ready == 1 &&
                    SelfRecovery_Read_str(NVS_KEY_READY_PN, PN_Code_Ready, sizeof(PN_Code_Ready)) == ESP_OK &&
                    PN_Code_Ready[0] != '\0')
                {
                    s_new_pn_ready = 1;
                    ESP_LOGI(TAG, "Recovered ready PN: %s", PN_Code_Ready);
                }
                else
                {
                    clear_ready_pn_runtime();
                }

                uint16_t start_pending = 0;
                uint64_t persisted_start_seq = 0;
                SelfRecovery_Read_uint16(NVS_KEY_START_PENDING, &start_pending);
                SelfRecovery_Read_uint64(NVS_KEY_START_SEQ, &persisted_start_seq);

                err = SelfRecovery_Read_uint16(NVS_KEY_AGING_VALID, &agingResumeState.aging_valid);

                if (err == ESP_OK && agingResumeState.aging_valid == 1)
                {
                    /*
                     * 真正运行中的老化恢复优先级最高。
                     * RUNNING状态下不允许存在下一台Ready PN或WAIT_PN任务。
                     */
                    clear_ready_pn_runtime();
                    persist_ready_pn_state();
                    persist_pending_start_state(false, 0);
                    s_pending_start_seq = 0;

                    SelfRecovery_Read_uint16("CR_Step", &agingResumeState.current_step);
                    SelfRecovery_Read_uint64("Deadline", &agingResumeState.Deadline);
                    SelfRecovery_Read_uint16("CycleIndex", &agingResumeState.CycleIndex);
                    SelfRecovery_Read_str(NVS_KEY_CURRENT_PN, PN_Code, sizeof(PN_Code));
                    SelfRecovery_Read_str("RecordId", RecordId, sizeof(RecordId));

                    uint16_t index_1 = 0;
                    SelfRecovery_Read_uint16("LogIndex", &index_1);
                    Get_Index_From_Flash(index_1);

                    agingResumeState.AgingJson = read_json_from_spiffs("Aging.json");
                    vTaskDelay(pdMS_TO_TICKS(1000));

                    if (agingResumeState.AgingJson != NULL && PN_Code[0] != '\0')
                    {
                        AgingLogActive = true;
                        aging_runtime_log("Power-loss recovery command loaded, resume_step=%u",
                                          agingResumeState.current_step);

                        if (aging_command_json_parse(agingResumeState.AgingJson))
                        {
                            s_aging_cmd_state = AGING_CMD_RUNNING;
                            ESP_LOGI(TAG, "Aging recovery succeeded, state=RUNNING, PN=%s", PN_Code);
                        }
                        else
                        {
                            AgingLogActive = false;
                            s_aging_cmd_state = AGING_CMD_IDLE;
                            memset(PN_Code, 0, sizeof(PN_Code));
                            SelfRecovery_Write_uint16(NVS_KEY_AGING_VALID, 0);
                            SelfRecovery_Write_str(NVS_KEY_CURRENT_PN, "");
                        }
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Aging recovery data invalid: Aging.json or current PN missing");
                        s_aging_cmd_state = AGING_CMD_IDLE;
                        memset(PN_Code, 0, sizeof(PN_Code));
                        SelfRecovery_Write_uint16(NVS_KEY_AGING_VALID, 0);
                        SelfRecovery_Write_str(NVS_KEY_CURRENT_PN, "");
                    }
                }
                else
                {
                    agingResumeState.aging_valid = 0;

                    if (start_pending == 1 && persisted_start_seq <= INT32_MAX)
                    {
                        s_pending_start_seq = (int)persisted_start_seq;
                        s_aging_cmd_state = AGING_CMD_WAIT_PN;
                        AgingLogActive = true;

                        char *pending_json = read_json_from_spiffs("Aging.json");
                        if (pending_json == NULL)
                        {
                            local_error_log("Recovered start_pending flag but Aging.json is missing");
                            s_aging_cmd_state = AGING_CMD_IDLE;
                            s_pending_start_seq = 0;
                            persist_pending_start_state(false, 0);
                            AgingLogActive = false;
                        }
                        else if (s_new_pn_ready == 1)
                        {
                            /* 掉电前两条命令其实都已到达，则上电联网后直接完成启动。 */
                            start_aging_with_ready_pn(pending_json, s_pending_start_seq);
                            free(pending_json);
                        }
                        else
                        {
                            aging_runtime_log("Recovered pending start_aging; still waiting for a new PN, cmd_seq=%d",
                                              s_pending_start_seq);
                            free(pending_json);
                        }
                    }
                    else
                    {
                        s_aging_cmd_state = AGING_CMD_IDLE;
                        s_pending_start_seq = 0;
                    }
                }
            }

            break;
        }
        else
        {
            ESP_LOGW(TAG, "Network is not available, MQTT And HTTP will not start");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    s_network_init_task_handle = NULL;

    vTaskDelete(NULL);
}

#pragma endregion

#pragma region 按钮测试任务

#define BUTTON_PIN 40
static void button_task(void *arg)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        //.pull_down_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io_conf);
    int click_count = 0;

    while (1)
    {

        if (gpio_get_level(BUTTON_PIN) == 0)
        {
            if (click_count == 0)
            {
                show_free_heap("内存");
                print_application_task_stacks();

                // send_control_command(Sensor1, 0xFF, 50);
                click_count = 2;
            }
            else if (click_count == 1)
            {
                show_free_heap("内存");

                click_count = 2;
            }
            else if (click_count == 2)
            {
                show_free_heap("内存");

                click_count = 3;
            }
            else if (click_count == 3)
            {
                show_free_heap("内存");
            }

            //  等待按钮释放
            while (gpio_get_level(BUTTON_PIN) == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

#pragma endregion

#pragma region 设备初始化与通用读写

// 待加入端口号来区分
static void Device_Init(Externaldevice **device1, int devicecount)
{
    if (devicecount == 0) // 没有外接检测设备
    {
        ESP_LOGI(TAG, "No external check devices to initialize");
        return;
    }
    for (int i = 0; i < devicecount; i++)
    {
        Externaldevice *device = &((*device1)[i]);
        // printf("设备名称: %s, ID: %d, 通信方式: %s, 协议: %s\n",device->name, device->id, device->CommunicationMode, device->Protocol);
        if (strncmp(device->CommunicationMode, "CAN", 3) == 0) // 待完善
        {
            // 初始化Can驱动
            esp_err_t ret = can_ext_driver_init();
            if (ret != ESP_OK)
            {
                // ESP_LOGE(TAG, "Failed to init CAN driver: %s", esp_err_to_name(ret));
                aging_error_log("Failed to initialize CAN driver, error=%s", esp_err_to_name(ret));
            }
            else
            {

                // 只有CAN初始化成功才启动
                ret = can_ext_driver_start();
                if (ret != ESP_OK)
                {
                    // ESP_LOGE(TAG, "Failed to start CAN driver: %s", esp_err_to_name(ret));
                    aging_error_log("Failed to start CAN driver, error=%s", esp_err_to_name(ret));
                }
                else
                {
                    device->hardware_initialized = true; // 标记CAN设备硬件已初始化

                    device->CurrentSendFunc = app_write_Can_data;

                    device->CurrentRecvFunc = app_read_Can_data;
                }
            }
        }
        else if (strncmp(device->CommunicationMode, "RS485", 5) == 0)
        {
            // 初始化RS485
            esp_err_t ret = app_RS485_init();
            if (ret == ESP_OK)
            {
                // xTaskCreate(app_RS485_data_handle, "rs485_data_handle", 3072, NULL, 5, NULL);
                device->CurrentSendFunc = app_write_RS485_data;
                device->CurrentRecvFunc = app_read_RS485_data;
                device->hardware_initialized = true; // 标记RS485设备硬件已初始化
            }
            else
            {
                // ESP_LOGW(TAG, "Failed to init RS485: %s", esp_err_to_name(ret));
                aging_error_log("Failed to initialize RS485, error=%s", esp_err_to_name(ret));
            }
        }
        else if (strncmp(device->CommunicationMode, "Bluetooth", 9) == 0)
        {
            // 初始化BLE控制模块
            // BluetoothDeviceCount = i;
            esp_err_t ret = ble_control_init();
            if (ret == ESP_OK)
            {
                ESP_LOGI(TAG, "BLE control module initialized successfully");
                // 设置BLE回调
                ble_control_set_callbacks(ble_scan_complete_cb, ble_connect_cb, ble_disconnect_cb);
                // 创建BLE数据处理任务
                if (BleData_task_handle == NULL)
                {
                    create_cpu1_task(app_ble_data_handle,
                                     "ble_data",
                                     LG_STACK_BLE_DATA,
                                     LG_PRIO_BLE_DATA,
                                     &BleData_task_handle);
                }
                device->CurrentSendFunc = ble_send_data;
                device->CurrentRecvFunc = app_ble_recv_data_Ack;
                device->hardware_initialized = true; // 蓝牙硬件已经初始化
            }
            else
            {
                // ESP_LOGE(TAG, "Failed to initialize BLE control module: %s", esp_err_to_name(ret));
                aging_error_log("Failed to initialize BLE control module, error=%s", esp_err_to_name(ret));
            }
        }
        else if (strncmp(device->CommunicationMode, "RS232", 5) == 0) // 待加入
        {
        }
        else
        {
        }
    }
}

static bool Data_Set_Method(Externaldevice *device, const char *idorname, const char *param, uint8_t slave_addr, uint8_t is_name)
{
    bool SetFlag = false;
    uint8_t RecBuf[256] = {0};

    if (!device || !idorname || !param)
    {
        return false;
    }

    /* ===================== SCPI 写入 ===================== */
    if (device->ProtoType == 0)
    {
        char tx_cmd[128];

        const scpi_cmd_item_t *cmd = NULL;
        if (is_name == 1)
        {
            cmd = scpi_dynamic_find_by_name(device->scpi_protocol, idorname);
        }
        else
        {
            cmd = scpi_dynamic_find_by_id(device->scpi_protocol, idorname);
        }

        if (!cmd)
        {
            ESP_LOGE(TAG, "SCPI command not found: %s", idorname);
            return false;
        }

        /*
         * SCPI 写命令：关键点
         * - param 作为写入值
         * - build command 会把 <NRf> 替换成 param
         */
        esp_err_t ret =
            scpi_dynamic_build_command(cmd,
                                       param,
                                       tx_cmd,
                                       sizeof(tx_cmd));

        if (ret == ESP_OK)
        {
            /* 注意：这里必须用 strlen，不是 sizeof */
            int send_len = device->CurrentSendFunc((uint8_t *)tx_cmd,
                                                   strlen(tx_cmd),
                                                   100);
            if (send_len <= 0)
            {
                return false;
            }

            /* 写命令一般不强依赖返回值，但为了兼容设备仍然读取一次。 */
            int len = device->CurrentRecvFunc(RecBuf,
                                              sizeof(RecBuf),
                                              pdMS_TO_TICKS(1000));

            SetFlag = true;

            if (len > 0)
            {
                ESP_LOGI(TAG, "SCPI write ack: %s", RecBuf);
            }
        }
        else
        {
            ESP_LOGE(TAG, "SCPI build command failed");
        }
    }

    /* ===================== MODBUS 写入（只用 0x06）===================== */
    else if (device->ProtoType == 1)
    {
        const ProtocolItem *item = NULL;
        if (is_name == 1)
        {
            item = ModbusJson_FindItemByName(&device->modbusprotocol, idorname);
        }
        else
        {
            item = ModbusJson_FindItemByID(&device->modbusprotocol, idorname);
        }

        if (!item)
        {
            ESP_LOGE(TAG, "Modbus item not found: %s", idorname);
            return false;
        }

        /*
         * 关键限制：
         * 只允许单寄存器写（0x06）
         */
        if (item->reg_len != 1)
        {
            ESP_LOGE(TAG, "Modbus write only supports 1 register (FC06)");
            return false;
        }

        ModbusFrame frame;
        memset(&frame, 0, sizeof(frame));

        /*
         * string → float → raw
         */
        double value_f = atof(param);
        int32_t raw_value = ModbusJson_PhysicalToRaw(item, value_f);

        /*
         * 生成 0x06 写单寄存器帧
         */
        if (Modbus_Generate06(slave_addr,
                              item->reg_addr,
                              (uint16_t)raw_value,
                              &frame))
        {
            int send_len = device->CurrentSendFunc(frame.data,
                                                   frame.len,
                                                   100);

            Modbus_FreeFrame(&frame);
            if (send_len <= 0)
            {
                return false;
            }

            int len = device->CurrentRecvFunc(RecBuf,
                                              sizeof(RecBuf),
                                              pdMS_TO_TICKS(1000));

            if (len > 0)
            {
                /*
                 * 简单校验：回包长度 >= 8 通常表示正常
                 */
                ESP_LOGI(TAG, "Modbus write success: %s = %s",
                         idorname, param);

                SetFlag = true;
            }
            else
            {
                ESP_LOGW(TAG, "Modbus write timeout");
            }
        }
        else
        {
            ESP_LOGE(TAG, "Modbus frame generate failed");
        }
    }
    /* ===================== CAN 写入 ===================== */
    else if (device->ProtoType == 2)
    {
        uint8_t tx_buf[32] = {0};
        can_ext_transfer_t transfer;
        const CanProtocolItem *item = NULL;

        int ret = -1;

        ret = make_can_write_transfer_by_nameorid(device, idorname, param, slave_addr, tx_buf, sizeof(tx_buf), &transfer, &item, is_name);

        if (ret != 0)
        {
            // printf("协议合成失败\r\n");
            return false;
        }

        /*
         * 这里才真正发送：
         * 内部会发送 0x60 WRITE_START + 0x61 WRITE_DATA
         */
        ret = device->CurrentSendFunc(&transfer,
                                      sizeof(transfer),
                                      pdMS_TO_TICKS(1000));
        if (ret <= 0)
        {
            // printf("设置DC开关发送失败\r\n");
            return false;
        }

        /*
         * 这里才真正接收写入应答：
         * 内部会接收 0x62 WRITE_RESPONSE
         */
        ret = device->CurrentRecvFunc(&transfer,
                                      sizeof(transfer),
                                      portMAX_DELAY);
        if (ret <= 0)
        {
            // printf("设置DC开关应答接收失败, error=%u\r\n", transfer.error_code);
            return false;
        }

        if (transfer.error_code != CAN_PROTO_ACK_OK)
        {
            // printf("设置DC开关失败, error=%u\r\n", transfer.error_code);
            return false;
        }
        SetFlag = true;
        // printf("设置DC开关成功\r\n");
    }
    return SetFlag;
}

// 外接设备使用name，老化设备使用id，is_name=1表示使用name，is_name=0表示使用id
static bool Data_Get_Method(Externaldevice *device, const char *idorname, uint8_t slave_addr, double *value, uint8_t is_name)
{
    bool GetFlag = false;
    uint8_t RecBuf[1024] = {0};

    if (device == NULL || idorname == NULL || value == NULL ||
        device->CurrentSendFunc == NULL || device->CurrentRecvFunc == NULL)
    {
        if (value != NULL)
        {
            *value = -1;
        }
        return false;
    }

    if (device->ProtoType == 0) // SCPI
    {
        char tx_cmd[128];
        const scpi_cmd_item_t *cmd = NULL;
        if (is_name == 1)
        {
            cmd = scpi_dynamic_find_by_name(device->scpi_protocol, idorname);
        }
        else
        {
            cmd = scpi_dynamic_find_by_id(device->scpi_protocol, idorname);
        }

        if (!cmd)
        {
            ESP_LOGE(TAG, "SCPI command not found");
            return GetFlag;
        }
        esp_err_t ret = scpi_dynamic_build_command(cmd, NULL, tx_cmd, sizeof(tx_cmd));
        if (ret == ESP_OK)
        {
            int send_len = device->CurrentSendFunc((uint8_t *)tx_cmd, strlen(tx_cmd), 50);
            if (send_len <= 0)
            {
                *value = -1;
                return false;
            }
            int len = device->CurrentRecvFunc(RecBuf, 1024, pdMS_TO_TICKS(1000));
            if (len > 0)
            {
                size_t count = 0;
                double parsed_value = 0;
                if (scpi_dynamic_parse_response(cmd, (char *)RecBuf, &parsed_value, 1, &count) == ESP_OK && count > 0)
                {
                    *value = (double)parsed_value;
                    GetFlag = true;
                }
                else
                {
                    *value = -1;
                }
            }
            else
            {
                *value = -1; // 读取失败
            }
        }
    }
    else if (device->ProtoType == 1) // Modbus
    {
        const ProtocolItem *item = NULL;
        if (is_name == 1)
        {
            item = ModbusJson_FindItemByName(&device->modbusprotocol, idorname);
        }
        else
        {
            item = ModbusJson_FindItemByID(&device->modbusprotocol, idorname);
        }
        if (!item)
        {
            ESP_LOGE(TAG, "Modbus item not found");
            *value = -1; // 读取失败
            return GetFlag;
        }
        ModbusFrame frame;
        if (Modbus_Generate03(slave_addr, item->reg_addr, item->reg_len, &frame))
        {
            int send_len = device->CurrentSendFunc(frame.data, frame.len, 100);

            Modbus_FreeFrame(&frame);
            if (send_len <= 0)
            {
                *value = -1;
                return false;
            }
            int len = device->CurrentRecvFunc(RecBuf, 1024, pdMS_TO_TICKS(1000));
            if (len > 0)
            {
                int32_t raw_value = 0;
                if (Modbus_Parse03RawValue(RecBuf, len, item->reg_len, item->is_unsigned, &raw_value))
                {
                    GetFlag = true; // 读取成功
                    *value = ModbusJson_RawToPhysical(item, raw_value);
                }
            }
            else
            {
                *value = -1; // 读取失败
            }
        }
    }
    else if (device->ProtoType == 2) // Can
    {
        uint8_t rx_buf[32] = {0};
        can_ext_transfer_t transfer;
        const CanProtocolItem *item = NULL;
        int ret = make_can_read_transfer_by_nameorid(device, idorname, slave_addr, rx_buf, sizeof(rx_buf), &transfer, &item, is_name);

        if (ret != 0)
        {
            // printf("读取协议合成失败\r\n");
            *value = -1;
            return GetFlag;
        }
        /*
         * 这里才真正发送读取请求：
         * 内部会发送 0x63 READ_START
         */
        ret = device->CurrentSendFunc(&transfer,
                                      sizeof(transfer),
                                      pdMS_TO_TICKS(1000));
        if (ret <= 0)
        {
            // printf("读取请求发送失败\r\n");
            *value = -1;
            return GetFlag;
        }

        /*
         * 这里才真正接收读取响应：
         * 内部会接收 0x64 READ_DATA_START + 0x65 READ_DATA_LOAD
         */
        ret = device->CurrentRecvFunc(&transfer,
                                      sizeof(transfer),
                                      portMAX_DELAY);
        if (ret <= 0)
        {
            // printf("读取响应接收失败, error=%u\r\n", transfer.error_code);
            *value = -1;
            return GetFlag;
        }

        if (!CanProtocol_DecodeNumValue(item, rx_buf, transfer.rx_len, value))
        {
            // printf("SOC数值解析失败\r\n");
            *value = -1;
            return GetFlag;
        }
        else
        {
            GetFlag = true; // 读取成功
        }
    }
    return GetFlag;
}

#pragma endregion

#pragma region 老化指令与工艺执行

static bool WaitAgingDeviceProtocolReady(uint32_t timeout_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_tick = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start_tick) < timeout_tick)
    {
        if (Aging_device != NULL &&
            Aging_device[0].HaveProtocol == true &&
            (Aging_device[0].scpi_protocol != NULL ||
             Aging_device[0].modbusprotocol.item_count > 0 ||
             Aging_device[0].canprotocol.item_count > 0))
        {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return false;
}

static void AgingDevice_RuntimeFree(void)
{
    if (Aging_device == NULL)
    {
        return;
    }

    if (Aging_device[0].scpi_protocol != NULL)
    {
        scpi_dynamic_free(Aging_device[0].scpi_protocol);
        Aging_device[0].scpi_protocol = NULL;
    }

    ModbusJson_Free(&Aging_device[0].modbusprotocol);
    memset(&Aging_device[0].modbusprotocol, 0, sizeof(Aging_device[0].modbusprotocol));

    /*
     * 不要 aging_steps_free(Aging_device[0].steps)
     * 因为 Aging_device[0].steps 指向 agingcfg.steps，由 aging_config_free() 统一释放。
     */
    Aging_device[0].steps = NULL;
    Aging_device[0].step_count = 0;

    free(Aging_device);
    Aging_device = NULL;
}

// 老化指令 Json 处理；返回 true 表示老化任务已成功创建。
static bool aging_command_json_parse(const char *packet_copy)
{

    if (packet_copy == NULL)
    {
        ESP_LOGE(TAG, "aging packet is NULL");
        aging_error_log("Aging packet is NULL");
        return false;
    }

    agingState = AgingIdle;
    agingDataAMode = IdleState;

    if (Aging_test_task_handle != NULL)
    {
        vTaskDelete(Aging_test_task_handle);
        Aging_test_task_handle = NULL;
    }
    if (AgingData_task_handle != NULL)
    {
        vTaskDelete(AgingData_task_handle);
        AgingData_task_handle = NULL;
    }
    if (ble_is_connected())
    {
        BTDisConnect = 1;
        ble_disconnect();
    }

    AgingDevice_RuntimeFree();

    if (agingcfg.device_count != 0 || agingcfg.step_count != 0)
    {
        aging_config_free(&agingcfg);
        memset(&agingcfg, 0, sizeof(agingcfg));
    }

    AgingErr err = aging_config_parse(packet_copy, &agingcfg);
    if (err != AGING_OK)
    {
        ESP_LOGE(TAG, "aging_config_parse failed, err=%d", err);
        aging_error_log("Aging configuration parse failed, error=%d", err);
        goto start_failed;
    }

    int64_t protoid = 0;
    get_json_int64(packet_copy, "ProtoID", &protoid);

    agingcfg.devices[0].ProtoID = protoid;

    if (agingcfg.device_count <= 0 || agingcfg.devices == NULL)
    {
        aging_error_log("Aging configuration has no device");
        goto start_failed;
    }

    if (agingcfg.step_count <= 0 || agingcfg.steps == NULL)
    {
        aging_error_log("Aging configuration has no step");
        goto start_failed;
    }

    if (agingcfg.devices[0].ProtoID == 0)
    {
        aging_error_log("Aging device ProtoID is invalid");
        goto start_failed;
    }

    if (agingcfg.devices[0].Communication == NULL)
    {
        aging_error_log("Aging device communication mode is NULL");
        goto start_failed;
    }

    aging_runtime_log("Aging configuration parsed, device_count=%u, step_count=%u, ProtoID=%lld, communication=%s",
                      (unsigned)agingcfg.device_count,
                      (unsigned)agingcfg.step_count,
                      agingcfg.devices[0].ProtoID,
                      agingcfg.devices[0].Communication);

    Aging_device = (Externaldevice *)app_malloc_prefer_psram(sizeof(Externaldevice));
    if (Aging_device == NULL)
    {
        aging_error_log("Aging device memory allocation failed");
        goto start_failed;
    }

    memset(Aging_device, 0, sizeof(Externaldevice));
    Aging_device[0].ProtoID = agingcfg.devices[0].ProtoID;
    Aging_device[0].LastProtoID = 0;
    Aging_device[0].online = false;
    Aging_device[0].hardware_initialized = false;
    Aging_device[0].HaveProtocol = false;
    Aging_device[0].ProtoType = 9;

    if (!copy_string_checked(Aging_device[0].CommunicationMode,
                             sizeof(Aging_device[0].CommunicationMode),
                             agingcfg.devices[0].Communication))
    {
        aging_error_log("Aging device communication mode is too long");
        goto start_failed;
    }

    Aging_device[0].steps = agingcfg.steps;
    Aging_device[0].step_count = agingcfg.step_count;

    Device_Init(&Aging_device, 1);
    if (!Aging_device[0].hardware_initialized)
    {
        aging_error_log("Aging device hardware initialization failed, communication=%s",
                        Aging_device[0].CommunicationMode);
        goto start_failed;
    }

    aging_runtime_log("Aging device hardware initialized, communication=%s", Aging_device[0].CommunicationMode);

    IsAgingDevice = 1;
    esp_err_t http_ret = test_http_getproto_New(Aging_device[0].ProtoID);

    if (http_ret != ESP_OK)
    {
        aging_error_log("Protocol request failed, ProtoID=%lld, error=%s",
                        Aging_device[0].ProtoID,
                        esp_err_to_name(http_ret));
        IsAgingDevice = 0;
        goto start_failed;
    }

    if (!WaitAgingDeviceProtocolReady(10000))
    {
        aging_error_log("Protocol loading timeout, ProtoID=%lld", Aging_device[0].ProtoID);
        IsAgingDevice = 0;
        goto start_failed;
    }

    IsAgingDevice = 0;
    aging_runtime_log("Aging device protocol loaded, ProtoID=%lld, ProtoType=%u",
                      Aging_device[0].ProtoID,
                      Aging_device[0].ProtoType);

    agingState = AgingStartCheck;
    BaseType_t task_ret = create_cpu1_task(Aging_Test_Task,
                                           "aging_ctrl",
                                           LG_STACK_AGING_CTRL,
                                           LG_PRIO_AGING_CTRL,
                                           &Aging_test_task_handle);
    if (task_ret != pdPASS)
    {
        aging_error_log("Aging task creation failed");
        agingState = AgingIdle;
        goto start_failed;
    }

    aging_runtime_log("Aging task created; entering external-device check");
    return true;

start_failed:
    IsAgingDevice = 0;
    agingState = AgingIdle;
    agingDataAMode = IdleState;
    AgingDevice_RuntimeFree();

    if (agingcfg.device_count != 0 || agingcfg.step_count != 0)
    {
        aging_config_free(&agingcfg);
        memset(&agingcfg, 0, sizeof(agingcfg));
    }
    spiffs_file_delete("Aging.json");

    return false;
}
// 老化步骤开始前/结束后执行动作列表，并记录每个失败动作。
static bool Aging_Execute_ActionList(AgingActionItem *actions, size_t action_count, int slave_addr, Externaldevice *cdevice, const char *step_method, const char *action_phase, uint8_t is_name)
{
    if (!actions || action_count == 0)
    {
        return true;
    }

    bool all_success = true;
    const char *device_name = (cdevice != NULL && cdevice->name[0] != '\0') ? cdevice->name : "aging_device";

    for (size_t j = 0; j < action_count; j++)
    {
        AgingActionItem *act = &actions[j];

        const char *parameter = (is_name == 1) ? act->ParaName : act->ParaID;

        if (parameter == NULL || act->ParaValue == NULL)
        {
            aging_error_log("Invalid action item, step=%s, phase=%s, device=%s, action_index=%u",
                            step_method != NULL ? step_method : "unknown",
                            action_phase != NULL ? action_phase : "unknown",
                            device_name,
                            (unsigned)(j + 1));
            all_success = false;
            continue;
        }

        ESP_LOGI(TAG, "Action: Para=%s Value=%s", parameter, act->ParaValue);

        if (!Data_Set_Method(cdevice, parameter, act->ParaValue, slave_addr, is_name))
        {
            aging_error_log("Action send failed, step=%s, phase=%s, device=%s, parameter=%s, value=%s",
                            step_method != NULL ? step_method : "unknown",
                            action_phase != NULL ? action_phase : "unknown",
                            device_name,
                            parameter,
                            act->ParaValue);
            all_success = false;
        }
    }

    return all_success;
}

// 检查设备是否在线，协议是否匹配，设备状态是否正常等，为老化测试做准备
static bool ExDevice_Check(void)
{
    if (read_device_count == 0)
    {
        ESP_LOGI(TAG, "No external check devices configured");
        return true;
    }

    for (int j = 0; j < read_device_count; j++)
    {
        Externaldevice *device = &read_devices[j];

        if (!device->hardware_initialized)
        {
            ESP_LOGE(TAG, "Test device %s hardware not initialized", device->name);
            return false;
        }

        if (device->steps == NULL ||
            device->step_count == 0 ||
            device->steps[0].sample_data == NULL ||
            device->steps[0].sample_data_count == 0 ||
            device->steps[0].sample_data[0].Value == NULL)
        {
            ESP_LOGE(TAG, "Test device %s has no sample item for online check", device->name);
            return false;
        }

        if (device->ProtoType == 0 && strncmp(device->Protocol, "SCPI", 4) != 0)
        {
            ESP_LOGE(TAG, "Test device %s SCPI protocol mismatch", device->name);
            return false;
        }
        if (device->ProtoType == 1 && strncmp(device->Protocol, "MODBUS", 6) != 0)
        {
            ESP_LOGE(TAG, "Test device %s Modbus protocol mismatch", device->name);
            return false;
        }
        if (device->ProtoType == 2 && strncmp(device->Protocol, "CAN", 3) != 0)
        {
            ESP_LOGE(TAG, "Test device %s CAN protocol mismatch", device->name);
            return false;
        }
        if (device->ProtoType > 2)
        {
            ESP_LOGE(TAG, "Test device %s has unsupported protocol type %u",
                     device->name,
                     device->ProtoType);
            return false;
        }

        double check_value = 0;
        const char *parameter = device->steps[0].sample_data[0].Value;
        if (!Data_Get_Method(device, parameter, 1, &check_value, 1))
        {
            device->online = false;
            ESP_LOGE(TAG, "Test device %s communication failed", device->name);
            return false;
        }

        device->online = true;

        ESP_LOGI(TAG, "Test device %s online check passed", device->name);
    }

    return true;
}

// static double aging_get_condition_value(const AgingStep *step, const char *expect_name, double default_value)

static void Aging_Task_Abort_Cleanup(void)
{
    agingState = AgingIdle;
    agingDataAMode = IdleState;

    if (AgingData_task_handle != NULL)
    {
        vTaskDelete(AgingData_task_handle);
        AgingData_task_handle = NULL;
    }

    if (ble_is_connected())
    {
        BTDisConnect = 1;
        ble_disconnect();
    }

    SelfRecovery_Erase_aging_valid();
    memset(&agingResumeState, 0, sizeof(agingResumeState));

    AgingDevice_RuntimeFree();
    if (agingcfg.device_count != 0 || agingcfg.step_count != 0)
    {
        aging_config_free(&agingcfg);
        memset(&agingcfg, 0, sizeof(agingcfg));
    }
    spiffs_file_delete("Aging.json");

    if (BleData_task_handle == NULL)
    {
        BaseType_t task_ret = create_cpu1_task(app_ble_data_handle,
                                               "ble_data",
                                               LG_STACK_BLE_DATA,
                                               LG_PRIO_BLE_DATA,
                                               &BleData_task_handle);
        if (task_ret != pdPASS)
        {
            aging_error_log("Restore BLE data task failed during aging abort cleanup");
        }
    }

    /* 异常终止后彻底回到IDLE，下一台必须重新set_pn。 */
    clear_aging_command_runtime();
}

static void Aging_Task_Abort_And_Delete(void)
{
    Aging_Task_Abort_Cleanup();
    AgingLogActive = false;
    s_consecutive_data_read_failures = 0;
    s_last_data_read_error_report_tick = 0;
    s_consecutive_data_upload_failures = 0;
    s_last_data_upload_error_report_tick = 0;
    Aging_test_task_handle = NULL;
    vTaskDelete(NULL); // 删除当前任务
}

void Aging_Test_Task(void *arg)
{
    uint8_t FailCount = 0;
    bool aging_action_logged = false;

    while (1)
    {
        switch (agingState)
        {
        case AgingIdle:
        {
            break;
        }
        case AgingStartCheck:
        {
            if (FailCount > 5)
            {
                aging_error_log("External-device check failed after %u attempts", (unsigned)FailCount);
                aging_state_publish("agingError");
                Aging_Task_Abort_And_Delete();
                return;
            }

            FailCount++;
            if (ExDevice_Check())
            {
                aging_runtime_log("External-device check passed");
                agingState = AgingDeviceCheck;

                if (AgingData_task_handle == NULL)
                {
                    BaseType_t task_ret = create_cpu1_task(app_AgingData_Get_handle,
                                                           "aging_get",
                                                           LG_STACK_AGING_GET,
                                                           LG_PRIO_AGING_GET,
                                                           &AgingData_task_handle);
                    if (task_ret != pdPASS)
                    {
                        aging_error_log("Aging data-acquisition task creation failed");
                        aging_state_publish("agingError");
                        Aging_Task_Abort_And_Delete();
                        return;
                    }
                }

                aging_runtime_log("Aging data-acquisition task created");
                FailCount = 0;
            }
            break;
        }
        case AgingDeviceCheck:
        {
            if (Aging_device == NULL)
            {
                aging_error_log("Aging device runtime object is NULL during device check");
                aging_state_publish("agingError");
                Aging_Task_Abort_And_Delete();
                return;
            }

            if (FailCount > 5)
            {
                aging_error_log("Aging-device connection or communication check failed after %u attempts, communication=%s",
                                (unsigned)FailCount,
                                Aging_device[0].CommunicationMode);
                aging_state_publish("agingError");
                Aging_Task_Abort_And_Delete();
                return;
            }

            FailCount++;

            if (strncmp(Aging_device[0].CommunicationMode, "Bluetooth", 9) == 0)
            {
                if (!ble_is_connected())
                {
                    aging_runtime_log("Connecting Bluetooth aging device, attempt=%u", (unsigned)FailCount);
                    ble_start_scan(10);

                    while (!ScanComplete)
                    {
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                    ScanComplete = false;

                    if (ble_connect_by_name(PN_Code))
                    {
                        aging_runtime_log("Bluetooth aging device connected");

                        vTaskDelay(pdMS_TO_TICKS(20000)); // 等待设备初始化完成，避免刚连接就读数据失败

                        if (BleData_task_handle != NULL)
                        {
                            vTaskDelete(BleData_task_handle);
                            BleData_task_handle = NULL;
                        }
                    }
                    else
                    {
                        aging_error_log("Bluetooth aging-device connection failed, attempt=%u",
                                        (unsigned)FailCount);
                        break;
                    }
                }

                if (Aging_device[0].modbusprotocol.item_count == 0)
                {
                    aging_error_log("Bluetooth aging device has no loaded Modbus protocol items");
                    break;
                }

                if (Aging_device[0].steps == NULL ||
                    Aging_device[0].step_count == 0)
                {
                    aging_error_log("Bluetooth aging device has no sample item for communication check");
                    break;
                }

                int itest = 0;
                for (; itest < Aging_device[0].step_count; itest++)
                {
                    if (Aging_device[0].steps[itest].sample_data_count != 0)
                    {
                        break;
                    }
                }
                double data = 0;
                const char *parameter = Aging_device[0].steps[itest].sample_data[0].Value;
                if (Data_Get_Method(&Aging_device[0], parameter, 1, &data, 0))
                {
                    aging_runtime_log("Bluetooth aging-device communication check passed, parameter=%s",
                                      parameter != NULL ? parameter : "unknown");
                    FailCount = 0;
                    agingState = AgingAction;
                }
                else
                {
                    aging_error_log("Bluetooth aging-device communication check failed, parameter=%s, attempt=%u",
                                    parameter != NULL ? parameter : "unknown",
                                    (unsigned)FailCount);
                }
            }
            else if (strncmp(Aging_device[0].CommunicationMode, "CAN", 3) == 0)
            {
                if (Aging_device[0].canprotocol.item_count == 0)
                {
                    aging_error_log("CAN aging device has no loaded protocol items");
                    break;
                }

                if (Aging_device[0].steps == NULL ||
                    Aging_device[0].step_count == 0 ||
                    Aging_device[0].steps[0].sample_data_count == 0)
                {
                    aging_error_log("CAN aging device has no sample item for communication check");
                    break;
                }

                double data = 0;
                const char *parameter = Aging_device[0].steps[0].sample_data[0].Value;
                if (Data_Get_Method(&Aging_device[0], parameter, 1, &data, 0))
                {
                    aging_runtime_log("CAN aging-device communication check passed, parameter=%s",
                                      parameter != NULL ? parameter : "unknown");
                    FailCount = 0;
                    agingState = AgingAction;
                }
                else
                {
                    aging_error_log("CAN aging-device communication check failed, parameter=%s, attempt=%u",
                                    parameter != NULL ? parameter : "unknown",
                                    (unsigned)FailCount);
                }
            }
            else if (strncmp(Aging_device[0].CommunicationMode, "RS485", 5) == 0)
            {
                if (Aging_device[0].steps == NULL ||
                    Aging_device[0].step_count == 0 ||
                    Aging_device[0].steps[0].sample_data_count == 0)
                {
                    aging_error_log("RS485 aging device has no sample item for communication check");
                    break;
                }

                double data = 0;
                const char *parameter = Aging_device[0].steps[0].sample_data[0].Value;
                if (Data_Get_Method(&Aging_device[0], parameter, 1, &data, 0))
                {
                    aging_runtime_log("RS485 aging-device communication check passed, parameter=%s",
                                      parameter != NULL ? parameter : "unknown");
                    FailCount = 0;
                    agingState = AgingAction;
                }
                else
                {
                    aging_error_log("RS485 aging-device communication check failed, parameter=%s, attempt=%u",
                                    parameter != NULL ? parameter : "unknown",
                                    (unsigned)FailCount);
                }
            }
            else
            {
                aging_error_log("Unsupported aging-device communication mode: %s",
                                Aging_device[0].CommunicationMode);
                aging_state_publish("agingError");
                Aging_Task_Abort_And_Delete();
                return;
            }

            break;
        }
        case AgingAction:
        {
            if (!aging_action_logged)
            {
                aging_runtime_log("Aging process entered action stage");
                aging_action_logged = true;
            }

            uint16_t i = 0;
            int c = 0;
            if (agingResumeState.aging_valid == 1)
            {
                i = agingResumeState.current_step;
                aging_runtime_log("Resuming aging process from step=%u", (unsigned)(i + 1));
                c = agingResumeState.CycleIndex;
            }

            // 循环次数
            for (; c < CycleIndex; c++)
            {
                // 保存循环次数

                esp_err_t ret = SelfRecovery_Write_uint16("CycleIndex", c);
                if (ret != ESP_OK)
                {
                    aging_error_log("Write current cycle index failed, cycle=%u, error=%s", (unsigned)(c + 1), esp_err_to_name(ret));
                }
                printf("Aging cycle %d started\n", c + 1);
                for (; i < agingcfg.step_count; i++)
                {
                    AgingStep *current_step = &agingcfg.steps[i];
                    const char *method = current_step->method != NULL ? current_step->method : "unknown";

                    esp_err_t ret = SelfRecovery_Write_uint16("CR_Step", i);
                    if (ret != ESP_OK)
                    {
                        aging_error_log("Write current aging step failed, step=%u, error=%s",
                                        (unsigned)(i + 1),
                                        esp_err_to_name(ret));
                    }

                    if (current_step->judging_conditions == NULL || current_step->judging_condition_count == 0 || !current_step->judging_conditions[0].valid)
                    {
                        aging_error_log("Invalid judging condition, step=%u/%u, method=%s", (unsigned)(i + 1), (unsigned)agingcfg.step_count, method);
                        continue;
                    }

                    double target_value = current_step->judging_conditions[0].value_num;
                    s_consecutive_data_read_failures = 0;
                    s_last_data_read_error_report_tick = 0;
                    aging_runtime_log("Step started, index=%u/%u, method=%s, target=%.3f", (unsigned)(i + 1), (unsigned)agingcfg.step_count, method, target_value);

                    CurrentAgingStep = i;

                    Aging_Execute_ActionList(current_step->pre_actions, current_step->pre_action_count, 1, &Aging_device[0], method, "pre_action", 0);

                    for (int j = 0; j < read_device_count; j++)
                    {
                        for (int k = 0; k < read_devices[j].step_count; k++)
                        {
                            if (read_devices[j].steps[k].method != NULL &&
                                strcmp(read_devices[j].steps[k].method, method) == 0)
                            {
                                Aging_Execute_ActionList(read_devices[j].steps[k].pre_actions,
                                                         read_devices[j].steps[k].pre_action_count,
                                                         1,
                                                         &read_devices[j],
                                                         read_devices[j].steps[k].method,
                                                         "pre_action", 1);
                                break;
                            }
                        }
                    }

                    if (strncmp(method, "Standing", 8) == 0)
                    {
                        if (AgingCMode == 2) // 群控模式才会触发
                        {
                            publish_aging_stage("Standing", AgingNumber);
                        }

                        time_t now = time(NULL);
                        uint64_t deadline = now + (uint64_t)(target_value * 60.0);
                        agingDataAMode = StandingState;

                        if (agingResumeState.aging_valid == 1 && agingResumeState.Deadline > 0)
                        {
                            deadline = agingResumeState.Deadline;
                        }
                        else
                        {
                            ret = SelfRecovery_Write_uint64("Deadline", deadline);
                            if (ret != ESP_OK)
                            {
                                aging_error_log("Write standing deadline failed, step=%u, error=%s",
                                                (unsigned)(i + 1),
                                                esp_err_to_name(ret));
                            }
                        }

                        while (now < deadline)
                        {
                            vTaskDelay(pdMS_TO_TICKS(10000));
                            now = time(NULL);
                        }

                        aging_runtime_log("Standing condition reached, step=%u, target_minutes=%.3f",
                                          (unsigned)(i + 1),
                                          target_value);
                    }
                    else if (strncmp(method, "Discharge", 9) == 0)
                    {
                        if (AgingCMode == 2) // 群控模式才会触发
                        {
                            publish_aging_stage("Discharge", AgingNumber);
                        }
                        agingDataAMode = DischargeState;
                        while (D_SOC >= target_value)
                        {
                            vTaskDelay(pdMS_TO_TICKS(10000));
                        }
                        aging_runtime_log("Discharge condition reached, step=%u, SOC=%.3f, target=%.3f",
                                          (unsigned)(i + 1),
                                          D_SOC,
                                          target_value);
                    }
                    else if (strncmp(method, "Recharge", 8) == 0)
                    {
                        if (AgingCMode == 2) // 群控模式才会触发
                        {
                            publish_aging_stage("Recharge", AgingNumber);
                        }
                        agingDataAMode = RechargeState;
                        while (D_SOC <= target_value)
                        {
                            vTaskDelay(pdMS_TO_TICKS(10000));
                        }
                        aging_runtime_log("Recharge condition reached, step=%u, SOC=%.3f, target=%.3f",
                                          (unsigned)(i + 1),
                                          D_SOC,
                                          target_value);
                    }
                    else
                    {
                        aging_error_log("Unsupported aging step method, step=%u/%u, method=%s",
                                        (unsigned)(i + 1),
                                        (unsigned)agingcfg.step_count,
                                        method);
                    }

                    Aging_Execute_ActionList(current_step->after_actions,
                                             current_step->after_action_count,
                                             1,
                                             &Aging_device[0],
                                             method,
                                             "after_action", 0);

                    for (int j = 0; j < read_device_count; j++)
                    {
                        for (int k = 0; k < read_devices[j].step_count; k++)
                        {
                            if (read_devices[j].steps[k].method != NULL &&
                                strcmp(read_devices[j].steps[k].method, method) == 0)
                            {
                                Aging_Execute_ActionList(read_devices[j].steps[k].after_actions,
                                                         read_devices[j].steps[k].after_action_count,
                                                         1,
                                                         &read_devices[j],
                                                         read_devices[j].steps[k].method,
                                                         "after_action", 1);
                                break;
                            }
                        }
                    }

                    aging_runtime_log("Step completed, index=%u/%u, method=%s", (unsigned)(i + 1), (unsigned)agingcfg.step_count, method);
                    if (AgingCMode == 2)
                    {
                        agingDataAMode = IdleState; // 群控模式下，老化步骤完成后，先把老化状态置为IdleState，等待上位机服务下发下一步任务
                        // 往上位机服务发送任务已完成，等待上位机服务下发下一步任务，得等所有节点都完成
                        publish_aging_complete(1, AgingNumber);
                        while (Cannextstep == 0)
                        {
                            vTaskDelay(pdMS_TO_TICKS(10000));
                        }
                        Cannextstep = 0;
                    }
                }
                i = 0; // 循环次数大于1时，重置步骤索引
            }

            aging_runtime_log("All aging steps completed; entering completion stage");
            agingState = AgingComplete;
            break;
        }
        case AgingComplete:
        {
            while (QueryLatestRecordBySNAndPushState(PN_Code, false, &g_db1_result) == 0)
            {
                if (g_db1_result.json_data[0] != '\0')
                {
                    if (app_mqtt_publish("device/%s/data/aging", g_db1_result.json_data, DEVICE_ID) > 0)
                    {
                        UpdateRecordPushStateBySNAndID(PN_Code, g_db1_result.seq_no, true);
                    }
                    else
                    {
                        aging_error_log("Historical aging-data retransmission failed, seq_no=%d",
                                        g_db1_result.seq_no);
                        break;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            agingState = AgingIdle;
            agingDataAMode = IdleState;
            agingResumeState.aging_valid = 0;

            if (ble_is_connected())
            {
                BTDisConnect = 1;
                ble_disconnect();
            }

            SelfRecovery_Erase_aging_valid();
            memset(&agingResumeState, 0, sizeof(agingResumeState));

            if (BleData_task_handle == NULL)
            {
                BaseType_t task_ret = create_cpu1_task(app_ble_data_handle,
                                                       "ble_data",
                                                       LG_STACK_BLE_DATA,
                                                       LG_PRIO_BLE_DATA,
                                                       &BleData_task_handle);
                if (task_ret != pdPASS)
                {
                    aging_error_log("Recover BLE data task failed after aging completion");
                }
            }

            AgingDevice_RuntimeFree();
            if (agingcfg.device_count != 0 || agingcfg.step_count != 0)
            {
                aging_config_free(&agingcfg);
                memset(&agingcfg, 0, sizeof(agingcfg));
            }
            spiffs_file_delete("Aging.json");

            s_aging_cmd_state = AGING_CMD_IDLE;
            s_pending_start_seq = 0;

            aging_runtime_log("Aging completed successfully");
            aging_state_publish("agingIdle");

            AgingLogActive = false;
            s_consecutive_data_read_failures = 0;
            s_last_data_read_error_report_tick = 0;
            s_consecutive_data_upload_failures = 0;
            s_last_data_upload_error_report_tick = 0;

            memset(PN_Code, 0, sizeof(PN_Code));
            clear_ready_pn_runtime();
            Aging_test_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#pragma endregion

#pragma region 老化采样与数据上传

// 老化设备通过这个函数获取参数ID对应的参数名称，方便在日志中打印
const char *AgingDataName(const char *id)
{
    if (Aging_device == NULL || id == NULL || id[0] == '\0')
    {
        return "Unknown";
    }

    if (Aging_device[0].ProtoType == 0)
    {
        const scpi_cmd_item_t *item = scpi_dynamic_find_by_id(Aging_device[0].scpi_protocol, id);
        return item ? item->name : "Unknown";
    }
    else if (Aging_device[0].ProtoType == 1)
    {
        const ProtocolItem *item = ModbusJson_FindItemByID(&Aging_device[0].modbusprotocol, id);
        return item ? item->name : "Unknown";
    }
    else if (Aging_device[0].ProtoType == 2)
    {
        const CanProtocolItem *item = CanJson_FindItemByID(&Aging_device[0].canprotocol, id);
        return item ? item->name : "Unknown";
    }
    else
    {
        return "Unknown";
    }
}

static const char *aging_report_device_name(void)
{
    if (agingcfg.devices != NULL && agingcfg.device_count > 0 &&
        agingcfg.devices[0].Type != NULL && agingcfg.devices[0].Type[0] != '\0')
    {
        return agingcfg.devices[0].Type;
    }

    if (Aging_device != NULL && Aging_device[0].name[0] != '\0')
    {
        return Aging_device[0].name;
    }

    return "AgingDevice";
}

static int find_external_step_index(const Externaldevice *device, const char *method)
{
    if (device == NULL || device->steps == NULL || device->step_count == 0 || method == NULL)
    {
        return -1;
    }

    for (size_t i = 0; i < device->step_count; i++)
    {
        if (device->steps[i].method != NULL && strcmp(device->steps[i].method, method) == 0)
        {
            return (int)i;
        }
    }

    return -1;
}

static bool add_aging_value_item(cJSON *data_array,
                                 const char *name,
                                 double value)
{
    if (data_array == NULL || name == NULL || name[0] == '\0')
    {
        return false;
    }

    cJSON *item = cJSON_CreateObject();
    if (item == NULL)
    {
        return false;
    }

    char value_string[32];
    snprintf(value_string, sizeof(value_string), "%.2f", value);
    cJSON_AddStringToObject(item, "name", name);
    cJSON_AddStringToObject(item, "value", value_string);
    cJSON_AddItemToArray(data_array, item);
    return true;
}

static void AgingUploadPacket_Free(AgingUploadPacket *packet)
{
    if (packet == NULL)
    {
        return;
    }

    free(packet->value_json);
    packet->value_json = NULL;
    free(packet);
}

/*
 * 采样时直接生成最终结构化 Value：
 *   - 老化设备必有，参数名通过 SampleData.Value(ID) -> AgingDataName(ID) 获取；
 *   - 外接设备可为 0~N 个，设备名使用 read_devices[i].name，参数名直接使用自身 SampleData.Value；
 *   - 当前步骤没有对应 SampleData 的外接设备不加入 Value。
 *
 * 同时在这一刻固化 PN / CurrentStep / TIMESTAMP，避免异步上传时步骤或 PN 已经变化。
 */
static int ProcessAgingStepData(AgingUploadPacket *packet)
{
    if (packet == NULL || Aging_device == NULL ||
        agingcfg.steps == NULL || CurrentAgingStep >= agingcfg.step_count)
    {
        aging_error_log("Invalid aging runtime state while sampling data");
        return -1;
    }

    memset(packet, 0, sizeof(*packet));
    snprintf(packet->sn, sizeof(packet->sn), "%s", PN_Code);
    packet->current_step = CurrentAgingStep;
    packet->timestamp = (int)time(NULL);

    AgingStep *aging_step = &agingcfg.steps[packet->current_step];
    cJSON *value_array = cJSON_CreateArray();
    cJSON *aging_group = cJSON_CreateObject();
    cJSON *aging_data = cJSON_CreateArray();
    if (value_array == NULL || aging_group == NULL || aging_data == NULL)
    {
        cJSON_Delete(value_array);
        cJSON_Delete(aging_group);
        cJSON_Delete(aging_data);
        return -1;
    }

    cJSON_AddStringToObject(aging_group, "Name", aging_report_device_name());
    cJSON_AddItemToObject(aging_group, "Data", aging_data);
    cJSON_AddItemToArray(value_array, aging_group);

    if (aging_step->sample_data_count <= 0)
    {
        cJSON_Delete(value_array);
        return -1;
    }

    /* 老化设备数据：SampleData.Value 是协议参数 ID。 */
    for (size_t i = 0; i < aging_step->sample_data_count; i++)
    {
        if (packet->value_count >= AGING_UPLOAD_VALUE_COUNT)
        {
            aging_error_log("Aging sample count exceeds limit=%u", (unsigned)AGING_UPLOAD_VALUE_COUNT);
            cJSON_Delete(value_array);
            return -1;
        }

        AgingSampleData *sample_data = &aging_step->sample_data[i];
        const char *parameter_id = sample_data->Value;
        double value = -1.0;
        bool read_ok = Data_Get_Method(&Aging_device[0], parameter_id, 1, &value, 0);
        aging_data_read_result(read_ok, &Aging_device[0], parameter_id);

        const char *parameter_name = AgingDataName(parameter_id);
        if (parameter_name == NULL || strcmp(parameter_name, "Unknown") == 0)
        {
            aging_error_log("Aging parameter name lookup failed, id=%s",
                            parameter_id != NULL ? parameter_id : "NULL");
            /* 查询失败时保留原始 ID，避免数据失去可追溯性。 */
            parameter_name = (parameter_id != NULL && parameter_id[0] != '\0') ? parameter_id : "Unknown";
        }

        if (read_ok && strstr(parameter_name, "SOC") != NULL)
        {
            D_SOC = value;
        }

        if (!add_aging_value_item(aging_data, parameter_name, value))
        {
            cJSON_Delete(value_array);
            return -1;
        }

        packet->value_count++;
    }

    /* 外接设备是可选的；没有外接设备时 Value 中只有上面的老化设备组。 */
    if (read_devices != NULL && read_device_count > 0)
    {
        for (int i = 0; i < read_device_count; i++)
        {
            int ext_step_index = find_external_step_index(&read_devices[i], aging_step->method);
            if (ext_step_index < 0)
            {
                /* 当前步骤没有对应配置属于正常情况，直接跳过该设备。 */
                continue;
            }

            AgingStep *ext_step = &read_devices[i].steps[ext_step_index];
            if (ext_step->sample_data == NULL || ext_step->sample_data_count == 0)
            {
                continue;
            }

            cJSON *ext_group = cJSON_CreateObject();
            cJSON *ext_data = cJSON_CreateArray();
            if (ext_group == NULL || ext_data == NULL)
            {
                cJSON_Delete(ext_group);
                cJSON_Delete(ext_data);
                cJSON_Delete(value_array);
                return -1;
            }

            const char *device_name = read_devices[i].name[0] != '\0' ? read_devices[i].name : "ExternalDevice";
            cJSON_AddStringToObject(ext_group, "Name", device_name);
            cJSON_AddItemToObject(ext_group, "Data", ext_data);
            cJSON_AddItemToArray(value_array, ext_group);

            for (size_t j = 0; j < ext_step->sample_data_count; j++)
            {
                if (packet->value_count >= AGING_UPLOAD_VALUE_COUNT)
                {
                    aging_error_log("Aging sample count exceeds limit=%u", (unsigned)AGING_UPLOAD_VALUE_COUNT);
                    cJSON_Delete(value_array);
                    return -1;
                }

                AgingSampleData *sample_data = &ext_step->sample_data[j];
                const char *parameter_name = sample_data->Value;
                if (parameter_name == NULL || parameter_name[0] == '\0')
                {
                    aging_error_log("External-device sample name is empty, device=%s", device_name);
                    cJSON_Delete(value_array);
                    return -1;
                }

                double value = -1.0;
                bool read_ok = Data_Get_Method(&read_devices[i], parameter_name, 1, &value, 1);
                aging_data_read_result(read_ok, &read_devices[i], parameter_name);

                if (!add_aging_value_item(ext_data, parameter_name, value))
                {
                    cJSON_Delete(value_array);
                    return -1;
                }

                packet->value_count++;
            }
        }
    }

    char *value_json = cJSON_PrintUnformatted(value_array);
    cJSON_Delete(value_array);
    if (value_json == NULL)
    {
        return -1;
    }

    size_t value_json_len = strlen(value_json);
    if (value_json_len == 0 || value_json_len > DB_VALUE_DATA_MAX_LEN)
    {
        aging_error_log("Structured Value JSON too long, len=%u, max=%u",
                        (unsigned)value_json_len,
                        (unsigned)DB_VALUE_DATA_MAX_LEN);
        free(value_json);
        return -1;
    }

    packet->value_json = value_json;
    return (int)packet->value_count;
}

// 老化数据采集任务
void app_AgingData_Get_handle(void *arg)
{
    while (1)
    {
        if (agingDataAMode != IdleState)
        {
            AgingUploadPacket *packet = (AgingUploadPacket *)app_calloc_prefer_psram(1, sizeof(AgingUploadPacket));
            if (packet == NULL)
            {
                aging_error_log("Allocate aging upload packet failed");
            }
            else
            {
                int sample_count = ProcessAgingStepData(packet);
                if (sample_count > 0 && packet->value_json != NULL)
                {
                    if (xQueueSend(Upload_data_queue, &packet, pdMS_TO_TICKS(50)) != pdTRUE)
                    {
                        aging_error_log("Aging upload-data queue is full; current sample was dropped");
                        AgingUploadPacket_Free(packet);
                    }
                }
                else
                {
                    printf("该阶段莫得数据\n");
                    AgingUploadPacket_Free(packet);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void app_DataUpload_Functiong(const AgingUploadPacket *packet, int idnum)
{
    if (packet == NULL || packet->value_json == NULL)
    {
        return;
    }

    char *publish_string = create_sensor_json(idnum,
                                              packet->sn,
                                              packet->current_step,
                                              packet->timestamp,
                                              packet->value_json);
    if (publish_string == NULL)
    {
        aging_error_log("Create aging-data report JSON failed, id=%d, step=%d",
                        idnum,
                        packet->current_step);
        return;
    }

    /*
     * 掉电可靠性策略：先落SQLite，再发MQTT。
     *
     * 1) 先以 pushed=0 提交数据库；
     * 2) 再尝试 MQTT；
     * 3) MQTT publish 被客户端接受后更新 pushed=1。
     *
     * 如果步骤2/3之间突然断电，SQLite仍保留 pushed=0，重启后最多重复补发一次，
     * 不会出现“MQTT刚发出但SQLite还没来得及保存”的本地数据丢失窗口。
     */
    int db_ret = InsertStructuredRecord(idnum,
                                        packet->sn,
                                        packet->current_step,
                                        packet->timestamp,
                                        false,
                                        packet->value_json);

    if (db_ret != 0)
    {
        aging_error_log("Persist aging data to SQLite failed before MQTT, id=%d, step=%d, sn=%s",
                        idnum,
                        packet->current_step,
                        packet->sn);
    }
    else
    {
        ESP_LOGI(TAG, "Aging data persisted before MQTT, id=%d, pushed=0, value_count=%u",
                 idnum,
                 (unsigned)packet->value_count);
    }

    bool upload_success = Network_Flag &&
                          (app_mqtt_publish("device/%s/data/aging",
                                            publish_string,
                                            DEVICE_ID) > 0);

    if (upload_success && db_ret == 0)
    {
        if (UpdateRecordPushStateBySNAndID(packet->sn, idnum, true) != 0)
        {
            /*
             * MQTT已提交但状态更新失败时保持 pushed=0 更安全。
             * 后续可能重复补发，但不会静默丢数据。
             */
            aging_error_log("MQTT submitted but SQLite pushed-state update failed, id=%d, sn=%s",
                            idnum,
                            packet->sn);
        }
    }

    aging_data_upload_result(upload_success, idnum);
    free(publish_string);
}

// 老化数据上传任务
void app_AgingData_Upload_handle(void *arg)
{
    int idnum = 0;
    char active_sn[AGING_UPLOAD_SN_MAX_LEN] = {0};

    while (1)
    {
        AgingUploadPacket *packet = NULL;
        if (xQueueReceive(Upload_data_queue, &packet, portMAX_DELAY) != pdTRUE || packet == NULL)
        {
            continue;
        }

        /*
         * 每个SN独立维护IDNUM。
         * 掉电恢复时，第一包数据到来后从SQLite查询该SN最后一个IDNUM并继续 +1。
         * 正常新SN则从0开始。
         */
        if (strncmp(active_sn, packet->sn, sizeof(active_sn)) != 0)
        {
            snprintf(active_sn, sizeof(active_sn), "%s", packet->sn);
            idnum = 0;

            if (agingResumeState.aging_valid == 1)
            {
                QueryResult resume_record = {0};
                if (QueryStructuredRecordLatestBySN(packet->sn, &resume_record) == 0)
                {
                    idnum = resume_record.seq_no + 1;
                    ESP_LOGI(TAG,
                             "Resume aging upload from SQLite: PN=%s, last_id=%d, next_id=%d",
                             packet->sn,
                             resume_record.seq_no,
                             idnum);
                }
                else
                {
                    ESP_LOGW(TAG,
                             "No SQLite history found for resumed PN=%s; IDNUM starts from 0",
                             packet->sn);
                }
            }
            else
            {
                ESP_LOGI(TAG, "New aging upload session: PN=%s, IDNUM starts from 0", packet->sn);
            }
        }

        app_DataUpload_Functiong(packet, idnum);
        idnum++;
        AgingUploadPacket_Free(packet);
    }
}

#pragma endregion

#pragma region 启动与任务创建

void app_task_init(void)
{
    // 初始化NVS
    ESP_ERROR_CHECK(nvs_flash_init());
    // 初始化SPIFFS,存配置相关的
    if (spiffs_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS");
        return;
    }
    // 初始化SQLite数据库，存储老化数据
    SqLite_Init();
    // 挂载存储设备，存log日志的
    esp_err_t ret = log_storage_mount();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Storage mount failed\n");
    }

    // 自恢复相关数据存储空间初始化，有就直接打开，没有就创建
    ret = SelfRecovery_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SelfRecovery init failed");
    }

    SelfRecovery_Read_uint64(NVS_KEY_DEVICE_NUMBER, &DeviceNumber);
    SelfRecovery_Read_uint64(NVS_KEY_SAFE_CODE, &SafeCode);
    // 初始化UART0
    app_uart_init();

    // 创建串口数据处理任务
    BaseType_t ret1 = 0;
    ret1 = create_cpu1_task(app_uart_data_handle, "uart_json", LG_STACK_UART_JSON, LG_PRIO_UART_JSON, &s_uart_json_task_handle);
    if (ret1 != pdPASS)
    {
        // ESP_LOGE(TAG, "uart_data_handle create failed");
        storage_write_record_cyclic(current_log_pn(), "uart_data_handle create failed!");
    }

    // 读取基本配置和外接设备配置
    ret = readconfig();
    if (ret != ESP_OK)
    {
        // ESP_LOGE(TAG, "Configuration read failed");
        storage_write_record_cyclic(current_log_pn(), "Configuration read failed");
        // return;
    }

    ESP_LOGI(TAG, "Configuration loaded successfully");
    Device_Init(&read_devices, read_device_count); // 初始化设备

    // UDP_Port作为一个开关，配置上位机修改之后才启动
    if (UDP_Port == 1)
    {
        mesh_init_Custom(); // 重新初始化Mesh网络以应用新的配置
        if (IsRoot == 0)    // 子节点才运行MQTT/HTTP/老化相关业务任务
        {
            // printf("我是子节点\n");
            //  初始化上传数据队列
            Upload_data_queue = xQueueCreate(3, sizeof(AgingUploadPacket *));
            if (Upload_data_queue == NULL)
            {
                // ESP_LOGE(TAG, "data queue create failed");
                storage_write_record_cyclic(current_log_pn(), "Upload_data_queue create failed!");
            }
            ret1 = create_cpu1_task(Init_ByNetwork_Flag,
                                    "net_init",
                                    LG_STACK_NET_INIT,
                                    LG_PRIO_NET_INIT,
                                    &s_network_init_task_handle);
            if (ret1 != pdPASS)
            {
                storage_write_record_cyclic(current_log_pn(), "Init_ByNetwork_Flag create failed!");
            }
            ret1 = create_cpu1_task(app_AgingData_Upload_handle,
                                    "aging_upload",
                                    LG_STACK_AGING_UPLOAD,
                                    LG_PRIO_AGING_UPLOAD,
                                    &s_aging_upload_task_handle);
            if (ret1 != pdPASS)
            {
                // ESP_LOGE(TAG, "app_AgingData_Upload_handle create failed");
                storage_write_record_cyclic(current_log_pn(), "app_AgingData_Upload_handle create failed!");
            }
        }
        else if (IsRoot == 1)
        {
            vTaskDelay(pdMS_TO_TICKS(5000)); // 等待网络初始化完成
            esp_err_t err = mqtt_init();
            if (err == ESP_ERR_NO_MEM)
            {
                storage_write_record_cyclic(current_log_pn(), "Failed to initialize MQTT");
            }
            else
            {
                err = mqtt_app_start();
                if (err != ESP_OK)
                {
                    storage_write_record_cyclic(current_log_pn(), "Failed to start MQTT");
                }
                else
                {
                    create_cpu1_task(app_MQTT_Rdata_handle,
                                     "mqtt_rx",
                                     LG_STACK_MQTT_RX,
                                     LG_PRIO_MQTT_RX,
                                     &s_mqtt_rx_task_handle);
                }
            }
        }
    }
    ret1 = create_cpu1_task(button_task,
                            "button",
                            LG_STACK_BUTTON,
                            LG_PRIO_BUTTON,
                            &s_button_task_handle);
    if (ret1 != pdPASS)
    {
        ESP_LOGE(TAG, "button_task create failed");
    }

    // 设置日志级别，后面注释掉
    esp_log_level_set("*", ESP_LOG_INFO);
}

#pragma endregion
