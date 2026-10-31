#!/usr/bin/env python3
"""真实 BLE 目标解析/NVS、白名单控制与命令接入回归，无需设备。
python3 tests/test_ble_target.py；TEST_SANITIZE=1 可启用 ASan/UBSan。
"""
import os
from pathlib import Path
import subprocess
import tempfile
from test_root_uplink import function_source

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', '/home/wbb/espidf/esp-idf-mesh-lite'))
MOCKS = r'''
#ifndef BLE_TEST_MOCKS_H
#define BLE_TEST_MOCKS_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "cJSON.h"
typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NVS_NOT_FOUND 4
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_BD_ADDR_LEN 6
#define ESP_BD_ADDR_STR "%02x:%02x:%02x:%02x:%02x:%02x"
#define ESP_BD_ADDR_HEX(a) a[0],a[1],a[2],a[3],a[4],a[5]
#define BLE_ADDR_TYPE_PUBLIC 0
#define BLE_WL_ADDR_TYPE_PUBLIC 0
#define BLE_WL_ADDR_TYPE_RANDOM 1
#define BLE_SCAN_FILTER_ALLOW_ALL 0
#define BLE_SCAN_FILTER_ALLOW_ONLY_WLST 1
#define ESP_BT_STATUS_SUCCESS 0
#define ESP_GATT_IF_NONE -1
#define ESP_GAP_SEARCH_INQ_RES_EVT 0
#define ESP_GAP_SEARCH_INQ_CMPL_EVT 1
#define ESP_BLE_AD_TYPE_NAME_CMPL 9
#define MAX_SCAN_DEVICES 128
#define DEFAULT_SCAN_TIME 10
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
typedef unsigned TickType_t;
typedef struct { bool held; } *SemaphoreHandle_t;
static TickType_t now_ticks;
static int xSemaphoreTake(SemaphoreHandle_t s, TickType_t timeout) { (void)timeout; if (s->held) return 0; s->held=true; return 1; }
static int xSemaphoreGive(SemaphoreHandle_t s) { assert(s->held); s->held=false; return 1; }
static TickType_t xTaskGetTickCount(void) {return now_ticks;}
static void vTaskDelay(TickType_t delay);
typedef uint8_t esp_bd_addr_t[6];
typedef int esp_gap_ble_cb_event_t;
enum {ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,
ESP_GAP_BLE_SCAN_RESULT_EVT, ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,
ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT, ESP_GAP_BLE_UPDATE_WHITELIST_COMPLETE_EVT};
enum {ESP_BLE_WHITELIST_ADD=1, ESP_BLE_WHITELIST_CLEAR=2};
typedef struct {
 struct {int status;} scan_param_cmpl, scan_start_cmpl, scan_stop_cmpl;
 struct {int wl_operation,status;} update_whitelist_cmpl;
 struct {int search_evt;uint8_t bda[6],ble_addr_type,ble_adv[64];uint8_t adv_data_len,scan_rsp_len;int rssi;} scan_rst;
 struct {int status,conn_int,min_int,max_int,latency,timeout;} update_conn_params;
} esp_ble_gap_cb_param_t;
static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static esp_err_t esp_ble_gap_stop_scanning(void);
static esp_err_t esp_ble_gap_clear_whitelist(void);
static esp_err_t esp_ble_gap_update_whitelist(bool add, uint8_t *bda, int type);
static esp_err_t esp_ble_gap_set_scan_params(void *params);
static esp_err_t esp_ble_gap_start_scanning(unsigned seconds);
static uint8_t *esp_ble_resolve_adv_data_by_type(uint8_t *adv, unsigned len, int type, uint8_t *name_len) {
 (void)type; *name_len=len ? adv[0]:0; return *name_len ? adv+1:NULL;
}
#endif
'''
STATE = r'''
#include "ble_target.h"
static struct {bool held;} control_lock,list_lock;
static SemaphoreHandle_t s_scan_control_mutex=(void *)&control_lock,s_device_list_mutex=(void *)&list_lock;
static bool IsInited=true,ble_Scan_complate;
static atomic_bool s_is_scanning,s_is_connected,s_connect_attempting,s_control_paused;
static BleTarget s_scan_target;
static struct {int gattc_if;} s_gattc_profile={.gattc_if=3};
static struct {int scan_filter_policy;} s_ble_scan_params;
typedef struct {char name[64];uint8_t bda[6],addr_type;int8_t rssi;bool is_connected;} ble_device_info_t;
static ble_device_info_t s_scanned_devices[MAX_SCAN_DEVICES];
static int s_scanned_count;
static void (*s_scan_cb)(ble_device_info_t *,int);
static const char *esp_err_to_name(esp_err_t err) {(void)err;return "mock";}
static void ble_coex_prefer_bt(bool enabled) {(void)enabled;}
static void ble_clear_scan_results(void);
static void add_scanned_device(esp_ble_gap_cb_param_t *scan_result);
static int locked_connects;
static bool ble_connect_by_name_locked(const char *name) {(void)name;locked_connects++;return true;}
static bool ble_connect_by_address_locked(uint8_t *bda,uint8_t type) {(void)bda;(void)type;locked_connects++;return true;}
static char trace[256],fail_op,drop_op,reject_op;
static int scheduled_event=-1,scheduled_wl;
static TickType_t scheduled_at;
static bool scheduled_success;
static int submitted_policy,submitted_type;
static bool cancel_on_delay;
static uint8_t submitted_mac[6];
static void emit(int event, int wl, bool success) {
 esp_ble_gap_cb_param_t p={0};int status=success?0:1;
 p.scan_param_cmpl.status=p.scan_start_cmpl.status=p.scan_stop_cmpl.status=status;
 p.update_whitelist_cmpl.status=status;p.update_whitelist_cmpl.wl_operation=wl;
 ble_gap_cb(event,&p);
}
static esp_err_t submit(char op,int event,int wl) {
 size_t n=strlen(trace);assert(n<sizeof(trace)-1);trace[n]=op;trace[n+1]=0;
 if (op==reject_op) return ESP_FAIL;
 if (op==drop_op) return ESP_OK;
 assert(scheduled_event==-1);scheduled_event=event;scheduled_wl=wl;
 scheduled_success=op!=fail_op;scheduled_at=now_ticks+20;return ESP_OK;
}
static void vTaskDelay(TickType_t delay) {
 now_ticks+=delay;
 if(cancel_on_delay){s_control_paused=true;cancel_on_delay=false;}
 if(scheduled_event!=-1 && now_ticks>=scheduled_at) {
  int event=scheduled_event;scheduled_event=-1;emit(event,scheduled_wl,scheduled_success);
 }
}
static esp_err_t esp_ble_gap_stop_scanning(void) {return submit('T',ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,0);}
static esp_err_t esp_ble_gap_clear_whitelist(void) {return submit('C',ESP_GAP_BLE_UPDATE_WHITELIST_COMPLETE_EVT,ESP_BLE_WHITELIST_CLEAR);}
static esp_err_t esp_ble_gap_update_whitelist(bool add,uint8_t *bda,int type) {
 assert(add);assert(type==0 || type==1);submitted_type=type;memcpy(submitted_mac,bda,6);return submit('A',ESP_GAP_BLE_UPDATE_WHITELIST_COMPLETE_EVT,ESP_BLE_WHITELIST_ADD);
}
static esp_err_t esp_ble_gap_set_scan_params(void *params) {
 (void)params;submitted_policy=s_ble_scan_params.scan_filter_policy;
 return submit('P',ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT,0);
}
static esp_err_t esp_ble_gap_start_scanning(unsigned seconds) {assert(seconds==20);return submit('S',ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,0);}
static unsigned char persisted[2][128],staged[128];
static size_t persisted_size[2],staged_size;
static int staged_slot,nvs_write_fail,nvs_commit_fail,nvs_read_fail,legacy_fail,commit_calls;
static nvs_handle_t my_handle=1;
esp_err_t nvs_open(const char *space,int mode,nvs_handle_t *h) {(void)mode;assert(!strcmp(space,"ble_target"));*h=1;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *p,size_t n) {
 (void)h;if(nvs_write_fail)return ESP_FAIL;assert(n<=128);staged_slot=!strcmp(key,"ready")?0:1;memcpy(staged,p,n);staged_size=n;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
 (void)h;commit_calls++;if(nvs_commit_fail)return ESP_FAIL;if(staged_size){memcpy(persisted[staged_slot],staged,staged_size);persisted_size[staged_slot]=staged_size;}return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *p,size_t *n) {
 (void)h;if(nvs_read_fail)return ESP_FAIL;int slot=!strcmp(key,"ready")?0:1;
 if(!persisted_size[slot])return ESP_ERR_NVS_NOT_FOUND;
 if(*n<persisted_size[slot])return ESP_FAIL;*n=persisted_size[slot];memcpy(p,persisted[slot],*n);return ESP_OK;
}
void nvs_close(nvs_handle_t h) {(void)h;staged_size=0;}
#define NVS_KEY_READY_PN "bt_name_Ready"
#define NVS_KEY_NEW_PN_READY "new_pn_ready"
enum {AGING_CMD_IDLE,AGING_CMD_RUNNING};
static char PN_Code_Ready[64];
static unsigned s_new_pn_ready;
static int s_aging_cmd_state;
static BleTarget s_ble_target_ready;
#define local_error_log(...) ((void)0)
static esp_err_t nvs_set_str(nvs_handle_t h,const char *k,const char *v) {(void)h;(void)k;(void)v;return legacy_fail?ESP_FAIL:ESP_OK;}
static esp_err_t SelfRecovery_Write_uint16(const char *k,uint16_t v) {(void)k;(void)v;return legacy_fail?ESP_FAIL:ESP_OK;}
static bool copy_string_checked(char *dst,size_t cap,const char *src) {if(strlen(src)>=cap)return false;strcpy(dst,src);return true;}
'''
TESTS = r'''
static cJSON *json(const char *s) {cJSON *p=cJSON_Parse(s);assert(p);return p;}
static void reset_scan(void) {
 s_is_scanning=s_is_connected=s_connect_attempting=s_control_paused=false;s_gattc_profile.gattc_if=3;
 s_scan_op=BLE_SCAN_OP_NONE;s_scan_op_result=ESP_OK;scheduled_event=-1;
 fail_op=drop_op=reject_op=0;trace[0]=0;locked_connects=0;s_scanned_count=0;
}
static void report(const BleTarget *target,const char *name,int rssi) {
 esp_ble_gap_cb_param_t p={0};p.scan_rst.search_evt=ESP_GAP_SEARCH_INQ_RES_EVT;
 memcpy(p.scan_rst.bda,target->bda,6);p.scan_rst.ble_addr_type=target->addr_type;p.scan_rst.rssi=rssi;
 if(name){p.scan_rst.adv_data_len=strlen(name)+1;p.scan_rst.ble_adv[0]=strlen(name);memcpy(p.scan_rst.ble_adv+1,name,strlen(name));}
 ble_gap_cb(ESP_GAP_BLE_SCAN_RESULT_EVT,&p);
}
int main(void) {
 BleTarget target={0},out={0},name={0};
 cJSON *p=json("{\"PN\":\"PN-A\",\"MAC\":\"c7:2A:18:32:34:D3\"}");
 assert(ble_target_parse(p,&target)==ESP_OK && target.use_mac && target.addr_type==0 && target.bda[0]==0xc7 && target.bda[5]==0xd3);
 int commits_before=commit_calls;legacy_fail=1;assert(SelfRecovery_Write_str("test","value")==ESP_FAIL && commit_calls==commits_before);legacy_fail=0;
 assert(accept_ready_pn_data(p)==ESP_OK && s_new_pn_ready && s_ble_target_ready.use_mac);
 assert(accept_ready_pn_data(p)==ESP_ERR_INVALID_STATE);clear_ready_pn_runtime();
 s_aging_cmd_state=AGING_CMD_RUNNING;assert(accept_ready_pn_data(p)==ESP_ERR_INVALID_STATE);s_aging_cmd_state=AGING_CMD_IDLE;
 nvs_write_fail=1;assert(accept_ready_pn_data(p)==ESP_FAIL && !s_new_pn_ready);nvs_write_fail=0;
 legacy_fail=1;assert(accept_ready_pn_data(p)==ESP_FAIL && !s_new_pn_ready);legacy_fail=0;
 cJSON_Delete(p);
 const char *bad[]={"{\"MAC\":null}","{\"MAC\":\"c7:2a:18:32:34:d3x\"}","{\"MAC\":\"c7-2a:18:32:34:d3\"}",
 "{\"MAC\":\"gg:2a:18:32:34:d3\"}","{\"AddrType\":0}","{\"MAC\":\"c7:2a:18:32:34:d3\",\"AddrType\":0.5}",
 "{\"MAC\":\"c7:2a:18:32:34:d3\",\"AddrType\":2}","{\"MAC\":\"c7:2a:18:32:34:d3\",\"AddrType\":\"0\"}"};
 for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i){p=json(bad[i]);out=target;assert(ble_target_parse(p,&out)==ESP_ERR_INVALID_ARG && !memcmp(&out,&target,sizeof(out)));cJSON_Delete(p);}
 p=json("{\"MAC\":\"c7:2a:18:32:34:d3\",\"AddrType\":1}");assert(ble_target_parse(p,&out)==ESP_OK && out.addr_type==1);cJSON_Delete(p);
 p=json("{\"PN\":\"PN-B\"}");assert(ble_target_parse(p,&out)==ESP_OK && !out.use_mac);assert(accept_ready_pn_data(p)==ESP_OK && !s_ble_target_ready.use_mac);clear_ready_pn_runtime();cJSON_Delete(p);
 assert(ble_target_save(true,"PN-A",&target)==ESP_OK);assert(ble_target_load(true,"PN-A",&out)==ESP_OK && out.use_mac);
 assert(ble_target_load(true,"PN-B",&out)==ESP_ERR_INVALID_STATE);
 nvs_commit_fail=1;assert(ble_target_save(true,"PN-B",&name)==ESP_FAIL);nvs_commit_fail=0;
 assert(ble_target_load(true,"PN-A",&out)==ESP_OK && out.use_mac);
 nvs_read_fail=1;assert(ble_target_load(true,"PN-A",&out)==ESP_FAIL);nvs_read_fail=0;
 persisted[0][0]=0;assert(ble_target_load(true,"PN-A",&out)==ESP_ERR_INVALID_STATE);persisted_size[0]=0;
 assert(ble_target_load(true,"legacy",&out)==ESP_OK && !out.use_mac);
 assert(ble_target_save(false,"PN-A",&target)==ESP_OK && ble_target_load(false,"PN-A",&out)==ESP_OK && out.use_mac);
 assert(ble_target_save(false,"PN-A",&name)==ESP_OK && ble_target_load(false,"PN-A",&out)==ESP_OK && !out.use_mac);
 puts("PASS command guards, MAC validation, PN binding, NVS errors and legacy restore");
 reset_scan();IsInited=false;assert(ble_start_target_scan(&target,20)==ESP_ERR_INVALID_STATE && !trace[0]);IsInited=true;
 s_gattc_profile.gattc_if=ESP_GATT_IF_NONE;assert(ble_start_target_scan(&target,20)==ESP_ERR_INVALID_STATE && !trace[0]);s_gattc_profile.gattc_if=3;
 assert(ble_start_target_scan(&target,20)==ESP_OK && !strcmp(trace,"CAPS") && submitted_policy==BLE_SCAN_FILTER_ALLOW_ONLY_WLST && !memcmp(submitted_mac,target.bda,6));
 report(&target,NULL,-100);assert(ble_scanned_has_address(target.bda,0) && s_scanned_count==1 && s_scanned_devices[0].rssi==-100);
 BleTarget other=target;other.bda[5]++;report(&other,"foreign",-20);other=target;other.addr_type=1;report(&other,"wrong type",-20);assert(s_scanned_count==1);
 assert(!ble_connect_by_address(target.bda,0));assert(ble_stop_scan_wait()==ESP_OK);assert(ble_connect_by_address(target.bda,0) && locked_connects==1);
 reset_scan();assert(ble_start_target_scan(&target,20)==ESP_OK);trace[0]=0;other=target;other.bda[5]++;
 assert(ble_start_target_scan(&other,20)==ESP_OK && !strcmp(trace,"TCAPS") && !memcmp(submitted_mac,other.bda,6));
 report(&target,"old",-20);assert(!s_scanned_count);report(&other,NULL,-90);assert(s_scanned_count==1);
 trace[0]=0;assert(ble_start_target_scan(&name,20)==ESP_OK && !strcmp(trace,"TCPS") && submitted_policy==BLE_SCAN_FILTER_ALLOW_ALL);
 report(&other,NULL,-20);assert(!s_scanned_count);report(&target,"PN-A",-90);assert(ble_scanned_has_name("PN-A"));
 puts("PASS async order, nameless/weak target, exact address/type, A to B and legacy mode");
 for(const char *op="CAPS";*op;op++) {
  reset_scan();fail_op=*op;assert(ble_start_target_scan(&target,20)==ESP_FAIL && !s_is_scanning);
  if(*op!='S')assert(strchr(trace,'S')==NULL);
  reset_scan();reject_op=*op;assert(ble_start_target_scan(&target,20)==ESP_FAIL && !s_is_scanning);
  reset_scan();drop_op=*op;assert(ble_start_target_scan(&target,20)==ESP_ERR_TIMEOUT);
  char saved[256];strcpy(saved,trace);assert(ble_start_target_scan(&target,20)==ESP_ERR_TIMEOUT && !strcmp(saved,trace));
  assert(!ble_connect_by_address(target.bda,0));
 }
 reset_scan();assert(ble_start_target_scan(&target,20)==ESP_OK);drop_op='T';trace[0]=0;
 assert(ble_start_target_scan(&other,20)==ESP_ERR_TIMEOUT && !strcmp(trace,"T"));
 assert(!ble_connect_by_address(target.bda,0));drop_op=0;emit(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,0,true);
 trace[0]=0;assert(ble_start_target_scan(&other,20)==ESP_OK && !strcmp(trace,"CAPS"));
 reset_scan();s_connect_attempting=true;assert(ble_start_target_scan(&target,20)==ESP_ERR_INVALID_STATE && !trace[0]);
 assert(ble_control_pause()==ESP_OK);assert(ble_control_resume()==ESP_ERR_INVALID_STATE && s_control_paused);
 s_connect_attempting=false;assert(ble_control_resume()==ESP_OK && !s_control_paused);
 reset_scan();assert(ble_start_target_scan(&target,20)==ESP_OK);assert(ble_control_pause()==ESP_OK);
 assert(!ble_connect_by_address(target.bda,0));assert(ble_start_target_scan(&target,20)==ESP_ERR_INVALID_STATE);
 vTaskDelay(30);ble_control_resume();assert(ble_start_target_scan(&target,20)==ESP_OK);
 reset_scan();other=target;other.addr_type=1;assert(ble_start_target_scan(&other,20)==ESP_OK && submitted_type==1);
 reset_scan();drop_op='C';cancel_on_delay=true;assert(ble_start_target_scan(&target,20)==ESP_ERR_INVALID_STATE && !strcmp(trace,"C"));
 assert(ble_control_pause()==ESP_OK && !control_lock.held);ble_control_resume();drop_op=0;
 emit(ESP_GAP_BLE_UPDATE_WHITELIST_COMPLETE_EVT,ESP_BLE_WHITELIST_CLEAR,true);
 assert(ble_start_target_scan(&target,20)==ESP_OK);
 reset_scan();s_scan_op=BLE_SCAN_OP_START;cancel_on_delay=true;
 assert(ble_start_target_scan(&target,20)==ESP_ERR_INVALID_STATE && s_scan_op==BLE_SCAN_OP_START && !trace[0]);
 puts("PASS submit/callback failures, timeouts, late completion, in-flight connection and pause");
 reset_scan();ble_control_set_budget_ms(45);drop_op='P';TickType_t begin=now_ticks;
 assert(ble_start_target_scan(&target,20)==ESP_ERR_TIMEOUT && now_ticks-begin==45);
 assert(s_scan_op==BLE_SCAN_OP_PARAMS);ble_control_set_budget_ms(0);
 puts("PASS total control budget bounds all async scan stages without losing the pending operation");
 return 0;
}
'''

