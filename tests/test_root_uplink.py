#!/usr/bin/env python3
"""Host regression of the actual dual_net.c and Mesh STA control against SDK mocks.
Run: python3 tests/test_root_uplink.py (requires a C compiler).
No ESP-IDF checkout or device required; this does not validate Mesh-Lite binary internals.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DUAL = ROOT / 'main/Communication/Internet/dual_net/dual_net.c'
MESH = ROOT / 'main/Communication/Internet/Mesh/mesh.c'


def function_source(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    cursor = opening + 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor]


MOCKS = r'''
#ifndef MOCKS_H
#define MOCKS_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <fcntl.h>
#include <unistd.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 5
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 6
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define LG_STACK_NET_MONITOR 3072
#define LG_PRIO_NET_MONITOR 3
#define LG_APP_CPU_CORE 1
static int lock_depth;
#define portENTER_CRITICAL(lock) do { (void)(lock); assert(lock_depth == 0); lock_depth++; } while (0)
#define portEXIT_CRITICAL(lock) do { (void)(lock); assert(lock_depth == 1); lock_depth--; } while (0)
typedef struct { uint32_t addr; } ip_addr_mock;
typedef struct { ip_addr_mock ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct { const char *key; bool up; esp_netif_ip_info_t ip; } esp_netif_t;
typedef struct { esp_netif_t *esp_netif; esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
typedef struct { uint8_t reason; } wifi_event_sta_disconnected_t;
typedef int esp_event_base_t;
typedef void *esp_event_handler_instance_t;
#define ETH_EVENT 1
#define IP_EVENT 2
#define WIFI_EVENT 3
#define ESP_EVENT_ANY_ID -1
#define ETHERNET_EVENT_CONNECTED 10
#define ETHERNET_EVENT_DISCONNECTED 11
#define ETHERNET_EVENT_STOP 12
#define IP_EVENT_ETH_GOT_IP 20
#define IP_EVENT_ETH_LOST_IP 21
#define IP_EVENT_STA_GOT_IP 22
#define IP_EVENT_STA_LOST_IP 23
#define WIFI_EVENT_STA_DISCONNECTED 30
#define WIFI_EVENT_STA_CONNECTED 31
#define WIFI_EVENT_STA_START 32
#define WIFI_REASON_NO_AP_FOUND 201
#define WIFI_REASON_AUTH_FAIL 202
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) ((ip)->addr & 255), (((ip)->addr >> 8) & 255), (((ip)->addr >> 16) & 255), (((ip)->addr >> 24) & 255)
static void mock_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; assert(!lock_depth); }
#define ESP_LOGI mock_log
#define ESP_LOGW mock_log
#define ESP_LOGE mock_log
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock"; }
static int IsRoot = 1;
static volatile uint8_t Network_Flag;
static char *SERVER_IP = "192.168.1.65";
static int SERVER_UDP_Port = 1883;
static int64_t now_us;
static int64_t esp_timer_get_time(void) { assert(!lock_depth); return now_us; }
static TickType_t xTaskGetTickCount(void) { assert(!lock_depth); return now_us / 1000; }
static int wake_count, route_count, route_error, register_count, unregister_count, register_fail_at;
static bool task_create_fail, tcp_reachable = true;
static esp_netif_t eth_mock = { .key = "ETH_WAN" }, wifi_mock = { .key = "WIFI_STA_DEF" };
static esp_netif_t *default_netif, *dns_netif;
static void (*route_hook)(void), (*connect_hook)(void), (*read_ip_hook)(void), (*wait_hook)(void);
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    assert(!lock_depth);
    return strcmp(key, "ETH_WAN") == 0 ? &eth_mock : &wifi_mock;
}
static bool esp_netif_is_netif_up(esp_netif_t *netif) { assert(!lock_depth); return netif && netif->up; }
static esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip) {
    assert(!lock_depth); *ip = netif->ip;
    if (read_ip_hook) { void (*hook)(void) = read_ip_hook; read_ip_hook = NULL; hook(); }
    return ESP_OK;
}
static const char *esp_netif_get_ifkey(esp_netif_t *netif) { return netif->key; }
static esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *netif, char *name) { (void)netif; strcpy(name, "eth0"); return ESP_OK; }
static esp_err_t esp_netif_set_default_netif(esp_netif_t *netif) {
    assert(!lock_depth); route_count++;
    if (route_error) return ESP_FAIL;
    default_netif = netif;
    if (route_hook) { void (*hook)(void) = route_hook; route_hook = NULL; hook(); }
    return ESP_OK;
}
static esp_netif_t *esp_netif_get_default_netif(void) { return default_netif; }
static esp_err_t esp_bridge_update_dns_info(esp_netif_t *netif, esp_netif_t *forward) {
    (void)forward; assert(!lock_depth); dns_netif = netif; return ESP_OK;
}
static esp_err_t esp_netif_dhcpc_stop(esp_netif_t *netif) { (void)netif; assert(!lock_depth); return ESP_OK; }
static esp_err_t esp_netif_dhcpc_start(esp_netif_t *netif) { (void)netif; assert(!lock_depth); return ESP_OK; }
static esp_err_t esp_wifi_connect(void) { assert(!lock_depth); return ESP_OK; }
static esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int id,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg, esp_event_handler_instance_t *instance) {
    (void)base; (void)id; (void)handler; (void)arg; assert(!lock_depth);
    if (++register_count == register_fail_at) return ESP_FAIL;
    *instance = (void *)(uintptr_t)register_count; return ESP_OK;
}
static esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int id, esp_event_handler_instance_t instance) {
    (void)base; (void)id; (void)instance; assert(!lock_depth); unregister_count++; return ESP_OK;
}
static BaseType_t xTaskCreatePinnedToCore(void (*task)(void *), const char *name, int stack, void *arg,
    int priority, TaskHandle_t *handle, int core) {
    (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)core; assert(!lock_depth);
    if (task_create_fail) return 0;
    *handle = (void *)1; return pdPASS;
}
static void xTaskNotifyGive(TaskHandle_t task) { (void)task; assert(!lock_depth); wake_count++; }
static jmp_buf worker_exit;
static int wait_count, max_wait_count;
static unsigned ulTaskNotifyTake(int clear, TickType_t timeout) {
    (void)clear; assert(!lock_depth);
    if (++wait_count >= max_wait_count) longjmp(worker_exit, 1);
    if (timeout != portMAX_DELAY) now_us += (int64_t)timeout * 1000;
    if (wait_hook) wait_hook();
    return 1;
}
static void storage_write_record_cyclic(const char *pn, const char *message) { (void)pn; (void)message; }
static void mqtt_app_process_uplink_reconnect(void) {}
static unsigned tcp_attempts, mesh_pause_count, mesh_resume_count;
static int mesh_disconnect_error;
static esp_err_t esp_mesh_lite_disconnect(void) { assert(!lock_depth); mesh_pause_count++; return mesh_disconnect_error; }
static void esp_mesh_lite_connect(void) { assert(!lock_depth); mesh_resume_count++; }
/* 此脚本只验证有线仲裁；完整重启状态/互斥在 test_system_restart.py 验证。 */
static bool mesh_control_take(void) { return true; }
static void mesh_control_give(void) {}
static bool restart_paused;
static bool system_restart_mesh_paused(void) { return restart_paused; }
static esp_err_t mesh_disconnect_locked(void) { return esp_mesh_lite_disconnect(); }
static void mesh_root_manage_sta_reconnect(bool wired_preferred, bool sta_has_ip);
/* Mock only socket I/O; use the real application's timeout/cleanup and probe counters. */
static int mock_socket(int domain, int type, int protocol) { (void)domain; (void)type; (void)protocol; return 10; }
static int mock_setsockopt(int fd, int level, int opt, const void *val, socklen_t len) { (void)fd; (void)level; (void)opt; (void)val; (void)len; return 0; }
static int mock_bind(int fd, const struct sockaddr *addr, socklen_t len) { (void)fd; (void)addr; (void)len; return 0; }
static int mock_fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }
static int mock_close(int fd) { (void)fd; return 0; }
static int mock_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)fd; (void)addr; (void)len; tcp_attempts++;
    if (connect_hook) { void (*hook)(void) = connect_hook; connect_hook = NULL; hook(); }
    if (tcp_reachable) return 0;
    errno = ECONNREFUSED; return -1;
}
#define socket mock_socket
#define setsockopt mock_setsockopt
#define bind mock_bind
#define fcntl mock_fcntl
#define close mock_close
#define connect mock_connect
#endif
'''

TESTS = r'''
static void reset(void) {
    restart_paused = false;
    memset(&s_state, 0, sizeof(s_state));
    s_state.initialized = true;
    eth_netif = &eth_mock; wifi_netif = &wifi_mock;
    eth_mock.up = wifi_mock.up = false;
    eth_mock.ip = wifi_mock.ip = (esp_netif_ip_info_t){0};
    s_monitor_task_handle = (void *)1;
    s_network_ready_callback = NULL; s_uplink_change_callback = NULL;
    s_sta_reconnect_paused_by_eth = false;
    Network_Flag = 0; default_netif = dns_netif = NULL;
    now_us = 0; wake_count = route_count = route_error = 0;
    register_count = unregister_count = register_fail_at = 0;
    task_create_fail = false; tcp_reachable = true;
    tcp_attempts = mesh_pause_count = mesh_resume_count = 0;
    mesh_disconnect_error = 0; wait_count = 0;
    route_hook = connect_hook = read_ip_hook = wait_hook = NULL;
    IsRoot = 1;
}
static void wifi_got_ip(void) {
    wifi_mock.up = true; wifi_mock.ip.ip.addr = 100;
    ip_event_got_ip_t event = { .esp_netif = &wifi_mock, .ip_info = wifi_mock.ip };
    dual_net_ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &event);
}
static void eth_got_ip(void) {
    eth_mock.up = true; eth_mock.ip.ip.addr = 200; eth_mock.ip.gw.addr = 254;
    ip_event_got_ip_t event = { .esp_netif = &eth_mock, .ip_info = eth_mock.ip };
    dual_net_ip_event_handler(NULL, IP_EVENT, IP_EVENT_ETH_GOT_IP, &event);
}
static void wifi_down(void) {
    wifi_mock.up = false;
    wifi_event_sta_disconnected_t event = { .reason = WIFI_REASON_NO_AP_FOUND };
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
}
static void eth_down(void) {
    eth_mock.up = false;
    eth_event_handler(NULL, ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, NULL);
}
static void reconcile(void) {
    assert(dual_net_update_route());
    dual_net_publish_network_flag();
    DualNetState state = dual_net_snapshot();
    mesh_root_manage_sta_reconnect(dual_net_eth_online(&state) &&
        state.active_uplink == DUAL_NET_UPLINK_ETH &&
        state.probe_fail_count < LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT, state.wifi_up);
    if (state.active_uplink != DUAL_NET_UPLINK_NONE) assert(dual_net_refresh_dns_to_softap() == ESP_OK);
}
static void run_worker(unsigned seconds) {
    max_wait_count = seconds + 2;
    if (setjmp(worker_exit) == 0) dual_net_monitor_task(NULL);
}
static void wifi_arrives_after_none_commit(dual_net_uplink_t target) {
    if (target == DUAL_NET_UPLINK_NONE) wifi_got_ip();
}
int main(void) {
    reset();
    wifi_got_ip();
    assert(route_count == 0); // event callback must not switch route
    reconcile();
    assert(Network_Flag == 1 && dual_net_is_wifi_active());
    eth_got_ip(); reconcile();
    assert(dual_net_is_wifi_active() && dual_net_has_eth_ip());
    dual_net_process_uplink_probe(); reconcile();
    assert(!dual_net_is_eth_uplink_ready() && dual_net_is_wifi_active());
    dual_net_process_uplink_probe(); reconcile();
    assert(dual_net_is_ethernet_active() && dns_netif == &eth_mock && mesh_pause_count == 0);
    wifi_down(); reconcile();
    assert(Network_Flag == 1 && dual_net_is_ethernet_active() && mesh_pause_count == 1);
    wifi_down(); reconcile();
    assert(mesh_pause_count == 1); // generated/repeated disconnect is idempotent
    eth_down(); reconcile();
    assert(Network_Flag == 0 && mesh_resume_count == 1 && !dual_net_has_eth_ip());
    wifi_got_ip(); reconcile();
    assert(dual_net_is_wifi_active() && dns_netif == &wifi_mock);
    puts("PASS WiFi -> validated Ethernet -> WiFi off -> Ethernet off -> WiFi recovery");

    reset(); eth_got_ip(); reconcile();
    assert(Network_Flag == 1 && dual_net_is_ethernet_active() && !dual_net_is_eth_uplink_ready());
    assert(mesh_pause_count == 1); // wired-only startup must not wait for two probes
    puts("PASS wired-only startup before probe confirmation");

    dual_net_process_uplink_probe(); dual_net_process_uplink_probe();
    uint32_t generation = s_state.eth_generation;
    eth_got_ip();
    assert(s_state.eth_generation == generation && dual_net_is_eth_uplink_ready());
    eth_mock.ip.ip.addr++;
    ip_event_got_ip_t changed = { .esp_netif = &eth_mock, .ip_info = eth_mock.ip };
    dual_net_ip_event_handler(NULL, IP_EVENT, IP_EVENT_ETH_GOT_IP, &changed);
    assert(s_state.eth_generation != generation && !dual_net_is_eth_uplink_ready());
    puts("PASS same-IP renewal retains confirmation; new IP invalidates it");

    reset(); eth_got_ip(); reconcile();
    connect_hook = eth_down;
    dual_net_process_uplink_probe();
    assert(!dual_net_has_eth_ip() && s_state.probe_ok_count == 0);
    reconcile(); assert(mesh_resume_count == 1);
    puts("PASS discard probe result after cable removed during connect");

    reset(); eth_got_ip(); reconcile();
    dual_net_process_uplink_probe(); dual_net_process_uplink_probe(); reconcile();
    tcp_reachable = false;
    for (int i = 0; i < LG_DUAL_NET_UPLINK_PROBE_FAIL_COUNT - 1; i++) {
        dual_net_process_uplink_probe(); reconcile();
        assert(mesh_resume_count == 0 && dual_net_is_eth_uplink_ready());
    }
    dual_net_process_uplink_probe(); reconcile();
    assert(mesh_resume_count == 1 && !dual_net_is_eth_uplink_ready() && Network_Flag == 1);
    assert(dual_net_get_uplink_state() == DUAL_NET_UPLINK_STATE_STALE);
    wifi_got_ip(); reconcile(); assert(dual_net_is_wifi_active());
    puts("PASS probe failure threshold restores STA without clearing valid Ethernet IP");

    reset(); wifi_got_ip(); reconcile();
    eth_got_ip(); s_state.eth_ready = true;
    route_error = 1;
    assert(!dual_net_update_route() && dual_net_is_wifi_active());
    route_error = 0; reconcile(); assert(dual_net_is_ethernet_active());
    route_hook = eth_down;
    assert(!dual_net_update_route() && !dual_net_is_ethernet_active());
    reconcile(); assert(dual_net_is_wifi_active());
    puts("PASS failed/stale route commit is not reported as active Ethernet");

    reset();
    s_state.active_uplink = DUAL_NET_UPLINK_ETH;
    s_uplink_change_callback = wifi_arrives_after_none_commit;
    run_worker(2);
    assert(dual_net_is_wifi_active() && Network_Flag == 1 && default_netif == &wifi_mock);
    puts("PASS event during committed-route callback is reconciled on next iteration");

    reset(); wifi_got_ip(); wifi_down();
    ip_event_got_ip_t stale_wifi = { .esp_netif = &wifi_mock, .ip_info = wifi_mock.ip };
    dual_net_ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &stale_wifi);
    assert(!s_state.wifi_up);
    puts("PASS late GOT_IP cannot revive a disconnected STA");

    reset(); s_state.initialized = false; s_monitor_task_handle = NULL;
    eth_mock.up = true; eth_mock.ip.ip.addr = 200;
    read_ip_hook = wifi_got_ip; // WiFi event during boot snapshot must not erase Ethernet
    assert(dual_net_init() == ESP_OK && dual_net_has_eth_ip());
    assert(s_state.wifi_up);
    reconcile(); assert(Network_Flag == 1);
    puts("PASS boot snapshot survives concurrent WiFi event");

    reset(); s_state.initialized = false; s_monitor_task_handle = NULL;
    register_fail_at = 2;
    assert(dual_net_init() != ESP_OK && !dual_net_is_initialized() && unregister_count == 1);
    reset(); s_state.initialized = false; s_monitor_task_handle = NULL; task_create_fail = true;
    assert(dual_net_init() != ESP_OK && !dual_net_is_initialized() && unregister_count == 3);
    puts("PASS initialization failure unregisters handlers and preserves fallback eligibility");

    reset(); mesh_disconnect_error = ESP_FAIL; eth_got_ip(); reconcile();
    wifi_down(); reconcile(); assert(mesh_pause_count == 1);
    eth_down(); reconcile(); assert(mesh_resume_count == 1);
    reset(); IsRoot = 0;
    s_state.initialized = false; s_monitor_task_handle = NULL;
    assert(dual_net_init() == ESP_OK && register_count == 0 && s_monitor_task_handle == NULL);
    mesh_root_manage_sta_reconnect(true, false);
    assert(mesh_pause_count == 0 && mesh_resume_count == 0);
    puts("PASS partial disconnect error still resumes; child ignores wired control");

    reset(); eth_got_ip(); wait_hook = wifi_down;
    run_worker(65);
    assert(tcp_attempts == 2 && mesh_pause_count == 1 && Network_Flag == 1);
    assert(dual_net_is_ethernet_active() && dual_net_is_eth_uplink_ready());
    puts("PASS event bursts wake monitor without accelerating 60-second probe cadence");
    return 0;
}
'''


def main():
    mesh = MESH.read_text()
    control = function_source(mesh, 'void mesh_root_manage_sta_reconnect(')
    with tempfile.TemporaryDirectory(prefix='root-uplink-test-') as directory:
        temp = Path(directory)
        (temp / 'mocks.h').write_text(MOCKS)
        for header in ['esp_log.h', 'esp_event.h', 'esp_netif.h', 'esp_wifi.h',
                       'esp_bridge.h', 'esp_eth.h', 'esp_timer.h', 'esp_err.h',
                       'mesh.h', 'mqtt_app.h', 'task_config.h', 'ConfigData.h', 'log.h',
                       'freertos/FreeRTOS.h', 'freertos/task.h', 'lwip/sockets.h', 'lwip/inet.h']:
            path = temp / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "mocks.h"\n')
        unit = temp / 'test.c'
        unit.write_text(f'#include "{DUAL}"\nstatic bool s_sta_reconnect_paused_by_eth;\n'
                        + control + '\n' + TESTS)
        binary = temp / 'test'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-Wno-unused-parameter', '-I', str(temp), str(unit), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
