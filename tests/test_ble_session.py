#!/usr/bin/env python3
"""真实 BLE 连接生命周期、收发及鉴权接入回归；TEST_SANITIZE=1 启用 ASan/UBSan。

提取实际函数和 SDK 事件分支，mock 蓝牙传输及密码学结果；不验证空口或密码算法。
"""
import os
from pathlib import Path
import subprocess
import tempfile
from test_root_uplink import function_source

ROOT = Path(__file__).resolve().parents[1]

MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <setjmp.h>
typedef int esp_err_t;
typedef int esp_gattc_cb_event_t;
typedef int esp_gatt_if_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_GATT_OK 0
#define ESP_GATT_WRITE_TYPE_NO_RSP 1
#define ESP_GATT_AUTH_REQ_NONE 0
#define ESP_BD_ADDR_LEN 6
#define ESP_BD_ADDR_STR "%02x:%02x:%02x:%02x:%02x:%02x"
#define ESP_BD_ADDR_HEX(a) a[0],a[1],a[2],a[3],a[4],a[5]
#define ESP_LOG_INFO 3
static char last_log[512];
static unsigned logged_plain, logged_wire, logged_encrypted;
static void log_info(const char *tag, const char *fmt, ...) {
 (void)tag; va_list ap;va_start(ap,fmt);vsnprintf(last_log,sizeof(last_log),fmt,ap);va_end(ap);
 unsigned long sid;
 if(strstr(fmt,"plain_bytes")) assert(sscanf(last_log,"蓝牙发送已受理: session=%lu, encrypted=%u, plain_bytes=%u, wire_bytes=%u",&sid,&logged_encrypted,&logged_plain,&logged_wire)==4);
}
#define ESP_LOGI log_info
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOG_BUFFER_HEX_LEVEL(tag,data,len,level) do {(void)(tag);(void)(data);assert((len)<=32);(void)(level);} while(0)
static const char *TAG="test";
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
#define PROFILE_NUM 1
#define PROFILE_A_APP_ID 0
#define INVALID_HANDLE 0
#define BUFFER_MAX 1200
#define BLE_PREF_CONN_INTERVAL 12
#define BLE_PREF_SUPERVISION_TIMEOUT 2000
typedef void *SemaphoreHandle_t;
static SemaphoreHandle_t s_device_list_mutex;
static int xSemaphoreTake(SemaphoreHandle_t h,unsigned wait) {(void)h;(void)wait;return 1;}
static int xSemaphoreGive(SemaphoreHandle_t h) {(void)h;return 1;}
typedef struct {uint8_t *data;uint16_t data_len;uint32_t wait_time,session_id;} ble_mtu_data_t;
typedef struct {uint8_t profile_id;uint8_t *data;uint16_t data_len;uint32_t session_id;} ble_data_t;
typedef struct {size_t item_size;unsigned head,count;unsigned char items[16][64];} Queue;
static Queue input_queue={.item_size=sizeof(ble_mtu_data_t)}, output_queue={.item_size=sizeof(ble_data_t)};
static Queue *data_queue=&input_queue,*data_forword_queue=&output_queue;
static jmp_buf task_exit;
static bool escape_on_empty,finish_assembly;
static void (*receive_hook)(void);
static int xQueueSend(Queue *q,const void *item,unsigned wait) {
 (void)wait;assert(q->item_size<=64);if(q->count==16)return 0;
 memcpy(q->items[(q->head+q->count++)%16],item,q->item_size);return 1;
}
static int xQueueReceive(Queue *q,void *item,unsigned wait) {
 (void)wait;if(receive_hook){void(*hook)(void)=receive_hook;receive_hook=NULL;hook();}
 if(!q->count){if(escape_on_empty && !finish_assembly)longjmp(task_exit,1);finish_assembly=false;return 0;}
 memcpy(item,q->items[q->head],q->item_size);q->head=(q->head+1)%16;q->count--;return 1;
}
static bool alloc_fail;
static void *app_malloc_prefer_psram(size_t bytes) {return alloc_fail?NULL:malloc(bytes);}
static uint8_t a_data[BUFFER_MAX];
static struct {uint16_t gattc_if,conn_id,mtu,notify_char_handle,write_char_handle;uint8_t remote_bda[6];bool service_found;} s_gattc_profile;
static struct {uint8_t bda[6];bool is_connected;} s_scanned_devices[1];
static int s_scanned_count;
static bool s_gatt_setup_done,s_connect_attempting;
static void(*s_disconnect_cb)(void);
static void ble_coex_prefer_bt(bool bt) {(void)bt;}
static void notify_connect_result(bool success) {(void)success;s_connect_attempting=false;}
static const char *esp_err_to_name(int e) {(void)e;return "mock";}
typedef struct {uint8_t bda[6];unsigned min_int,max_int,latency,timeout;} esp_ble_conn_update_params_t;
static int esp_ble_gattc_send_mtu_req(int iface,int conn) {(void)iface;(void)conn;return ESP_OK;}
static int esp_ble_gap_update_conn_params(esp_ble_conn_update_params_t *p) {(void)p;return ESP_OK;}
enum {ESP_GATTC_CONNECT_EVT,ESP_GATTC_CFG_MTU_EVT,ESP_GATTC_NOTIFY_EVT,ESP_GATTC_DISCONNECT_EVT};
typedef union {
 struct {uint16_t conn_id;uint8_t remote_bda[6];} connect;
 struct {int status;uint16_t conn_id,mtu;} cfg_mtu;
 struct {uint16_t conn_id,handle,value_len;uint8_t remote_bda[6];uint8_t *value;} notify;
 struct {int reason;} disconnect;
} esp_ble_gattc_cb_param_t;
static void gattc_profile_event_handler(esp_gattc_cb_event_t,int,esp_ble_gattc_cb_param_t*);
uint32_t ble_session_id(void);
bool ble_session_is_current(uint32_t);
bool ble_is_connected(void);
int app_ble_recv_data_for_session(uint8_t*,int,uint32_t,uint32_t*);
static unsigned send_calls,encrypt_calls,decrypt_calls,key_gen_calls,final_calls,uart_calls,close_calls;
static int sdk_fail,encrypt_fail,auth1_fail,auth4_fail,auth6_fail;
static uint8_t sent[1024];
static unsigned sent_len;
static void(*crypto_hook)(void);
static esp_err_t esp_ble_gattc_write_char(int iface,int conn,int handle,int len,uint8_t *data,int type,int auth) {
 assert(iface==3 && conn==s_gattc_profile.conn_id && handle==0x22);
 assert(type==ESP_GATT_WRITE_TYPE_NO_RSP && auth==ESP_GATT_AUTH_REQ_NONE);
 assert(len>0 && len<=500);send_calls++;sent_len=len;memcpy(sent,data,len);return sdk_fail?ESP_FAIL:ESP_OK;
}
static esp_err_t esp_ble_gattc_close(int iface,int conn) {(void)iface;(void)conn;close_calls++;return ESP_OK;}
static int app_enc_process_Encrypt(const uint8_t *data,uint16_t len,uint8_t *out,uint16_t *out_len) {
 (void)data;encrypt_calls++;if(crypto_hook){void(*h)(void)=crypto_hook;crypto_hook=NULL;h();}
 if(encrypt_fail)return ESP_FAIL;*out_len=6+((len+15)/16)*16;memset(out,0xa5,*out_len);return ESP_OK;
}
static int app_enc_process_Decrypt(const uint8_t *data,size_t len,uint8_t *out,uint16_t *out_len) {
 (void)data;(void)len;decrypt_calls++;memset(out,0x11,8);*out_len=8;return ESP_OK;
}
static void RandomSendPKey(void) {key_gen_calls++;}
static int app_enc_process_data(const uint8_t *data,size_t len,uint8_t *out) {
 (void)data;assert(len==10);memset(out,0x02,10);return auth1_fail?ESP_FAIL:ESP_OK;
}
static int app_enc_process_data4(uint8_t *data,size_t len,uint8_t *out) {
 (void)data;assert(len==146);memset(out,0x05,146);return auth4_fail?ESP_FAIL:ESP_OK;
}
static int app_enc_process_data6(uint8_t *data,size_t len,uint8_t *out) {
 (void)data;assert(len==18);final_calls++;out[0]=0;out[1]=1;
 if(crypto_hook){void(*h)(void)=crypto_hook;crypto_hook=NULL;h();}
 return auth6_fail?ESP_FAIL:ESP_OK;
}
static int app_write_UART_data(uint8_t *data,int len,unsigned wait) {(void)data;(void)wait;uart_calls++;return len;}
'''

TESTS = r'''
static uint32_t connect_device(unsigned addr) {
 esp_ble_gattc_cb_param_t p={0};p.connect.conn_id=3;p.connect.remote_bda[0]=addr;
 s_gattc_profile.gattc_if=3;gattc_profile_event_handler(ESP_GATTC_CONNECT_EVT,3,&p);
 s_gattc_profile.write_char_handle=0x22;s_gattc_profile.notify_char_handle=0x1f;
 assert(!ble_session_is_encrypted());return ble_session_id();
}
static void dropped_link(void) {
 esp_ble_gattc_cb_param_t p={0};p.disconnect.reason=8;
 gattc_profile_event_handler(ESP_GATTC_DISCONNECT_EVT,3,&p);assert(!ble_session_is_encrypted());
}
static void replace_link(void) {dropped_link();connect_device(2);}
static void rx_packet(const uint8_t *bytes,unsigned len,unsigned handle,unsigned addr) {
 esp_ble_gattc_cb_param_t p={0};p.notify.conn_id=3;p.notify.handle=handle;p.notify.remote_bda[0]=addr;
 p.notify.value=(uint8_t*)bytes;p.notify.value_len=len;gattc_profile_event_handler(ESP_GATTC_NOTIFY_EVT,3,&p);
}
static uint8_t auth1[10]={'*','*',1},auth4[146]={0,0x86},auth6[18]={0,0x10};
static void begin_auth(uint32_t sid) {
 assert(app_ble_process_auth(auth1,sizeof(auth1),sid));
 assert(app_ble_process_auth(auth4,sizeof(auth4),sid));
 assert(!ble_session_is_encrypted());
}
static void run_assembler(void) {
 escape_on_empty=finish_assembly=true;
 if(!setjmp(task_exit))app_ble_recv_task(NULL);
 escape_on_empty=false;
}
static void enqueue_output(uint32_t sid,const uint8_t *bytes,unsigned len) {
 ble_data_t item={0};item.data=malloc(len);assert(item.data);memcpy(item.data,bytes,len);
 item.data_len=len;item.session_id=sid;assert(xQueueSend(data_forword_queue,&item,0));
}
static void run_worker(void) {
 escape_on_empty=true;if(!setjmp(task_exit))app_ble_data_handle(NULL);escape_on_empty=false;
}
static unsigned fragment_hook_calls;
static uint8_t new_fragment[3]={0x33,0x44,0x55};
static void switch_between_fragments(void) {
 if(fragment_hook_calls++==0){receive_hook=switch_between_fragments;return;}
 replace_link();rx_packet(new_fragment,sizeof(new_fragment),0x1f,2);
}
int main(void) {
 setbuf(stdout,NULL);
 uint8_t raw[8]={1,3,0,1,0,1,0xd5,0xca},out[1024];uint32_t received_sid;
 uint32_t encrypted_sid=connect_device(1);begin_auth(encrypted_sid);
 assert(app_ble_process_auth(auth6,sizeof(auth6),encrypted_sid));
 assert(ble_session_is_encrypted() && uart_calls==1);
 unsigned finals=final_calls;
 assert(app_ble_process_auth(auth6,sizeof(auth6),encrypted_sid) && final_calls==finals);
 assert(ble_send_data(raw,8,0)==1 && sent_len==22 && logged_plain==8 && logged_wire==22 && logged_encrypted==1);
 assert(ble_send_auth_data(encrypted_sid,raw,8)==1 && sent_len==8 && !memcmp(sent,raw,8));
 ble_disconnect();assert(close_calls==1 && !ble_session_is_encrypted());
 assert(!ble_session_set_encrypted(encrypted_sid,true));
 assert(ble_send_data(raw,8,0)==0);dropped_link();
 uint32_t plain_sid=connect_device(2);assert(plain_sid!=encrypted_sid);
 assert(!ble_session_set_encrypted(encrypted_sid,true));
 assert(app_ble_process_auth(auth6,sizeof(auth6),encrypted_sid));
 assert(app_ble_process_auth(auth6,sizeof(auth6),plain_sid) && !ble_session_is_encrypted());
 assert(ble_send_data(raw,8,0)==1 && sent_len==8 && !memcmp(sent,raw,8));
 assert(logged_plain==8 && logged_wire==8 && logged_encrypted==0);
 puts("PASS encrypted device -> disconnect -> plaintext device sends original 8 bytes; stale/orphan auth rejected");

 for(int failure=1;failure<=4;failure++) {
  replace_link();uint32_t sid=ble_session_id();
  auth1_fail=failure==1;auth4_fail=failure==2;auth6_fail=failure==3;
  begin_auth(sid);sdk_fail=failure==4;
  assert(app_ble_process_auth(auth6,sizeof(auth6),sid) && !ble_session_is_encrypted());
  auth1_fail=auth4_fail=auth6_fail=sdk_fail=0;
 }
 replace_link();begin_auth(ble_session_id());crypto_hook=replace_link;
 assert(app_ble_process_auth(auth6,sizeof(auth6),ble_session_id()) && !ble_session_is_encrypted());
 assert(ble_send_data(raw,8,0)==1 && sent_len==8);
 assert(ble_session_set_encrypted(ble_session_id(),true));crypto_hook=replace_link;
 unsigned sends=send_calls;assert(ble_send_data(raw,8,0)==0 && send_calls==sends);
 assert(ble_session_set_encrypted(ble_session_id(),true));encrypt_fail=1;
 assert(ble_send_data(raw,8,0)==0);encrypt_fail=0;
 assert(ble_send_data(NULL,8,0)==0 && ble_send_data(raw,-1,0)==0 && ble_send_data(raw,0,0)==0);
 uint8_t large[501]={0};assert(ble_send_data(large,sizeof(large),0)==0);
 assert(ble_send_data(large,500,0)==0); /* 加密后超过 500 字节，拒绝提交。 */
 puts("PASS auth/ACK failures keep plaintext; disconnect during auth or encryption cannot submit/enable new session");

 replace_link();plain_sid=ble_session_id();
 unsigned queued=input_queue.count;rx_packet(raw,8,0x20,2);rx_packet(raw,8,0x1f,1);assert(input_queue.count==queued);
 rx_packet(raw,8,0x1f,2);assert(input_queue.count==1);
 ble_mtu_data_t *fragment=(void*)input_queue.items[input_queue.head];assert(fragment->wait_time==0 && fragment->session_id==plain_sid);
 run_assembler();assert(app_ble_recv_data_for_session(out,sizeof(out),0,&received_sid)==8);
 assert(received_sid==plain_sid && !memcmp(raw,out,8));
 rx_packet(raw,8,0x1f,2);replace_link();run_assembler();assert(output_queue.count==0);
 rx_packet(raw,8,0x1f,2);receive_hook=switch_between_fragments;run_assembler();
 assert(app_ble_recv_data_for_session(out,sizeof(out),0,&received_sid)==sizeof(new_fragment));
 assert(!memcmp(out,new_fragment,sizeof(new_fragment))); /* 旧连接已拼接部分不能混入新连接。 */
 enqueue_output(plain_sid,raw,8);assert(app_ble_recv_data_for_session(out,sizeof(out),0,&received_sid)==-1);
 uint8_t over[33]={0};enqueue_output(ble_session_id(),over,sizeof(over));
 memset(out,0xcc,sizeof(out));assert(app_ble_recv_data_for_session(out,16,0,&received_sid)==-1 && out[16]==0xcc);
 alloc_fail=true;rx_packet(raw,8,0x1f,2);alloc_fail=false;assert(input_queue.count==0);
 for(unsigned i=0;i<16;i++)rx_packet(raw,8,0x1f,2);
 rx_packet(raw,8,0x1f,2);assert(input_queue.count==16);run_assembler();
 while(output_queue.count) assert(app_ble_recv_data_for_session(out,sizeof(out),0,&received_sid)>0);
 puts("PASS real notify -> fragment -> business queue identity; wrong handle/peer, stale packets, oversize and allocation/queue failures");

 replace_link();plain_sid=ble_session_id();begin_auth(plain_sid);
 enqueue_output(plain_sid,auth6,sizeof(auth6));unsigned decrypts=decrypt_calls;run_worker();
 assert(ble_session_is_encrypted() && decrypt_calls==decrypts); /* 结果包不再落入业务解密。 */
 replace_link();enqueue_output(ble_session_id(),raw,8);decrypts=decrypt_calls;
 unsigned uarts=uart_calls;run_worker();assert(decrypt_calls==decrypts && uart_calls==uarts+1);
 enqueue_output(ble_session_id(),raw,8);assert(app_ble_recv_data_Ack(out,sizeof(out),0)==8 && !memcmp(raw,out,8));
 assert(ble_session_set_encrypted(ble_session_id(),true));enqueue_output(ble_session_id(),raw,8);
 assert(app_ble_recv_data_Ack(out,sizeof(out),0)==8 && decrypt_calls==decrypts+1);
 puts("PASS final auth consumed exactly once; plaintext forward and aging receive bypass decryption");
 return 0;
}
'''


def main():
    ble = (ROOT / 'main/Communication/Ble_control/Ble_control.c').read_text()
    app = (ROOT / 'main/Task/appTask.c').read_text()
    start = ble.index('static atomic_bool s_is_connected = false;')
    end = ble.index('static char s_connected_name', start)
    source = MOCKS + ble[start:end]
    for signature in [
        'bool ble_is_connected(', 'void ble_disconnect(',
        'static int ble_send_for_session(', 'int ble_send_data(', 'int ble_send_auth_data(',
        'void app_ble_recv_task(', 'int app_ble_recv_data_form_remote(',
        'int app_ble_recv_data_for_session(',
    ]:
        source += '\n' + function_source(ble, signature)
    # 编译真实回调中与本次修复相关的完整分支，SDK 其他事件由独立目标回归覆盖。
    handler = function_source(ble, 'static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)\n{')
    source += '\nstatic void gattc_profile_event_handler(esp_gattc_cb_event_t event,int gattc_if,esp_ble_gattc_cb_param_t *param) {switch(event) {\n'
    for name in ['CONNECT', 'CFG_MTU', 'NOTIFY', 'DISCONNECT']:
        start = handler.index(f'    case ESP_GATTC_{name}_EVT:')
        end = handler.find('\n    case ', start + 1)
        if end < 0:
            end = handler.index('\n    default:', start)
        source += handler[start:end]
    source += '\n default:break;}}\n'
    for signature in ['static int app_ble_recv_data_Ack(', 'static bool app_ble_process_auth(', 'void app_ble_data_handle(']:
        source += '\n' + function_source(app, signature)
    with tempfile.TemporaryDirectory(prefix='ble-session-test-') as directory:
        temp = Path(directory)
        (temp / 'test.c').write_text(source + TESTS)
        binary = temp / 'test'
        args = [os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-variable',
                '-Wno-misleading-indentation', str(temp / 'test.c'), '-o', str(binary)]
        if os.environ.get('TEST_SANITIZE') == '1':
            args[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(args, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == '__main__':
    main()
