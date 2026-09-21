#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_app_start(void);
void mqtt_app_stop(void);
void mqtt_app_restart(void);
int app_mqtt_publish(char *topic_name, char *publish_string, char *UDP_ID);

esp_err_t mqtt_init(void);

int app_read_MQTT_topic(char *topic_buffer, int buffer_len, uint32_t timeout);
int app_read_MQTT_data(char *data_buffer, int buffer_len, uint32_t timeout);

#ifdef __cplusplus
}
#endif