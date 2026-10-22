# MQTT QoS 修改计划（设备端 / 固件）

> 配套文档：
> - `DeviceService-main/docs/MQTT_QoS_改造方案.md`（问题分析与方案选型）
> - `DeviceService-main/docs/MQTT_QoS_修改计划_上位机.md`（上位机侧，含 S* 变更）
>
> **范围**：本文档只覆盖**设备端**（`ESP32Aging-main/`），是 D* 变更的完整实施依据。
> 上位机侧的改动形态（DI 注入、MQTTnet 订阅选项、Redis 幂等）与本文档完全不同，见上位机文档。
> 状态：**待评审，未实施**
> 编写日期：2026-10-21　固件版本基线：`version.txt = 0.0.2`

---

## 1. 范围与前提

### 1.1 本文档解决什么

| 问题 | 设备端根因 | 对应变更 |
|---|---|---|
| 命令丢失后无法补偿 | 去重命中即静默丢弃、不回 reply | D1-3 |
| 断线数据不补 | `QueryUnpushedRecords` 无调用者 | D1-4 |
| 改 QoS 1 后消息在队列层被静默丢 | 接收队列仅 10 槽，满时仍回 PUBACK | D1-1 / D1-2 |
| 下行命令被降级为 QoS 0 | 14 条订阅全请求 0 | D2-1 |
| 降级无法被发现 | SUBACK 只判 `<= 2` | D2-2 |
| 告警格式不合法 | `event/warn` 发裸字符串 | D3-1 |
| 订阅了服务端不存在的主题 | 6 条死订阅 | D3-2 |

### 1.2 本文档**不**解决什么

- **`AgingCommandGate` 门卡死**：那是服务端状态机问题，设备端无需改动。
- **`clean_session` 改 0**：**明确保持 `disable_clean_session = false`（clean session = 1）**，理由见 §12.1。
- **`buffer.size = 1024` 分片**：下行大报文分片机制本身工作正常，本次不动。

### 1.3 一个必须知道的事实：两端协议版本不同

| 端 | 协议版本 | 证据 |
|---|---|---|
| 服务端 | **MQTT 5.0**（MQTTnet 5 默认 `V500`） | 反射实测 |
| 设备端 | **MQTT 3.1.1** | `mqtt_app.c:702` `.protocol_ver = MQTT_PROTOCOL_V_3_1_1` |

通过 Broker 互通没问题（Broker 做版本桥接），但排障时要注意：

- 设备端**没有** MQTT 5 的 Session Expiry、Message Expiry、Reason Code
- 设备端 LWT 语义是 3.1.1 的，`retain=0` 不会留脏（这点是对的）
- 抓包对比两端报文时字段集不同，不要误判为协议错误

**结论：本次不统一协议版本。** 改版本会牵动 LWT 报文格式、SUBACK 返回结构、`disable_clean_session` 语义，风险远大于收益。

---

## 2. 现状基线（设备端事实）

| 项 | 现状 | 位置 |
|---|---|---|
| 发布 QoS | **1**（`enqueue(topic, payload, 0, 1, 0, true)`） | `mqtt_app.c:887` |
| 订阅 QoS | **0**（14 条全部 `.qos = 0`） | `mqtt_app.c:1005/1011/1018` |
| SUBACK 校验 | `(uint8_t)event->data[i] <= 2U`（降级也算成功） | `mqtt_app.c:515` |
| clean session | `disable_clean_session = false` → **1** | `mqtt_app.c:701` |
| keepalive | 60 s（根/子相同） | `mqtt_app.c:54-55` |
| LWT | `device/public/down_line`, qos=1, retain=0, **Seq 恒为 0** | `mqtt_app.c:688/703-705` |
| 接收队列 | `MQTT_TOPIC_QUEUE_LEN 10` / `MQTT_DATA_QUEUE_LEN 10` | `mqtt_app.c:31-32` |
| 队列满行为 | `xQueueSend` 失败 → `ESP_LOGE` + `break`，**回调仍返回，SDK 仍回 PUBACK** | `mqtt_app.c:583-587/606-613` |
| 命令去重 | 16 项内存环形表，命中即 `goto cleanup`，**不回 reply** | `appTask.c:2377-2443` |
| 补传 | `QueryUnpushedRecords` 无调用者；仅老化结束补当前 PN | `SqLite.c:1907` / `appTask.c:4747` |
| 上行送达判据 | `app_mqtt_publish() > 0`（入队成功即置 `pushed=1`） | `appTask.c:5220-5227` |
| `event/warn` | 裸字符串，非 JSON | `appTask.c:2100/3008` |
| 死订阅 | `set_config`/`remote_log`/`local_log`/`get_lost_aging`/`CanRestart`/架级 `stop_aging` | `mqtt_app.c:986-1000` |

### 2.1 内存基线（实施前必须实测）

| 区域 | 当前占用 | 说明 |
|---|---|---|
| Topic 队列 | 10 × ~66 B ≈ 0.7 KB | `xQueueCreate` 走内部 RAM |
| Data 队列 | 10 × ~1026 B ≈ 10 KB | 内部 RAM |
| ack 位图 | 32 KB | PSRAM（`MQTT_ACK_MAP_BYTES`） |
| 运行日志队列 | 3 × ~322 B ≈ 1 KB | 内部 RAM |

**实施前打点**：

