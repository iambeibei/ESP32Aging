// ble_control.c
#include "Ble_control.h"
#include "task_config.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatt_common_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "appTask.h"
#include "app_enc.h"

static const char *TAG = "BLE_CTRL";

// 配置参数
#define MAX_SCAN_DEVICES 50
#define DEFAULT_SCAN_TIME 10
#define REMOTE_SERVICE_UUID 0xFF00
#define REMOTE_NOTIFY_CHAR_UUID 0xFF01
#define REMOTE_WRITE_CHAR_UUID 0xFF02
#define INVALID_HANDLE 0
#define PROFILE_NUM 1
#define PROFILE_A_APP_ID 0

bool ble_Scan_complate; // 扫描完成标志

// 消息队列
// static QueueHandle_t scan_result_queue;  //扫描结果队列
static QueueHandle_t data_queue;         // 接收到的数据队列
static QueueHandle_t data_forword_queue; // 转发的数据队列

// BLE设备列表
static ble_device_info_t s_scanned_devices[MAX_SCAN_DEVICES];
static int s_scanned_count = 0;
static bool s_is_scanning = false;
static bool s_is_connected = false;
static char s_connected_name[20] = {0};
static SemaphoreHandle_t s_device_list_mutex = NULL; // 互斥锁保护设备列表

// 回调函数
static ble_scan_cb_t s_scan_cb = NULL;
static ble_connect_cb_t s_connect_cb = NULL;
static ble_disconnect_cb_t s_disconnect_cb = NULL;

// GATT profile结构
typedef struct
{
    esp_gattc_cb_t gattc_cb;
    uint16_t gattc_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t notify_char_handle;
    uint16_t write_char_handle;
    esp_bd_addr_t remote_bda;
    bool service_found;
} gattc_profile_t;

static gattc_profile_t s_gattc_profile = {
    .gattc_cb = NULL,
    .gattc_if = ESP_GATT_IF_NONE,
    .app_id = PROFILE_A_APP_ID,
    .conn_id = 0,
    .service_start_handle = 0,
    .service_end_handle = 0,
    .notify_char_handle = 0,
    .write_char_handle = 0,
    .remote_bda = {0},
    .service_found = false,
};

// UUID定义
static esp_bt_uuid_t s_remote_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid.uuid16 = REMOTE_SERVICE_UUID,
};

static esp_bt_uuid_t s_notify_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid.uuid16 = REMOTE_NOTIFY_CHAR_UUID,
};

static esp_bt_uuid_t s_write_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid.uuid16 = REMOTE_WRITE_CHAR_UUID,
};

static esp_bt_uuid_t s_notify_descr_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG,
};

// 扫描参数
static esp_ble_scan_params_t s_ble_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE};

// 函数声明
static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void ble_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void add_scanned_device(esp_ble_gap_cb_param_t *scan_result);
static ble_device_info_t *find_device_by_address(uint8_t *bda);

// 蓝牙扫描列表任务
//  void app_ble_scan_list_task(void *arg)
//  {
//      esp_ble_gap_cb_param_t result;
//      while (1)
//      {
//          if (xQueueReceive(scan_result_queue, &result, portMAX_DELAY) == pdTRUE)
//          {
//              add_scanned_device(&result);
//          }
//      }
//  }

static uint8_t a_data[BUFFER_MAX];

void app_ble_recv_task(void *arg)
{
    uint16_t datalen = 0;
    uint32_t wait_time = portMAX_DELAY;
    while (1)
    {
        ble_mtu_data_t mtu_recv;
        if (xQueueReceive(data_queue, &mtu_recv, wait_time) == pdTRUE) /* 获取队列 */
        {
            if ((datalen + mtu_recv.data_len) < BUFFER_MAX)
            {
                memcpy(&a_data[datalen], mtu_recv.data, mtu_recv.data_len);
                datalen += mtu_recv.data_len;
                wait_time = mtu_recv.wait_time;
            }
            else
            {
                wait_time = 0;
            }
            free(mtu_recv.data);
        }
        else
        {
            ESP_LOGI(TAG, "ble a recv len: %d", datalen);
            ble_data_t ble_data;
            ble_data.data = malloc(datalen);
            if (ble_data.data)
            {
                ble_data.data_len = datalen;
                ble_data.profile_id = PROFILE_A_APP_ID;
                // memcpy(ble_data.remote, gl_profile_tab[PROFILE_A_APP_ID].remote_bda, 6);
                memcpy(ble_data.data, a_data, datalen);
                if (!data_forword_queue || xQueueSend(data_forword_queue, &ble_data, pdMS_TO_TICKS(250)) != pdTRUE)
                {
                    free(ble_data.data);
                }
            }
            datalen = 0;
            wait_time = portMAX_DELAY;
        }
    }
}

