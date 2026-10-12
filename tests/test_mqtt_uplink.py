#!/usr/bin/env python3
"""Compile real dual_net/MQTT code with SDK mocks; exercise handover and stop races.
Run on Linux: python3 tests/test_mqtt_uplink.py (C compiler and pthread required).
"""
from pathlib import Path
import os
import subprocess
import tempfile
from test_root_uplink import MOCKS, MESH, DUAL, function_source

ROOT = Path(__file__).resolve().parents[1]
MQTT = ROOT / 'main/APP/MQTT/mqtt_app.c'

EXTRA_MOCKS = r'''
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include <stdatomic.h>
#define ESP_ERR_TIMEOUT 7
#define ESP_LOGD mock_log
#define portTICK_PERIOD_MS 1
static void vTaskDelay(TickType_t ticks) { assert(!lock_depth); now_us += ticks * 1000; usleep(1000); }
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
/* Static RTOS mutex; callbacks may not acquire it, even if stop/start holds it. */
typedef struct { pthread_mutex_t mutex; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static _Thread_local bool in_sdk_callback;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {
    assert(!lock_depth); assert(pthread_mutex_init(&storage->mutex, NULL) == 0); return storage;
}
static int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) {
    assert(!lock_depth && !in_sdk_callback);
    int result = timeout == 0 ? pthread_mutex_trylock(&mutex->mutex) : pthread_mutex_lock(&mutex->mutex);
    return result == 0 ? pdTRUE : 0;
}
static int xSemaphoreGive(SemaphoreHandle_t mutex) { return pthread_mutex_unlock(&mutex->mutex) == 0 ? pdTRUE : 0; }

typedef void *QueueHandle_t;
static int queue_create_count, publish_count, subscribe_count, config_report_count;
static QueueHandle_t xQueueCreate(int length, size_t size) { (void)length; (void)size; return (void *)(uintptr_t)++queue_create_count; }
static int xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout) { (void)queue; (void)item; (void)timeout; return pdTRUE; }
static int xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout) { (void)queue; (void)item; (void)timeout; return 0; }
typedef struct { int unused; } cJSON;
static cJSON *cJSON_CreateObject(void) { return NULL; }
static void cJSON_AddNumberToObject(cJSON *object, const char *key, double value) { (void)object; (void)key; (void)value; }
static void cJSON_AddStringToObject(cJSON *object, const char *key, const char *value) { (void)object; (void)key; (void)value; }
static void cJSON_AddItemToObject(cJSON *object, const char *key, cJSON *value) { (void)object; (void)key; (void)value; }
static void cJSON_Delete(cJSON *object) { (void)object; }
static char *cJSON_PrintUnformatted(cJSON *object) { (void)object; return NULL; }
static const char *DEVICE_ID = "test-device", *AgingNumber = "rack";
typedef struct { const char *version; } MockAppDesc;
static const MockAppDesc *esp_app_get_description(void) { static MockAppDesc desc = {"test"}; return &desc; }
static void Config_Report(int type) { (void)type; config_report_count++; }
#define MQTT_TRANSPORT_OVER_TCP 1
#define MQTT_PROTOCOL_V_3_1_1 4
#define MQTT_ERROR_TYPE_TCP_TRANSPORT 1
#define MQTT_EVENT_BEFORE_CONNECT 7
#define MQTT_EVENT_CONNECTED 1
#define MQTT_EVENT_DISCONNECTED 2
#define MQTT_EVENT_SUBSCRIBED 3
#define MQTT_EVENT_UNSUBSCRIBED 4
#define MQTT_EVENT_PUBLISHED 5
#define MQTT_EVENT_DATA 6
#define MQTT_EVENT_ERROR 8

typedef struct {
    struct { struct { const char *hostname; int port, transport; } address; } broker;
    struct { const char *username; struct { const char *password; } authentication; } credentials;
    struct {
        int keepalive, protocol_ver; bool disable_clean_session;
        struct { const char *topic, *msg; int qos, retain; } last_will;
    } session;
    struct { int timeout_ms, reconnect_timeout_ms; bool disable_auto_reconnect; } network;
    struct { int size, out_size; } buffer;
} esp_mqtt_client_config_t;
typedef struct { bool alive, running; int sdk_phase; } MockMqttClient;
typedef MockMqttClient *esp_mqtt_client_handle_t;
typedef struct { int error_type; } MockMqttError;
typedef struct {
    esp_mqtt_client_handle_t client;
    int event_id, msg_id, topic_len, data_len, current_data_offset, total_data_len;
    char *topic, *data;
    MockMqttError *error_handle;
} MockMqttEvent;
typedef MockMqttEvent *esp_mqtt_event_handle_t;
static MockMqttClient sdk_clients[32];
static esp_mqtt_client_config_t captured_config;
static int init_count, start_count, stop_count, destroy_count, disconnect_count, reconnect_count;
static int sdk_disconnect_error, sdk_reconnect_error, sdk_stop_error, sdk_destroy_error, sdk_start_error;
static void (*sdk_handler)(void *, esp_event_base_t, int32_t, void *);
static void (*sdk_start_hook)(void);
static pthread_mutex_t test_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_cond = PTHREAD_COND_INITIALIZER;
static bool block_stop, stop_entered, allow_stop, block_publish, publish_entered, allow_publish;

static void sdk_emit(esp_mqtt_client_handle_t client, int id) {
    assert(client != NULL && client->alive);
    if (id == MQTT_EVENT_BEFORE_CONNECT) client->sdk_phase = 1;
    if (id == MQTT_EVENT_CONNECTED) client->sdk_phase = 2;
    if (id == MQTT_EVENT_DISCONNECTED) client->sdk_phase = 3;
    MockMqttEvent event = { .client = client, .event_id = id };
    bool previous_callback = in_sdk_callback;
    in_sdk_callback = true;
    sdk_handler(NULL, 0, id, &event);
    in_sdk_callback = previous_callback;
}
static esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config) {
    assert(!lock_depth); captured_config = *config;
    MockMqttClient *client = &sdk_clients[init_count++];
    client->alive = true; client->running = false; return client;
}
static esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, int id,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg) {
    (void)client; (void)id; (void)arg; sdk_handler = handler; return ESP_OK;
}
static esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client) {
    assert(!lock_depth && client->alive); start_count++;
    if (sdk_start_error) return ESP_FAIL;
    client->running = true; sdk_emit(client, MQTT_EVENT_BEFORE_CONNECT);
    if (sdk_start_hook) sdk_start_hook();
    return ESP_OK;
}
static esp_err_t esp_mqtt_client_disconnect(esp_mqtt_client_handle_t client) {
    assert(!lock_depth && client->alive && client->running); disconnect_count++;
    return sdk_disconnect_error ? ESP_FAIL : ESP_OK; // asynchronous; test dispatches event later
}
static esp_err_t esp_mqtt_client_reconnect(esp_mqtt_client_handle_t client) {
    assert(!lock_depth && client->alive && client->running); reconnect_count++;
    assert(client->sdk_phase == 3); // must wait for actual SDK DISCONNECTED state
    return sdk_reconnect_error ? ESP_FAIL : ESP_OK;
}
static esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client) {
    assert(!lock_depth && !in_sdk_callback && client->alive); stop_count++;
    if (sdk_stop_error) return ESP_FAIL;
    pthread_mutex_lock(&test_gate);
    stop_entered = true; pthread_cond_broadcast(&test_cond);
    while (block_stop && !allow_stop) pthread_cond_wait(&test_cond, &test_gate);
    pthread_mutex_unlock(&test_gate);
    // stopping SDK may dispatch final events while lifecycle mutex is held
    sdk_emit(client, MQTT_EVENT_DISCONNECTED);
    client->running = false; return ESP_OK;
}
static esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client) {
    assert(!lock_depth && client->alive && !client->running); destroy_count++;
    if (sdk_destroy_error) return ESP_FAIL;
    client->alive = false; return ESP_OK;
}
static int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int qos) {
    (void)topic; (void)qos; assert(client->alive && client->running); subscribe_count++; return 1;
}
static int esp_mqtt_client_publish(esp_mqtt_client_handle_t client, const char *topic,
    const char *data, int len, int qos, int retain) {
    (void)topic; (void)data; (void)len; (void)qos; (void)retain; assert(client->alive); publish_count++;
    pthread_mutex_lock(&test_gate);
    publish_entered = true; pthread_cond_broadcast(&test_cond);
    while (block_publish && !allow_publish) pthread_cond_wait(&test_cond, &test_gate);
    pthread_mutex_unlock(&test_gate);
    assert(client->alive); return 10;
}
'''

