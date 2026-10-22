#!/usr/bin/env python3
"""Real restart service and Mesh control/event functions with host SDK mocks.

Run: python3 tests/test_system_restart.py (C compiler, pthread, local IDF cJSON).
Optional: ASAN_OPTIONS=detect_leaks=0 TEST_SANITIZE=1 python3 tests/test_system_restart.py
SDK radio/RTC internals and actual power loss still require hardware validation.
"""
from pathlib import Path
import os
import subprocess
import tempfile
from test_root_uplink import function_source

ROOT = Path(__file__).resolve().parents[1]
IDF = Path('/home/wbb/espidf/esp-idf-mesh-lite')
MESH = ROOT / 'main/Communication/Internet/Mesh/mesh.c'
RESTART = ROOT / 'main/APP/Restart/system_restart.c'
APP = ROOT / 'main/Task/appTask.c'

MOCKS = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <pthread.h>
#include <setjmp.h>
#include "cJSON.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_TIMEOUT 4
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((uint32_t)(ms))
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 1
#define RTC_NOINIT_ATTR
#define CONFIG_FREERTOS_UNICORE 0
#define DUAL_NET_ENABLE 0
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
static _Thread_local int critical_depth;
#define portENTER_CRITICAL(lock) do { assert(!critical_depth); pthread_mutex_lock(lock); critical_depth++; } while (0)
#define portEXIT_CRITICAL(lock) do { critical_depth--; pthread_mutex_unlock(lock); } while (0)
typedef pthread_mutex_t StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static _Thread_local bool control_owned;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *memory) {
    assert(pthread_mutex_init(memory,NULL)==0); return memory;
}
static int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) {
    (void)timeout; assert(!critical_depth);
    if(pthread_mutex_trylock(mutex)!=0) return 0;
    control_owned=true; return pdTRUE;
}
static int xSemaphoreGive(SemaphoreHandle_t mutex) {
    assert(control_owned); control_owned=false; return pthread_mutex_unlock(mutex)==0 ? pdTRUE : 0;
}
static atomic_llong now_us;
static int64_t esp_timer_get_time(void) { assert(!critical_depth); return now_us; }
static TickType_t xTaskGetTickCount(void) { return (uint64_t)now_us/1000; }
static void (*delay_hook)(void), (*disconnect_hook)(void);
static void vTaskDelay(TickType_t ticks) {
    assert(!critical_depth); now_us+=(int64_t)ticks*1000;
    if(delay_hook) {void (*hook)(void)=delay_hook;delay_hook=NULL;hook();}
}
static void mock_log(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;assert(!critical_depth);}
#define ESP_LOGI mock_log
#define ESP_LOGW mock_log
#define ESP_LOGE mock_log
#define ESP_LOGD mock_log
static const char *esp_err_to_name(esp_err_t error) {(void)error;return "mock";}
static int task_creates, task_create_error;
static atomic_int notifications;
static int app_create_restart_service_task(TaskFunction_t task, TaskHandle_t *out) {
    (void)task;assert(!critical_depth);task_creates++;
    if(task_create_error)return 0;*out=(void*)1;return pdPASS;
}
static void xTaskNotifyGive(TaskHandle_t task) {assert(task!=NULL && !critical_depth);notifications++;}
static unsigned ulTaskNotifyTake(int clear, TickType_t ticks) {(void)clear;vTaskDelay(ticks);return 0;}
static int IsRoot;
static atomic_uchar Network_Flag;
static uint8_t sdk_level=2;
static unsigned disconnect_calls, connect_calls, rtc_erases, restart_calls;
static esp_err_t disconnect_error;
static uint8_t esp_mesh_lite_get_level(void) {assert(!critical_depth);return sdk_level;}
static esp_err_t esp_mesh_lite_disconnect(void) {
    assert(!critical_depth && control_owned); disconnect_calls++;
    if(disconnect_hook)disconnect_hook();return disconnect_error;
}
static void esp_mesh_lite_connect(void) {assert(!critical_depth && control_owned);connect_calls++;}
static void esp_mesh_lite_erase_rtc_store(void) {assert(!critical_depth && control_owned);rtc_erases++;}
static jmp_buf restart_exit;
static bool expect_restart;
static void esp_restart(void) {
    assert(!critical_depth);restart_calls++;assert(expect_restart);longjmp(restart_exit,1);
}
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(mac) (mac)[0],(mac)[1],(mac)[2],(mac)[3],(mac)[4],(mac)[5]
typedef int esp_event_base_t;
#define IP_EVENT 1
#define WIFI_EVENT 2
#define IP_EVENT_STA_GOT_IP 3
#define IP_EVENT_STA_LOST_IP 4
#define WIFI_EVENT_STA_DISCONNECTED 5
#define WIFI_REASON_ASSOC_LEAVE 8
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) 1u,2u,3u,4u
typedef struct {uint32_t addr;} fake_ip_t;
typedef struct {struct {fake_ip_t ip;} ip_info;} ip_event_got_ip_t;
typedef struct {uint8_t reason,ssid_len,ssid[32],bssid[6];} wifi_event_sta_disconnected_t;
typedef struct {uint8_t bssid[6];int rssi;} wifi_ap_record_t;
static esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out) {
    *out=(wifi_ap_record_t){.bssid={1,2,3,4,5,6},.rssi=-50};return ESP_OK;
}
static void sntp_sync_task(void *arg) {(void)arg;}
static int xTaskCreatePinnedToCore(TaskFunction_t task,const char *name,int stack,void *arg,
    int priority,TaskHandle_t *out,int cpu) {
    (void)task;(void)name;(void)stack;(void)arg;(void)priority;(void)out;assert(cpu==1);return pdPASS;
}
static int reply_code,reply_seq,reply_calls;
static char reply_message[128];
static void device_response_publish_point(int seq,int code,const char *message) {
    reply_seq=seq;reply_code=code;reply_calls++;snprintf(reply_message,sizeof(reply_message),"%s",message);
}
'''

TESTS = r'''
static void reset_case(void) {
    if(control_owned)mesh_control_give();
    if(s_mesh_control_mutex)pthread_mutex_destroy(s_mesh_control_mutex);
    s_mesh_control_mutex=xSemaphoreCreateMutexStatic(&s_mesh_control_mutex_buf);
    s_initialized=s_planned=s_paused=s_recovery_committed=false;s_restart_task=NULL;
    s_plan=(system_restart_plan_t){0};s_pause_at_us=s_restart_at_us=0;
    s_disconnect_attempts=0;s_disconnected=false;s_disconnect_retry_at_us=0;
    s_parent_ip_online=false;s_parent_online_since=0;s_parent_fail_count=0;
    s_last_valid_level=0;s_local_disconnect_pending=false;
    memset(s_last_parent_bssid,0,6);s_heal_restart_count=0;
    s_sta_reconnect_paused_by_eth=false;s_trial_active=false;s_sntp_started=true;
    now_us=0;IsRoot=0;sdk_level=2;disconnect_calls=connect_calls=rtc_erases=restart_calls=0;
    disconnect_error=ESP_OK;task_create_error=task_creates=0;notifications=0;
    Network_Flag=0;expect_restart=false;delay_hook=disconnect_hook=NULL;reply_calls=0;
    assert(system_restart_init()==ESP_OK);
}
static void got_ip(void) {
    ip_event_got_ip_t event={0};network_event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&event);
}
static void disconnected(uint8_t reason) {
    wifi_event_sta_disconnected_t event={.reason=reason,.ssid_len=4,.ssid="mesh",.bssid={1,2,3,4,5,6}};
    network_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&event);
}
static void lost_ip(void) {network_event_handler(NULL,IP_EVENT,IP_EVENT_STA_LOST_IP,NULL);}
static void run_command(const char *json,int seq) {
    cJSON *object=cJSON_Parse(json);assert(object);app_handle_restart_command(object,seq);cJSON_Delete(object);
}
static void poll_restart(void) {
    expect_restart=true;
    if(setjmp(restart_exit)==0) {system_restart_poll();assert(!"restart expected");}
    expect_restart=false;
}
static void fast_restart(void) {
    expect_restart=true;
    if(setjmp(restart_exit)==0) {mesh_heal_connect_fail_fast_path();assert(!"restart expected");}
    expect_restart=false;
    if(control_owned)mesh_control_give();
}
static void accept_during_recovery(void) {
    system_restart_plan_t plan;assert(system_restart_schedule(88,&plan)==ESP_OK);
}
static void recover_during_delay(void) {got_ip();}
static void local_disconnect_event(void) {disconnected(WIFI_REASON_ASSOC_LEAVE);}
static void *parallel_schedule(void *arg) {
    system_restart_plan_t plan;assert(system_restart_schedule((intptr_t)arg,&plan)==ESP_OK);return NULL;
}
int main(void) {
    system_restart_plan_t plan;
    reset_case();s_initialized=false;s_restart_task=NULL;task_creates=0;task_create_error=1;
    assert(system_restart_init()==ESP_ERR_NO_MEM);
    assert(system_restart_schedule(1,&plan)==ESP_ERR_INVALID_STATE && !s_planned);
    task_create_error=0;assert(system_restart_init()==ESP_OK);assert(system_restart_init()==ESP_OK);
    assert(task_creates==2);assert(system_restart_schedule(-1,&plan)==ESP_ERR_INVALID_ARG);
    assert(system_restart_schedule(1,NULL)==ESP_ERR_INVALID_ARG);
    puts("PASS failed task creation rejects commands; initialization retries once and remains idempotent");

    for(unsigned level=1;level<=6;level++) {
        reset_case();IsRoot=level==1;sdk_level=level;
        assert(system_restart_schedule(123,&plan)==ESP_OK);
        assert(plan.level==level && plan.delay_seconds==level*30);
        assert(system_restart_is_planned() && !system_restart_mesh_paused());
        now_us=29999999;system_restart_poll();assert(disconnect_calls==0 && restart_calls==0);
        now_us=30000000;
        if(level==1)poll_restart();else {
            system_restart_poll();assert(s_paused && disconnect_calls==1 && restart_calls==0);
            now_us=(int64_t)level*30000000-1;system_restart_poll();assert(!restart_calls);
            now_us++;poll_restart();
        }
        assert(restart_calls==1 && disconnect_calls==1 && rtc_erases==0);
    }
    puts("PASS all six layers keep the 30-second window, pause once, then restart root-first at 30-second intervals");

    reset_case();IsRoot=1;sdk_level=0;assert(mesh_get_restart_level()==1);
    IsRoot=0;assert(mesh_get_restart_level()==6);sdk_level=3;assert(mesh_get_current_level()==3);
    sdk_level=0;assert(mesh_get_restart_level()==3);sdk_level=1;assert(mesh_get_restart_level()==3);
    sdk_level=9;assert(mesh_get_restart_level()==3);
    puts("PASS root level is fixed; disconnected children use their last valid level or maximum fallback");

    reset_case();sdk_level=3;assert(system_restart_schedule(10,&plan)==ESP_OK);
    int64_t first_deadline=s_restart_at_us;now_us=20000000;sdk_level=0;
    assert(system_restart_schedule(10,&plan)==ESP_OK);assert(system_restart_schedule(11,&plan)==ESP_OK);
    assert(plan.command_seq==10 && plan.level==3 && s_restart_at_us==first_deadline && s_pause_at_us==30000000);
    now_us=30000000;system_restart_poll();sdk_level=5;Network_Flag=0;
    assert(system_restart_schedule(12,&plan)==ESP_OK && s_restart_at_us==first_deadline);
    puts("PASS duplicate/new sequences, network loss and level changes cannot move an accepted deadline");

    reset_case();const char *invalid[]={"{}","{\"Data\":null}","{\"Data\":{}}",
        "{\"Data\":{\"Restart\":true}}","{\"Data\":{\"Restart\":\"1\"}}",
        "{\"Data\":{\"Restart\":0}}","{\"Data\":{\"Restart\":1.5}}",
        "{\"Data\":{\"Restart\":257}}","{\"Data\":{\"Restart\":-255}}"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        run_command(invalid[i],i);assert(reply_code==0 && !s_planned);
    }
    run_command("{\"Data\":{\"Restart\":1}}",99);assert(reply_code==1 && reply_seq==99 && s_plan.command_seq==99);
    assert(strstr(reply_message,"level=2") && strstr(reply_message,"restart=60s"));
    now_us=10000000;run_command("{\"Data\":{\"Restart\":1}}",100);
    assert(reply_seq==100 && strstr(reply_message,"plan_seq=99") && s_restart_at_us==60000000);
    puts("PASS actual MQTT handler validates exact numeric 1 and acknowledges accepted/original plans without waiting");

    reset_case();s_initialized=false;run_command("{\"Data\":{\"Restart\":1}}",1);
    assert(reply_code==0 && !s_planned && !restart_calls);
    puts("PASS unavailable restart service produces a negative receipt rather than synchronous reboot");

    reset_case();sdk_level=3;disconnect_error=ESP_FAIL;assert(system_restart_schedule(1,&plan)==ESP_OK);
    now_us=30000000;system_restart_poll();assert(disconnect_calls==1);
    now_us+=999999;system_restart_poll();assert(disconnect_calls==1);
    now_us++;system_restart_poll();assert(disconnect_calls==2);
    now_us+=1000000;system_restart_poll();assert(disconnect_calls==3);
    now_us=80000000;system_restart_poll();assert(disconnect_calls==3);
    now_us=90000000;poll_restart();assert(restart_calls==1 && rtc_erases==0);
    puts("PASS disconnect errors retry at most three times and never extend the reboot deadline");

    reset_case();assert(system_restart_schedule(1,&plan)==ESP_OK);
    assert(mesh_control_take());now_us=30000000;system_restart_poll();
    assert(s_paused && disconnect_calls==0 && s_disconnect_attempts==1);mesh_control_give();
    now_us+=1000000;system_restart_poll();assert(disconnect_calls==1 && s_disconnected);
    puts("PASS pause waits behind in-flight application control with a bounded retry");

    reset_case();IsRoot=1;assert(system_restart_schedule(1,&plan)==ESP_OK);
    mesh_root_manage_sta_reconnect(true,false);assert(disconnect_calls==1);
    mesh_root_manage_sta_reconnect(false,false);assert(connect_calls==1);
    now_us=30000000;poll_restart();unsigned connections=connect_calls;
    mesh_root_manage_sta_reconnect(true,false);mesh_root_manage_sta_reconnect(false,false);
    assert(connect_calls==connections && rtc_erases==0);
    puts("PASS wired arbitration remains active during propagation but cannot reconnect after planned pause");

    reset_case();for(int i=0;i<3;i++)disconnected(202);assert(s_parent_fail_count==3);
    got_ip();assert(s_parent_fail_count==3);now_us=1000000;disconnected(5);lost_ip();
    assert(s_parent_fail_count==4 && !s_parent_ip_online);
    now_us=50000000;disconnected(202);assert(s_parent_fail_count==5);
    puts("PASS brief GOT_IP does not clear failures, LOST_IP does not double count, stale online time cannot reset failures");

    reset_case();s_parent_fail_count=4;got_ip();now_us=9999000;mesh_parent_stability_poll(xTaskGetTickCount());
    assert(s_parent_fail_count==4);now_us=10000000;mesh_parent_stability_poll(xTaskGetTickCount());
    assert(s_parent_fail_count==0);disconnected(5);assert(s_parent_fail_count==1);
    now_us=90000000;disconnected(5);assert(s_parent_fail_count==2);
    puts("PASS ten continuous online seconds reset once; later offline failures accumulate normally");

    reset_case();s_parent_fail_count=4;got_ip();now_us=5000000;got_ip();
    now_us=10000000;disconnected(5);assert(s_parent_fail_count==1);
    puts("PASS DHCP renewal preserves stable start and a disconnect at the stability boundary starts a new failure run");

    reset_case();disconnect_hook=local_disconnect_event;assert(mesh_control_take());
    assert(mesh_disconnect_locked()==ESP_OK);mesh_control_give();assert(s_parent_fail_count==0);
    disconnect_hook=NULL;disconnected(WIFI_REASON_ASSOC_LEAVE);assert(s_parent_fail_count==1);
    assert(system_restart_schedule(1,&plan)==ESP_OK);disconnected(202);lost_ip();assert(s_parent_fail_count==1);
    puts("PASS local intentional leave and planned outages are excluded without ignoring unsolicited disconnects");

    reset_case();s_parent_fail_count=5;Network_Flag=1;mesh_heal_connect_fail_fast_path();assert(!restart_calls);
    Network_Flag=0;assert(system_restart_schedule(1,&plan)==ESP_OK);
    mesh_heal_connect_fail_fast_path();mesh_heal_watchdog(false,9999999);
    mesh_failover_restart("test");assert(!restart_calls && !rtc_erases && !disconnect_calls);
    puts("PASS healthy IP or an accepted plan suppresses child fast recovery, watchdog and root failover restart");

    reset_case();s_parent_fail_count=5;delay_hook=accept_during_recovery;
    mesh_heal_connect_fail_fast_path();assert(s_planned && !rtc_erases && !restart_calls && !s_recovery_committed);
    puts("PASS a plan accepted during recovery guard wins before RTC erasure or reboot commitment");

    reset_case();s_parent_fail_count=5;delay_hook=recover_during_delay;
    mesh_heal_connect_fail_fast_path();assert(Network_Flag && !rtc_erases && !s_recovery_committed);
    puts("PASS a link recovered during the final guard cancels automatic reboot");

    reset_case();s_parent_fail_count=5;fast_restart();
    assert(rtc_erases==1 && restart_calls==1 && s_heal_restart_count==1 && s_recovery_committed);
    assert(system_restart_schedule(2,&plan)==ESP_ERR_INVALID_STATE);
    for(unsigned reboot=2;reboot<=3;reboot++) {
        s_recovery_committed=false;s_parent_fail_count=5;fast_restart();assert(s_heal_restart_count==reboot);
    }
    s_recovery_committed=false;s_parent_fail_count=5;mesh_heal_connect_fail_fast_path();
    assert(rtc_erases==3 && restart_calls==3 && s_heal_restart_count==3 && s_parent_fail_count==0);
    puts("PASS child recovery erases only Mesh RTC before reboot and preserves the three-reboot power-cycle budget");

    reset_case();now_us=1000;mesh_heal_watchdog(false,xTaskGetTickCount());
    for(int i=0;i<3;i++){now_us+=180000000;mesh_heal_watchdog(false,xTaskGetTickCount());}
    assert(disconnect_calls==3 && connect_calls==3 && !rtc_erases);
    now_us+=180000000;expect_restart=true;
    if(setjmp(restart_exit)==0){mesh_heal_watchdog(false,xTaskGetTickCount());assert(!"watchdog restart expected");}
    expect_restart=false;mesh_control_give();assert(rtc_erases==1 && s_heal_restart_count==1);
    puts("PASS exhausted soft recovery uses the same clean-record reboot path as consecutive-failure recovery");

    reset_case();pthread_t threads[4];for(int i=0;i<4;i++)assert(pthread_create(&threads[i],NULL,parallel_schedule,(void*)(intptr_t)(10+i))==0);
    for(int i=0;i<4;i++)assert(pthread_join(threads[i],NULL)==0);
    assert(s_plan.command_seq>=10 && s_plan.command_seq<=13 && s_restart_at_us==60000000 && task_creates==1);
    puts("PASS concurrent schedules produce a single immutable plan and reuse the one service task");
    return 0;
}
'''


def main():
    mesh = MESH.read_text()
    prefix = mesh[mesh.index('#define MESH_MAX_LEVEL'):mesh.index('/* 只有 net_monitor')]
    names = ['void mesh_root_manage_sta_reconnect(', 'uint8_t mesh_get_current_level(',
             'uint8_t mesh_get_restart_level(', 'esp_err_t mesh_pause_for_restart(',
             'static void mesh_update_network_flag(', 'static void mesh_parent_online_changed(',
             'static void mesh_parent_stability_poll(', 'static void mesh_restart_without_parent(',
             'static void mesh_heal_watchdog(', 'static void mesh_heal_connect_fail_fast_path(',
             'static void note_parent_connect_failure(', 'static void network_event_handler(',
             'static void mesh_failover_restart(']
    functions = '\n'.join(function_source(mesh, name) for name in names)
    handler = function_source(APP.read_text(), 'static void app_handle_restart_command(')
    with tempfile.TemporaryDirectory(prefix='system-restart-test-') as directory:
        tmp = Path(directory)
        (tmp / 'mocks.h').write_text(MOCKS)
        for name in ['esp_err.h', 'esp_log.h', 'esp_system.h', 'esp_timer.h',
                     'freertos/FreeRTOS.h', 'freertos/task.h', 'sdkconfig.h', 'appTask.h']:
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "mocks.h"\n')
        (tmp / 'task_config.h').write_text(f'#include "{ROOT / "main/Task/task_config.h"}"\n')
        (tmp / 'mesh.h').write_text(f'#include "{ROOT / "main/Communication/Internet/Mesh/mesh.h"}"\n')
        unit = tmp / 'test.c'
        unit.write_text(f'#include "mocks.h"\n#include "system_restart.h"\n#include "task_config.h"\n'
                        + prefix + '\nstatic bool s_sta_reconnect_paused_by_eth;\n' + functions
                        + f'\n#define TAG restart_test_tag\n#include "{RESTART}"\n#undef TAG\n'
                        + handler + '\n' + TESTS)
        cjson = IDF / 'components/json/cJSON'
        args = [os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-pthread',
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
                '-Wno-unused-variable', '-Wno-misleading-indentation',
                '-I', str(tmp), '-I', str(RESTART.parent), '-I', str(cjson),
                str(unit), str(cjson / 'cJSON.c'), '-lm', '-o', str(tmp / 'test')]
        if os.environ.get('TEST_SANITIZE') == '1':
            args[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(args, check=True)
        subprocess.run([str(tmp / 'test')], check=True, timeout=30)


if __name__ == '__main__':
    main()