```c
ESP_LOGI(TAG, "heap: free=%u largest_internal=%u psram_free=%u",
         (unsigned)esp_get_free_heap_size(),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
```

判定：`largest_internal > 80 KB` 才允许把 Data 队列加到 32；否则降到 20。

---

## 3. 变更总览

| ID | 变更 | 文件 | 工时 | 依赖 | 风险 |
|---|---|---|---|---|---|
| D1-1 | 接收队列加深 | `mqtt_app.c:31-32` | 0.3 | 内存实测 | 中 |
| D1-2 | 队列满计数与告警 | `mqtt_app.c:583/606/1211` | 0.2 | D1-1 | 低 |
| D1-3 | 去重表改"重放回复" | `appTask.c:2377-2443` + 5 处回复函数 | 1.5 | 服务端 S2-1 已上线 | 中 |
| D1-4 | 补传 worker | `appTask.c` / `task_config.h` | 1.7 | 服务端 S2-1 已上线 | 中 |
| D2-1 | 订阅 QoS → 1 | `mqtt_app.c:1005/1011/1018` | 0.1 | D1-1、D1-2 | 中 |
| D2-2 | SUBACK 严格校验 | `mqtt_app.c:515` | 0.1 | D2-1 | 低 |
| D3-1 | `warn` 改标准 JSON | `appTask.c:2100/3008` | 0.3 | 服务端 S1-1 已上线 | 低 |
| D3-2 | 删除 6 个死订阅 | `mqtt_app.c:986-1000` | 0.2 | 确认 S4-5 不做 | 低 |
| — | **合计** | | **4.4** | | |

**实施顺序硬约束**：

```
D1-1 → D1-2 → D2-1 → D2-2      （队列必须先于 QoS，否则消息在队列层静默丢）
服务端 S2-1 → D1-3 → D1-4      （幂等必须先于去重重放，否则重放会制造重复）
服务端 S1-1 → D3-1             （服务端要先能容忍裸文本，否则改固件期间告警仍丢）
```

---

## 4. D1-1 接收队列加深

### 4.1 位置

`main/APP/MQTT/mqtt_app.c:31-32`

### 4.2 现状

```c
#define MQTT_TOPIC_QUEUE_LEN 10
#define MQTT_DATA_QUEUE_LEN 10
```

### 4.3 为什么必须先做

`MQTT_EVENT_DATA` 回调**不做业务处理**，只把 topic/data 拷进队列就返回（`:565-618`）。队列满时：

```c
if (xQueueSend(Data_queue, &data_msg, pdMS_TO_TICKS(100)) != pdTRUE)
{
    ESP_LOGE(TAG, "Failed to enqueue MQTT data fragment %d, len=%d", ...);
    break;          // ← 只跳出拷贝循环，回调照常返回
}
```

回调返回后，esp-mqtt 认为事件已处理，**照常发 PUBACK**。

于是：**Broker 认为消息已送达，设备实际把它丢了。**

在 QoS 0 下这无所谓（本来就不保证送达）。但一旦订阅改成 QoS 1，这就把"能观测到的丢"变成了"观测不到的丢"——比现状更危险，因为人会以为问题已解决。

### 4.4 改动

```c
/* QoS1 订阅后 Broker 会持续投递直到收到 PUBACK。
 * 队列满会导致 xQueueSend 失败但 SDK 仍回 PUBACK（静默丢消息），
 * 因此深度必须覆盖一次批量下发的峰值。
 * 内存：Topic ~66B/槽，Data ~1026B/槽。32 槽合计约 34KB 内部 RAM。 */
#define MQTT_TOPIC_QUEUE_LEN 32
#define MQTT_DATA_QUEUE_LEN  32
```

### 4.5 内存代价

| 队列 | 单槽 | 10 槽 | 32 槽 | 增量 |
|---|---|---|---|---|
| Topic | ~66 B | 0.7 KB | 2.1 KB | +1.4 KB |
| Data | ~1026 B | 10 KB | 32 KB | **+22 KB** |
| 合计 | — | 10.7 KB | 34.1 KB | **+23.4 KB** |

### 4.6 若内存不足的降级方案

按优先级依次尝试：

1. `MQTT_DATA_QUEUE_LEN` 降到 **20**（+10 KB）
2. `mqtt_data_msg_t` 的 `data[1024]` 改为 **512**（需确认 `MQTT_DATA_ITEM_CAPACITY` 的所有使用点，拼包逻辑在 `appTask.c:2118` 附近，`MQTT_DATA_Cache` 上限 5 KB 不变）
3. Data 队列改为 **PSRAM 指针池**（改动最大，不推荐在本期做）

### 4.7 验收

- 压测 200 条/秒突发 2 秒，`xQueueSend` 无失败
- `free heap` 下降与预期一致（±2 KB）
- 正常老化一个完整周期，`heap` 无持续下降（无泄漏）

### 4.8 回滚

改回 10，OTA 回退到 `version.txt` 记录的上一版本。

---

## 5. D1-2 队列满计数与告警

### 5.1 位置

`mqtt_app.c:583-587`（topic）、`:606-613`（data）、`:1211-1226`（诊断汇总）

### 5.2 改动 1 — 新增计数器

在 `mqtt_app.c:49` 附近（`s_rejected_count` 旁边）：

```c
static atomic_uint s_queue_overflow_count;
```

### 5.3 改动 2 — 两处入队失败点

