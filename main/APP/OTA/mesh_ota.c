/**
 * @file mesh_ota.c
 * @brief Mesh-Lite 原生 OTA 分发适配层实现
 *
 * 对外接口与设计说明见 mesh_ota.h；总体架构：
 *
 *   mqtt_rx ──通知──▶ cmd_task ──子节点: 错峰 + update.json + transmit_file_start
 *                        │         ──根节点: pause → 唤醒 dl_task → restart
 *                        ▼
 *                  Mesh-Lite 库（闭源）：逐级请求 / LAN OTA 传输 / OTA 事件
 *                        │
 *        ┌───────────────┼────────────────┐
 *        ▼               ▼                ▼
 *  provide_file_cb   get_file_cb    get_file_done_cb
 *  （按版本匹配分区   （惰性 begin +    （end + set_boot +
 *    读出供数, 缓存）   write_with_offset） 置 PENDING_RESTART）
 *
 * 关键不变量：
 *   - extern_url_ota_cb 为禁用桩：子节点 mesh 内拿不到目标版本即失败，
 *     由 cmd_task 的有限重试吸收，绝不回落外网（产品决策，见 mesh_ota.h）；
 *   - dl_task 只服务根节点路径，与 transmit_file_start 的调用者
 *     （cmd_task）分离，规避"库任务上下文回调再等 cmd_task"的死锁面；
 *   - s_ota_state（ota.c 状态机）是唯一状态事实源，本层只经
 *     simple_ota_set_state()/simple_ota_get_state() 读写；
 *   - 无运行期 malloc：EventGroup 全部静态分配，互斥用短临界区。
 *
 * 回调上下文警告：provide/get/done 与 OTA 事件处理器均可能来自闭源库
 * 内部任务或 esp_event 任务，不得假设调用方身份、不得长时间持锁；
 * esp_ota_* 可重入（同 handle 串行由本层 s_ota_lock 保证）。
 */

#include <string.h>

#include "esp_app_format.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh_lite.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mesh_ota.h"
#include "ota.h"
#include "task_config.h"

static const char *TAG = "mesh_ota";

/* -------------------------------------------------------------------------- */
/* 可标定常量（现场按实测修正，见 MESH_LITE_OTA_INTEGRATION.md 标定方法）        */
/* -------------------------------------------------------------------------- */

/*
 * 每层级错峰步长。标定值 = max(根外网下载耗时, mesh 一跳传输耗时) × 1.5。
 * 初值 60s 依据：2.1MB 固件 + 笔记本热点最差外网 ~45s + 一跳 mesh ~30~60s。
 */
#define LG_MESH_OTA_LEVEL_STAGGER_SEC 60

/* 同层启动抖动：打散同层节点对父节点的射频请求峰值。 */
#define LG_MESH_OTA_STAGGER_JITTER_SEC 15

/* 失败自动重试次数（首次 + 重试共 3 次尝试）。 */
#define LG_MESH_OTA_RETRY_MAX 2

/* 重试间隔；重试复用首次已获取的 metadata/size，不重复外网请求。 */
#define LG_MESH_OTA_RETRY_INTERVAL_SEC 90

/* 根节点外网下载等待上限：防 dl_task 异常时 cmd_task 永久挂起。 */
#define LG_MESH_OTA_ROOT_DL_TIMEOUT_MS (10u * 60u * 1000u)

/* 防御下限：读不到层级时按最小子节点层处理（根恒为 1）。 */
#define LG_MESH_OTA_MIN_CHILD_LEVEL 2

/* PROGRESS 事件限频步长（百分比）。 */
#define LG_MESH_OTA_PROGRESS_LOG_STEP_PCT 10

/* -------------------------------------------------------------------------- */
/* 内部状态                                                                     */
/* -------------------------------------------------------------------------- */

/*
 * xEventGroupSetBits/ClearBits 返回值（是否需上下文切换）在本模块所有调用点
 * 均处任务上下文（cmd_task / dl_task / esp_event 任务），无 ISR 路径，按
 * FreeRTOS 语义可忽略，故不逐个检查。
 */

