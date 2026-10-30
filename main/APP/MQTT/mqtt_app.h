#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "dual_net.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_app_start(void);
/* start/init/stop 为任务上下文，可能阻塞；stop/restart 不可在 SDK事件回调或ISR调用。 */
void mqtt_app_stop(void);
void mqtt_app_restart(void);
/** 唯一 CPU1 服务任务入口，由应用建任务封装创建；不可作为普通函数调用。
 * 每次连接恢复订阅及上线通知；完整配置自动上报每次启动成功入队一次，客户端重建不清零。
 * 上位机主动 get_config 的回应不受此限制，SDK 接受入队不表示上位机落库。 */
void mqtt_app_service_task(void *arg);
/** 返回 >=0 表示接受（QoS0 为0）；-2容量不足，-1其他失败，不表示服务器落库。 */
int app_mqtt_publish(char *topic_name, char *publish_string, char *UDP_ID);

typedef enum {
    MQTT_EVENT_REPORT_STATE,   /* 老化状态，使用预留容量。 */
    MQTT_EVENT_REPORT_TASKCP,  /* 群控完成，使用预留容量并检查有效性。 */
    MQTT_EVENT_REPORT_CONTEXT, /* 程序、工步、阶段和设备模式。 */
    MQTT_EVENT_REPORT_WARN,
} MqttEventReportKind;

/** 任务上下文、可并发、非 ISR/SDK 回调接口；队列互斥锁最多等待100ms。
 * 复制完整主题和原始 JSON，调用者仍拥有传入字符串。device_id 可为 NULL，
 * 非空时 topic_name 必须是仅含一个 %s 的主题模板。
 * 0=本次开机内已缓存（不是 PUBACK），-2=容量不足，-1=参数/长度/初始化失败。
 * 队列保留到 PUBACK；MQTT 重建不清空，整机重启清空。 */
int app_mqtt_event_submit(const char *topic_name, const char *payload,
                          const char *device_id, MqttEventReportKind kind);

/** 任务上下文，可并发，非 ISR/SDK 回调接口。使当前会话/工步的未投递 TaskCP
 * 失效；已经开始交 SDK 的报文不能撤销。以内部代次隔离后续工步，不修改线上 JSON。 */
void app_mqtt_event_cancel_taskcp(void);

/** 当前是否已连接 Broker（原子读，供补传 worker 等轮询方使用）。 */
bool app_mqtt_is_connected(void);

esp_err_t mqtt_init(void);

/** 根节点上行变化回调：提交最新出口，不阻塞、不重建客户端。仅任务上下文，非ISR。 */
void mqtt_app_on_uplink_changed(dual_net_uplink_t uplink);

/** 由现有 net_monitor 调用，非阻塞处理因生命周期锁忙而延后的请求。非ISR。 */
void mqtt_app_process_uplink_reconnect(void);

int app_read_MQTT_topic(char *topic_buffer, int buffer_len, uint32_t timeout);
int app_read_MQTT_data(char *data_buffer, int buffer_len, uint32_t timeout);

#ifdef __cplusplus
}
#endif
