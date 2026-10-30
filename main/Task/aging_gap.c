#include "aging_gap.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app_mem.h"
#include "SqLite.h"
#include "GetProto.h"
#include "mqtt_app.h"
#include "ConfigData.h"
#include "log.h"
#include "cJSON.h"

#define GAP_PENDING_MAX 16
/* 16个结束意图 + 3个队列包 + 1个上传工作包 + 当前会话及切换余量。 */
#define GAP_SESSION_SLOTS (GAP_PENDING_MAX + 6)
#define GAP_RESPONSE_BYTES (16U * 1024U)
#define GAP_SEND_MS 1000
#define GAP_SETTLE_MS 5000
#define GAP_ROUNDS_MAX 3
#define GAP_QUEUE_SUBMIT_MS 50 /* 保持原采样队列的有界等待。 */
#define GAP_STACK_WARN_BYTES 1024U
#define GAP_STACK_WARN_INTERVAL_MS 60000U

typedef enum { GAP_QUERY, GAP_SEND, GAP_VERIFY } GapPhase;
typedef struct {
    bool used, closed, intent;
    uint32_t token, accepted, processed, persisted, revision, trigger;
    char pn[DB_PN_MAX_LEN];
    char record_id[DB_RECORD_ID_MAX_LEN + 1];
    int next_id, last_id;
    unsigned round;
    GapPhase phase;
    TickType_t due;
} GapSession;

typedef struct {
    uint32_t token, revision;
    int *ids;
    int count, index, missing;
    char *response;
    QueryResult *record; /* 约4.7KiB，禁止放任务栈。 */
} GapWork;

static const char *TAG = "aging_gap";
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static GapSession *s_sessions;
static GapWork s_work; /* 只有 http_auto 访问，动态缓冲均优先PSRAM。 */
static uint32_t s_next_token, s_current;
static bool s_running;
static atomic_uint s_trigger;
static atomic_bool s_mqtt_ready;
/* 仅 http_auto 使用；诊断不写本地存储、不进入 MQTT 告警队列。 */
static bool s_stack_warned;
static TickType_t s_stack_last_warn;

static void gap_log_stack(const char *phase)
{
    UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL); /* ESP-IDF 单位为字节。 */
    ESP_LOGI(TAG, "补查任务栈水位 phase=%s minimum_free_bytes=%u", phase, (unsigned)free_bytes);
    TickType_t now = xTaskGetTickCount();
    if (free_bytes < GAP_STACK_WARN_BYTES &&
        (!s_stack_warned || now - s_stack_last_warn >= pdMS_TO_TICKS(GAP_STACK_WARN_INTERVAL_MS))) {
        s_stack_warned = true;
        s_stack_last_warn = now;
        ESP_LOGW(TAG, "http_auto 栈余量不足: minimum_free_bytes=%u", (unsigned)free_bytes);
    }
}

static bool gap_lock(void)
{
    return s_mutex != NULL && s_sessions != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE;
}

static void gap_unlock(void)
{
    if (xSemaphoreGive(s_mutex) != pdTRUE) ESP_LOGE(TAG, "补查会话锁释放失败");
}

static GapSession *gap_find(uint32_t token)
{
    for (unsigned i = 0; i < GAP_SESSION_SLOTS; ++i)
        if (s_sessions[i].used && s_sessions[i].token == token) return &s_sessions[i];
    return NULL;
}

static void gap_warn(const char *pn, const char *record_id, const char *message)
{
    ESP_LOGW(TAG, "%s PN=%s RecordId=%s", message, pn, record_id);
    storage_write_record_cyclic(pn, message);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return;
    cJSON *data = cJSON_AddObjectToObject(root, "Data");
    bool valid = data != NULL && cJSON_AddNumberToObject(root, "Seq", time(NULL)) != NULL &&
        cJSON_AddNumberToObject(data, "Code", 1003) != NULL &&
        cJSON_AddStringToObject(data, "Msg", message) != NULL &&
        cJSON_AddStringToObject(data, "RecordId", record_id) != NULL;
    char *json = valid ? cJSON_PrintUnformatted(root) : NULL;
    if (json != NULL) {
        if (app_mqtt_event_submit("device/%s/event/warn", json, DEVICE_ID, MQTT_EVENT_REPORT_WARN) < 0)
            ESP_LOGW(TAG, "采样缺口告警缓存失败");
        free(json);
    }
    cJSON_Delete(root);
}