#ifdef CONFIG_ESP_MESH_LITE_OTA_ENABLE
/* 以下为实现主体；Kconfig 关闭时仅保留文件末尾的对外降级 stub。 */

/* cmd_task 事件位 */
#define CMD_EVT_REQUEST BIT0 /* MQTT ota 命令到达（mesh_ota_handle_mqtt_command） */
#define CMD_EVT_RETRY   BIT1 /* OTA 失败/需重试（事件处理器或本任务置位） */

/* dl_task 事件位 */
#define DL_EVT_START BIT0 /* 唤醒 dl_task 开始外网下载（cmd_task 根路径置位） */
#define DL_EVT_DONE  BIT1 /* 外网下载成功 */
#define DL_EVT_FAIL  BIT2 /* 外网下载失败 */

static StaticEventGroup_t s_cmd_events_buf;
static EventGroupHandle_t s_cmd_events;
static StaticEventGroup_t s_dl_events_buf;
static EventGroupHandle_t s_dl_events;

/*
 * OTA 接收会话互斥：保护 s_ota_handle/s_update_partition 的惰性 begin，
 * 并串行化 write/end。必须用 mutex 而非 portMUX 临界区——esp_ota_begin 的
 * 整槽擦除与 esp_ota_end 的镜像校验可达数百毫秒，关中断临界区不可接受
 * （本地规范"临界区必须短小"）。静态分配，禁运行期 malloc
 * （对齐 mqtt_app.c 的 StaticSemaphore 范式）。
 */
static StaticSemaphore_t s_ota_mutex_buf;
static SemaphoreHandle_t s_ota_mutex;
static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_update_partition;

/*
 * 供数分区匹配缓存锁：仅保护 fw_version/partition/valid 三字段的读写
 * （微秒级），portMUX 短临界区可接受；flash 读取均在锁外进行。
 */
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool valid;
    char fw_version[SIMPLE_OTA_VERSION_MAX_LEN];
    const esp_partition_t *partition;
} s_provide_cache;

/* 子节点路径的已获取参数（重试复用）；根节点不使用。 */
static simple_ota_metadata_t s_metadata;
static size_t s_firmware_size_bytes;
static bool s_metadata_valid;

/* 已执行的重试次数（首试为 0）。 */
static uint8_t s_retry_count;

static bool s_initialized;

/* -------------------------------------------------------------------------- */
/* 供数：按 fw_version 匹配 app 分区（running 优先，次为另一 ota 槽）             */
/* -------------------------------------------------------------------------- */

/*
 * 读分区镜像头部的 esp_app_desc_t 比对版本。擦除态/非镜像分区返回 false。
 * 调用上下文：库任务（provide_file_cb），只读 flash，可重入。
 */
static bool partition_version_matches(const esp_partition_t *part, const char *fw_version)
{
    if (part == NULL || fw_version == NULL) {
        return false;
    }

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) {
        return false;
    }

    return strncmp(desc.version, fw_version, sizeof(desc.version)) == 0;
}

static void provide_cache_store(const char *fw_version, const esp_partition_t *part)
{
    taskENTER_CRITICAL(&s_cache_lock);
    strlcpy(s_provide_cache.fw_version, fw_version, sizeof(s_provide_cache.fw_version));
    s_provide_cache.partition = part;
    s_provide_cache.valid = true;
    taskEXIT_CRITICAL(&s_cache_lock);
}

static void provide_cache_invalidate(void)
{
    taskENTER_CRITICAL(&s_cache_lock);
    s_provide_cache.valid = false;
    taskEXIT_CRITICAL(&s_cache_lock);
}

/*
 * 双路径覆盖闭源库"节点已有固件"判定条件的不可见性：
 *   1) running 分区（本节点已重启运行新固件的场景）；
 *   2) ota_0 / ota_1 另一槽（本项目 CanRestart 延迟重启的主场景：
 *      新固件已下载写入、otadata 已指向、但仍在运行旧固件）。
 * 调用上下文：库任务（provide_file_cb），只读，可重入。
 */