```c
if (xQueueSend(Topic_queue, &topic_msg, pdMS_TO_TICKS(100)) != pdTRUE)
{
    atomic_fetch_add(&s_queue_overflow_count, 1);
    ESP_LOGE(TAG, "MQTT topic queue overflow, total=%u",
             (unsigned)atomic_load(&s_queue_overflow_count));
    break;
}
```

Data 队列同理。

### 5.4 改动 3 — 纳入 10 秒诊断

现有诊断块（`:1211-1226`）：

```c
unsigned rejected = atomic_load(&s_rejected_count), deleted = atomic_load(&s_deleted_count);
```

改为同时读 `overflow`，并在有变化时打印：

```c
unsigned overflow = atomic_load(&s_queue_overflow_count);
if (rejected != last_rejected || deleted != last_deleted || overflow != last_overflow)
    ESP_LOGW(TAG, "MQTT pressure: rejected=%u expired=%u queue_overflow=%u last_outbox_bytes=%u",
             rejected, deleted, overflow, atomic_load(&s_last_outbox_bytes));
```

### 5.5 验收

人为把队列改回 2 制造溢出，诊断日志出现 `queue_overflow` 计数增长。

### 5.6 回滚

计数器可保留，无副作用。

---

## 6. D1-3 去重表改"重放回复"

> **前置**：服务端 S2-1（老化采样幂等）必须已上线。否则重放会制造重复数据。

### 6.1 位置

`main/Task/appTask.c:2363-2403`（表结构）、`:2438-2443`（命中分支）、5 处回复函数

### 6.2 现状与问题

```c
#define MQTT_SEQ_DEDUP_CAPACITY 16
static int s_mqtt_seq_history[MQTT_SEQ_DEDUP_CAPACITY];
```

```c
if (mqtt_seq_is_duplicate(Seq->valueint)) {
    ESP_LOGW(TAG, "Duplicate MQTT command seq=%d ignored, topic=%.*s", ...);
    goto cleanup;              // ← 不回 reply
}
```

代码注释（`:2368-2371`）写得很明确："命中后静默忽略——不回 reply、不上 MQTT、不执行业务；服务器未收 ack 的后续同 Seq 重试同样被收敛，直至服务器侧放弃。"

**死锁**：设备无法区分"重复下发"和"上次回复丢了"，把所有重试都丢掉 → 服务端重试 → 无回复 → 放弃 → 命令永久悬挂，只能重启设备恢复。

### 6.3 改动 1 — 表结构

```c
#define MQTT_SEQ_DEDUP_CAPACITY 16
#define MQTT_SEQ_REPLY_MAX      512

typedef struct {
    int  seq;
    bool valid;
    char topic_suffix[16];              /* "point"/"state"/"pn"/"config"/"setlog" */
    char payload[MQTT_SEQ_REPLY_MAX];   /* 原始回复 JSON，重放时原样发出 */
} MqttSeqRecord;

static MqttSeqRecord *s_mqtt_seq_history;   /* PSRAM，启动时分配 */
static size_t s_mqtt_seq_history_count;
static size_t s_mqtt_seq_next;
```

**内存**：16 × (4 + 1 + 16 + 512) ≈ **8.5 KB**，用 `app_malloc_prefer_psram`（`main/Tool/app_mem.h:10`）。

**分配点**：`mqtt_app.c` 的 `mqtt_init_locked()` 内，与 `s_ack_map` 分配放在一起（`s_ack_map` 是 32 KB PSRAM，已在 PSRAM 分配路径上）。

**为什么放 PSRAM**：内部 RAM 已经要为 D1-1 让出 23 KB，这 8.5 KB 不该再占内部。

### 6.4 改动 2 — 记录与查找

```c
static MqttSeqRecord *mqtt_seq_find(int seq)
{
    if (s_mqtt_seq_history == NULL) return NULL;
    for (size_t i = 0; i < s_mqtt_seq_history_count; ++i)
        if (s_mqtt_seq_history[i].valid && s_mqtt_seq_history[i].seq == seq)
            return &s_mqtt_seq_history[i];
    return NULL;
}

/* 只登记 Seq（防止重复执行），payload 由回复函数后续填充。 */
static void mqtt_seq_record(int seq)
{
    if (s_mqtt_seq_history == NULL) return;
    if (mqtt_seq_find(seq) != NULL) return;

    size_t idx;
    if (s_mqtt_seq_history_count < MQTT_SEQ_DEDUP_CAPACITY) {
        idx = s_mqtt_seq_history_count++;
    } else {
        idx = s_mqtt_seq_next;
        s_mqtt_seq_next = (s_mqtt_seq_next + 1) % MQTT_SEQ_DEDUP_CAPACITY;
    }

    MqttSeqRecord *r = &s_mqtt_seq_history[idx];
    memset(r, 0, sizeof(*r));
    r->seq = seq;
    r->valid = true;                    /* valid=已处理；payload[0]=='\0' 表示尚未产生回复 */
}

/* 回复发出后填充缓存，供重复命令重放。 */
static void mqtt_seq_record_reply(int seq, const char *suffix, const char *payload)
{
    if (s_mqtt_seq_history == NULL || suffix == NULL || payload == NULL) return;

    MqttSeqRecord *r = mqtt_seq_find(seq);
    if (r == NULL) return;              /* 未登记（异常路径），不创建 */

    if (strlen(payload) >= MQTT_SEQ_REPLY_MAX) {
        /* 超长（典型是 config 回复）不缓存，退化为旧行为：重复命令不重放。 */
        ESP_LOGW(TAG, "Reply too long to cache for replay, seq=%d, len=%u",
                 seq, (unsigned)strlen(payload));
        return;
    }

    strlcpy(r->topic_suffix, suffix, sizeof(r->topic_suffix));
    strlcpy(r->payload, payload, sizeof(r->payload));
}
```