int app_ble_recv_data_form_remote(uint8_t *data, int max_len, uint32_t wait_time)
{
    ble_data_t ble_data = {0};
    if (!data || !data_forword_queue || xQueueReceive(data_forword_queue, &ble_data, wait_time) != pdTRUE)
    {
        return -1;
    }

    memset(data, 0, max_len); // 清空数据缓冲区
    // esp_log_buffer_hex(GATTC_TAG, ble_data.data, ble_data.data_len);
    int copy_len = -1;
    uint8_t id = ble_data.profile_id;
    if (id < PROFILE_NUM)
    {
        copy_len = ble_data.data_len % (max_len + 1);
        memcpy(data, ble_data.data, copy_len);
    }
    else
    {
        ESP_LOGE(TAG, "ble profile id error [id: %d]", id);
    }
    free(ble_data.data);
    return copy_len;
}
//==================== 初始化函数 ====================
static bool IsInited = false;
esp_err_t ble_control_init(void)
{
    if (IsInited)
    {
        ESP_LOGW(TAG, "BLE control module already initialized");
        return ESP_OK;
    }

    esp_err_t ret;
    ble_Scan_complate = false;
    ESP_LOGI(TAG, "Initializing BLE control module");

    // scan_result_queue = xQueueCreate(50, sizeof(esp_ble_gap_cb_param_t)); // 初始化长度为50的扫描结果队列
    // if (scan_result_queue == NULL)
    // {
    //     ESP_LOGE(TAG, "scan_result_queue create failed");
    //     return;
    // }

    data_queue = xQueueCreate(5, sizeof(ble_mtu_data_t)); // 初始化长度为10的接收数据队列
    if (data_queue == NULL)
    {
        ESP_LOGE(TAG, "data_queue create failed");
        return ESP_FAIL;
    }

    data_forword_queue = xQueueCreate(5, sizeof(ble_data_t));
    if (data_forword_queue == NULL)
    {
        ESP_LOGE(TAG, "data_forword_queue create failed");
        return ESP_FAIL;
    }

    // 创建互斥锁
    if (s_device_list_mutex == NULL)
    {
        s_device_list_mutex = xSemaphoreCreateMutex();
    }

    // 释放经典蓝牙内存
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Failed to release classic BT memory: %s", esp_err_to_name(ret));
        return ret;
    }

    // 初始化蓝牙控制器
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize controller: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enable controller: %s", esp_err_to_name(ret));
        return ret;
    }

    // 初始化Bluedroid
    ret = esp_bluedroid_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init bluedroid: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enable bluedroid: %s", esp_err_to_name(ret));
        return ret;
    }

    // 注册GAP回调
    ret = esp_ble_gap_register_callback(ble_gap_cb);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register GAP callback: 0x%x", ret);
        return ret;
    }

    // 注册GATTC回调
    ret = esp_ble_gattc_register_callback(ble_gattc_cb);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register GATTC callback: 0x%x", ret);
        return ret;
    }

    // 注册GATT应用
    ret = esp_ble_gattc_app_register(PROFILE_A_APP_ID);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register GATT app: 0x%x", ret);
        return ret;
    }

    // 设置本地MTU
    esp_ble_gatt_set_local_mtu(500); // 设置较大的MTU以支持更大数据传输

    ESP_LOGI(TAG, "BLE control module initialized successfully");

    // xTaskCreate(app_ble_scan_list_task, "app_ble_scan_list_task", 3072, NULL, 10, NULL);
    if (xTaskCreatePinnedToCore(app_ble_recv_task,
                                "ble_recv",
                                LG_STACK_BLE_RECV,
                                NULL,
                                LG_PRIO_BLE_RECV,
                                NULL,
                                LG_APP_CPU_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create BLE receive task");
        return ESP_ERR_NO_MEM;
    }

    IsInited = true;
    return ret;
}