static const esp_partition_t *find_firmware_partition(const char *fw_version)
{
    if (fw_version == NULL) {
        return NULL;
    }

    /* 缓存命中（短临界区）。 */
    taskENTER_CRITICAL(&s_cache_lock);
    if (s_provide_cache.valid &&
        strncmp(s_provide_cache.fw_version, fw_version, sizeof(s_provide_cache.fw_version)) == 0) {
        const esp_partition_t *cached = s_provide_cache.partition;
        taskEXIT_CRITICAL(&s_cache_lock);
        return cached;
    }
    taskEXIT_CRITICAL(&s_cache_lock);

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (partition_version_matches(running, fw_version)) {
        provide_cache_store(fw_version, running);
        return running;
    }

    static const esp_partition_subtype_t ota_subtypes[] = {
        ESP_PARTITION_SUBTYPE_APP_OTA_0,
        ESP_PARTITION_SUBTYPE_APP_OTA_1,
    };
    for (size_t i = 0; i < sizeof(ota_subtypes) / sizeof(ota_subtypes[0]); i++) {
        const esp_partition_t *part =
            esp_partition_find_first(ESP_PARTITION_TYPE_APP, ota_subtypes[i], NULL);
        if (partition_version_matches(part, fw_version)) {
            provide_cache_store(fw_version, part);
            return part;
        }
    }

    return NULL;
}

/* -------------------------------------------------------------------------- */
/* LAN OTA 文件传输三回调（组件契约，见 esp_mesh_lite_core.h）                    */
/* -------------------------------------------------------------------------- */

/*
 * 提供方：把 param 请求的数据块（offset/data_size）从匹配分区读出填入
 * param->data。读取范围按分区边界钳位；库按 64KB 对齐 filesize 请求，
 * 超出镜像实际大小的 padding 段填 0xFF（擦除态值，写入无害）。
 * 调用上下文：闭源库内部任务；不得持锁做 flash 读（仅短临界区查缓存）。
 */
static esp_err_t provide_file_cb(esp_mesh_lite_lan_ota_file_transfer_param_t *param)
{
    if (param == NULL || param->data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (param->data_size == 0) {
        return ESP_OK;
    }

    const esp_partition_t *part = find_firmware_partition(param->fw_version);
    if (part == NULL) {
        ESP_LOGE(TAG, "provide: no app partition with version=%s",
                 param->fw_version ? param->fw_version : "(null)");
        return ESP_ERR_NOT_FOUND;
    }

    size_t to_read_bytes = param->data_size;
    bool overflow = false;
    if (param->offset >= part->size) {
        to_read_bytes = 0;
        overflow = true;
    } else if (param->offset + to_read_bytes > part->size) {
        to_read_bytes = (size_t)(part->size - param->offset);
        overflow = true;
    }

    if (to_read_bytes > 0) {
        esp_err_t err = esp_partition_read(part, param->offset, param->data, to_read_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "provide: read failed at offset=%u: %s",
                     (unsigned)param->offset, esp_err_to_name(err));
            return err;
        }
    }
    if (overflow) {
        memset(param->data + to_read_bytes, 0xFF, param->data_size - to_read_bytes);
    }

    return ESP_OK;
}

/*
 * 接收方：首次调用惰性 esp_ota_begin（OTA_SIZE_UNKNOWN 整槽擦除，
 * 与 perform_http_ota 一致），随后一律 esp_ota_write_with_offset——
 * 窗口传输（OTA_WND_DEFAULT=8256，6 包）可能乱序到达，offset 为权威位置。
 * 调用上下文：闭源库内部任务；s_ota_lock 短临界区保证多会话回调串行。
 */