### 6.5 改动 3 — 命中分支

`appTask.c:2438-2443` 改为：

```c
MqttSeqRecord *dup = mqtt_seq_find(Seq->valueint);
if (dup != NULL) {
    /*
     * 重复命令：不执行业务，但必须重放上次的回复。
     * 否则服务端重试永远拿不到 ack，命令永久悬挂（见 CHILD_NODE_ROOT_LOSS_DESIGN 的补偿前提）。
     * payload 为空说明首次执行尚未产生回复（极窄窗口），此时只能静默丢弃。
     */
    if (dup->payload[0] != '\0') {
        ESP_LOGW(TAG, "Duplicate seq=%d, replaying last reply (%s)", Seq->valueint, dup->topic_suffix);
        char replay_topic[64];
        int n = snprintf(replay_topic, sizeof(replay_topic),
                         "device/%%s/cmd/reply/%s", dup->topic_suffix);
        if (n > 0 && n < (int)sizeof(replay_topic))
            app_mqtt_publish(replay_topic, dup->payload, DEVICE_ID);
    } else {
        ESP_LOGW(TAG, "Duplicate seq=%d ignored (no cached reply yet)", Seq->valueint);
    }
    goto cleanup;
}
mqtt_seq_record(Seq->valueint);
```

### 6.6 改动 4 — 5 处回复函数登记

| 函数 | 位置 | suffix | 登记点 |
|---|---|---|---|
| `device_response_publish_point` | `appTask.c:596` | `"point"` | `app_mqtt_publish(...) >= 0` 之后 |
| `device_response_publish_state` | `:612` | `"state"` | 同上 |
| `device_response_publish_setlog` | `:628` | `"setlog"` | 同上 |
| `publish_pn_reply` | `:2136` | `"pn"` | 同上 |
| `Config_Report` | `:645` | `"config"` | 同上 |

示例：

```c
static void device_response_publish_point(int cmd_seq, int code, const char *msg)
{
    char *response = create_device_response(time(NULL), cmd_seq, code, msg);
    if (response) {
        if (app_mqtt_publish("device/%s/cmd/reply/point", response, DEVICE_ID) < 0) {
            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg), "MQTT response Publish Failed: %s", msg);
            storage_write_record_cyclic(current_log_pn(), log_msg);
        } else {
            mqtt_seq_record_reply(cmd_seq, "point", response);   /* ← 新增 */
        }
        free(response);
    }
}
```

### 6.7 关键约束（逐条都有坑）

| # | 约束 | 原因 |
|---|---|---|
| 1 | **必须复用原 payload，不能重新构造** | `create_device_response` 的外层 `Seq` 是 `time(NULL)`（`Json_data.c:1093`）。重建会变成新 Seq，服务端可能当新回复处理 |
| 2 | 单写者 | 目前只由 `mqtt_rx`（优先级 7，`task_config.h:41`）单任务访问。若重放逻辑搬到别的任务必须加锁 |
| 3 | 两个游标同步维护 | `s_mqtt_seq_next` 只在写满后推进，`s_mqtt_seq_history_count` 兼作有效长度，改造时不能只改一个 |
| 4 | `CmdSeq` 语义 | 设备用**收到的外层 `Seq`** 作为回复的 `CmdSeq`（`appTask.c:2451/2464`），不是 `Data.CmdSeq`。重放缓存的 payload 已含正确值，无需换算 |
| 5 | epoch 变化清表 | 重连后旧回复无意义。在 `s_connection_epoch++` 处（`mqtt_app.c:371/400/726/787`）清 `s_mqtt_seq_history_count` |
| 6 | 超长不缓存 | `config` 回复可能 > 512 B，超长时退化为旧行为并打 Warning |
| 7 | 缓存未填充的窄窗口 | `mqtt_seq_record` 在业务执行前调用（`:2443`），回复稍后生成。若重复命令恰好落在这个窗口，`payload[0]=='\0'`，只能静默丢弃 |

### 6.8 验收

1. 同一 `Seq` 连发两条 `start_aging` → 第二条收到**与第一条完全相同**的回复（外层 `Seq`、`CmdSeq`、`Code`、`Msg` 全部一致）
2. 服务端用原 Seq 重试时不再 `timed_out`
3. `esp_get_free_heap_size()` 与 PSRAM 占用变化 < 10 KB
4. 发第 17 条不同 Seq 命令后，第 1 条的重复命令退化为"不重放"（可接受，日志可见）
5. 内存泄漏检查：连续 1000 条命令后 PSRAM 无持续下降

### 6.9 回滚

恢复 `:2438-2443` 的静默丢弃分支，`s_mqtt_seq_history` 保持 `NULL`（不分配）即可停用。缓存随重启释放。

---

## 7. D1-4 补传 worker

> **前置**：服务端 S2-1 已上线（补传必然产生重复，需要服务端幂等兜底）。

### 7.1 现状

`QueryUnpushedRecords`（`SqLite.c:1907`）**全仓库无调用者**。补传只有两处：

