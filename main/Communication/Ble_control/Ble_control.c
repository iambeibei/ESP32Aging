// ble_control.c
#include "Ble_control.h"
#include "task_config.h"
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
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
#include "app_mem.h"
#include "esp_attr.h"
#include "esp_coexist.h"

static const char *TAG = "BLE_CTRL";

// 配置参数
/*
 * 扫描结果列表容量（2026-10-26 由 50 上调为 128）：
 * 密集现场带完整名称的设备可能超过 50 台，达到上限后新设备被静默丢弃，
 * 目标设备若出现较晚就永远进不了列表（表现为 not found in scan results）。
 * 数组位于 EXT_RAM_BSS（PSRAM），128 项约 10KB，内部 RAM 不增加。
 */
#define MAX_SCAN_DEVICES 128
#define DEFAULT_SCAN_TIME 10
#define REMOTE_SERVICE_UUID 0xFF00
#define REMOTE_NOTIFY_CHAR_UUID 0xFF01
#define REMOTE_WRITE_CHAR_UUID 0xFF02
#define INVALID_HANDLE 0
#define PROFILE_NUM 1
#define PROFILE_A_APP_ID 0

/*
 * 连接参数偏好（2026-10-16，CONNECT_EVT 时由主控端主动提议）：
 * interval 12 × 1.25ms = 15ms（与 DUT 惯常请求值一致）；
 * supervision timeout 2000 × 10ms = 20s——实机日志显示 DUT 建链后会请求
 * timeout=400（4s），WiFi/BLE 射频共存抖动可能触发 rsn 0x08
 * （Connection Timeout）掉线。主控端提议宽松值可显著提高链路存活率，
 * 最终生效值以 ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT 日志为准。
 */
#define BLE_PREF_CONN_INTERVAL 12
#define BLE_PREF_SUPERVISION_TIMEOUT 2000

bool ble_Scan_complate; // 扫描完成标志

// 消息队列
// static QueueHandle_t scan_result_queue;  //扫描结果队列
static QueueHandle_t data_queue;         // 接收到的数据队列
static QueueHandle_t data_forword_queue; // 转发的数据队列

// BLE设备列表
/* GAP/GATTC 回调为任务上下文；应用扫描列表不供控制器 DMA/ISR 使用。 */
EXT_RAM_BSS_ATTR static ble_device_info_t s_scanned_devices[MAX_SCAN_DEVICES];
static int s_scanned_count = 0;
static atomic_bool s_is_scanning = false;
static atomic_bool s_is_connected = false;
/* 高 31 位为连接代次，最低位为加密状态；CAS 防止旧鉴权迟到启用新连接加密。 */
static atomic_uint s_session_state;
static atomic_bool s_session_closing = true;

static void ble_session_reset(void)
{
    unsigned state = atomic_load(&s_session_state);
    while (!atomic_compare_exchange_weak(&s_session_state, &state,
                                         (state & ~1u) + 2u)) { }
}

uint32_t ble_session_id(void)
{
    return atomic_load(&s_session_state) & ~1u;
}

bool ble_session_is_encrypted(void)
{
    return (atomic_load(&s_session_state) & 1u) != 0;
}

bool ble_session_is_current(uint32_t session_id)
{
    return atomic_load(&s_is_connected) && !atomic_load(&s_session_closing) &&
           session_id == ble_session_id();
}

bool ble_session_set_encrypted(uint32_t session_id, bool encrypted)
{
    unsigned state = atomic_load(&s_session_state);
    do {
        if (!ble_session_is_current(session_id) || (state & ~1u) != session_id) return false;
    } while (!atomic_compare_exchange_weak(&s_session_state, &state,
                                           encrypted ? (state | 1u) : (state & ~1u)));
    return true;
}
static char s_connected_name[20] = {0};
static SemaphoreHandle_t s_device_list_mutex = NULL; // 互斥锁保护设备列表
static SemaphoreHandle_t s_scan_control_mutex;
/* 列表锁保护目标快照；只在停扫确认后由任务更新。 */
static BleTarget s_scan_target;
static atomic_bool s_control_paused;

// 回调函数
static ble_scan_cb_t s_scan_cb = NULL;
static ble_connect_cb_t s_connect_cb = NULL;
static ble_disconnect_cb_t s_disconnect_cb = NULL;

/*
 * 连接尝试状态机（Bluedroid 回调与 CPU1 任务共享，使用原子标志）：
 *   s_connect_attempting    —— enh_open 已被接受，连接结果尚未交付
 *   s_connect_result_pending—— 结果尚未经 s_connect_cb 交付；实测建立失败时
 *                              DISCONNECT_EVT 与 OPEN_EVT(133) 会先后到达
 *                              （间隔约 20ms），用本标志保证只上报一次。
 * 目的：把「连接建立失败」与「已建立连接后掉线」分流——
 *   建立失败 → s_connect_cb(false)（老化任务据此重试）；
 *   已建立连接掉线 → s_disconnect_cb()（老化任务据此优雅中止，行为不变）。
 */