static esp_err_t get_file_cb(esp_mesh_lite_lan_ota_file_transfer_param_t *param)
{
    if (param == NULL || param->data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (param->data_size == 0) {
        return ESP_OK;
    }

    /*
     * 持锁覆盖惰性 begin + write：库对多会话回调的串行性不可见，此处以
     * mutex 保证同一 handle 的 begin/write 不并发（flash 操作可跨毫秒，
     * 用 mutex 而非关中断临界区）。只可能等同一节点的另一会话，无死锁面
     * （回调内不再回调本模块）。
     */
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "get_file: take OTA mutex failed");
        return ESP_FAIL;
    }

    if (s_ota_handle == 0) {
        const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
        if (update == NULL) {
            xSemaphoreGive(s_ota_mutex);
            ESP_LOGE(TAG, "get_file: no next update partition");
            return ESP_ERR_NOT_FOUND;
        }

        esp_ota_handle_t handle = 0;
        esp_err_t err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &handle);
        if (err != ESP_OK) {
            xSemaphoreGive(s_ota_mutex);
            ESP_LOGE(TAG, "get_file: esp_ota_begin failed: %s", esp_err_to_name(err));
            return err;
        }

        s_ota_handle = handle;
        s_update_partition = update;
        ESP_LOGI(TAG, "get_file: OTA session started on %s", update->label);
    }

    esp_err_t err = esp_ota_write_with_offset(s_ota_handle, param->data,
                                              param->data_size, param->offset);
    xSemaphoreGive(s_ota_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_file: write at offset=%u failed: %s",
                 (unsigned)param->offset, esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

/*
 * 完成：esp_ota_end（镜像校验）+ esp_ota_set_boot_partition → 置
 * PENDING_RESTART（不自动重启，等 CanRestart 统一点火，AGENTS.md）。
 * 失败路径 abort 并置 FAILED（OTA_FINISH 事件随后汇入重试调度）。
 * 调用上下文：闭源库内部任务。
 */
static esp_err_t get_file_done_cb(void)
{
    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "get_file_done: take OTA mutex failed");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle = s_ota_handle;
    const esp_partition_t *update = s_update_partition;
    s_ota_handle = 0;
    s_update_partition = NULL;
    xSemaphoreGive(s_ota_mutex);

    if (handle == 0 || update == NULL) {
        ESP_LOGW(TAG, "get_file_done: no active OTA session");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_file_done: esp_ota_end failed: %s", esp_err_to_name(err));
        esp_ota_abort(handle);
        simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
        return err;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_file_done: set_boot_partition failed: %s", esp_err_to_name(err));
        simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
        return err;
    }

    /*
     * 本节点现已持有新版本（update 槽）。失效供数缓存，使后续子节点
     * 请求时 find_firmware_partition 重新匹配到该槽（中间父级联场景）。
     */
    provide_cache_invalidate();
    simple_ota_set_state(SIMPLE_OTA_STATE_PENDING_RESTART);
    ESP_LOGI(TAG, "Mesh OTA complete; pending restart (CanRestart trigger)");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* OTA 事件处理器（esp_event 任务上下文，非 ISR）                                 */
/* -------------------------------------------------------------------------- */

static void mesh_ota_event_handler(void *arg, esp_event_base_t base,
                                   int32_t event_id, void *event_data)
{
    (void)arg;

    if (base != ESP_MESH_LITE_EVENT) {
        return;
    }

    switch (event_id) {
    case ESP_MESH_LITE_EVENT_OTA_START:
        ESP_LOGI(TAG, "Mesh OTA event: start");
        break;

    case ESP_MESH_LITE_EVENT_OTA_PROGRESS: {
        const esp_mesh_lite_event_ota_progress_t *prog = event_data;
        if (prog != NULL && (prog->percentage % LG_MESH_OTA_PROGRESS_LOG_STEP_PCT) == 0) {
            ESP_LOGI(TAG, "Mesh OTA event: progress %u%%", prog->percentage);
        }
        break;
    }

    case ESP_MESH_LITE_EVENT_OTA_FINISH: {
        const esp_mesh_lite_event_ota_finish_t *fin = event_data;
        esp_mesh_lite_ota_finish_reason_t reason =
            (fin != NULL) ? fin->reason : (esp_mesh_lite_ota_finish_reason_t)ESP_MESH_LITE_EVENT_OTA_FAIL;

        if (reason == ESP_MESH_LITE_EVENT_OTA_SUCCESS) {
            ESP_LOGI(TAG, "Mesh OTA event: finish, success");
            /*
             * get_file_done 通常已置 PENDING_RESTART；此处仅对仍处于
             * RUNNING 的非常规顺序做兜底（状态只升不降，不覆盖既有终态）。
             */
            if (simple_ota_get_state() == SIMPLE_OTA_STATE_RUNNING) {
                simple_ota_set_state(SIMPLE_OTA_STATE_PENDING_RESTART);
            }
        } else {
            ESP_LOGE(TAG, "Mesh OTA event: finish, fail, reason=%d", (int)reason);
            if (simple_ota_get_state() == SIMPLE_OTA_STATE_RUNNING) {
                simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
            }
            /* 通知 cmd_task 按重试间隔再试（cmd_task 内做间隔延迟）。 */
            xEventGroupSetBits(s_cmd_events, CMD_EVT_RETRY);
        }
        break;
    }

    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* 子节点禁用桩：mesh 内全链路无目标版本时命中                                    */
/* -------------------------------------------------------------------------- */

/*
 * extern_url_ota_cb 禁用桩（产品决策：子节点不走外网兜底）。
 * 命中即返回失败，由 cmd_task 有限重试吸收；重试耗尽置 FAILED 等服务器
 * 重发命令。不打 firmware URL，防日志泄露内网拓扑。
 * 调用上下文：闭源库内部任务。
 */
static esp_err_t mesh_ota_extern_disabled(void)
{
    ESP_LOGW(TAG, "Mesh-internal firmware unavailable; external fallback disabled by design");
    return ESP_FAIL;
}

/* -------------------------------------------------------------------------- */
/* cmd_task：状态预检 + 层级分流 + 重试调度                                       */
/* -------------------------------------------------------------------------- */

/* 置重试位（未耗尽时）；调用方随后返回，WaitBits 立即唤醒走重试流程。 */
static void schedule_retry(void)
{
    if (s_retry_count < LG_MESH_OTA_RETRY_MAX) {
        xEventGroupSetBits(s_cmd_events, CMD_EVT_RETRY);
    } else {
        ESP_LOGE(TAG, "OTA retry exhausted (%u/%u); waiting for server resend",
                 (unsigned)s_retry_count, (unsigned)LG_MESH_OTA_RETRY_MAX);
        simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
    }
}

/*
 * 根节点路径：暂停子节点 OTA 请求（库内排队）→ 唤醒 dl_task 外网下载
 * 唯一 1 份 → 恢复子节点。下载成败均恢复，失败汇入重试调度。
 * 调用上下文：cmd_task。
 */
static void handle_root_path(void)
{
    ESP_LOGI(TAG, "Root OTA path: pause children, external download (single copy)");

    esp_err_t err = esp_mesh_lite_ota_notify_child_node_pause();
    if (err != ESP_OK) {
        /* 继续下载：子节点请求可能直接穿透，由禁用桩/重试兜底。 */
        ESP_LOGW(TAG, "notify_child_node_pause failed: %s", esp_err_to_name(err));
    }

    xEventGroupClearBits(s_dl_events, DL_EVT_DONE | DL_EVT_FAIL);
    xEventGroupSetBits(s_dl_events, DL_EVT_START);

    EventBits_t bits = xEventGroupWaitBits(s_dl_events,
                                           DL_EVT_DONE | DL_EVT_FAIL,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(LG_MESH_OTA_ROOT_DL_TIMEOUT_MS));

    if (bits & DL_EVT_DONE) {
        ESP_LOGI(TAG, "Root OTA download completed; resume children");
    } else {
        ESP_LOGE(TAG, "Root OTA download failed or timed out; resume children for retry");
        /* dl_task 失败时 simple_ota 状态已为 FAILED；超时路径防御性置位。 */
        if (bits == 0) {
            simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
        }
    }

    /* 无论成败都恢复子节点：放行库内排队请求（届时根已就绪或走重试）。 */
    err = esp_mesh_lite_ota_notify_child_node_restart();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "notify_child_node_restart failed: %s", esp_err_to_name(err));
    }

    if (!(bits & DL_EVT_DONE)) {
        schedule_retry();
    }
}

/*
 * 子节点路径：层级错峰（(level-1)×步长 + 抖动）→ 首试拉 update.json +
 * 版本门 + Content-Length 探测 → transmit_file_start（extern 禁用桩）。
 * 之后由库驱动 get_file_cb/get_file_done 与 OTA_FINISH 事件收尾。
 * 重试复用首次 metadata/size，不重复外网请求。
 * 调用上下文：cmd_task；重试间隔延迟已由 cmd_task 主循环统一处理。
 */
static void handle_child_path(void)
{
    uint8_t level = esp_mesh_lite_get_level();
    if (level < LG_MESH_OTA_MIN_CHILD_LEVEL) {
        /* 防御：非根却读不到层级时按最小子节点层处理。 */
        level = LG_MESH_OTA_MIN_CHILD_LEVEL;
    }

    /* 层级错峰 + 同层抖动。 */
    uint32_t stagger_ms = (uint32_t)(level - 1) * (uint32_t)LG_MESH_OTA_LEVEL_STAGGER_SEC * 1000u;
    uint32_t jitter_ms = (uint32_t)esp_random() %
                         ((uint32_t)LG_MESH_OTA_STAGGER_JITTER_SEC * 1000u + 1u);
    uint32_t delay_ms = stagger_ms + jitter_ms;

    ESP_LOGI(TAG, "Child OTA: level=%u, stagger %u.%03us before mesh request",
             (unsigned)level, (unsigned)(delay_ms / 1000u), (unsigned)(delay_ms % 1000u));
    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    /* 错峰期间状态可能被改变（重复命令/CanRestart 前的其他流程）。 */
    if (simple_ota_get_state() != SIMPLE_OTA_STATE_RUNNING) {
        ESP_LOGW(TAG, "Child OTA aborted during stagger: state=%d",
                 (int)simple_ota_get_state());
        return;
    }

    /* 首试获取参数；重试复用。 */
    if (!s_metadata_valid) {
        esp_err_t err = simple_ota_fetch_metadata(&s_metadata);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fetch update.json failed: %s", esp_err_to_name(err));
            simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
            schedule_retry();
            return;
        }

        if (!simple_ota_version_is_newer(s_metadata.version)) {
            ESP_LOGI(TAG, "Firmware already up to date (%s); skip OTA", s_metadata.version);
            simple_ota_set_state(SIMPLE_OTA_STATE_IDLE);
            return;
        }

        err = simple_ota_probe_firmware_size(s_metadata.file, &s_firmware_size_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "probe firmware size failed: %s", esp_err_to_name(err));
            simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
            schedule_retry();
            return;
        }

        s_metadata_valid = true;
        ESP_LOGI(TAG, "Child OTA target: version=%s size=%u bytes",
                 s_metadata.version, (unsigned)s_firmware_size_bytes);
    }

    /* 发起 mesh 内 OTA：extern 传禁用桩，强制 mesh 内路径。 */
    esp_mesh_lite_file_transmit_config_t cfg = {
        .type = ESP_MESH_LITE_OTA_TRANSMIT_FIRMWARE,
        .size = s_firmware_size_bytes,
        .extern_url_ota_cb = mesh_ota_extern_disabled,
    };
    strlcpy(cfg.fw_version, s_metadata.version, sizeof(cfg.fw_version));

    esp_err_t err = esp_mesh_lite_transmit_file_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "transmit_file_start failed: %s", esp_err_to_name(err));
        simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
        schedule_retry();
        return;
    }

    ESP_LOGI(TAG, "transmit_file_start: version=%s size=%u bytes; mesh-internal path only",
             cfg.fw_version, (unsigned)s_firmware_size_bytes);
    /* 后续由库回调 get_file_cb / get_file_done 与 OTA_FINISH 事件驱动。 */
}