void ble_control_set_callbacks(ble_scan_cb_t scan_cb, ble_connect_cb_t connect_cb, ble_disconnect_cb_t disconnect_cb)
{
    s_scan_cb = scan_cb;
    s_connect_cb = connect_cb;
    s_disconnect_cb = disconnect_cb;

    ESP_LOGI(TAG, "BLE callbacks registered");
}

// ==================== 扫描相关函数 ====================

bool ble_start_scan(uint8_t scan_time)
{
    if (s_is_scanning)
    {
        ESP_LOGW(TAG, "Scanning already in progress");
        return false;
    }

    if (s_is_connected)
    {
        ESP_LOGW(TAG, "Please disconnect first before scanning");
        return false;
    }

    // 清空之前的扫描结果
    ble_clear_scan_results();

    // 设置扫描参数
    esp_err_t ret = esp_ble_gap_set_scan_params(&s_ble_scan_params);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Set scan params failed: 0x%x", ret);
        return false;
    }

    // 开始扫描
    ret = esp_ble_gap_start_scanning(scan_time > 0 ? scan_time : DEFAULT_SCAN_TIME);
    if (ret == ESP_OK)
    {
        s_is_scanning = true;
        ble_Scan_complate = false;
        ESP_LOGI(TAG, "BLE scanning started for %d seconds", scan_time);
        return true;
    }
    else
    {
        ESP_LOGE(TAG, "Failed to start scanning: 0x%x", ret);
        return false;
    }
}

void ble_stop_scan(void)
{
    if (s_is_scanning)
    {
        esp_ble_gap_stop_scanning();
        s_is_scanning = false;
        ESP_LOGI(TAG, "Scanning stopped");
    }
}

bool ble_is_scanning(void)
{
    return s_is_scanning;
}

int ble_get_scanned_devices(ble_device_info_t *devices, int max_count)
{
    if (devices == NULL || max_count <= 0)
    {
        return 0;
    }

    int count = 0;

    if (s_device_list_mutex != NULL)
    {
        xSemaphoreTake(s_device_list_mutex, portMAX_DELAY);
    }

    for (int i = 0; i < s_scanned_count && count < max_count; i++)
    {
        memcpy(&devices[count], &s_scanned_devices[i], sizeof(ble_device_info_t));
        count++;
    }

    if (s_device_list_mutex != NULL)
    {
        xSemaphoreGive(s_device_list_mutex);
    }

    return count;
}

void ble_clear_scan_results(void)
{
    if (s_device_list_mutex != NULL)
    {
        xSemaphoreTake(s_device_list_mutex, portMAX_DELAY);
    }

    memset(s_scanned_devices, 0, sizeof(s_scanned_devices));
    s_scanned_count = 0;

    if (s_device_list_mutex != NULL)
    {
        xSemaphoreGive(s_device_list_mutex);
    }

    ESP_LOGI(TAG, "Scan results cleared");
}

static void add_scanned_device(esp_ble_gap_cb_param_t *scan_result)
{
    uint8_t *adv_name = NULL;
    uint8_t adv_name_len = 0;

    // 解析设备名称
    adv_name = esp_ble_resolve_adv_data_by_type(scan_result->scan_rst.ble_adv,
                                                scan_result->scan_rst.adv_data_len +
                                                    scan_result->scan_rst.scan_rsp_len,
                                                ESP_BLE_AD_TYPE_NAME_CMPL,
                                                &adv_name_len);

    if (adv_name == NULL || adv_name_len == 0)
    {
        return; // 无名设备，忽略
    }

    if (s_device_list_mutex != NULL)
    {
        xSemaphoreTake(s_device_list_mutex, portMAX_DELAY);
    }

    // 检查是否已存在
    for (int i = 0; i < s_scanned_count; i++)
    {
        if (memcmp(s_scanned_devices[i].bda, scan_result->scan_rst.bda, ESP_BD_ADDR_LEN) == 0)
        {
            // 更新RSSI
            s_scanned_devices[i].rssi = scan_result->scan_rst.rssi;

            if (s_device_list_mutex != NULL)
            {
                xSemaphoreGive(s_device_list_mutex);
            }
            return;
        }
    }

    // 添加新设备
    if (s_scanned_count < MAX_SCAN_DEVICES)
    {
        ble_device_info_t *dev = &s_scanned_devices[s_scanned_count];
        memcpy(dev->bda, scan_result->scan_rst.bda, ESP_BD_ADDR_LEN);
        dev->addr_type = scan_result->scan_rst.ble_addr_type;
        dev->rssi = scan_result->scan_rst.rssi;
        dev->is_connected = false;

        // 复制名称
        int name_len = adv_name_len < (int)sizeof(dev->name) - 1 ? adv_name_len : sizeof(dev->name) - 1;
        memcpy(dev->name, adv_name, name_len);
        dev->name[name_len] = '\0';

        s_scanned_count++;

        ESP_LOGI(TAG, "Found device [%d]: %s, RSSI: %d",
                 s_scanned_count, dev->name, dev->rssi);
    }

    if (s_device_list_mutex != NULL)
    {
        xSemaphoreGive(s_device_list_mutex);
    }
}