TESTS = r'''
static void reset(void) {
    sdk_stop_error = sdk_destroy_error = sdk_disconnect_error = sdk_reconnect_error = sdk_start_error = 0;
    if (s_client != NULL) mqtt_app_stop();
    memset(&s_mqtt_control, 0, sizeof(s_mqtt_control));
    atomic_store(&s_mqtt_connected, false);
    s_client_started = false; s_client_start_count = s_mqtt_connect_count = 0;
    Topic_queue = Data_queue = NULL;
    memset(&s_state, 0, sizeof(s_state)); s_state.initialized = true;
    eth_netif = &eth_mock; wifi_netif = &wifi_mock;
    s_monitor_task_handle = (void *)1;
    eth_mock.up = wifi_mock.up = true;
    eth_mock.ip.ip.addr = 200; wifi_mock.ip.ip.addr = 100;
    s_state.eth_link_raw = s_state.eth_up = s_state.eth_ready = s_state.wifi_up = true;
    s_state.eth_ip = eth_mock.ip;
    s_state.active_uplink = DUAL_NET_UPLINK_WIFI;
    s_uplink_change_callback = mqtt_app_on_uplink_changed;
    default_netif = &wifi_mock;
    now_us = 0; IsRoot = 1;
    init_count = start_count = stop_count = destroy_count = disconnect_count = reconnect_count = 0;
    queue_create_count = publish_count = subscribe_count = config_report_count = 0;
    sdk_start_hook = NULL;
    block_stop = stop_entered = allow_stop = block_publish = publish_entered = allow_publish = false;
}
static void start_wifi(void) {
    mqtt_app_on_uplink_changed(DUAL_NET_UPLINK_WIFI);
    assert(mqtt_init() == ESP_OK && mqtt_app_start() == ESP_OK);
    sdk_emit(s_client, MQTT_EVENT_CONNECTED);
    assert(atomic_load(&s_mqtt_connected));
}
static void set_route(dual_net_uplink_t uplink) {
    s_state.requested_uplink = uplink;
    s_state.revision++;
    assert(dual_net_update_route());
}
static void change_while_starting(void) { set_route(DUAL_NET_UPLINK_ETH); }
static void *stop_thread(void *arg) { (void)arg; mqtt_app_stop(); return NULL; }
static void *publish_thread(void *arg) { (void)arg; assert(app_mqtt_publish("test", "payload", NULL) == 10); return NULL; }
int main(void) {
    reset(); start_wifi();
    esp_mqtt_client_handle_t original = s_client;
    assert(captured_config.session.keepalive == 60 && captured_config.network.reconnect_timeout_ms == 0);
    set_route(DUAL_NET_UPLINK_ETH);
    assert(disconnect_count == 1 && reconnect_count == 0 && !atomic_load(&s_mqtt_connected));
    for (int i = 0; i < 5; i++) { assert(dual_net_update_route()); mqtt_app_process_uplink_reconnect(); }
    assert(disconnect_count == 1); // stable route/DNS/probe observation must not cycle MQTT
    sdk_emit(s_client, MQTT_EVENT_DISCONNECTED);
    assert(reconnect_count == 1);
    sdk_emit(s_client, MQTT_EVENT_BEFORE_CONNECT);
    sdk_emit(s_client, MQTT_EVENT_CONNECTED);
    assert(s_client == original && start_count == 1 && init_count == 1 && destroy_count == 0 && queue_create_count == 2);
    assert(atomic_load(&s_mqtt_connected) && subscribe_count == 8);
    set_route(DUAL_NET_UPLINK_WIFI);
    sdk_emit(s_client, MQTT_EVENT_DISCONNECTED);
    assert(disconnect_count == 2 && reconnect_count == 2);
    puts("PASS committed Ethernet/WiFi route forces single-client reconnect and restores subscriptions");

    reset(); start_wifi(); set_route(DUAL_NET_UPLINK_ETH); set_route(DUAL_NET_UPLINK_WIFI); set_route(DUAL_NET_UPLINK_ETH);
    assert(disconnect_count == 1);
    sdk_emit(s_client, MQTT_EVENT_DISCONNECTED);
    sdk_emit(s_client, MQTT_EVENT_BEFORE_CONNECT);
    assert(s_mqtt_control.attempt_generation == s_mqtt_control.route_generation);
    sdk_emit(s_client, MQTT_EVENT_CONNECTED);
    assert(reconnect_count == 1 && atomic_load(&s_mqtt_connected));
    puts("PASS rapid handovers coalesce to latest route");

    reset(); mqtt_app_on_uplink_changed(DUAL_NET_UPLINK_WIFI);
    assert(mqtt_init() == ESP_OK);
    sdk_start_hook = change_while_starting;
    assert(mqtt_app_start() == ESP_OK && disconnect_count == 0);
    sdk_emit(s_client, MQTT_EVENT_CONNECTED);
    assert(disconnect_count == 1 && subscribe_count == 0 && config_report_count == 0);
    sdk_emit(s_client, MQTT_EVENT_DISCONNECTED);
    sdk_emit(s_client, MQTT_EVENT_BEFORE_CONNECT);
    sdk_emit(s_client, MQTT_EVENT_CONNECTED);
    assert(atomic_load(&s_mqtt_connected) && subscribe_count == 4 && start_count == 1);
    puts("PASS route change during connect rejects stale success without reporting online");

    reset(); s_state.active_uplink = DUAL_NET_UPLINK_ETH;
    mqtt_app_on_uplink_changed(DUAL_NET_UPLINK_ETH);
    assert(mqtt_init() == ESP_OK && mqtt_app_start() == ESP_OK);
    sdk_emit(s_client, MQTT_EVENT_CONNECTED);
    assert(disconnect_count == 0 && reconnect_count == 0);
    puts("PASS wired startup connects once without unnecessary disconnect");

    reset(); start_wifi();
    sdk_emit(s_client, MQTT_EVENT_DISCONNECTED); // ordinary failure does not accelerate
    assert(reconnect_count == 0);
    set_route(DUAL_NET_UPLINK_ETH);
    assert(reconnect_count == 1 && disconnect_count == 0);
    sdk_emit(s_client, MQTT_EVENT_BEFORE_CONNECT);
    sdk_emit(s_client, MQTT_EVENT_DISCONNECTED); // broker still unavailable
    for (int i = 0; i < 5; i++) mqtt_app_process_uplink_reconnect();
    assert(reconnect_count == 1);
    puts("PASS waiting client accelerates once; failed new connection resumes normal backoff");

    reset(); start_wifi(); set_route(DUAL_NET_UPLINK_ETH);
    sdk_reconnect_error = 1;
    sdk_emit(s_client, MQTT_EVENT_DISCONNECTED);
    for (int i = 0; i < 5; i++) mqtt_app_process_uplink_reconnect();
    assert(reconnect_count == 1); // invalid SDK state cannot create retry storm
    puts("PASS rejected force-reconnect retains SDK automatic recovery without busy retries");

    reset(); start_wifi(); sdk_disconnect_error = 1;
    set_route(DUAL_NET_UPLINK_ETH);
    for (int i = 0; i < 10; i++) mqtt_app_process_uplink_reconnect();
    assert(disconnect_count == MQTT_UPLINK_DISCONNECT_RETRY_MAX);
    assert(!s_mqtt_control.switch_pending && init_count == 1);
    puts("PASS disconnect API failure is bounded and never destroys client");

    reset(); start_wifi();
    s_state.wifi_up = s_state.eth_up = s_state.eth_link_raw = false; s_state.revision++;
    assert(dual_net_update_route() && dual_net_get_active_uplink() == DUAL_NET_UPLINK_NONE);
    sdk_emit(s_client, MQTT_EVENT_DISCONNECTED);
    for (int i = 0; i < 5; i++) mqtt_app_process_uplink_reconnect();
    assert(disconnect_count == 0 && reconnect_count == 0);
    s_state.wifi_up = true; s_state.revision++;
    assert(dual_net_update_route());
    assert(reconnect_count == 1);
    puts("PASS no uplink waits; restored uplink immediately wakes reconnection");

    reset(); IsRoot = 0;
    mqtt_app_on_uplink_changed(DUAL_NET_UPLINK_ETH);
    assert(mqtt_init() == ESP_OK && mqtt_app_start() == ESP_OK);
    assert(captured_config.session.keepalive == 15 && captured_config.network.reconnect_timeout_ms == 2000);
    sdk_emit(s_client, MQTT_EVENT_CONNECTED);
    assert(subscribe_count > 4);
    sdk_emit(s_client, MQTT_EVENT_DISCONNECTED);
    sdk_emit(s_client, MQTT_EVENT_BEFORE_CONNECT);
    int previous_subscriptions = subscribe_count;
    sdk_emit(s_client, MQTT_EVENT_CONNECTED);
    assert(subscribe_count == previous_subscriptions * 2);
    assert(disconnect_count == 0 && reconnect_count == 0 && init_count == 1);
    puts("PASS child timing and automatic resubscription; no root handover control");

    reset(); start_wifi();
    SemaphoreHandle_t mutex = mqtt_lifecycle_mutex();
    assert(xSemaphoreTake(mutex, 0) == pdTRUE);
    set_route(DUAL_NET_UPLINK_ETH);
    assert(disconnect_count == 0 && s_mqtt_control.switch_pending);
    mqtt_lifecycle_unlock(mutex);
    mqtt_app_process_uplink_reconnect();
    assert(disconnect_count == 1);
    puts("PASS lifecycle contention defers handover without blocking network monitor");

    reset(); start_wifi(); block_stop = true;
    pthread_t stopper;
    assert(pthread_create(&stopper, NULL, stop_thread, NULL) == 0);
    pthread_mutex_lock(&test_gate);
    while (!stop_entered) pthread_cond_wait(&test_cond, &test_gate);
    pthread_mutex_unlock(&test_gate);
    set_route(DUAL_NET_UPLINK_ETH);
    assert(disconnect_count == 0 && reconnect_count == 0);
    pthread_mutex_lock(&test_gate); allow_stop = true; pthread_cond_broadcast(&test_cond); pthread_mutex_unlock(&test_gate);
    assert(pthread_join(stopper, NULL) == 0);
    assert(s_client == NULL && destroy_count == 1 && !atomic_load(&s_mqtt_connected));
    puts("PASS stop/route race: no callback lifecycle lock, deadlock or use-after-destroy");

    reset(); start_wifi(); block_publish = true;
    pthread_t publisher;
    assert(pthread_create(&publisher, NULL, publish_thread, NULL) == 0);
    pthread_mutex_lock(&test_gate);
    while (!publish_entered) pthread_cond_wait(&test_cond, &test_gate);
    pthread_mutex_unlock(&test_gate);
    assert(pthread_create(&stopper, NULL, stop_thread, NULL) == 0);
    pthread_mutex_lock(&test_gate);
    while (!stop_entered) pthread_cond_wait(&test_cond, &test_gate);
    assert(destroy_count == 0); // outstanding publisher still owns reference
    allow_publish = true; pthread_cond_broadcast(&test_cond);
    pthread_mutex_unlock(&test_gate);
    assert(pthread_join(publisher, NULL) == 0 && pthread_join(stopper, NULL) == 0);
    assert(s_client == NULL && destroy_count == 1 && s_mqtt_control.publish_refs == 0);
    puts("PASS in-flight publish drains before client destruction");

    reset(); start_wifi(); sdk_stop_error = 1;
    mqtt_app_stop(); assert(s_client != NULL && sdk_clients[0].alive && destroy_count == 0);
    set_route(DUAL_NET_UPLINK_ETH);
    assert(disconnect_count == 0 && reconnect_count == 0 && mqtt_app_start() == ESP_ERR_INVALID_STATE);
    sdk_stop_error = 0; mqtt_app_stop(); assert(s_client == NULL);
    reset(); sdk_start_error = 1;
    assert(mqtt_app_start() != ESP_OK && s_client == NULL);
    sdk_start_error = 0; assert(mqtt_app_start() == ESP_OK);
    puts("PASS failed stop retains handle safely; failed start rolls back for retry");

    mqtt_app_stop();
    puts("All MQTT handover regression scenarios passed");
    return 0;
}
'''