static void mesh_ota_cmd_task(void *arg)
{
    (void)arg;

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_cmd_events,
                                               CMD_EVT_REQUEST | CMD_EVT_RETRY,
                                               pdTRUE,
                                               pdFALSE,
                                               portMAX_DELAY);
        bool is_retry = (bits & CMD_EVT_RETRY) != 0;

        /*
         * 状态预检（异步化后第二条 ota 命令可能在 RUNNING 期间到达；
         * 原同步实现天然免疫，此处显式拒绝，防双下载/双传输会话）。
         */
        simple_ota_state_t st = simple_ota_get_state();
        if (st == SIMPLE_OTA_STATE_PENDING_RESTART) {
            ESP_LOGW(TAG, "OTA pending restart; ignoring %s",
                     is_retry ? "retry" : "command");
            continue;
        }
        if (st == SIMPLE_OTA_STATE_RUNNING) {
            ESP_LOGW(TAG, "OTA already running; ignoring %s",
                     is_retry ? "retry" : "command");
            continue;
        }

        if (is_retry && s_retry_count >= LG_MESH_OTA_RETRY_MAX) {
            ESP_LOGE(TAG, "OTA retry exhausted (%u/%u); waiting for server resend",
                     (unsigned)s_retry_count, (unsigned)LG_MESH_OTA_RETRY_MAX);
            simple_ota_set_state(SIMPLE_OTA_STATE_FAILED);
            continue;
        }

        /*
         * 重试间隔对所有路径统一在此延迟（根外网下载失败同样需要退避）。
         * 延迟期间到达的新 REQUEST 会被本循环按 RETRY 合并处理——重试本身
         * 即是对该命令的响应，语义等价。
         */
        if (is_retry) {
            s_retry_count++;
            ESP_LOGI(TAG, "OTA retry %u/%u in %us",
                     (unsigned)s_retry_count, (unsigned)LG_MESH_OTA_RETRY_MAX,
                     (unsigned)LG_MESH_OTA_RETRY_INTERVAL_SEC);
            vTaskDelay(pdMS_TO_TICKS((uint32_t)LG_MESH_OTA_RETRY_INTERVAL_SEC * 1000u));

            /* 等待期间可能已由其他路径完成（如服务器重发触发根侧完成）。 */
            if (simple_ota_get_state() == SIMPLE_OTA_STATE_PENDING_RESTART) {
                ESP_LOGI(TAG, "OTA retry aborted: pending restart");
                continue;
            }
        } else {
            s_retry_count = 0;
        }
        simple_ota_set_state(SIMPLE_OTA_STATE_RUNNING);

        uint8_t level = esp_mesh_lite_get_level();
        if (level <= 1) {
            handle_root_path();
        } else {
            handle_child_path();
        }
    }
}

