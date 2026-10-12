#pragma once

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
int app_mqtt_publish(char *topic_name, char *publish_string, char *UDP_ID);

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