- 老化正常结束（`appTask.c:4747`，只补当前 PN）
- 服务端点播 `get_lost_aging`（服务端未实现）

中止的老化（`stop_aging`）或换 PN 后，`pushed=0` 的记录永久滞留。

### 7.2 新增任务宏

`main/Task/task_config.h`，放在 `LG_STACK_MQTT_SERVICE` 之后：

```c
/* MQTT 重连补传：SQLite pushed=0 → MQTT。
 * QueryResult 单条约 4.7KB，走 PSRAM 堆，栈 4096 足够。 */
#define LG_STACK_MQTT_BACKFILL     4096
#define LG_PRIO_MQTT_BACKFILL      4
```

### 7.3 任务实现

```c
#define MQTT_BACKFILL_BATCH 4

static TaskHandle_t s_mqtt_backfill_task = NULL;

static void mqtt_backfill_task(void *arg)
{
    /* QueryResult 约 4.7KB/条，4 条约 19KB，必须 PSRAM。 */
    QueryResult *batch = app_malloc_prefer_psram(sizeof(QueryResult) * MQTT_BACKFILL_BATCH);
    if (batch == NULL) {
        ESP_LOGE(TAG, "backfill buffer alloc failed, size=%u",
                 (unsigned)(sizeof(QueryResult) * MQTT_BACKFILL_BATCH));
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        /* mqtt_service 在 MQTT_SETUP_DONE 时 notify；60s 兜底轮询防通知丢失 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000));

        if (!atomic_load(&s_mqtt_connected)) continue;

        for (;;) {
            int count = 0;
            if (QueryUnpushedRecords(batch, MQTT_BACKFILL_BATCH, &count) != 0 || count == 0)
                break;

            bool blocked = false;
            for (int i = 0; i < count; ++i) {
                if (batch[i].json_data[0] == '\0') continue;

                int id = app_mqtt_publish("device/%s/data/aging", batch[i].json_data, DEVICE_ID);
                if (id > 0) {
                    UpdateRecordPushStateByPNAndID(batch[i].PN, batch[i].seq_no, true);
                } else if (id == -2) {
                    /* outbox 预算不足：退避后重试，绝不能 break 退出整个循环
                     * （现状 appTask.c:4763-4768 就是 break，导致剩余记录静默滞留） */
                    ESP_LOGW(TAG, "backfill blocked by outbox budget, retry later");
                    blocked = true;
                    break;
                } else {
                    blocked = true;   /* 未连接/epoch 变化，下一轮再试 */
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (blocked) break;
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

BaseType_t app_create_mqtt_backfill_task(void)
{
    return create_cpu1_task(mqtt_backfill_task, "mqtt_backfill",
                            LG_STACK_MQTT_BACKFILL, LG_PRIO_MQTT_BACKFILL,
                            &s_mqtt_backfill_task);
}
```

### 7.4 触发点

`mqtt_app.c` 的 `mqtt_service_poll()` 中，`MQTT_SETUP_DONE` 分支（`:1184` 之后）：

```c
work->phase = MQTT_SETUP_DONE;
mqtt_backfill_wake();          /* 新增：唤醒补传任务 */
```

在 `mqtt_app.c` 暴露一个弱耦合接口（避免 appTask 依赖 mqtt_app 内部变量）：

```c
void mqtt_backfill_wake(void)
{
    extern TaskHandle_t app_get_mqtt_backfill_handle(void);
    TaskHandle_t h = app_get_mqtt_backfill_handle();
    if (h != NULL) xTaskNotifyGive(h);
}
```

> 更简单的做法：把 `s_mqtt_backfill_task` 声明为 `mqtt_app.c` 的静态变量，由 `app_create_mqtt_backfill_task()` 回填句柄。两种都可行，实施时取一种，不要两套并存。

### 7.5 任务注册

`appTask.c` 中创建 `mqtt_rx` 的位置（`:2982-2991`）之后，同条件追加：

```c
if (app_create_mqtt_backfill_task() != pdPASS)
{
    storage_write_record_cyclic(current_log_pn(), "Failed to create MQTT backfill task");
    vTaskDelay(pdMS_TO_TICKS(1000));
    continue;
}
```

### 7.6 参数与理由

| 参数 | 取值 | 理由 |
|---|---|---|
| `MQTT_BACKFILL_BATCH` | 4 | `QueryResult` 约 4.7 KB/条（`sqllib1.h:33-50`），4 条 ≈ 19 KB，必须 PSRAM |
| 批间隔 | 100 ms | 避免打满 outbox 56 KB 预算（`MQTT_OUTBOX_NORMAL_BYTES`） |
| 轮询兜底 | 60 s | 防止 notify 丢失后永不触发 |
| 优先级 | 4 | 低于 `mqtt_rx`(7)，等于 `mqtt_service`(4)，不抢占接收 |
| 栈 | 4096 | `QueryResult` 在 PSRAM 堆，不在栈上 |
| SQLite 锁 | 由 `QueryUnpushedRecords` 内部持有（超时 10 s） | 单批 4 条，持锁时间短 |

### 7.7 为什么不能复用 `Aging_Test_Task` 的补传段

`appTask.c:4745-4771` 那段不能直接搬：