def main():
    # Use a real host critical-section mutex for concurrency cases, retaining lock-context assertions.
    mocks = MOCKS.replace('static int lock_depth;', '''static _Thread_local int lock_depth;
#include <pthread.h>
static pthread_mutex_t critical_mutex = PTHREAD_MUTEX_INITIALIZER;''')
    mocks = mocks.replace('lock_depth++; } while (0)', 'assert(pthread_mutex_lock(&critical_mutex) == 0); lock_depth++; } while (0)')
    mocks = mocks.replace('lock_depth--; } while (0)', 'lock_depth--; assert(pthread_mutex_unlock(&critical_mutex) == 0); } while (0)')
    mocks = mocks.replace('static void mqtt_app_process_uplink_reconnect(void) {}', 'void mqtt_app_process_uplink_reconnect(void);')
    mocks = mocks.replace('#endif\n', EXTRA_MOCKS + '\n#endif\n')
    with tempfile.TemporaryDirectory(prefix='mqtt-uplink-test-') as directory:
        temp = Path(directory)
        (temp / 'mocks.h').write_text(mocks)
        for header in ['esp_log.h', 'esp_event.h', 'esp_netif.h', 'esp_wifi.h', 'esp_bridge.h',
                       'esp_eth.h', 'esp_timer.h', 'esp_err.h', 'esp_system.h', 'esp_tls.h',
                       'mqtt_client.h', 'cJSON.h', 'esp_app_desc.h', 'appTask.h',
                       'mesh.h', 'task_config.h', 'ConfigData.h', 'log.h',
                       'freertos/FreeRTOS.h', 'freertos/task.h', 'freertos/semphr.h',
                       'lwip/sockets.h', 'lwip/inet.h']:
            path = temp / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "mocks.h"\n')
        control = function_source(MESH.read_text(), 'void mesh_root_manage_sta_reconnect(')
        unit = temp / 'test.c'
        unit.write_text(f'#include "{DUAL}"\n#define TAG mqtt_test_tag\n#include "{MQTT}"\n#undef TAG\n'
                        + 'static bool s_sta_reconnect_paused_by_eth;\n' + control + '\n' + TESTS)
        binary = temp / 'test'
        args = [os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-pthread',
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
                '-I', str(temp), '-I', str(MQTT.parent), '-I', str(DUAL.parent), str(unit), '-o', str(binary)]
        subprocess.run(args, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == '__main__':
    main()