def main():
    ble = (ROOT / 'main/Communication/Ble_control/Ble_control.c').read_text()
    app = (ROOT / 'main/Task/appTask.c').read_text()
    start = ble.index('typedef enum {\n    BLE_SCAN_OP_NONE')
    end = ble.index('/*\n * GATT 设置阶段', start)
    source = '#include "mocks.h"\n' + STATE + function_source(ble, "static void ble_control_unlock(") + ble[start:end]
    functions = ['ble_clear_scan_results', 'ble_start_target_scan', 'ble_stop_scan_wait',
                 'ble_control_pause', 'ble_control_resume', 'ble_scanned_has_name', 'ble_scanned_has_address',
                 'add_scanned_device', 'ble_gap_cb', 'ble_connect_control_enter', 'ble_connect_by_name', 'ble_connect_by_address']
    # 完整签名+换行定位定义，避免把前置声明误当作实现。
    import re
    for name in functions:
        match = re.search(r'^(?:static )?(?:void|bool|esp_err_t) '+name+r'\([^;]*?\)\n\{', ble, re.M)
        assert match, name
        source += '\n' + function_source(ble[match.start():], match.group(0)[:-2])
    source += '\n' + function_source((ROOT / 'main/Tool/SelfRecovery.c').read_text(), 'esp_err_t SelfRecovery_Write_str(')
    for name in ['clear_ready_pn_runtime' , 'persist_ready_pn_state', 'accept_ready_pn_data']:
        match = re.search(r'^static (?:void|esp_err_t) '+name+r'\([^;]*?\)\n\{', app, re.M)
        assert match, name
        source += '\n' + function_source(app[match.start():], match.group(0)[:-2])
    with tempfile.TemporaryDirectory(prefix='ble-target-test-') as directory:
        temp = Path(directory)
        (temp / 'mocks.h').write_text(MOCKS)
        for header in ['esp_err.h', 'nvs.h', 'esp_log.h']:
            (temp / header).write_text('#include "mocks.h"\n')
        (temp / 'test.c').write_text(source + TESTS)
        cjson = IDF / 'components/json/cJSON'
        binary = temp / 'test'
        args = [os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unused-variable', '-Wno-misleading-indentation',
                '-I', str(temp), '-I', str(ROOT / 'main/Communication/Ble_control'), '-I', str(cjson),
                str(temp / 'test.c'), str(ROOT / 'main/Communication/Ble_control/ble_target.c'),
                str(cjson / 'cJSON.c'), '-lm', '-o', str(binary)]
        if os.environ.get('TEST_SANITIZE') == '1':
            args[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(args, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)

if __name__ == '__main__':
    main()
