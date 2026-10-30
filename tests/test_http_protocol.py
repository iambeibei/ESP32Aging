#!/usr/bin/env python3
"""Exercise real HTTP accumulation/ownership and application target routing with SDK mocks.
No device/network needed: python3 tests/test_http_protocol.py.
"""
from pathlib import Path
import os
import subprocess
import tempfile
from test_root_uplink import function_source

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', '/home/wbb/espidf/esp-idf-mesh-lite'))
MOCKS = r'''
#ifndef HTTP_TEST_MOCKS_H
#define HTTP_TEST_MOCKS_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
#include "cJSON.h"
typedef int esp_err_t;
typedef unsigned TickType_t;
typedef struct { pthread_mutex_t lock; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_TIMEOUT 5
#define ESP_ERR_INVALID_RESPONSE 6
#define ESP_ERR_NOT_SUPPORTED 7
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffU
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define HTTP_METHOD_GET 0
#define HTTP_METHOD_POST 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static void app_memory_log(const char *stage) { (void)stage; }
#define LG_URL_HTTP_PROTOCOL_WHOLE_URL "https://protocol"
#define LG_URL_MAX_LEN 320
#define LG_URL_GET_LOST_IDNUM_URL_FMT "http://%s:11282/DeviceServiceCallback/GetMissingIdNums"
static esp_err_t lg_url_build(const char *fmt,char *out,size_t size){snprintf(out,size,fmt,"10.0.0.1");return ESP_OK;}
static const char *esp_err_to_name(esp_err_t e) { (void)e; return "mock"; }
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { pthread_mutex_init(&s->lock,NULL); return s; }
static int xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) { (void)wait; return pthread_mutex_lock(&s->lock)==0; }
static int xSemaphoreGive(SemaphoreHandle_t s) { return pthread_mutex_unlock(&s->lock)==0; }
static bool fail_allocation;
static void *heap_caps_realloc(void *p,size_t n,int caps) { (void)caps; return fail_allocation ? NULL : realloc(p,n); }
typedef struct { unsigned cap,size,count,head; char *data; pthread_mutex_t lock; } *QueueHandle_t;
static QueueHandle_t xQueueCreate(int n,size_t size) {
    QueueHandle_t q=calloc(1,sizeof(*q)); q->cap=n; q->size=size; q->data=calloc(n,size); pthread_mutex_init(&q->lock,NULL); return q;
}
static int xQueueSend(QueueHandle_t q,const void *p,TickType_t wait) {
    (void)wait; pthread_mutex_lock(&q->lock); int ok=q->count<q->cap;
    if(ok) {memcpy(q->data+((q->head+q->count)%q->cap)*q->size,p,q->size); q->count++;}
    pthread_mutex_unlock(&q->lock); return ok;
}
static int xQueueReceive(QueueHandle_t q,void *p,TickType_t wait) {
    (void)wait; pthread_mutex_lock(&q->lock); int ok=q->count>0;
    if(ok) {memcpy(p,q->data+q->head*q->size,q->size); q->head=(q->head+1)%q->cap; q->count--;}
    pthread_mutex_unlock(&q->lock); return ok;
}
enum {HTTP_EVENT_ON_CONNECTED,HTTP_EVENT_ON_DATA,HTTP_EVENT_ON_FINISH,HTTP_EVENT_ERROR,HTTP_EVENT_DISCONNECTED};
typedef struct { int event_id; void *user_data,*data; int data_len; } esp_http_client_event_t;
typedef struct {
    const char *url; int method,timeout_ms,buffer_size,buffer_size_tx; bool keep_alive_enable;
    void *crt_bundle_attach,*user_data; esp_err_t (*event_handler)(esp_http_client_event_t *);
} esp_http_client_config_t;
typedef struct { esp_http_client_config_t config; char body[256]; } *esp_http_client_handle_t;
static void *esp_crt_bundle_attach;
static int http_status=200, mode;
static bool interleave;
static bool fail_client_init,fail_post_field,gap_empty;
static char gap_request[256];
static pthread_barrier_t request_barrier;
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    if(fail_client_init)return NULL;
    esp_http_client_handle_t c=calloc(1,sizeof(*c)); c->config=*config; return c;
}
static esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *k,const char *v) { (void)c;(void)k;(void)v;return ESP_OK; }
static esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c,const char *b,int n) { if(fail_post_field)return ESP_FAIL;assert(n<256);memcpy(c->body,b,n);return ESP_OK; }
static void emit(esp_http_client_handle_t c,int event,void *body,int n) {
    esp_http_client_event_t e={.event_id=event,.user_data=c->config.user_data,.data=body,.data_len=n}; c->config.event_handler(&e);
}
static esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    char body[256]; int id=atoi(c->body);
    snprintf(body,sizeof(body),"{\"data\":{\"protocolTypeStr\":\"%s\",\"id\":%d,\"label\":\"A B\\nC\"}}",id==11?"SCPI":"MODBUS",id);
    if(strstr(c->config.url,"GetMissingIdNums")){
        snprintf(gap_request,sizeof(gap_request),"%s",c->body);
        snprintf(body,sizeof(body),"{\"Data\":{\"Ids\":%s}}",gap_empty?"[]":"[2,0,1]");
    }
    emit(c,HTTP_EVENT_ON_CONNECTED,NULL,0);
    if(mode==4)return ESP_FAIL; /* SDK perform 检测到请求体短写/传输失败。 */
    if(mode==1) { // oversize
        char *large=calloc(1,16385);emit(c,HTTP_EVENT_ON_DATA,large,16385);free(large);
    } else if(mode==2) {
        emit(c,HTTP_EVENT_ON_DATA,body,10);emit(c,HTTP_EVENT_ERROR,NULL,0);return ESP_FAIL;
    } else {
        int half=strlen(body)/2; emit(c,HTTP_EVENT_ON_DATA,body,half);
        if(interleave) pthread_barrier_wait(&request_barrier);
        emit(c,HTTP_EVENT_ON_DATA,body+half,strlen(body)-half);
    }
    emit(c,HTTP_EVENT_ON_FINISH,NULL,0);return ESP_OK;
}
static int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c;return http_status; }
static bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { (void)c;return mode!=2&&mode!=3; }
static esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { emit(c,HTTP_EVENT_DISCONNECTED,NULL,0);free(c);return ESP_OK; }
#endif
'''
APP_MOCKS = r'''
#define MAX_EXTERNAL_DEVICE_COUNT 6
typedef struct { int item_count; } ProtocolItems;
typedef struct { int64_t ProtoID; bool HaveProtocol; size_t step_count; void *scpi_protocol,*steps; ProtocolItems modbusprotocol; } Externaldevice;
static Externaldevice *Aging_device,*read_devices;
static unsigned read_device_count=2, import_count;
static bool block_import, import_entered, release_import, free_started, free_done;
static pthread_mutex_t import_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t import_cond=PTHREAD_COND_INITIALIZER;
static void scpi_dynamic_free(void *p) { free(p); }
static void ModbusJson_Free(ProtocolItems *p) { p->item_count=0; }
static esp_err_t import_protocol(Externaldevice *device,const char *json) {
    cJSON *root=cJSON_Parse(json); assert(root);
    assert(cJSON_GetObjectItem(root,"id")->valueint==device->ProtoID);
    assert(strcmp(cJSON_GetObjectItem(root,"label")->valuestring,"A B\nC")==0);
    pthread_mutex_lock(&import_gate);
    import_entered=true; pthread_cond_broadcast(&import_cond);
    while(block_import && !release_import) pthread_cond_wait(&import_cond,&import_gate);
    pthread_mutex_unlock(&import_gate);
    device->HaveProtocol=true; import_count++; cJSON_Delete(root);return ESP_OK;
}
static esp_err_t import_scpi_protocol_to_device(Externaldevice *d,const char *j) {return import_protocol(d,j);}
static esp_err_t import_modbus_protocol_to_device(Externaldevice *d,const char *j) {return import_protocol(d,j);}
static esp_err_t import_can_protocol_to_device(Externaldevice *d,const char *j) {return import_protocol(d,j);}
'''
TESTS = r'''
static void *external_request(void *arg) { (void)arg;assert(request_device_protocol(HTTP_PROTOCOL_EXTERNAL,0)==ESP_OK);return NULL; }
static void *aging_request(void *arg) { (void)arg;assert(request_device_protocol(HTTP_PROTOCOL_AGING,0)==ESP_OK);return NULL; }
static void *install_response(void *arg) { parse_http_protocol_response(arg); return NULL; }
static void *free_aging_device(void *arg) {
    (void)arg; pthread_mutex_lock(&import_gate);free_started=true;pthread_cond_broadcast(&import_cond);pthread_mutex_unlock(&import_gate);
    AgingDevice_RuntimeFree();
    pthread_mutex_lock(&import_gate);free_done=true;pthread_cond_broadcast(&import_cond);pthread_mutex_unlock(&import_gate);return NULL;
}
static void drain(void) { HttpProtocolResponse r; while(http_receive_protocol(&r,0)==ESP_OK) http_protocol_response_release(&r); }
static int json_alloc_left=-1;
static void *json_allocate(size_t bytes){if(json_alloc_left==0)return NULL;if(json_alloc_left>0)json_alloc_left--;return malloc(bytes);}
int main(void) {
    assert(Http_init()==ESP_OK);QueueHandle_t original=HttpData_queue;assert(Http_init()==ESP_OK && original==HttpData_queue);
    s_protocol_lifecycle_mutex=xSemaphoreCreateMutexStatic(&s_protocol_lifecycle_storage);
    read_devices=calloc(2,sizeof(*read_devices));read_devices[0].ProtoID=11;read_devices[1].ProtoID=33;
    Aging_device=calloc(1,sizeof(*Aging_device));Aging_device->ProtoID=22;
    interleave=true;pthread_barrier_init(&request_barrier,NULL,2);
    pthread_t a,b;pthread_create(&a,NULL,external_request,NULL);pthread_create(&b,NULL,aging_request,NULL);
    pthread_join(a,NULL);pthread_join(b,NULL);interleave=false;
    HttpProtocolResponse first,second;
    assert(http_receive_protocol(&first,0)==ESP_OK && http_receive_protocol(&second,0)==ESP_OK);
    assert(first.target.kind!=second.target.kind);
    parse_http_protocol_response(&second);parse_http_protocol_response(&first);
    assert(import_count==2 && read_devices[0].HaveProtocol && Aging_device->HaveProtocol && !read_devices[1].HaveProtocol);
    http_protocol_response_release(&first);http_protocol_response_release(&first);http_protocol_response_release(&second);
    puts("PASS interleaved requests preserve complete bodies, whitespace and per-device routing");

    assert(request_device_protocol(HTTP_PROTOCOL_AGING,0)==ESP_OK);
    assert(http_receive_protocol(&first,0)==ESP_OK);
    unsigned before=import_count;AgingDevice_RuntimeFree();
    Aging_device=calloc(1,sizeof(*Aging_device));Aging_device->ProtoID=22;
    parse_http_protocol_response(&first);assert(import_count==before && !Aging_device->HaveProtocol);
    http_protocol_response_release(&first);
    assert(request_device_protocol(HTTP_PROTOCOL_AGING,0)==ESP_OK);
    assert(http_receive_protocol(&first,0)==ESP_OK);parse_http_protocol_response(&first);
    assert(Aging_device->HaveProtocol);http_protocol_response_release(&first);
    puts("PASS cancelled aging response cannot target replacement device even with same ProtoID");

    assert(request_device_protocol(HTTP_PROTOCOL_EXTERNAL,0)==ESP_OK);
    assert(http_receive_protocol(&first,0)==ESP_OK);
    assert(request_device_protocol(HTTP_PROTOCOL_EXTERNAL,0)==ESP_OK);
    before=import_count;parse_http_protocol_response(&first);assert(import_count==before);http_protocol_response_release(&first);drain();
    puts("PASS superseded request generation is rejected");

    http_status=503;assert(request_device_protocol(HTTP_PROTOCOL_EXTERNAL,0)==ESP_ERR_INVALID_RESPONSE);assert(HttpData_queue->count==0);http_status=200;
    mode=1;assert(request_device_protocol(HTTP_PROTOCOL_EXTERNAL,0)==ESP_ERR_INVALID_SIZE);assert(HttpData_queue->count==0);
    mode=2;assert(request_device_protocol(HTTP_PROTOCOL_EXTERNAL,0)==ESP_FAIL);assert(HttpData_queue->count==0);mode=0;
    fail_allocation=true;assert(request_device_protocol(HTTP_PROTOCOL_EXTERNAL,0)==ESP_ERR_NO_MEM);fail_allocation=false;
    assert(HttpData_queue->count==0);
    for(int i=0;i<3;i++) assert(request_device_protocol(HTTP_PROTOCOL_EXTERNAL,0)==ESP_OK);
    assert(request_device_protocol(HTTP_PROTOCOL_EXTERNAL,0)==ESP_ERR_TIMEOUT);assert(HttpData_queue->count==3);drain();
    puts("PASS non-2xx, overflow, incomplete data, allocation failure and queue full transfer no partial response");
    test_http_connection_simple();assert(HttpData_queue->count==0);
    assert(request_device_protocol(HTTP_PROTOCOL_EXTERNAL,6)==ESP_ERR_INVALID_ARG);
    assert(request_device_protocol(HTTP_PROTOCOL_AGING,0)==ESP_OK);
    assert(http_receive_protocol(&first,0)==ESP_OK);
    block_import=true;import_entered=false;release_import=false;free_started=free_done=false;
    pthread_create(&a,NULL,install_response,&first);
    pthread_mutex_lock(&import_gate);while(!import_entered)pthread_cond_wait(&import_cond,&import_gate);pthread_mutex_unlock(&import_gate);
    pthread_create(&b,NULL,free_aging_device,NULL);
    pthread_mutex_lock(&import_gate);while(!free_started)pthread_cond_wait(&import_cond,&import_gate);
    assert(!free_done && pthread_mutex_trylock(&s_protocol_lifecycle_mutex->lock)==EBUSY);
    release_import=true;pthread_cond_broadcast(&import_cond);pthread_mutex_unlock(&import_gate);
    pthread_join(a,NULL);pthread_join(b,NULL);assert(Aging_device==NULL && free_done);http_protocol_response_release(&first);
    puts("PASS protocol installation and concurrent device free share lifetime lock");
    AgingDevice_RuntimeFree();free(read_devices);
    char out[512];
    assert(test_http_post_record_query(123,2,"record-A",out,sizeof(out))==ESP_OK);
    cJSON *query=cJSON_Parse(gap_request);assert(query);
    cJSON *data=cJSON_GetObjectItem(query,"Data");
    assert(cJSON_GetObjectItem(query,"Seq")->valueint==123&&cJSON_GetObjectItem(data,"IdNum")->valueint==2);
    assert(!strcmp(cJSON_GetObjectItem(data,"RecordId")->valuestring,"record-A"));cJSON_Delete(query);
    assert(strstr(out,"[2,0,1]")&&HttpData_queue->count==0);
    gap_empty=true;assert(test_http_post_record_query(123,2,"record-A",out,sizeof(out))==ESP_OK&&strstr(out,"[]"));gap_empty=false;
    http_status=503;assert(test_http_post_record_query(123,2,"record-A",out,sizeof(out))==ESP_ERR_INVALID_RESPONSE&&out[0]==0);http_status=200;
    for(int error_mode=1;error_mode<=4;error_mode++){
        mode=error_mode;assert(test_http_post_record_query(123,2,"record-A",out,sizeof(out))!=ESP_OK&&out[0]==0);
    }
    mode=0;assert(test_http_post_record_query(123,2,"record-A",out,8)==ESP_ERR_INVALID_SIZE&&out[0]==0);
    fail_allocation=true;assert(test_http_post_record_query(123,2,"record-A",out,sizeof(out))==ESP_ERR_NO_MEM);fail_allocation=false;
    fail_client_init=true;assert(test_http_post_record_query(123,2,"record-A",out,sizeof(out))==ESP_ERR_NO_MEM);fail_client_init=false;
    fail_post_field=true;assert(test_http_post_record_query(123,2,"record-A",out,sizeof(out))==ESP_FAIL);fail_post_field=false;
    cJSON_Hooks hooks={.malloc_fn=json_allocate,.free_fn=free};cJSON_InitHooks(&hooks);
    for(int allocation=0;allocation<6;allocation++){json_alloc_left=allocation;assert(test_http_post_record_query(123,2,"record-A",out,sizeof(out))==ESP_ERR_NO_MEM);}
    json_alloc_left=-1;cJSON_InitHooks(NULL);
    assert(test_http_post_record_query(123,-1,"record-A",out,sizeof(out))==ESP_ERR_INVALID_ARG);
    assert(test_http_post_record_query(123,2,"",out,sizeof(out))==ESP_ERR_INVALID_ARG);
    puts("PASS real missing-ID HTTP request preserves wire format and rejects non-2xx, short writes, truncation, incomplete responses and allocation failures");
    free(HttpData_queue->data);free(HttpData_queue);
    puts("All HTTP protocol regression scenarios passed");return 0;
}
'''