/* -------------------------------------------------------------------------- */
/* dl_task：根节点外网下载（唯一执行 simple_ota 下载链的任务）                     */
/* -------------------------------------------------------------------------- */

static void mesh_ota_dl_task(void *arg)
{
    (void)arg;

    for (;;) {
        xEventGroupWaitBits(s_dl_events, DL_EVT_START, pdTRUE, pdFALSE, portMAX_DELAY);

        ESP_LOGI(TAG, "dl_task: starting external download");
        esp_err_t err = simple_ota_check_and_update_default();

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "dl_task: external download failed: %s", esp_err_to_name(err));
            /* simple_ota_check_and_update 内部已置 FAILED；此处只发事件。 */
            xEventGroupSetBits(s_dl_events, DL_EVT_FAIL);
        } else {
            ESP_LOGI(TAG, "dl_task: external download completed");
            xEventGroupSetBits(s_dl_events, DL_EVT_DONE);
        }
    }
}

#endif /* CONFIG_ESP_MESH_LITE_OTA_ENABLE */

/* -------------------------------------------------------------------------- */
/* 对外接口                                                                     */
/* -------------------------------------------------------------------------- */

esp_err_t mesh_ota_init(void)
{
#ifndef CONFIG_ESP_MESH_LITE_OTA_ENABLE
    ESP_LOGE(TAG, "CONFIG_ESP_MESH_LITE_OTA_ENABLE not set; mesh OTA unavailable");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_initialized) {
        return ESP_OK;
    }

    s_cmd_events = xEventGroupCreateStatic(&s_cmd_events_buf);
    s_dl_events = xEventGroupCreateStatic(&s_dl_events_buf);
    if (s_cmd_events == NULL || s_dl_events == NULL) {
        ESP_LOGE(TAG, "Failed to create static event groups");
        return ESP_ERR_NO_MEM;
    }

    s_ota_mutex = xSemaphoreCreateMutexStatic(&s_ota_mutex_buf);
    if (s_ota_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create static OTA mutex");
        return ESP_ERR_NO_MEM;
    }

    /* LAN OTA 文件传输三回调。 */
    static esp_mesh_lite_lan_ota_file_transfer_cb_t lan_ota_cb = {
        .provide_file_cb = provide_file_cb,
        .get_file_cb = get_file_cb,
        .get_file_done = get_file_done_cb,
    };
    esp_mesh_lite_ota_register_file_transfer_cb(&lan_ota_cb);

    /* OTA 事件：一次注册整个 base（ESP_EVENT_ANY_ID），handler 内部分流。 */
    esp_err_t err = esp_event_handler_register(ESP_MESH_LITE_EVENT, ESP_EVENT_ANY_ID,
                                               mesh_ota_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Register OTA event handler failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreatePinnedToCore(mesh_ota_cmd_task, "mesh_ota_cmd",
                                LG_STACK_MESH_OTA, NULL, LG_PRIO_MESH_OTA,
                                NULL, LG_APP_CPU_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mesh_ota_cmd task");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(mesh_ota_dl_task, "mesh_ota_dl",
                                LG_STACK_MESH_OTA_DL, NULL, LG_PRIO_MESH_OTA_DL,
                                NULL, LG_APP_CPU_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mesh_ota_dl task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "mesh_ota initialized: stagger=%us/level jitter<=%us retry=%ux@%us",
             (unsigned)LG_MESH_OTA_LEVEL_STAGGER_SEC,
             (unsigned)LG_MESH_OTA_STAGGER_JITTER_SEC,
             (unsigned)LG_MESH_OTA_RETRY_MAX,
             (unsigned)LG_MESH_OTA_RETRY_INTERVAL_SEC);
    return ESP_OK;
#endif /* CONFIG_ESP_MESH_LITE_OTA_ENABLE */
}