static atomic_bool s_connect_attempting = false;
static atomic_bool s_connect_result_pending = false;

/* 仅释放当前任务已经成功取得的互斥锁。 */
static void ble_control_unlock(SemaphoreHandle_t mutex)
{
    if (xSemaphoreGive(mutex) != pdTRUE) ESP_LOGE(TAG, "释放蓝牙控制锁失败");
}

/* 超时后保留在途操作，直到原回调到达，避免迟到结果确认下一次同类请求。 */
typedef enum {
    BLE_SCAN_OP_NONE, BLE_SCAN_OP_STOP, BLE_SCAN_OP_CLEAR,
    BLE_SCAN_OP_ADD, BLE_SCAN_OP_PARAMS, BLE_SCAN_OP_START
} BleScanOp;
#define BLE_SCAN_STEP_TIMEOUT_MS 5000
static atomic_int s_scan_op = BLE_SCAN_OP_NONE;
static atomic_int s_scan_op_result = ESP_OK;

static void ble_scan_op_complete(BleScanOp op, bool success)
{
    if (atomic_load(&s_scan_op) == (int)op) {
        atomic_store(&s_scan_op_result, success ? ESP_OK : ESP_FAIL);
        atomic_store(&s_scan_op, BLE_SCAN_OP_NONE);
    }
}

static esp_err_t ble_scan_op_wait(void)
{
    TickType_t start = xTaskGetTickCount();
    if (s_control_paused) return ESP_ERR_INVALID_STATE;
    while (atomic_load(&s_scan_op) != BLE_SCAN_OP_NONE) {
        if (s_control_paused) return ESP_ERR_INVALID_STATE;
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(BLE_SCAN_STEP_TIMEOUT_MS))
            return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return atomic_load(&s_scan_op_result);
}

static esp_err_t ble_scan_op_finish(esp_err_t submitted)
{
    if (submitted != ESP_OK) {
        atomic_store(&s_scan_op, BLE_SCAN_OP_NONE);
        return submitted;
    }
    return ble_scan_op_wait();
}

/* 调用方持扫描控制锁；回调不取此锁。 */
static esp_err_t ble_stop_scan_locked(void)
{
    if (atomic_load(&s_scan_op) != BLE_SCAN_OP_NONE) {
        esp_err_t ret = ble_scan_op_wait();
        if (ret != ESP_OK) return ret;
    }
    if (!s_is_scanning) return ESP_OK;
    atomic_store(&s_scan_op, BLE_SCAN_OP_STOP);
    return ble_scan_op_finish(esp_ble_gap_stop_scanning());
}
/*
 * GATT 设置阶段是否已得出结论：连接建立后依次进行服务发现、notify 特征
 * 注册与 CCCD 写入；本标志在 CCCD 写结果到达（ESP_GATTC_WRITE_DESCR_EVT）
 * 或降级结论（描述符缺失/特征缺失/断开）时置位，下一次 enh_open 受理时清除。
 * Bluedroid 回调写、CPU1 老化任务经 ble_gatt_setup_done() 轮询读，
 * 用于在建链后对端停止响应时快速失败重试，避免傻等固定延时。
 */
static volatile bool s_gatt_setup_done = false;

/*
 * Wi-Fi/BLE 射频共存偏好（2026-10-26）：
 * 扫描与建链阶段临时提升蓝牙优先级（ESP_COEX_PREFER_BT），降低密集 BLE
 * 环境下「Wi-Fi 抢占射频 → 广播漏收 / CONNECT_IND 丢失（HCI rsn 0x3E）」；
 * GATT 设置结论、建链失败、断开、停止扫描或老化中止时恢复 BALANCE，
 * 避免长期压缩 Wi-Fi 上行。s_coex_bt_boost 保证重复调用幂等。
 */
static bool s_coex_bt_boost = false;

static void ble_coex_prefer_bt(bool enable)
{
    if (s_coex_bt_boost == enable)
    {
        return;
    }

    esp_err_t err = esp_coex_preference_set(enable ? ESP_COEX_PREFER_BT : ESP_COEX_PREFER_BALANCE);
    if (err != ESP_OK)
    {
        /* 共存模块未就绪等情形：保留旧状态，下次调用重试。 */
        ESP_LOGW(TAG, "Coex preference %s failed: %s",
                 enable ? "PREFER_BT" : "BALANCE", esp_err_to_name(err));
        return;
    }

    s_coex_bt_boost = enable;
    ESP_LOGI(TAG, "Coex preference: %s", enable ? "PREFER_BT" : "BALANCE");
}

/* GATT 设置阶段得出结论：交付老化任务轮询，同时结束扫描/建链期间的蓝牙优先。 */
static void ble_gatt_setup_conclude(void)
{
    s_gatt_setup_done = true;
    ble_coex_prefer_bt(false);
}