static ble_device_info_t *find_device_by_address(uint8_t *bda)
{
    for (int i = 0; i < s_scanned_count; i++)
    {
        if (memcmp(s_scanned_devices[i].bda, bda, ESP_BD_ADDR_LEN) == 0)
        {
            return &s_scanned_devices[i];
        }
    }
    return NULL;
}

//==================== 连接相关函数 ====================

bool ble_connect_by_name(const char *device_name)
{
    if (s_is_connected)
    {
        ESP_LOGW(TAG, "Already connected to a device");
        return false;
    }

    if (s_gattc_profile.gattc_if == ESP_GATT_IF_NONE)
    {
        ESP_LOGE(TAG, "GATT interface not ready");
        return false;
    }

    // 在扫描结果中查找设备
    ble_device_info_t *target = NULL;

    if (s_device_list_mutex != NULL)
    {
        xSemaphoreTake(s_device_list_mutex, portMAX_DELAY);
    }

    for (int i = 0; i < s_scanned_count; i++)
    {
        if (strcmp(s_scanned_devices[i].name, device_name) == 0)
        {
            target = &s_scanned_devices[i];
            break;
        }
    }

    if (s_device_list_mutex != NULL)
    {
        xSemaphoreGive(s_device_list_mutex);
    }

    if (target == NULL)
    {
        ESP_LOGE(TAG, "Device '%s' not found in scan results", device_name);
        return false;
    }

    // 保存连接的目标名称
    strncpy(s_connected_name, device_name, sizeof(s_connected_name) - 1);
    s_connected_name[sizeof(s_connected_name) - 1] = '\0';

    // 创建连接参数
    esp_ble_gatt_creat_conn_params_t conn_params = {
        .remote_addr_type = target->addr_type,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .is_direct = true,
        .is_aux = false,
        .phy_mask = 0};
    memcpy(conn_params.remote_bda, target->bda, ESP_BD_ADDR_LEN);

    // 发起连接
    esp_err_t ret = esp_ble_gattc_enh_open(s_gattc_profile.gattc_if, &conn_params);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Connecting to device: %s", device_name);
        return true;
    }
    else
    {
        ESP_LOGE(TAG, "Failed to connect: 0x%x", ret);
        return false;
    }
}

bool ble_connect_by_address(uint8_t *bda, uint8_t addr_type)
{
    if (s_is_connected)
    {
        ESP_LOGW(TAG, "Already connected to a device");
        return false;
    }

    if (s_gattc_profile.gattc_if == ESP_GATT_IF_NONE)
    {
        ESP_LOGE(TAG, "GATT interface not ready");
        return false;
    }

    // 查找设备名称
    ble_device_info_t *dev = find_device_by_address(bda);
    if (dev)
    {
        strncpy(s_connected_name, dev->name, sizeof(s_connected_name) - 1);
    }
    else
    {
        strcpy(s_connected_name, "Unknown");
    }

    // 创建连接参数
    esp_ble_gatt_creat_conn_params_t conn_params = {
        .remote_addr_type = addr_type,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .is_direct = true,
        .is_aux = false,
        .phy_mask = 0};
    memcpy(conn_params.remote_bda, bda, ESP_BD_ADDR_LEN);

    esp_err_t ret = esp_ble_gattc_enh_open(s_gattc_profile.gattc_if, &conn_params);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Connecting to device at address: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));
        return true;
    }
    else
    {
        ESP_LOGE(TAG, "Failed to connect: 0x%x", ret);
        return false;
    }
}