| # | 原因 |
|---|---|
| 1 | 只补当前 PN（`PN_Code`），依赖全局单块 `g_db1_result`，无法覆盖所有 PN |
| 2 | 耦合 `aging_ble_link_lost_pending()` 与 `Aging_Task_Abort_And_Delete()`，在别的任务调用会误触发老化中止 |
| 3 | 循环内 `vTaskDelay(100)`，放进 `mqtt_service_poll`（100 ms 轮询）会阻塞上线状态机 |
| 4 | `PN_Code` 在 `AgingComplete` 阶段会被清空 |
| 5 | 失败即 `break`（`:4763-4768`），剩余记录静默滞留 |

### 7.8 一个必须澄清的预期

**不要指望 Broker 离线队列做补偿。**

30 台 × 5 秒/条 = 2.1 万条/小时。Broker `max_queued_messages` / `max_mqueue_len` 默认在 1000 量级，一次 8 小时停机远超容量。**真正的补偿只能来自设备端 SQLite。**

### 7.9 验收

1. 断网 60 秒后恢复，30 秒内缺口被补齐（`XRANGE lgw:device:{id}:aging:{pn}:history - +` 验证）
2. 中止的老化（`stop_aging`）数据在重连后也能补回
3. 补传不产生重复（依赖服务端 S2-1）
4. 补传期间 `mqtt_rx` 无抢占（`uxTaskGetSystemState` 观察）
5. `pushed=1` 只在 `app_mqtt_publish > 0` 后写入（即"入队成功"，不是 PUBACK——这是已知局限，见 §12.3）

### 7.10 回滚

不创建任务即可（编译开关 `CONFIG_APP_MQTT_BACKFILL` 或直接注释注册行）。

---

## 8. D2-1 订阅 QoS → 1

### 8.1 位置

`main/APP/MQTT/mqtt_app.c:1005 / 1011 / 1018`

### 8.2 改动

新增宏：

```c
/* 下行命令必须 QoS1：QoS0 下命令可静默丢失，服务端只能等超时。
 * 与 D2-2 的 SUBACK 校验共用此宏，避免两处不一致。 */
#define MQTT_SUBSCRIBE_QOS 1
```

三处 `.qos = 0` 改为 `.qos = MQTT_SUBSCRIBE_QOS`：

```c
topics[count] = (esp_mqtt_topic_t){.filter = names[count], .qos = MQTT_SUBSCRIBE_QOS};
```

覆盖全部 14 条 filter（common 4 条 + root 1 条 / child 9 条）。

### 8.3 一个例外说明

`device/+/log/+` 中的 `log/runtime` 由设备以 **QoS 0** 发布（`mqtt_app.c:903-919` 走独立 best-effort 队列）。订阅 QoS 1 不会让它变可靠（`min(0,1)=0`），属预期，无需特殊处理。

### 8.4 验收

- 设备日志 SUBACK 返回码全为 1
- 服务端发布后抓包可见 PUBACK 往返
- `queue_overflow` 计数保持 0（验证 D1-1 生效）

### 8.5 回滚

`MQTT_SUBSCRIBE_QOS` 改回 0。

---

## 9. D2-2 SUBACK 严格校验

### 9.1 位置

`mqtt_app.c:515`

### 9.2 现状

```c
for (int i = 0; i < event->data_len && accepted; ++i)
    accepted = (uint8_t)event->data[i] <= 2U;      /* 降级授予 0 也算成功 */
```

### 9.3 改动

```c
/* 请求 QoS1 时必须确认 Broker 真的授予了 1。
 * 只判 <=2 会让降级静默通过——改了等于没改，且日志看起来一切正常。 */
for (int i = 0; i < event->data_len && accepted; ++i)
    accepted = ((uint8_t)event->data[i] == MQTT_SUBSCRIBE_QOS);
```

### 9.4 验收

把 Broker ACL 配成只允许 QoS 0，设备应报订阅失败并回退到 `MQTT_SETUP_SUBSCRIBE` 重试（`mqtt_app.c:1124-1126`），而不是静默降级。

### 9.5 回滚

改回 `<= 2U`。

---

## 10. D3-1 `warn` 改标准 JSON

### 10.1 位置

`appTask.c:2100`（外接设备自检失败）、`appTask.c:3008`（SQLite 重置）

### 10.2 现状

```c
app_mqtt_publish("device/%s/event/warn", "Failed to ExDevice_Check online and protocol match", DEVICE_ID);
```

服务端 `DeviceEventHandler.HandleAsync` 第一行就是 `JObject.Parse`（`DeviceEventHandler.cs:58`），裸字符串必抛 → 打 Error → return。**所有告警 100% 丢弃。**

### 10.3 改动

`appTask.c:2100`：

```c
char warn_json[192];
snprintf(warn_json, sizeof(warn_json),
         "{\"Seq\":%lld,\"Data\":{\"Code\":1001,\"Msg\":\"Failed to ExDevice_Check online and protocol match\"}}",
         (long long)time(NULL));
if (app_mqtt_publish("device/%s/event/warn", warn_json, DEVICE_ID) < 0)
{
    storage_write_record_cyclic(current_log_pn(), "MQTT AgingStartCheck Error Publish Failed");
}
```

`appTask.c:3008` 同理，`Code` 用 1002，`Msg` 保留原 `warn_msg` 内容（需转义引号，或直接拼接已知安全的固定文本）。

### 10.4 顺序约束

**服务端 S1-1 必须先上线**（服务端容忍裸文本）。否则改固件期间：旧服务端 + 新固件能工作，但新服务端 + 旧固件仍丢。S1-1 上线后两端都能工作，D3-1 只是让格式规范化。

### 10.5 验收