// GATT profile结构
typedef struct
{
    esp_gattc_cb_t gattc_cb;
    uint16_t gattc_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t mtu;
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
    .mtu = 23, /* ATT 默认 MTU，协商成功后更新。 */
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

/*
 * 扫描参数（2026-10-26）：scan_window 由 0x30 提升到与 scan_interval 相等，
 * 占空比 48/80(60%) → 80/80(100%)。现场 BLE 设备密集，广播信道（37/38/39）
 * 碰撞会让目标设备在多数扫描轮次完全扫不到；连续监听可显著降低漏包。
 * 代价：单轮 20s 扫描期间 BLE 持续占用射频，Wi-Fi/Mesh 吞吐略降（可接受）。
 *
 * 约束：scan_type 必须保持 ACTIVE、scan_duplicate 必须保持 DISABLE。
 * 20 字符的 PN 名称放不进 31 字节广播包，只能从扫描响应（SCAN_RSP）取得，
 * 被动扫描永远拿不到名称；关闭重复过滤才能让同一设备的每次广播都触发
 * SCAN_REQ，在密集环境下反复重试获取名称。
 */
static esp_ble_scan_params_t s_ble_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x50,
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

EXT_RAM_BSS_ATTR static uint8_t a_data[BUFFER_MAX];

void app_ble_recv_task(void *arg)
{
    uint16_t datalen = 0;
    uint32_t wait_time = portMAX_DELAY;
    uint32_t session_id = 0;
    while (1)
    {
        ble_mtu_data_t mtu_recv;
        if (xQueueReceive(data_queue, &mtu_recv, wait_time) == pdTRUE) /* 获取队列 */
        {
            if (!ble_session_is_current(mtu_recv.session_id)) {
                free(mtu_recv.data);
                if (!ble_session_is_current(session_id)) {
                    datalen = 0;
                    wait_time = portMAX_DELAY;
                }
                continue;
            }
            if (session_id != mtu_recv.session_id) datalen = 0;
            session_id = mtu_recv.session_id;
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
            if (datalen == 0 || !ble_session_is_current(session_id)) {
                datalen = 0;
                wait_time = portMAX_DELAY;
                continue;
            }
            ESP_LOGI(TAG, "ble a recv len: %d", datalen);
            ble_data_t ble_data = {0};
            ble_data.data = app_malloc_prefer_psram(datalen);
            if (ble_data.data)
            {
                ble_data.data_len = datalen;
                ble_data.profile_id = PROFILE_A_APP_ID;
                ble_data.session_id = session_id;
                // memcpy(ble_data.remote, gl_profile_tab[PROFILE_A_APP_ID].remote_bda, 6);
                memcpy(ble_data.data, a_data, datalen);
                if (!data_forword_queue || xQueueSend(data_forword_queue, &ble_data, pdMS_TO_TICKS(250)) != pdTRUE)
                {
                    ESP_LOGW(TAG, "蓝牙业务接收队列已满或未初始化，丢弃 %u 字节", (unsigned)datalen);
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
    uint32_t session_id;
    return app_ble_recv_data_for_session(data, max_len, wait_time, &session_id);
}

int app_ble_recv_data_for_session(uint8_t *data, int max_len, uint32_t wait_time,
                                  uint32_t *session_id)
{
    ble_data_t ble_data = {0};
    if (!data || max_len <= 0 || !session_id || !data_forword_queue ||
        xQueueReceive(data_forword_queue, &ble_data, wait_time) != pdTRUE)
    {
        return -1;
    }

    memset(data, 0, max_len); // 清空数据缓冲区
    // esp_log_buffer_hex(GATTC_TAG, ble_data.data, ble_data.data_len);
    int copy_len = -1;
    uint8_t id = ble_data.profile_id;
    if (id < PROFILE_NUM && ble_session_is_current(ble_data.session_id) &&
        ble_data.data_len <= max_len)
    {
        copy_len = ble_data.data_len;
        memcpy(data, ble_data.data, copy_len);
        *session_id = ble_data.session_id;
    }
    else
    {
        ESP_LOGW(TAG, "丢弃蓝牙接收包: profile=%u, bytes=%u, session=%lu, current=%lu",
                 (unsigned)id, (unsigned)ble_data.data_len,
                 (unsigned long)ble_data.session_id, (unsigned long)ble_session_id());
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
    if (s_scan_control_mutex == NULL) s_scan_control_mutex = xSemaphoreCreateMutex();
    if (s_device_list_mutex == NULL || s_scan_control_mutex == NULL) return ESP_ERR_NO_MEM;

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
    const BleTarget target = {0};
    return ble_start_target_scan(&target, scan_time) == ESP_OK;
}

esp_err_t ble_start_target_scan(const BleTarget *target, uint8_t scan_time)
{
    if (target == NULL || target->addr_type > 1) return ESP_ERR_INVALID_ARG;
    if (!IsInited || s_scan_control_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_scan_control_mutex, pdMS_TO_TICKS(BLE_SCAN_STEP_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    /* GATT 注册异步完成，初始化返回不代表接口已就绪。 */
    if (s_control_paused || s_is_connected || s_connect_attempting || s_gattc_profile.gattc_if == ESP_GATT_IF_NONE)
        goto done;
    ret = ble_stop_scan_locked();
    if (ret != ESP_OK) goto done;
    ble_clear_scan_results();

    atomic_store(&s_scan_op, BLE_SCAN_OP_CLEAR);
    ret = ble_scan_op_finish(esp_ble_gap_clear_whitelist());
    if (ret != ESP_OK) goto done;
    if (target->use_mac) {
        atomic_store(&s_scan_op, BLE_SCAN_OP_ADD);
        /* SDK 参数非 const，使用本地快照，调用不会改变目标地址。 */
        esp_bd_addr_t bda;
        memcpy(bda, target->bda, sizeof(bda));
        ret = ble_scan_op_finish(esp_ble_gap_update_whitelist(true, bda,
            target->addr_type == BLE_ADDR_TYPE_PUBLIC ? BLE_WL_ADDR_TYPE_PUBLIC : BLE_WL_ADDR_TYPE_RANDOM));
        if (ret != ESP_OK) goto done;
    }
    s_ble_scan_params.scan_filter_policy = target->use_mac ?
        BLE_SCAN_FILTER_ALLOW_ONLY_WLST : BLE_SCAN_FILTER_ALLOW_ALL;
    atomic_store(&s_scan_op, BLE_SCAN_OP_PARAMS);
    ret = ble_scan_op_finish(esp_ble_gap_set_scan_params(&s_ble_scan_params));
    if (ret != ESP_OK) goto done;
    if (xSemaphoreTake(s_device_list_mutex, pdMS_TO_TICKS(BLE_SCAN_STEP_TIMEOUT_MS)) != pdTRUE) {
        ret = ESP_ERR_TIMEOUT;
        goto done;
    }
    s_scan_target = *target;
    xSemaphoreGive(s_device_list_mutex);
    ble_Scan_complate = false;
    ble_coex_prefer_bt(true);
    atomic_store(&s_scan_op, BLE_SCAN_OP_START);
    ret = ble_scan_op_finish(esp_ble_gap_start_scanning(scan_time ? scan_time : DEFAULT_SCAN_TIME));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "蓝牙扫描已启动，时长=%u 秒，模式=%s", (unsigned)(scan_time ? scan_time : DEFAULT_SCAN_TIME),
                 target->use_mac ? "MAC白名单" : "名称");
        if (target->use_mac)
            ESP_LOGI(TAG, "扫描目标 MAC: " ESP_BD_ADDR_STR ", 地址类型: %u",
                     ESP_BD_ADDR_HEX(target->bda), (unsigned)target->addr_type);
    }
done:
    if (ret != ESP_OK) {
        ble_coex_prefer_bt(false);
        ESP_LOGE(TAG, "蓝牙扫描配置失败，不启动后续扫描: %s", esp_err_to_name(ret));
    }
    ble_control_unlock(s_scan_control_mutex);
    return ret;
}

esp_err_t ble_stop_scan_wait(void)
{
    if (!IsInited || s_scan_control_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_scan_control_mutex, pdMS_TO_TICKS(BLE_SCAN_STEP_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t ret = ble_stop_scan_locked();
    ble_coex_prefer_bt(false);
    ble_control_unlock(s_scan_control_mutex);
    return ret;
}

esp_err_t ble_control_pause(void)
{
    s_control_paused = true;
    if (s_scan_control_mutex == NULL) return ESP_OK;
    if (xSemaphoreTake(s_scan_control_mutex, pdMS_TO_TICKS(BLE_SCAN_STEP_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    /* 若无在途操作，可以非阻塞提交停扫；后续会话仍须等待实际回调。 */
    if (s_is_scanning && atomic_load(&s_scan_op) == BLE_SCAN_OP_NONE) {
        atomic_store(&s_scan_op, BLE_SCAN_OP_STOP);
        esp_err_t ret = esp_ble_gap_stop_scanning();
        if (ret != ESP_OK) {
            atomic_store(&s_scan_op, BLE_SCAN_OP_NONE);
            ESP_LOGW(TAG, "暂停蓝牙时停扫提交失败: %s", esp_err_to_name(ret));
        }
    }
    ble_coex_prefer_bt(false);
    ble_control_unlock(s_scan_control_mutex);
    return ESP_OK;
}

esp_err_t ble_control_resume(void)
{
    if (s_connect_attempting) return ESP_ERR_INVALID_STATE;
    s_control_paused = false;
    return ESP_OK;
}

void ble_stop_scan(void)
{
    esp_err_t ret = ble_stop_scan_wait();
    if (ret != ESP_OK) ESP_LOGW(TAG, "停止蓝牙扫描未确认: %s", esp_err_to_name(ret));
}

/* 供上层在老化中止/检查轮次边界兜底恢复 Wi-Fi/BLE 平衡，避免长期压缩 Wi-Fi。 */
void ble_restore_coex_balance(void)
{
    ble_coex_prefer_bt(false);
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

bool ble_scanned_has_name(const char *name)
{
    if (name == NULL || name[0] == '\0')
    {
        return false;
    }

    bool found = false;

    if (s_device_list_mutex != NULL)
    {
        xSemaphoreTake(s_device_list_mutex, portMAX_DELAY);
    }

    for (int i = 0; i < s_scanned_count; i++)
    {
        if (strcmp(s_scanned_devices[i].name, name) == 0)
        {
            found = true;
            break;
        }
    }

    if (s_device_list_mutex != NULL)
    {
        xSemaphoreGive(s_device_list_mutex);
    }

    return found;
}

bool ble_scanned_has_address(const uint8_t *bda, uint8_t addr_type)
{
    if (bda == NULL || s_device_list_mutex == NULL) return false;
    if (xSemaphoreTake(s_device_list_mutex, pdMS_TO_TICKS(BLE_SCAN_STEP_TIMEOUT_MS)) != pdTRUE)
        return false;
    bool found = false;
    for (int i = 0; i < s_scanned_count; ++i) {
        if (s_scanned_devices[i].addr_type == addr_type &&
            memcmp(s_scanned_devices[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_device_list_mutex);
    return found;
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
    if (s_control_paused || !s_is_scanning || s_device_list_mutex == NULL) return;
    if (xSemaphoreTake(s_device_list_mutex, portMAX_DELAY) != pdTRUE) return;
    /* 控制器白名单之外再核对目标，避免切换前排队的旧报告进入新列表。 */
    if (s_scan_target.use_mac &&
        (scan_result->scan_rst.ble_addr_type != s_scan_target.addr_type ||
         memcmp(scan_result->scan_rst.bda, s_scan_target.bda, ESP_BD_ADDR_LEN) != 0)) {
        xSemaphoreGive(s_device_list_mutex);
        return;
    }
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
        if (!s_scan_target.use_mac) {
            xSemaphoreGive(s_device_list_mutex);
            return; // 名称模式仍忽略无名称设备
        }
        adv_name = (uint8_t *)"未命名";
        adv_name_len = strlen((char *)adv_name);
    }

    // 检查是否已存在
    for (int i = 0; i < s_scanned_count; i++)
    {
        if (s_scanned_devices[i].addr_type == scan_result->scan_rst.ble_addr_type &&
            memcmp(s_scanned_devices[i].bda, scan_result->scan_rst.bda, ESP_BD_ADDR_LEN) == 0)
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

        ESP_LOGI(TAG, "Found device [%d]: %s, MAC: " ESP_BD_ADDR_STR ", 地址类型: %u, RSSI: %d",
                 s_scanned_count, dev->name, ESP_BD_ADDR_HEX(dev->bda),
                 (unsigned)dev->addr_type, dev->rssi);
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

static bool ble_connect_by_name_locked(const char *device_name)
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

    /* 建链窗口同样需要蓝牙优先：Wi-Fi 抢占射频会让 CONNECT_IND 丢失，
     * 对端不回应即以 HCI rsn 0x3E 建链失败。 */
    ble_coex_prefer_bt(true);

    // 发起连接
    /* 先发布在途状态，避免快速回调早于 API 返回时被漏掉。 */
    s_connect_attempting = true;
    s_connect_result_pending = true;
    s_gatt_setup_done = false;
    ble_session_reset();
    esp_err_t ret = esp_ble_gattc_enh_open(s_gattc_profile.gattc_if, &conn_params);

    if (ret == ESP_OK)
    {
        /* 标记连接尝试在途：结果由 CONNECT_EVT/OPEN_EVT/DISCONNECT_EVT
         * 经 notify_connect_result() 一次性交付（成功或建立失败）。 */
        ESP_LOGI(TAG, "Connecting to device: %s", device_name);
        return true;
    }
    else
    {
        s_connect_attempting = false;
        s_connect_result_pending = false;
        ESP_LOGE(TAG, "Failed to connect: 0x%x", ret);
        /* 请求未被受理：立即恢复平衡，不留蓝牙优先。 */
        ble_coex_prefer_bt(false);
        return false;
    }
}

static bool ble_connect_by_address_locked(uint8_t *bda, uint8_t addr_type)
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

    /* 与按名称连接一致：建链窗口提升蓝牙优先，失败/断开时由状态机恢复。 */
    ble_coex_prefer_bt(true);

    /* 先发布在途状态，避免快速回调早于 API 返回时被漏掉。 */
    s_connect_attempting = true;
    s_connect_result_pending = true;
    s_gatt_setup_done = false;
    ble_session_reset();
    esp_err_t ret = esp_ble_gattc_enh_open(s_gattc_profile.gattc_if, &conn_params);

    if (ret == ESP_OK)
    {
        /* 标记连接尝试在途：结果由 CONNECT_EVT/OPEN_EVT/DISCONNECT_EVT
         * 经 notify_connect_result() 一次性交付（成功或建立失败）。 */
        ESP_LOGI(TAG, "Connecting to device at address: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));
        return true;
    }
    else
    {
        s_connect_attempting = false;
        s_connect_result_pending = false;
        ESP_LOGE(TAG, "Failed to connect: 0x%x", ret);
        /* 请求未被受理：立即恢复平衡，不留蓝牙优先。 */
        ble_coex_prefer_bt(false);
        return false;
    }
}

/* 扫描配置和连接提交共用控制锁，回调不等待控制锁。 */
static bool ble_connect_control_enter(void)
{
    if (!IsInited || s_scan_control_mutex == NULL ||
        xSemaphoreTake(s_scan_control_mutex, pdMS_TO_TICKS(BLE_SCAN_STEP_TIMEOUT_MS)) != pdTRUE)
        return false;
    if (s_control_paused || s_is_scanning || s_connect_attempting || s_is_connected ||
        atomic_load(&s_scan_op) != BLE_SCAN_OP_NONE) {
        ble_control_unlock(s_scan_control_mutex);
        return false;
    }
    return true;
}

bool ble_connect_by_name(const char *device_name)
{
    if (device_name == NULL || device_name[0] == '\0' || !ble_connect_control_enter()) return false;
    bool accepted = ble_connect_by_name_locked(device_name);
    ble_control_unlock(s_scan_control_mutex);
    return accepted;
}

bool ble_connect_by_address(uint8_t *bda, uint8_t addr_type)
{
    if (bda == NULL || addr_type > 1 || !ble_connect_control_enter()) return false;
    bool accepted = ble_connect_by_address_locked(bda, addr_type);
    ble_control_unlock(s_scan_control_mutex);
    return accepted;
}

void ble_disconnect(void)
{
    /* 立即使鉴权结果和队列旧包失效，不等异步断开回调。 */
    atomic_store(&s_session_closing, true);
    ble_session_reset();
    if (s_is_connected)
    {
        esp_err_t ret = esp_ble_gattc_close(s_gattc_profile.gattc_if, s_gattc_profile.conn_id);
        if (ret != ESP_OK) ESP_LOGE(TAG, "提交蓝牙断开失败: %s", esp_err_to_name(ret));
        else ESP_LOGI(TAG, "Disconnecting...");
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

/*
 * GATT 设置阶段是否已得出结论（服务发现 + notify 注册 + CCCD 写确认，
 * 或降级结论/断开）。连接建立成功后由老化任务轮询（带超时）：
 * 对端若建链后停止响应（CCCD 写永无回应），可快速失败重试，
 * 不再傻等固定延时后才在通信检查中暴露。
 * 仅 Bluedroid 回调写、任务上下文读；无锁，读取为原子字长访问。
 */
bool ble_gatt_setup_done(void)
{
    return s_gatt_setup_done;
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

static int ble_send_for_session(uint32_t session_id, const uint8_t *data, int len,
                                bool encrypt)
{
    if (!data || len <= 0 || len > 500) {
        ESP_LOGE(TAG, "蓝牙发送参数无效: bytes=%d", len);
        return 0;
    }
    if (!ble_session_is_current(session_id)) {
        ESP_LOGW(TAG, "蓝牙连接已失效，取消发送: session=%lu", (unsigned long)session_id);
        return 0;
    }
    if (s_gattc_profile.write_char_handle == INVALID_HANDLE) {
        ESP_LOGE(TAG, "Write characteristic not found");
        return 0;
    }

    uint8_t sendBuf[1024] = {0};
    uint16_t nlen = 0;
    if (encrypt)
    {
        if (app_enc_process_Encrypt(data, len, sendBuf, &nlen) != ESP_OK) {
            ESP_LOGE(TAG, "蓝牙报文加密失败");
            return 0;
        }
    }
    else
    {
        memcpy(sendBuf, data, len);
        nlen = len;
    }

    if (nlen == 0 || nlen > 500 || !ble_session_is_current(session_id)) {
        ESP_LOGW(TAG, "取消蓝牙发送: wire_bytes=%u, session=%lu, current=%lu",
                 (unsigned)nlen, (unsigned long)session_id, (unsigned long)ble_session_id());
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
        ESP_LOGI(TAG, "蓝牙发送已受理: session=%lu, encrypted=%u, plain_bytes=%d, wire_bytes=%u",
                 (unsigned long)session_id, (unsigned)encrypt, len, (unsigned)nlen);
        /* 仅预览前 32 字节，避免鉴权大包和周期采样日志挤占蓝牙任务。 */
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, sendBuf, nlen < 32 ? nlen : 32, ESP_LOG_INFO);
        return 1;
    }
    else
    {
        ESP_LOGE(TAG, "Failed to send data: 0x%x", ret);
        return 0;
    }
}

int ble_send_data(void *pdata, int len, uint32_t timeout)
{
    (void)timeout; /* SDK 异步提交，不在此等待业务回复。 */
    unsigned state = atomic_load(&s_session_state);
    return ble_send_for_session(state & ~1u, pdata, len, (state & 1u) != 0);
}

int ble_send_auth_data(uint32_t session_id, const uint8_t *data, int len)
{
    return ble_send_for_session(session_id, data, len, false);
}

// ==================== GAP回调 ====================
// 处理扫描结果、连接状态变化等事件
static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: // 扫描参数设置完成事件
        ble_scan_op_complete(BLE_SCAN_OP_PARAMS, param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS);
        ESP_LOGD(TAG, "Scan params set complete");
        break;

    case ESP_GAP_BLE_UPDATE_WHITELIST_COMPLETE_EVT:
        if (param->update_whitelist_cmpl.wl_operation == ESP_BLE_WHITELIST_CLEAR)
            ble_scan_op_complete(BLE_SCAN_OP_CLEAR, param->update_whitelist_cmpl.status == ESP_BT_STATUS_SUCCESS);
        else if (param->update_whitelist_cmpl.wl_operation == ESP_BLE_WHITELIST_ADD)
            ble_scan_op_complete(BLE_SCAN_OP_ADD, param->update_whitelist_cmpl.status == ESP_BT_STATUS_SUCCESS);
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT: // 扫描开始完成事件
        s_is_scanning = param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS;
        ble_scan_op_complete(BLE_SCAN_OP_START, s_is_scanning);
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
        if (param->scan_stop_cmpl.status == ESP_BT_STATUS_SUCCESS) s_is_scanning = false;
        ble_scan_op_complete(BLE_SCAN_OP_STOP, param->scan_stop_cmpl.status == ESP_BT_STATUS_SUCCESS);
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
/*
 * 交付一次连接尝试结果（成功或失败）。DISCONNECT_EVT 与 OPEN_EVT 双到达时，
 * 首个事件即清 pending 并交付，后续事件自然被吞，保证 once 语义。
 * 仅在 Bluedroid 回调上下文调用：只读/写 volatile 标志并调用已注册回调，
 * 不碰存储、MQTT 与任务（符合 .ai/coding-standard.md 回调约束）。
 */
static void notify_connect_result(bool success)
{
    if (!s_connect_result_pending)
    {
        return; /* 迟到/重复事件：本次尝试的结果已交付过 */
    }
    s_connect_result_pending = false;
    s_connect_attempting = false;
    if (s_connect_cb != NULL)
    {
        s_connect_cb(success, s_connected_name);
    }
}

static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(TAG, "Connected to " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(param->connect.remote_bda));
        s_gattc_profile.conn_id = param->connect.conn_id;
        s_gattc_profile.mtu = 23;
        memcpy(s_gattc_profile.remote_bda, param->connect.remote_bda, ESP_BD_ADDR_LEN);
        ble_session_reset();
        atomic_store(&s_session_closing, false);
        s_is_connected = true;
        ESP_LOGI(TAG, "蓝牙新连接默认明文: session=%lu", (unsigned long)ble_session_id());

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

        // 连接建立成功：经 notify_connect_result 一次性交付结果
        notify_connect_result(true);
        s_gatt_setup_done = false; // 新的 GATT 设置阶段开始

        // 发送MTU请求
        esp_ble_gattc_send_mtu_req(gattc_if, param->connect.conn_id);

        /*
         * 主动提议宽松的连接参数：DUT 惯于请求 supervision timeout=400（4s），
         * WiFi/BLE 射频共存下一有抖动即 rsn 0x08 掉线（2026-10-16 实机日志）。
         * 主控端提议 timeout=BLE_PREF_SUPERVISION_TIMEOUT（20s），对端通常接受；
         * 被拒绝或对端后续再请求紧参数时，以 UPDATE_CONN_PARAMS_EVT 日志为准。
         */
        esp_ble_conn_update_params_t conn_params = {0};
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(conn_params.bda));
        conn_params.min_int = BLE_PREF_CONN_INTERVAL;
        conn_params.max_int = BLE_PREF_CONN_INTERVAL;
        conn_params.latency = 0;
        conn_params.timeout = BLE_PREF_SUPERVISION_TIMEOUT;
        esp_err_t conn_ret = esp_ble_gap_update_conn_params(&conn_params);
        if (conn_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Conn param update request failed: %s", esp_err_to_name(conn_ret));
        }
        break;

    case ESP_GATTC_OPEN_EVT: // 连接完成事件
        if (param->open.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG, "Open failed: %d", param->open.status);
            /* 建立失败：结束蓝牙优先，再经 connect 回调上报供老化任务重试 */
            ble_coex_prefer_bt(false);
            notify_connect_result(false);
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
        if (param->cfg_mtu.status == ESP_GATT_OK &&
            param->cfg_mtu.conn_id == s_gattc_profile.conn_id && param->cfg_mtu.mtu >= 23)
            s_gattc_profile.mtu = param->cfg_mtu.mtu;
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

        char_result = app_calloc_prefer_psram(count, sizeof(esp_gattc_char_elem_t));
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
        else
        {
            /* 无通知特征：notify 注册不会发生，GATT 设置阶段以此降级结论收尾，
             * 避免老化任务的设置等待空转到超时。 */
            ESP_LOGE(TAG, "Notify characteristic 0xFF01 not found");
            ble_gatt_setup_conclude();
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
        else
        {
            ESP_LOGE(TAG, "Write characteristic 0xFF02 not found");
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
            ble_gatt_setup_conclude(); /* 降级结论：不会再有 CCCD 写结果 */
            break;
        }

        descr_result = app_calloc_prefer_psram(descr_count, sizeof(esp_gattc_descr_elem_t));
        if (descr_result == NULL)
        {
            ESP_LOGE(TAG, "Memory allocation failed");
            ble_gatt_setup_conclude(); /* 降级结论：不会再有 CCCD 写结果 */
            break;
        }

        status = esp_ble_gattc_get_descr_by_char_handle(gattc_if, s_gattc_profile.conn_id,
                                                        s_gattc_profile.notify_char_handle,
                                                        s_notify_descr_uuid,
                                                        descr_result, &descr_count);

        if (status == ESP_GATT_OK && descr_count > 0)
        {
            /* CCCD 写入发起；结果由 ESP_GATTC_WRITE_DESCR_EVT 交付 */
            esp_ble_gattc_write_char_descr(gattc_if, s_gattc_profile.conn_id,
                                           descr_result[0].handle,
                                           sizeof(notify_en),
                                           (uint8_t *)&notify_en,
                                           ESP_GATT_WRITE_TYPE_RSP,
                                           ESP_GATT_AUTH_REQ_NONE);
        }
        else
        {
            /* 描述符查找失败：不会再有 CCCD 写结果，交付降级结论 */
            ESP_LOGE(TAG, "CCCD descriptor 0x2902 not found");
            ble_gatt_setup_conclude();
        }

        free(descr_result);
        break;

    case ESP_GATTC_NOTIFY_EVT:
        if (!ble_session_is_current(ble_session_id()) ||
            param->notify.conn_id != s_gattc_profile.conn_id ||
            memcmp(param->notify.remote_bda, s_gattc_profile.remote_bda, ESP_BD_ADDR_LEN) != 0 ||
            param->notify.handle != s_gattc_profile.notify_char_handle || param->notify.value_len == 0) {
            ESP_LOGW(TAG, "忽略非当前连接或非当前通知特征的蓝牙数据");
            break;
        }
        ESP_LOGI(TAG, "蓝牙原始接收: session=%lu, handle=0x%04x, bytes=%u",
                 (unsigned long)ble_session_id(), param->notify.handle, param->notify.value_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, param->notify.value,
                                 param->notify.value_len < 32 ? param->notify.value_len : 32,
                                 ESP_LOG_INFO);

        ble_mtu_data_t recv = {0};
        recv.session_id = ble_session_id();
        recv.data = app_malloc_prefer_psram(param->notify.value_len);
        if (!recv.data)
        {
            ESP_LOGE(TAG, "recv.data = malloc(param->notify.value_len)");
            break;
        }
        recv.data_len = param->notify.value_len;
        memcpy(recv.data, param->notify.value, param->notify.value_len);
        /* NOTIFY 参数不能读 cfg_mtu 联合体成员，使用本连接已协商的 MTU。 */
        recv.wait_time = recv.data_len < s_gattc_profile.mtu - 3 ? 0 : pdMS_TO_TICKS(85);
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
        /* CCCD 写结果到达：GATT 设置阶段得出结论（成败均交付，由老化任务
         * 后续通信检查判定 notify 通道是否真正可用）。 */
        ble_gatt_setup_conclude();
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
    {
        ESP_LOGI(TAG, "Disconnected, reason: 0x%02x", param->disconnect.reason);
        /* 建链中断或已建立连接掉线：恢复 Wi-Fi/BLE 平衡（幂等）。 */
        ble_coex_prefer_bt(false);
        /* 断开前的连接态判定：区分「建立失败」与「已建立连接后掉线」 */
        bool was_established = s_is_connected;
        bool establishing = s_connect_attempting;
        s_is_connected = false;
        ble_session_reset();
        s_gattc_profile.conn_id = 0; /* 清理陈旧 conn_id，避免下次误用 */
        s_gattc_profile.service_found = false;
        s_gattc_profile.notify_char_handle = 0;
        s_gattc_profile.write_char_handle = 0;
        s_gatt_setup_done = false; /* 设置阶段随连接一同失效 */

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

        if (establishing)
        {
            /*
             * 连接建立过程中断开（如 HCI rsn 0x3E 建链失败）：属建立失败，
             * 经 connect 回调上报供老化任务重试；不触发断开回调，避免被误判为
             * 「异常断开」而一次失败即中止老化（随后到达的 OPEN_EVT 133
             * 会被 notify_connect_result 的 once 语义吞掉，不会二次上报）。
             */
            ESP_LOGW(TAG, "Disconnected during connection establishment; treating as open failure");
            notify_connect_result(false);
        }
        else if (was_established)
        {
            // 已建立连接后掉线：调用断开连接回调（老化任务据此优雅中止，行为不变）
            if (s_disconnect_cb != NULL)
            {
                s_disconnect_cb();
            }
        }
        else
        {
            /* 未连接且无在途尝试的迟到/陈旧事件：仅记录，不误触发任何回调 */
            ESP_LOGW(TAG, "Stale disconnect event ignored: no active connection or attempt");
        }
        break;
    }

    default:
        break;
    }
}