void ble_disconnect(void)
{
    if (s_is_connected)
    {
        esp_ble_gattc_close(s_gattc_profile.gattc_if, s_gattc_profile.conn_id);
        ESP_LOGI(TAG, "Disconnecting...");
    }
    else
    {
        ESP_LOGW(TAG, "Not connected to any device");
    }
}

bool ble_is_connected(void)
{
    return s_is_connected;
}

const char *ble_get_connected_device_name(void)
{
    if (s_is_connected)
    {
        return s_connected_name;
    }
    return NULL;
}

bool ble_get_connected_device_addr(uint8_t *bda)
{
    if (s_is_connected && bda != NULL)
    {
        memcpy(bda, s_gattc_profile.remote_bda, ESP_BD_ADDR_LEN);
        return true;
    }
    return false;
}

// ==================== 数据发送函数 ====================

int ble_send_data(void *pdata, int len, uint32_t timeout)
{
    uint8_t sendBuf[1024] = {0};
    uint16_t nlen = 0;
    if (IsEnc)
    {
        app_enc_process_Encrypt(pdata, len, sendBuf, &nlen);

    }
    else
    {
        memcpy(sendBuf, pdata, len);
        nlen = len;
    }

    if (!s_is_connected)
    {
        ESP_LOGE(TAG, "Not connected to any device");
        return 0;
    }

    if (s_gattc_profile.write_char_handle == INVALID_HANDLE)
    {
        ESP_LOGE(TAG, "Write characteristic not found");
        return 0;
    }

    if (len > 500)
    {
        ESP_LOGE(TAG, "Data too long: %d > 500", len);
        return 0;
    }

    esp_err_t ret = esp_ble_gattc_write_char(s_gattc_profile.gattc_if,
                                             s_gattc_profile.conn_id,
                                             s_gattc_profile.write_char_handle,
                                             nlen,
                                             sendBuf,
                                             ESP_GATT_WRITE_TYPE_NO_RSP,
                                             ESP_GATT_AUTH_REQ_NONE);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Data sent: %d bytes", len);
        return 1;
    }
    else
    {
        ESP_LOGE(TAG, "Failed to send data: 0x%x", ret);
        return 0;
    }
}

// ==================== GAP回调 ====================
// 处理扫描结果、连接状态变化等事件
static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: // 扫描参数设置完成事件
        ESP_LOGD(TAG, "Scan params set complete");
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT: // 扫描开始完成事件
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(TAG, "Scan start failed: %x", param->scan_start_cmpl.status);
            s_is_scanning = false;
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:                                 // 扫描结果事件
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) // 发现设备事件
        {
            // 处理扫描结果,添加到扫描设备列表
            add_scanned_device(param);
            // xQueueSend(scan_result_queue, param, pdMS_TO_TICKS(250)); // 发送到队列中，等待处理
        }
        else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) // 扫描完成事件
        {
            s_is_scanning = false;
            ESP_LOGI(TAG, "Scan completed, found %d devices", s_scanned_count);

            // 调用扫描完成回调
            if (s_scan_cb != NULL)
            {
                s_scan_cb(s_scanned_devices, s_scanned_count);
            }
            ble_Scan_complate = true;
        }

        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_is_scanning = false;
        ESP_LOGI(TAG, "Scan stopped");
        break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT: // 连接参数更新事件
        ESP_LOGI(TAG, "Connection params updated: int %d, latency %d, timeout %d",
                 param->update_conn_params.conn_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;

    default:
        break;
    }
}