/* 锁内仅变更小型元数据；返回告警文本，调用方必须在解锁后写本地日志。 */
static const char *gap_finalize(GapSession *session)
{
    if (!session->closed || session->accepted != session->processed || session->intent) return NULL;
    if (session->persisted == 0 || session->last_id < 0) {
        session->used = false;
        return NULL;
    }
    if (session->record_id[0] == '\0') {
        session->used = false;
        return "采样缺口无法补查：会话RecordId缺失";
    }
    unsigned pending = 0;
    for (unsigned i = 0; i < GAP_SESSION_SLOTS; ++i) {
        GapSession *old = &s_sessions[i];
        if (!old->used || !old->intent) continue;
        pending++;
        if (strcmp(old->pn, session->pn) == 0 && strcmp(old->record_id, session->record_id) == 0) {
            if (old->last_id < session->last_id) old->last_id = session->last_id;
            old->revision++;
            old->round = 0;
            session->used = false;
            return NULL;
        }
    }
    if (pending >= GAP_PENDING_MAX) {
        session->used = false;
        return "采样补查意图已满，本轮仅保留SQLite数据";
    }
    session->intent = true;
    session->revision++;
    session->round = 0;
    return NULL;
}

esp_err_t aging_gap_init(void)
{
    if (s_sessions != NULL) return ESP_OK;
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    s_sessions = app_calloc_prefer_psram(GAP_SESSION_SLOTS, sizeof(*s_sessions));
    return s_sessions != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

bool aging_gap_session_begin(const char *pn, const char *record_id, bool resume)
{
    if (pn == NULL || pn[0] == '\0' || record_id == NULL || strlen(pn) >= DB_PN_MAX_LEN ||
        strlen(record_id) > DB_RECORD_ID_MAX_LEN || !gap_lock()) return false;
    GapSession *slot = NULL;
    if (s_current == 0) {
        for (unsigned i = 0; i < GAP_SESSION_SLOTS; ++i)
            if (!s_sessions[i].used) { slot = &s_sessions[i]; break; }
    }
    if (slot == NULL) { gap_unlock(); return false; }
    if (++s_next_token == 0) ++s_next_token;
    *slot = (GapSession){.used = true, .token = s_next_token, .last_id = -1};
    snprintf(slot->pn, sizeof(slot->pn), "%s", pn);
    snprintf(slot->record_id, sizeof(slot->record_id), "%s", record_id);
    s_current = slot->token;
    s_running = true; /* 协议拉取、建链阶段也暂停历史补查。 */
    uint32_t token = slot->token;
    gap_unlock();
    if (resume && record_id[0] != '\0') {
        QueryResult *record = app_calloc_prefer_psram(1, sizeof(*record));
        if (record == NULL) {
            aging_gap_session_close(); aging_gap_idle();
            return false;
        }
        int query = QueryStructuredRecordBySession(pn, record_id, -1, record);
        if (query == -2) {
            free(record);
            aging_gap_session_close(); aging_gap_idle();
            return false; /* 数据库错误不能按“没有历史”重新从0覆盖旧编号。 */
        }
        if (query == 0 && gap_lock()) {
            slot = gap_find(token);
            if (slot == NULL || slot->closed || s_current != token) {
                gap_unlock(); free(record); return false;
            }
            slot->last_id = record->seq_no;
            slot->next_id = record->seq_no < INT_MAX ? record->seq_no + 1 : INT_MAX;
            /* 已恢复的持久数据也需要在本轮结束后补查，即使没有新采样。 */
            slot->persisted = 1;
            gap_unlock();
        }
        free(record);
    }
    return true;
}

void aging_gap_session_close(void)
{
    if (!gap_lock()) return;
    GapSession *slot = gap_find(s_current);
    s_current = 0;
    if (slot == NULL) { gap_unlock(); return; }
    slot->closed = true;
    const char *warning = gap_finalize(slot);
    char pn[DB_PN_MAX_LEN], record_id[DB_RECORD_ID_MAX_LEN + 1];
    snprintf(pn, sizeof(pn), "%s", slot->pn);
    snprintf(record_id, sizeof(record_id), "%s", slot->record_id);
    gap_unlock();
    if (warning != NULL) gap_warn(pn, record_id, warning);
}

void aging_gap_idle(void)
{
    if (!gap_lock()) return;
    if (s_running) {
        s_running = false;
        atomic_fetch_add(&s_trigger, 1);
    }
    gap_unlock();
}

uint32_t aging_gap_sample_snapshot(char *pn, size_t pn_bytes, char *record_id, size_t record_bytes)
{
    if (pn == NULL || record_id == NULL || !gap_lock()) return 0;
    GapSession *slot = gap_find(s_current);
    uint32_t token = 0;
    if (slot != NULL && !slot->closed && strlen(slot->pn) < pn_bytes && strlen(slot->record_id) < record_bytes) {
        snprintf(pn, pn_bytes, "%s", slot->pn);
        snprintf(record_id, record_bytes, "%s", slot->record_id);
        token = slot->token;
    }
    gap_unlock();
    return token;
}

bool aging_gap_sample_submit(uint32_t token, QueueHandle_t queue, const void *item)
{
    if (queue == NULL || item == NULL || !gap_lock()) return false;
    GapSession *slot = gap_find(token);
    bool accepted = slot != NULL && !slot->closed &&
                    xQueueSend(queue, item, pdMS_TO_TICKS(GAP_QUEUE_SUBMIT_MS)) == pdTRUE;
    if (accepted) slot->accepted++;
    gap_unlock();
    return accepted;
}

int aging_gap_sample_next_id(uint32_t token)
{
    if (!gap_lock()) return -1;
    GapSession *slot = gap_find(token);
    int id = -1;
    if (slot != NULL && slot->processed < slot->accepted && slot->next_id < INT_MAX) {
        id = slot->next_id++;
        slot->last_id = id;
    }
    gap_unlock();
    return id;
}

void aging_gap_sample_done(uint32_t token, bool persisted)
{
    if (!gap_lock()) return;
    GapSession *slot = gap_find(token);
    if (slot == NULL || slot->processed >= slot->accepted) { gap_unlock(); return; }
    slot->processed++;
    if (persisted) slot->persisted++;
    const char *warning = gap_finalize(slot);
    char pn[DB_PN_MAX_LEN], record_id[DB_RECORD_ID_MAX_LEN + 1];
    snprintf(pn, sizeof(pn), "%s", slot->pn);
    snprintf(record_id, sizeof(record_id), "%s", slot->record_id);
    gap_unlock();
    if (warning != NULL) gap_warn(pn, record_id, warning);
}

void aging_gap_mqtt_ready(bool ready)
{
    bool previous = atomic_exchange(&s_mqtt_ready, ready);
    if (ready && !previous) atomic_fetch_add(&s_trigger, 1);
}

static int gap_id_compare(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* 空列表是成功，与非法JSON/非法数字明确区分。所有权移交给http_auto工作槽。 */
static bool gap_parse_ids(const char *json, int last_id, int **out_ids, int *out_count)
{
    *out_ids = NULL; *out_count = 0;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(json, &end, true);
    if (root == NULL) return false;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "Data");
    cJSON *ids = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "Ids") : NULL;
    if (!cJSON_IsArray(ids)) { cJSON_Delete(root); return false; }
    int count = cJSON_GetArraySize(ids);
    int *values = count > 0 ? app_calloc_prefer_psram((size_t)count, sizeof(int)) : NULL;
    if (count > 0 && values == NULL) { cJSON_Delete(root); return false; }
    int index = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, ids) {
        if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > last_id ||
            item->valuedouble != (double)item->valueint) {
            free(values); cJSON_Delete(root); return false;
        }
        values[index++] = item->valueint;
    }
    if (count > 1) qsort(values, (size_t)count, sizeof(int), gap_id_compare);
    int unique = 0;
    for (int i = 0; i < count; ++i)
        if (i == 0 || values[i] != values[i - 1]) values[unique++] = values[i];
    cJSON_Delete(root);
    *out_ids = values; *out_count = unique;
    return true;
}