1. 触发外接设备自检失败 5 次，服务端 `channelWarn` 收到消息
2. `lgw:device:{id}:event:warn` 的 `Payload` 是合法 JSON
3. `Code` 字段能被服务端解析（当前服务端只取 `Data.Msg`，`Code` 为后续扩展预留）

### 10.6 回滚

改回裸字符串（服务端 S1-1 仍能兜底）。

---

## 11. D3-2 删除 6 个死订阅

### 11.1 位置

`mqtt_app.c:986-1000`

### 11.2 待删清单

| 订阅 | 服务端是否发布 | 处置 |
|---|---|---|
| `server/{id}/cmd/set_config` | 从不 | 删 |
| `server/{id}/cmd/remote_log` | 从不 | 删（同时删 `device_response_publish_setlog`，否则又多一个无 handler 的上行） |
| `server/{id}/cmd/local_log` | 从不 | 删 |
| `server/{id}/cmd/get_lost_aging` | 从不 | 删（除非决定做服务端 S4-5） |
| `server/rack/{n}/CanRestart` | 已移除 | 删 |
| `server/{n}/public/cmd/stop_aging` | 从不 | 删 |

### 11.3 保留

`ota`、`get_config`、`CPower`（根）、`start_aging`/`stop_aging`/`set_pn`/`get_state`/`CanNextstep`（子）。

### 11.4 风险

删订阅会让对应功能彻底不可用。**实施前需确认这 6 个功能确实已废弃**（尤其 `remote_log` / `local_log`，若现场排障还在用串口以外的日志通道，需保留并让服务端补实现）。

### 11.5 验收

- 设备订阅条数从 13/14 降到 8/9
- 服务端 `S0-3` 的未匹配 topic 计数不再增长
- 设备日志无 `SUBACK` 失败

---

## 12. 三个关键取舍（需评审确认）

### 12.1 保持 `clean_session = 1`（不改）

**决定：不改。** `mqtt_app.c:701` 的 `disable_clean_session = false` 保持原样。

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| clean_session = 1（现状） | 每次连接状态干净；订阅重建逻辑已在（`mqtt_app.c:1158-1172`）；无 Broker 侧会话堆积 | 离线期间命令全丢 | **采用** |
| clean_session = 0 | Broker 为离线设备排队下行命令 | 长时间离线会堆积；会话状态与配置变更（换架、换 DEVICE_ID）耦合；`CanNextstep` 等时效性指令排队后重放可能造成误动作 | 不采用 |

**理由**：下行命令的可靠性应通过"服务端重试 + 设备去重重放"解决，而不是依赖 Broker 队列。Broker 队列无法表达"这条命令已经过期"。

### 12.2 `pushed=1` 的语义保持"已入队"

**决定：本次不改。**

现状 `appTask.c:5220-5227` 在 `app_mqtt_publish() > 0` 后就置 `pushed=1`，即"已交给 SDK"，不是"Broker 已确认"。

要做到真正的 PUBACK 跟踪，需要给业务发布单独开 ack 捕获窗口——现有 `s_ack_map` 只在 setup 阶段 `capture=true`（`mqtt_app.c:165`），业务发布时 `capture=false`，`MQTT_EVENT_PUBLISHED` 到达后标记被静默丢弃。

**工时 2 人天，收益有限**：QoS 1 下 Broker 通常毫秒级回 PUBACK，与"入队成功"的差距极小；真正的丢失发生在 Broker→服务端那一跳（已由服务端幂等 + 设备补传覆盖）。**建议放到下一期。**

### 12.3 去重容量保持 16

**决定：不改。**

提高到 64 会让 PSRAM 占用从 8.5 KB 涨到 34 KB。16 条足以覆盖"批量下发 + 重试"的窗口（服务端重试间隔 5 秒，16 条命令在 5 秒内到达的概率极低）。若现场出现"第 17 条重复命令未被重放"，再评估。

---

## 13. 内存预算

| 项 | 类型 | 增量 | 说明 |
|---|---|---|---|
| Data 队列 10 → 32 | 内部 RAM | **+22 KB** | 主要成本 |
| Topic 队列 10 → 32 | 内部 RAM | +1.4 KB | |
| 去重表（16 条） | PSRAM | +8.5 KB | `app_malloc_prefer_psram` |
| 补传批缓冲（4 条） | PSRAM | +18.8 KB | `QueryResult` 4.7 KB/条 |
| **合计** | 内部 +23.4 KB / PSRAM +27.3 KB | | |

**前置门槛**：`largest_internal > 80 KB` 才按上表实施；否则 Data 队列降到 20（内部增量降为 +10 KB）。

**实施后必须回归**：连续跑 3 个完整老化周期，`heap` 与 `psram` 均无持续下降。

---

## 14. 测试计划

### 14.1 构建

```bash
idf.py build
idf.py size        # 检查 app 分区余量 > 10%
```

### 14.2 单机验证（OTA 前）

| # | 场景 | 预期 |
|---|---|---|
| 1 | 正常老化全流程 | 采样、阶段、TaskCP、agingIdle 全部正常 |
| 2 | 同 Seq 重复命令 | 收到与首次完全相同的回复（D1-3） |
| 3 | 断网 60 秒 | 恢复后 30 秒内数据补齐（D1-4） |
| 4 | `stop_aging` 中途停止 | 滞留数据在重连后补传 |
| 5 | 外接设备自检失败 | 服务端收到 warn（D3-1） |
| 6 | 突发 200 条/秒 | `queue_overflow=0`（D1-1/D1-2） |
| 7 | 连续 1000 条命令 | PSRAM 无持续下降（D1-3 无泄漏） |
| 8 | 断电重启 | 重连、重订阅、上线正常 |