// ==================== GATTC回调 ====================
// 处理连接、服务发现、特征读写等事件
static void ble_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    // 如果是注册事件，保存gattc_if
    if (event == ESP_GATTC_REG_EVT)
    {
        if (param->reg.status == ESP_GATT_OK)
        {
            s_gattc_profile.gattc_if = gattc_if;
            ESP_LOGI(TAG, "GATT client registered, interface: %d", gattc_if);
        }
        else
        {
            ESP_LOGE(TAG, "GATT register failed: %d", param->reg.status);
        }
        return;
    }

    // 调用profile事件处理
    if (gattc_if == s_gattc_profile.gattc_if || gattc_if == ESP_GATT_IF_NONE)
    {
        gattc_profile_event_handler(event, gattc_if, param);
    }
}
static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(TAG, "Connected to " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(param->connect.remote_bda));
        s_gattc_profile.conn_id = param->connect.conn_id;
        memcpy(s_gattc_profile.remote_bda, param->connect.remote_bda, ESP_BD_ADDR_LEN);
        s_is_connected = true;

        // 更新扫描列表中的连接状态
        if (s_device_list_mutex != NULL)
        {
            xSemaphoreTake(s_device_list_mutex, portMAX_DELAY);
        }
        for (int i = 0; i < s_scanned_count; i++)
        {
            if (memcmp(s_scanned_devices[i].bda, param->connect.remote_bda, ESP_BD_ADDR_LEN) == 0)
            {
                s_scanned_devices[i].is_connected = true;
                break;
            }
        }
        if (s_device_list_mutex != NULL)
        {
            xSemaphoreGive(s_device_list_mutex);
        }

        // 调用连接成功回调
        if (s_connect_cb != NULL)
        {
            s_connect_cb(true, s_connected_name);
        }

        // 发送MTU请求
        esp_ble_gattc_send_mtu_req(gattc_if, param->connect.conn_id);
        break;

    case ESP_GATTC_OPEN_EVT: // 连接完成事件
        if (param->open.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG, "Open failed: %d", param->open.status);
        }
        break;

    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
        if (param->dis_srvc_cmpl.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG, "Service discover failed: %d", param->dis_srvc_cmpl.status);
            break;
        }
        ESP_LOGI(TAG, "Service discover complete");
        // 开始搜索服务
        esp_ble_gattc_search_service(gattc_if, param->dis_srvc_cmpl.conn_id, &s_remote_service_uuid);

        break;

    case ESP_GATTC_CFG_MTU_EVT:
        ESP_LOGI(TAG, "MTU exchange: %d", param->cfg_mtu.mtu);
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            param->search_res.srvc_id.uuid.uuid.uuid16 == REMOTE_SERVICE_UUID)
        {
            ESP_LOGI(TAG, "Service found");
            s_gattc_profile.service_start_handle = param->search_res.start_handle;
            s_gattc_profile.service_end_handle = param->search_res.end_handle;
            s_gattc_profile.service_found = true;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG, "Service search failed: %x", param->search_cmpl.status);
            break;
        }

        if (!s_gattc_profile.service_found)
        {
            ESP_LOGE(TAG, "Service 0xFF00 not found");
            break;
        }

        ESP_LOGI(TAG, "Service search complete, discovering characteristics");

        // 查找通知特征
        uint16_t count = 0;
        esp_gatt_status_t status;
        esp_gattc_char_elem_t *char_result = NULL;

        // 获取特征数量
        status = esp_ble_gattc_get_attr_count(gattc_if, s_gattc_profile.conn_id,
                                              ESP_GATT_DB_CHARACTERISTIC,
                                              s_gattc_profile.service_start_handle,
                                              s_gattc_profile.service_end_handle,
                                              INVALID_HANDLE, &count);

        if (status != ESP_GATT_OK || count == 0)
        {
            ESP_LOGE(TAG, "No characteristics found");
            break;
        }

        char_result = malloc(sizeof(esp_gattc_char_elem_t) * count);
        if (char_result == NULL)
        {
            ESP_LOGE(TAG, "Memory allocation failed");
            break;
        }

        // 查找通知特征
        status = esp_ble_gattc_get_char_by_uuid(gattc_if, s_gattc_profile.conn_id,
                                                s_gattc_profile.service_start_handle,
                                                s_gattc_profile.service_end_handle,
                                                s_notify_char_uuid,
                                                char_result, &count);

        if (status == ESP_GATT_OK && count > 0)
        {
            s_gattc_profile.notify_char_handle = char_result[0].char_handle; // 通知句柄赋值
            ESP_LOGI(TAG, "Notify characteristic found: 0x%04x", char_result[0].char_handle);

            // 注册通知
            esp_ble_gattc_register_for_notify(gattc_if, s_gattc_profile.remote_bda,
                                              char_result[0].char_handle);
        }

        // 查找写特征
        count = 1;
        status = esp_ble_gattc_get_char_by_uuid(gattc_if, s_gattc_profile.conn_id,
                                                s_gattc_profile.service_start_handle,
                                                s_gattc_profile.service_end_handle,
                                                s_write_char_uuid,
                                                char_result, &count);

        if (status == ESP_GATT_OK && count > 0)
        {

            s_gattc_profile.write_char_handle = char_result[0].char_handle; // 写句柄赋值
            ESP_LOGI(TAG, "Write characteristic found: 0x%04x", char_result[0].char_handle);
        }

        free(char_result);
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        if (param->reg_for_notify.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG, "Register notify failed: %d", param->reg_for_notify.status);
            break;
        }

        ESP_LOGI(TAG, "Register notify success");

        // 查找并配置CCCD
        uint16_t descr_count = 0;
        esp_gattc_descr_elem_t *descr_result = NULL;
        uint16_t notify_en = 1;

        status = esp_ble_gattc_get_attr_count(gattc_if, s_gattc_profile.conn_id,
                                              ESP_GATT_DB_DESCRIPTOR,
                                              s_gattc_profile.service_start_handle,
                                              s_gattc_profile.service_end_handle,
                                              s_gattc_profile.notify_char_handle,
                                              &descr_count);

        if (status != ESP_GATT_OK || descr_count == 0)
        {
            ESP_LOGE(TAG, "No descriptors found");
            break;
        }

        descr_result = malloc(sizeof(esp_gattc_descr_elem_t) * descr_count);
        if (descr_result == NULL)
        {
            ESP_LOGE(TAG, "Memory allocation failed");
            break;
        }

        status = esp_ble_gattc_get_descr_by_char_handle(gattc_if, s_gattc_profile.conn_id,
                                                        s_gattc_profile.notify_char_handle,
                                                        s_notify_descr_uuid,
                                                        descr_result, &descr_count);

        if (status == ESP_GATT_OK && descr_count > 0)
        {
            esp_ble_gattc_write_char_descr(gattc_if, s_gattc_profile.conn_id,
                                           descr_result[0].handle,
                                           sizeof(notify_en),
                                           (uint8_t *)&notify_en,
                                           ESP_GATT_WRITE_TYPE_RSP,
                                           ESP_GATT_AUTH_REQ_NONE);
        }

        free(descr_result);
        break;

    case ESP_GATTC_NOTIFY_EVT:
        // ESP_LOGI(TAG, "Notification received: %d bytes", param->notify.value_len);
        // esp_log_buffer_hex(TAG, param->notify.value, param->notify.value_len);

        ble_mtu_data_t recv;
        recv.data = malloc(param->notify.value_len);
        if (!recv.data)
        {
            ESP_LOGE(TAG, "recv.data = malloc(param->notify.value_len)");
            break;
        }
        recv.data_len = param->notify.value_len;
        memcpy(recv.data, param->notify.value, param->notify.value_len);
        if (recv.data_len < (param->cfg_mtu.mtu - 3))
        {
            recv.wait_time = 0;
        }
        else
        {
            recv.wait_time = pdMS_TO_TICKS(85);
        }
        if (!data_queue || xQueueSend(data_queue, &recv, pdMS_TO_TICKS(250)) != pdTRUE)
        {
            ESP_LOGE(TAG, "send data to data_queue failed");
            free(recv.data);
        }
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG, "Descriptor write failed: %x", param->write.status);
        }
        else
        {
            ESP_LOGI(TAG, "Descriptor write success");
        }
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG, "Characteristic write failed: %x", param->write.status);
        }
        else
        {
            ESP_LOGD(TAG, "Characteristic write success");
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(TAG, "Disconnected, reason: 0x%02x", param->disconnect.reason);
        s_is_connected = false;
        s_gattc_profile.service_found = false;
        s_gattc_profile.notify_char_handle = 0;
        s_gattc_profile.write_char_handle = 0;

        // 更新扫描列表中的连接状态
        if (s_device_list_mutex != NULL)
        {
            xSemaphoreTake(s_device_list_mutex, portMAX_DELAY);
        }
        for (int i = 0; i < s_scanned_count; i++)
        {
            s_scanned_devices[i].is_connected = false;
        }
        if (s_device_list_mutex != NULL)
        {
            xSemaphoreGive(s_device_list_mutex);
        }

        // 调用断开连接回调
        if (s_disconnect_cb != NULL)
        {
            s_disconnect_cb();
        }
        break;

    default:
        break;
    }
}