static void gap_work_clear_ids(void)
{
    free(s_work.ids); s_work.ids = NULL;
    s_work.count = s_work.index = s_work.missing = 0;
}

static bool gap_can_run(const GapSession *snapshot)
{
    if (!atomic_load(&s_mqtt_ready) || !app_mqtt_is_connected() || !gap_lock()) return false;
    GapSession *slot = gap_find(snapshot->token);
    bool valid = !s_running && slot != NULL && slot->intent &&
                 slot->revision == snapshot->revision && slot->trigger == atomic_load(&s_trigger);
    gap_unlock();
    return valid;
}

/* 每次触发最多3轮；耗尽后等待新的上线/空闲边沿，不做永久定时网络重试。 */
static void gap_retry(GapSession *slot, TickType_t now)
{
    slot->round++;
    slot->phase = GAP_QUERY;
    slot->due = now + pdMS_TO_TICKS(slot->round == 2 ? 10000 : 30000);
}

void aging_gap_poll(void)
{
    if (!atomic_load(&s_mqtt_ready) || !app_mqtt_is_connected() || !gap_lock()) return;
    if (s_running) { gap_unlock(); return; }
    GapSession *slot = NULL;
    uint32_t trigger = atomic_load(&s_trigger);
    for (unsigned i = 0; i < GAP_SESSION_SLOTS; ++i) {
        GapSession *candidate = &s_sessions[i];
        if (!candidate->used || !candidate->intent) continue;
        if (candidate->round == 0 || candidate->trigger != trigger) {
            candidate->trigger = trigger; candidate->round = 1;
            candidate->phase = GAP_QUERY;
            candidate->due = xTaskGetTickCount() + pdMS_TO_TICKS(GAP_SETTLE_MS);
        }
        if (candidate->round <= GAP_ROUNDS_MAX &&
            (slot == NULL || candidate->token < slot->token)) slot = candidate;
    }
    if (slot == NULL || (int32_t)(xTaskGetTickCount() - slot->due) < 0) { gap_unlock(); return; }
    GapSession snapshot = *slot;
    gap_unlock();
    if (s_work.token != snapshot.token || s_work.revision != snapshot.revision || snapshot.phase == GAP_QUERY) {
        gap_work_clear_ids();
        s_work.token = snapshot.token; s_work.revision = snapshot.revision;
    }
    if (s_work.response == NULL) s_work.response = app_calloc_prefer_psram(1, GAP_RESPONSE_BYTES + 1U);
    if (s_work.record == NULL) s_work.record = app_calloc_prefer_psram(1, sizeof(*s_work.record));
    if (s_work.response == NULL || s_work.record == NULL) {
        if (gap_lock()) {
            slot = gap_find(snapshot.token);
            if (slot != NULL && slot->revision == snapshot.revision) gap_retry(slot, xTaskGetTickCount());
            gap_unlock();
        }
        gap_warn(snapshot.pn, snapshot.record_id, "采样补查内存分配失败，有限重试");
        return;
    }
    if (!gap_can_run(&snapshot)) return;
    if (snapshot.phase != GAP_SEND) {
        s_work.response[0] = '\0';
        esp_err_t err = test_http_post_record_query((int32_t)time(NULL), snapshot.last_id,
            snapshot.record_id, s_work.response, GAP_RESPONSE_BYTES + 1U);
        if (!gap_can_run(&snapshot)) return; /* 新老化/重连/会话合并：不应用旧结果。 */
        int *ids = NULL, count = 0;
        bool valid = err == ESP_OK && gap_parse_ids(s_work.response, snapshot.last_id, &ids, &count);
        gap_log_stack("query");
        ESP_LOGI(TAG, "采样缺号查询 RecordId=%s round=%u phase=%d last_id=%d missing=%d valid=%d",
                 snapshot.record_id, snapshot.round, snapshot.phase, snapshot.last_id, count, valid);
        if (!gap_lock()) { free(ids); return; }
        slot = gap_find(snapshot.token);
        if (slot == NULL || slot->revision != snapshot.revision || s_running) {
            gap_unlock(); free(ids); return;
        }
        if (valid && count == 0) {
            slot->used = false; /* 仅服务器明确无缺号，才结束意图。 */
            gap_unlock(); free(ids); gap_work_clear_ids();
            gap_log_stack("complete");
            ESP_LOGI(TAG, "本轮采样补查完成 PN=%s RecordId=%s last_id=%d",
                     snapshot.pn, snapshot.record_id, snapshot.last_id);
            return;
        }
        bool exhausted = false;
        if (!valid || snapshot.phase == GAP_VERIFY) {
            gap_retry(slot, xTaskGetTickCount());
            exhausted = slot->round > GAP_ROUNDS_MAX;
        }
        if (valid && !exhausted) {
            gap_work_clear_ids(); s_work.ids = ids; ids = NULL; s_work.count = count;
            slot->phase = GAP_SEND;
            if (snapshot.phase == GAP_QUERY) slot->due = xTaskGetTickCount();
        }
        gap_unlock(); free(ids);
        if (exhausted) gap_warn(snapshot.pn, snapshot.record_id, "采样补查三轮未完成，保留意图等待下次上线或空闲");
        return;
    }
    if (s_work.index >= s_work.count) {
        int missing = s_work.missing;
        ESP_LOGI(TAG, "采样缺号补发本轮已提交 RecordId=%s count=%d unavailable=%d，等待服务器确认",
                 snapshot.record_id, s_work.count - missing, missing);
        gap_work_clear_ids();
        if (gap_lock()) {
            slot = gap_find(snapshot.token);
            if (slot != NULL && slot->revision == snapshot.revision) {
                slot->phase = GAP_VERIFY;
                slot->due = xTaskGetTickCount() + pdMS_TO_TICKS(GAP_SETTLE_MS);
            }
            gap_unlock();
        }
        if (missing > 0) {
            char message[128];
            snprintf(message, sizeof(message), "服务器缺号中有%d条本地已无数据，无法恢复", missing);
            gap_warn(snapshot.pn, snapshot.record_id, message);
        }
        return;
    }
    int id = s_work.ids[s_work.index];
    int query = QueryStructuredRecordBySession(snapshot.pn, snapshot.record_id, id, s_work.record);
    if (query == -2) {
        gap_log_stack("record_error");
        bool exhausted = false;
        if (gap_lock()) {
            slot = gap_find(snapshot.token);
            if (slot != NULL && slot->revision == snapshot.revision) {
                gap_retry(slot, xTaskGetTickCount());
                exhausted = slot->round > GAP_ROUNDS_MAX;
            }
            gap_unlock();
        }
        if (exhausted) gap_warn(snapshot.pn, snapshot.record_id, "数据库读取失败，保留补查意图");
        return;
    }
    if (query != 0 ||
        s_work.record->json_data[0] == '\0') {
        s_work.missing++; s_work.index++;
        return;
    }
    if (!gap_can_run(&snapshot)) return;
    int publish_id = app_mqtt_publish("device/%s/data/aging", s_work.record->json_data, DEVICE_ID);
    gap_log_stack("send");
    if (!gap_lock()) return;
    slot = gap_find(snapshot.token);
    bool exhausted = false;
    if (slot != NULL && slot->revision == snapshot.revision) {
        if (publish_id > 0) {
            s_work.index++;
            slot->due = xTaskGetTickCount() + pdMS_TO_TICKS(GAP_SEND_MS);
        } else {
            gap_retry(slot, xTaskGetTickCount());
            exhausted = slot->round > GAP_ROUNDS_MAX;
        }
    }
    gap_unlock();
    if (exhausted) gap_warn(snapshot.pn, snapshot.record_id, "采样补发连续失败，保留意图等待新触发");
}