def main():
    source = (ROOT / 'main/APP/HTTP/GetProto.c').read_text()
    app = (ROOT / 'main/Task/appTask.c').read_text()
    state_start = app.index('static StaticSemaphore_t s_protocol_lifecycle_storage;')
    state_end = app.index('static esp_err_t import_scpi_protocol_to_device', state_start)
    application = app[state_start:state_end] + '\n' + function_source(app, 'static void parse_http_protocol_response(')
    application += '\n' + function_source(app, 'static void AgingDevice_RuntimeFree(void)\n{')
    with tempfile.TemporaryDirectory(prefix='http-protocol-test-') as directory:
        temp = Path(directory)
        (temp / 'mocks.h').write_text(MOCKS)
        for header in ['esp_err.h', 'esp_http_client.h', 'esp_log.h', 'esp_heap_caps.h', 'esp_crt_bundle.h',
                       'service_urls.h', 'app_mem.h', 'freertos/FreeRTOS.h', 'freertos/queue.h']:
            path = temp / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "mocks.h"\n')
        unit = temp / 'test.c'
        unit.write_text(source + APP_MOCKS + application + TESTS)
        binary = temp / 'test'
        cjson = IDF / 'components/json/cJSON'
        args = [os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-pthread',
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
                '-I', str(temp), '-I', str(ROOT / 'main/APP/HTTP'), '-I', str(cjson),
                str(unit), str(cjson / 'cJSON.c'), '-lm', '-o', str(binary)]
        if os.environ.get('TEST_SANITIZE') == '1':
            args[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(args, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)

if __name__ == '__main__':
    main()
