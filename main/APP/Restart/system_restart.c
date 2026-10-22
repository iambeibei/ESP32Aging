/** 一键重启：先留传播窗口，再暂停 Mesh，根先于深层重启。计划不跨启动保存。 */
#include "system_restart.h"

#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "appTask.h"
#include "mesh.h"
#include "task_config.h"

#define RESTART_POLL_MS 100
#define RESTART_DISCONNECT_RETRY_MS 1000
#define RESTART_DISCONNECT_MAX_ATTEMPTS 3

static const char *TAG = "system_restart";
static portMUX_TYPE s_restart_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_restart_task;
static bool s_initialized, s_planned, s_paused, s_recovery_committed;
static system_restart_plan_t s_plan;
static int64_t s_pause_at_us, s_restart_at_us;
/* 仅服务任务访问下面的执行状态。 */
static unsigned s_disconnect_attempts;
static bool s_disconnected;
static int64_t s_disconnect_retry_at_us;

bool system_restart_is_planned(void)
{
    portENTER_CRITICAL(&s_restart_lock);
    bool value = s_planned;
    portEXIT_CRITICAL(&s_restart_lock);
    return value;
}

bool system_restart_mesh_paused(void)
{
    portENTER_CRITICAL(&s_restart_lock);
    bool value = s_paused;
    portEXIT_CRITICAL(&s_restart_lock);
    return value;
}

bool system_restart_claim_recovery(void)
{
    portENTER_CRITICAL(&s_restart_lock);
    bool allowed = !s_planned && !s_recovery_committed;
    if (allowed) s_recovery_committed = true;
    portEXIT_CRITICAL(&s_restart_lock);
    return allowed;
}

esp_err_t system_restart_schedule(int32_t command_seq, system_restart_plan_t *out_plan)
{
    if (command_seq < 0 || out_plan == NULL) return ESP_ERR_INVALID_ARG;
    /* 层数与时间采样均在临界区外，SDK 不能在 spinlock 内调用。 */
    uint8_t level = mesh_get_restart_level();
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_restart_lock);
    if (!s_initialized || s_recovery_committed) {
        portEXIT_CRITICAL(&s_restart_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_planned) {
        s_plan = (system_restart_plan_t){
            .command_seq = command_seq,
            .level = level,
            .delay_seconds = LG_RESTART_BASE_DELAY_SEC + (level - 1u) * LG_RESTART_LEVEL_INTERVAL_SEC,
        };
        s_pause_at_us = now_us + (int64_t)LG_RESTART_BASE_DELAY_SEC * 1000000;
        s_restart_at_us = now_us + (int64_t)s_plan.delay_seconds * 1000000;
        s_planned = true;
    }
    *out_plan = s_plan;
    TaskHandle_t task = s_restart_task;
    portEXIT_CRITICAL(&s_restart_lock);
    xTaskNotifyGive(task);
    ESP_LOGI(TAG, "RESTART_SCHEDULED seq=%" PRId32 " plan_seq=%" PRId32 " level=%u pause=%us restart=%us",
             command_seq, out_plan->command_seq, (unsigned)out_plan->level,
             (unsigned)LG_RESTART_BASE_DELAY_SEC, (unsigned)out_plan->delay_seconds);
    return ESP_OK;
}

/* 不持 spinlock 调用 SDK；断开最多重试 3 次，不修改原截止时间。 */
static void system_restart_poll(void)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_restart_lock);
    bool active = s_planned;
    bool enter_pause = active && !s_paused && now_us >= s_pause_at_us;
    if (enter_pause) s_paused = true;
    bool paused = s_paused;
    int64_t restart_at_us = s_restart_at_us;
    system_restart_plan_t plan = s_plan;
    portEXIT_CRITICAL(&s_restart_lock);
    if (!active) return;
    if (enter_pause) {
        ESP_LOGW(TAG, "RESTART_MESH_PAUSE seq=%" PRId32 " level=%u remaining_ms=%" PRId64,
                 plan.command_seq, (unsigned)plan.level, (restart_at_us > now_us ? restart_at_us - now_us : 0) / 1000);
    }
    if (paused && !s_disconnected && s_disconnect_attempts < RESTART_DISCONNECT_MAX_ATTEMPTS &&
        now_us >= s_disconnect_retry_at_us) {
        ++s_disconnect_attempts;
        esp_err_t err = mesh_pause_for_restart();
        s_disconnected = err == ESP_OK;
        s_disconnect_retry_at_us = now_us + (int64_t)RESTART_DISCONNECT_RETRY_MS * 1000;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RESTART_MESH_PAUSE failed=%s attempt=%u/%u; deadline unchanged",
                     esp_err_to_name(err), s_disconnect_attempts, RESTART_DISCONNECT_MAX_ATTEMPTS);
        }
    }
    /* 根的暂停时间与重启时间相同：尝试一次断开后立即重启，不为重试延长时间。 */
    if (esp_timer_get_time() >= restart_at_us) {
        ESP_LOGW(TAG, "RESTART_EXECUTE seq=%" PRId32 " level=%u keep_mesh_record=1 disconnected=%d",
                 plan.command_seq, (unsigned)plan.level, s_disconnected);
        esp_restart();
    }
}

static void system_restart_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 通知计数可合并，状态/截止时间为事实源，不依赖单次通知。 */
        ulTaskNotifyTake(pdTRUE, system_restart_is_planned() ? pdMS_TO_TICKS(RESTART_POLL_MS) : portMAX_DELAY);
        system_restart_poll();
    }
}

esp_err_t system_restart_init(void)
{
    if (s_initialized) return ESP_OK; /* 本接口由启动任务串行调用。 */
    TaskHandle_t task = NULL;
    if (app_create_restart_service_task(system_restart_task, &task) != pdPASS) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_restart_lock);
    s_restart_task = task;
    s_initialized = true;
    portEXIT_CRITICAL(&s_restart_lock);
    return ESP_OK;
}