void mesh_ota_handle_mqtt_command(void)
{
#ifndef CONFIG_ESP_MESH_LITE_OTA_ENABLE
    ESP_LOGE(TAG, "Mesh OTA not compiled in; ignoring ota command");
    return;
#else
    if (s_cmd_events == NULL) {
        ESP_LOGE(TAG, "mesh_ota not initialized; ignoring ota command");
        return;
    }

    /*
     * 状态预检（与 cmd_task 内重复，但提前拦截可避免无谓唤醒）：
     * PENDING_RESTART 忽略（防二次下载覆盖运行槽）、RUNNING 拒绝（防双下载）。
     */
    simple_ota_state_t st = simple_ota_get_state();
    if (st == SIMPLE_OTA_STATE_PENDING_RESTART) {
        ESP_LOGW(TAG, "OTA pending restart; ignoring ota command");
        return;
    }
    if (st == SIMPLE_OTA_STATE_RUNNING) {
        ESP_LOGW(TAG, "OTA already running; ignoring ota command");
        return;
    }

    xEventGroupSetBits(s_cmd_events, CMD_EVT_REQUEST);
    ESP_LOGI(TAG, "OTA command received; dispatched to cmd_task");
#endif /* CONFIG_ESP_MESH_LITE_OTA_ENABLE */
}

simple_ota_state_t mesh_ota_get_state(void)
{
    return simple_ota_get_state();
}