### 14.3 灰度验证（OTA 后）

| 阶段 | 范围 | 观察时长 | 通过标准 |
|---|---|---|---|
| 灰度 1 | 1 台非关键设备 | 24 小时 | 无异常重启、heap 稳定、命令回复正常 |
| 灰度 2 | 1 个架 | 1 个完整老化周期 | 补传正常、无重复采样、群控放行正常 |
| 全量 | 其余架 | 1 个完整周期 | 同上 |

### 14.4 回归清单

- [ ] `start_aging` / `stop_aging` / `set_pn` / `get_state` / `get_config`
- [ ] `CPower`（根节点）
- [ ] `CanNextstep`（子节点）
- [ ] OTA 分层升级（至少 2 层）
- [ ] 根节点上行切换（ETH↔WiFi）后重连
- [ ] 子节点父节点丢失后自愈重连
- [ ] `local_log` / `remote_log`（若保留）

---

## 15. OTA 发布与回滚

### 15.1 发布流程

```
1. 确认服务端 S1-1、S2-1 已上线并稳定 ≥ 1 周
2. 记录当前 version.txt 与各设备 FirewareVersion 基线
3. 单台 OTA → 观察 24 小时 → 通过后进灰度
4. 按架灰度（每次 1 个架）→ 观察 1 个完整老化周期
5. 全量
```

### 15.2 回滚

| 场景 | 动作 |
|---|---|
| 单台异常 | OTA 回退到 `version.txt` 记录的上一版本 |
| 整架异常 | 停止该架 OTA，回退该架全部设备 |
| 服务端不兼容 | 服务端可先回滚（QoS 改动向后兼容：旧固件订阅 QoS 0 对新服务端只是仍降级，不会报错） |

### 15.3 向后兼容性说明

| 组合 | 行为 |
|---|---|
| 新固件（订阅 QoS 1）+ 旧服务端（发布 QoS 1） | 正常，端到端 QoS 1 生效 |
| 旧固件（订阅 QoS 0）+ 新服务端（发布 QoS 1） | 仍降级为 QoS 0，**不报错**，但无收益 |
| 新固件 + 新服务端 | 全链路 QoS 1 |

**因此混跑期间安全**，不需要版本协商。但灰度验收时要注意区分——旧固件设备的指标不会改善。

---

## 16. 任务清单

### D1 承载能力
- [ ] 实测 `free heap` / `largest_internal` / `psram_free`，确认 §13 门槛
- [ ] D1-1 队列加深（10 → 32 或 20）
- [ ] D1-2 队列满计数与 10 秒诊断
- [ ] D1-3 去重表结构改造 + PSRAM 分配
- [ ] D1-3 5 处回复函数登记
- [ ] D1-3 命中分支改为重放
- [ ] D1-3 epoch 变化清表
- [ ] D1-4 补传 worker + 任务注册
- [ ] D1-4 `MQTT_SETUP_DONE` 唤醒点
- [ ] 单机验证 §14.2 全部 8 项

### D2 QoS 切换
- [ ] D2-1 `MQTT_SUBSCRIBE_QOS` 宏 + 3 处订阅
- [ ] D2-2 SUBACK 严格校验
- [ ] 验证 Broker 授予 QoS = 1

### D3 协议清理
- [ ] D3-1 `warn` 改 JSON（2 处）
- [ ] D3-2 删除 6 个死订阅（需先确认功能废弃）
- [ ] 同步删除 `device_response_publish_setlog`（若删 `remote_log`）

### 发布
- [ ] 单台 OTA + 24 小时观察
- [ ] 按架灰度 + 1 个老化周期观察
- [ ] 全量

---

## 17. 工作量汇总

| 变更 | 工时 |
|---|---|
| D1-1 | 0.3 |
| D1-2 | 0.2 |
| D1-3 | 1.5 |
| D1-4 | 1.7 |
| D2-1 | 0.1 |
| D2-2 | 0.1 |
| D3-1 | 0.3 |
| D3-2 | 0.2 |
| **开发合计** | **4.4** |
| 单机验证 | 1.0 |
| OTA 灰度 | 1.0 |
| **总计** | **6.4 人天** |

---

## 18. 决策点

| ID | 决策 | 选项 | 建议 |
|---|---|---|---|
| **D-1** | 设备 `IDNUM` 生成规则 | 全局递增 / 每轮从 0 开始 / 时间戳派生 | **必须先确认**。若每轮从 0 开始，服务端 S2-1 的去重键必须加老化会话标识，否则会误杀下一轮数据 |
| D-2 | Data 队列深度 | 32 / 20 / 64 | 按 §13 门槛实测决定 |
| D-3 | `clean_session` | 保持 1 / 改 0 | **保持 1**（§12.1） |
| D-4 | 是否保留 `remote_log` / `local_log` | 保留（服务端补实现）/ 删除 | 若无人使用则删，省 1 人天 |
| D-5 | 是否做 PUBACK 跟踪（§12.2） | 做 / 不做 | 本期不做，下一期评估 |
| D-6 | 去重容量 16 → 64 | 改 / 不改 | 不改（§12.3） |
