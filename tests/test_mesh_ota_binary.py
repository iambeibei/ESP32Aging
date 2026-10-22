#!/usr/bin/env python3
"""Run actual ota.c + mesh_ota.c against SDK/flash/HTTP mocks on the host.

The closed Mesh-Lite library is NOT emulated: this verifies application admission,
validation, retry and staging, not BINARY interoperability. Hardware is required.
Run: python3 tests/test_mesh_ota_binary.py (cc, OpenSSL headers/libcrypto required).
Optional: ASAN_OPTIONS=detect_leaks=0 TEST_SANITIZE=1 python3 tests/test_mesh_ota_binary.py
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDF = Path('/home/wbb/espidf/esp-idf-mesh-lite')
MOCK = r'''
#ifndef BINARY_OTA_MOCK_H
#define BINARY_OTA_MOCK_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <openssl/sha.h>
#define CONFIG_ESP_MESH_LITE_OTA_ENABLE 1
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_INVALID_VERSION 5
#define ESP_ERR_NO_MEM 6
#define ESP_ERR_NOT_SUPPORTED 7
#define ESP_ERR_INVALID_RESPONSE 8
#define ESP_ERR_OTA_VALIDATE_FAILED 9
typedef int esp_err_t;
static const char *esp_err_to_name(esp_err_t e) { (void)e;return "mock error"; }
static size_t mock_strlcpy(char *out,const char *in,size_t cap) { size_t n=strlen(in);if(cap){size_t k=n<cap-1?n:cap-1;memcpy(out,in,k);out[k]=0;}return n; }
#define strlcpy mock_strlcpy
static unsigned ready_logs;
static time_t wall_time=1782373476;
static time_t mock_time(time_t *out) {if(out)*out=wall_time;return wall_time;}
#define time mock_time
static char *DEVICE_ID="100000010",*AgingNumber="2";
static bool mqtt_connected=true;
static int mqtt_publish_result=42;
static unsigned mqtt_publish_attempts,mqtt_accepted;
static char reported_topic[64],reported_payload[256];
static bool app_mqtt_is_connected(void) {return mqtt_connected;}
static int app_mqtt_publish(char *topic,char *payload,char *device_id) {
    assert(ready_logs && device_id==NULL);
    assert(strlen(topic)<sizeof(reported_topic) && strlen(payload)<sizeof(reported_payload));
    strcpy(reported_topic,topic);strcpy(reported_payload,payload);mqtt_publish_attempts++;
    if(mqtt_publish_result>=0)mqtt_accepted++;
    return mqtt_publish_result;
}
static char last_ready[512];
static void mock_log(const char *tag,const char *fmt,...) {
    (void)tag; va_list args;va_start(args,fmt);
    if(strncmp(fmt,"OTA_BINARY_READY ",17)==0) {ready_logs++;vsnprintf(last_ready,sizeof(last_ready),fmt,args);}
    va_end(args);
}
#define ESP_LOGI mock_log
#define ESP_LOGW mock_log
#define ESP_LOGE mock_log
#define ESP_LOGD mock_log
#define BIT0 1u
#define BIT1 2u
#define BIT2 4u
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(n) ((uint32_t)(n))
typedef uint32_t TickType_t,EventBits_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static TickType_t now;
static TickType_t xTaskGetTickCount(void) {return now;}
static void vTaskDelay(TickType_t ticks) {now+=ticks;}
typedef struct {pthread_mutex_t lock;} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) {assert(!pthread_mutex_init(&s->lock,NULL));return s;}
static int xSemaphoreTake(SemaphoreHandle_t s,TickType_t timeout) {(void)timeout;assert(!pthread_mutex_lock(&s->lock));return pdTRUE;}
static int xSemaphoreGive(SemaphoreHandle_t s) {assert(!pthread_mutex_unlock(&s->lock));return pdTRUE;}
typedef struct {EventBits_t bits;} StaticEventGroup_t;
typedef StaticEventGroup_t *EventGroupHandle_t;
static EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *g) {g->bits=0;return g;}
static EventBits_t xEventGroupSetBits(EventGroupHandle_t g,EventBits_t bits) {return g->bits|=bits;}
static EventBits_t xEventGroupWaitBits(EventGroupHandle_t g,EventBits_t bits,int clear,int all,TickType_t timeout) {
    (void)all;(void)timeout;EventBits_t out=g->bits&bits;if(clear)g->bits&=~bits;return out;
}
static unsigned task_count;
static int xTaskCreatePinnedToCore(void (*fn)(void *),const char *name,int stack,void *arg,int prio,void *handle,int core) {
    (void)fn;(void)name;(void)stack;(void)arg;(void)prio;(void)handle;assert(core==1);task_count++;return pdPASS;
}
#define LG_APP_CPU_CORE 1
#define LG_STACK_MESH_OTA 8192
#define LG_PRIO_MESH_OTA 7
#define LG_STACK_MESH_OTA_DL 8192
#define LG_PRIO_MESH_OTA_DL 4


#define ESP_ERR_NVS_NOT_FOUND 100
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 101
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef unsigned nvs_handle_t;
static unsigned char nvs_blob[1024], nvs_staged[1024];
static size_t nvs_blob_size,nvs_staged_size;
static bool nvs_exists,nvs_deleting,fail_nvs_open,fail_nvs_set,fail_nvs_commit,fail_nvs_erase;
static unsigned nvs_writes,nvs_commits,nvs_erases;
static esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h) {
    assert(!strcmp(name,"ota_cache"));
    if(fail_nvs_open)return ESP_FAIL;
    if(mode==NVS_READONLY && !nvs_exists)return ESP_ERR_NVS_NOT_FOUND;
    *h=1;nvs_staged_size=0;nvs_deleting=false;return ESP_OK;
}
static esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *data,size_t *bytes) {
    assert(h==1 && !strcmp(key,"image_v1"));
    if(!nvs_exists)return ESP_ERR_NVS_NOT_FOUND;
    if(data){assert(*bytes>=nvs_blob_size);memcpy(data,nvs_blob,nvs_blob_size);}*bytes=nvs_blob_size;return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t bytes) {
    assert(h==1 && !strcmp(key,"image_v1") && bytes<=sizeof(nvs_staged));nvs_writes++;
    if(fail_nvs_set)return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    memcpy(nvs_staged,data,bytes);nvs_staged_size=bytes;return ESP_OK;
}
static esp_err_t nvs_erase_key(nvs_handle_t h,const char *key) {
    assert(h==1 && !strcmp(key,"image_v1"));nvs_erases++;
    if(fail_nvs_erase)return ESP_FAIL;
    if(!nvs_exists)return ESP_ERR_NVS_NOT_FOUND;
    nvs_deleting=true;return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h) {
    assert(h==1);nvs_commits++;if(fail_nvs_commit)return ESP_FAIL;
    if(nvs_deleting){nvs_exists=false;nvs_blob_size=0;}
    else if(nvs_staged_size){memcpy(nvs_blob,nvs_staged,nvs_staged_size);nvs_blob_size=nvs_staged_size;nvs_exists=true;}
    return ESP_OK;
}
static void nvs_close(nvs_handle_t h) {(void)h;nvs_staged_size=0;nvs_deleting=false;}
static uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t *data,uint32_t bytes) {
    crc=~crc;for(uint32_t i=0;i<bytes;i++){crc^=data[i];for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^(0xEDB88320u&-(crc&1u));}return ~crc;
}

#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 16
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 17
typedef struct {unsigned size,address;int type,subtype;char label[16];} esp_partition_t;
typedef struct {char version[32];} esp_app_desc_t;
static esp_partition_t running_part={.size=131072,.address=0x10000,.subtype=16,.label="ota_0"};
static esp_partition_t update_part={.size=131072,.address=0x30000,.subtype=17,.label="ota_1"};
static const esp_partition_t *current_part=&running_part,*boot_part=&running_part,*writing_part;
static unsigned char flash_bytes[131072],running_flash_bytes[131072],fixture[4103];
static size_t fixture_size=sizeof(fixture), write_cursor;
static char image_version[32]="1.1.0",running_image_version[32]="1.0.0";
static unsigned begin_count,end_count,abort_count,boot_count;
static bool handle_live,fail_end,fail_read,fail_boot,fail_image_verify;
static uint32_t override_image_size;
static pthread_mutex_t read_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t read_condition=PTHREAD_COND_INITIALIZER;
static bool block_read,read_entered,release_read,start_entered,start_finished;
typedef unsigned esp_ota_handle_t;
#define OTA_SIZE_UNKNOWN ((size_t)-1)
static const esp_partition_t *esp_ota_get_running_partition(void) {return current_part;}
static const esp_partition_t *esp_ota_get_boot_partition(void) {return boot_part;}
static const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) {
    (void)p;return current_part==&running_part?&update_part:&running_part;
}
static const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label) {
    const esp_partition_t *parts[]={&running_part,&update_part};
    for(unsigned i=0;i<2;i++)if(type==0 && parts[i]->subtype==subtype && !strcmp(parts[i]->label,label))return parts[i];
    return NULL;
}
static esp_err_t esp_ota_get_partition_description(const esp_partition_t *p,esp_app_desc_t *d) {
    memset(d,0,sizeof(*d));strlcpy(d->version,p==&running_part?running_image_version:image_version,sizeof(d->version));return ESP_OK;
}
static unsigned char *part_bytes(const esp_partition_t *p) {assert(p==&running_part || p==&update_part);return p==&update_part?flash_bytes:running_flash_bytes;}
static esp_err_t esp_ota_begin(const esp_partition_t *p,size_t size,esp_ota_handle_t *h) {
    (void)size;assert(p!=current_part && !handle_live);memset(part_bytes(p),0xFF,131072);
    begin_count++;handle_live=true;write_cursor=0;writing_part=p;*h=1;return ESP_OK;
}
static esp_err_t esp_ota_write(esp_ota_handle_t h,const void *data,size_t bytes) {
    assert(h==1 && handle_live && bytes<=131072-write_cursor);
    memcpy(part_bytes(writing_part)+write_cursor,data,bytes);write_cursor+=bytes;return ESP_OK;
}
static esp_err_t esp_ota_write_with_offset(esp_ota_handle_t h,const void *data,size_t bytes,uint32_t offset) {
    assert(h==1 && handle_live && offset<=131072 && bytes<=131072-offset);
    memcpy(part_bytes(writing_part)+offset,data,bytes);return ESP_OK;
}
static esp_err_t esp_ota_end(esp_ota_handle_t h) {
    assert(h==1 && handle_live);handle_live=false;end_count++;return fail_end?ESP_ERR_OTA_VALIDATE_FAILED:ESP_OK;
}
static esp_err_t esp_ota_abort(esp_ota_handle_t h) {assert(h==1 && handle_live);handle_live=false;abort_count++;return ESP_OK;}
static esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) {
    assert(nvs_exists && nvs_blob_size>0);boot_count++;
    if(fail_boot)return ESP_FAIL;
    boot_part=p;return ESP_OK;
}
static esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *data,size_t bytes) {
    assert(offset<=131072 && bytes<=131072-offset);if(fail_read)return ESP_FAIL;
    if(block_read){
        pthread_mutex_lock(&read_gate);read_entered=true;pthread_cond_broadcast(&read_condition);
        while(!release_read)pthread_cond_wait(&read_condition,&read_gate);
        pthread_mutex_unlock(&read_gate);
    }
    memcpy(data,part_bytes(p)+offset,bytes);return ESP_OK;
}
typedef struct {uint32_t offset,size;} esp_partition_pos_t;
typedef struct {uint32_t image_len;} esp_image_metadata_t;
#define ESP_IMAGE_VERIFY 0
static esp_err_t esp_image_verify(int mode,const esp_partition_pos_t *pos,esp_image_metadata_t *image) {
    assert(mode==ESP_IMAGE_VERIFY && pos->size==131072);
    if(fail_image_verify)return ESP_ERR_OTA_VALIDATE_FAILED;
    image->image_len=override_image_size?override_image_size:fixture_size;return ESP_OK;
}
static unsigned restart_count;
static void esp_restart(void) {restart_count++;abort();}

typedef const char *esp_event_base_t;
static const char *ESP_MESH_LITE_EVENT="mesh";
#define ESP_EVENT_ANY_ID -1
#define ESP_MESH_LITE_EVENT_OTA_START 1
#define ESP_MESH_LITE_EVENT_OTA_PROGRESS 2
#define ESP_MESH_LITE_EVENT_OTA_FINISH 3
typedef enum {ESP_MESH_LITE_EVENT_OTA_SUCCESS,ESP_MESH_LITE_EVENT_OTA_FAIL} esp_mesh_lite_ota_finish_reason_t;
typedef struct {unsigned percentage;} esp_mesh_lite_event_ota_progress_t;
typedef struct {esp_mesh_lite_ota_finish_reason_t reason;} esp_mesh_lite_event_ota_finish_t;
typedef struct {char *fw_version;int filesize;size_t offset;char *data;size_t data_size;} esp_mesh_lite_lan_ota_file_transfer_param_t;
typedef struct {
    esp_err_t (*provide_file_cb)(esp_mesh_lite_lan_ota_file_transfer_param_t *);
    esp_err_t (*get_file_cb)(esp_mesh_lite_lan_ota_file_transfer_param_t *);
    esp_err_t (*get_file_done)(void);
} esp_mesh_lite_lan_ota_file_transfer_cb_t;
typedef enum {ESP_MESH_LITE_OTA_TRANSMIT_FIRMWARE,ESP_MESH_LITE_OTA_TRANSMIT_BINARY} esp_mesh_lite_ota_transmit_type_t;
typedef struct {
    esp_mesh_lite_ota_transmit_type_t type;union {char fw_version[32],file_name[32];};
    size_t size;esp_err_t (*extern_url_ota_cb)(void);
} esp_mesh_lite_file_transmit_config_t;
static unsigned level=1,IsRoot=1,transmit_count,registration_count,resume_count;
static bool fail_register,fail_resume,fail_transmit;
static char registered_name[32];
static esp_mesh_lite_file_transmit_config_t sent_config;
static unsigned esp_mesh_lite_get_level(void) {return level;}
static esp_err_t esp_mesh_lite_lan_ota_set_file_name(char *name) {
    registration_count++;memcpy(registered_name,name,32);assert(memchr(name,0,32));return fail_register?ESP_FAIL:ESP_OK;
}
static esp_err_t esp_mesh_lite_ota_notify_child_node_restart(void) {resume_count++;return fail_resume?ESP_FAIL:ESP_OK;}
static esp_err_t esp_mesh_lite_ota_notify_child_node_pause(void) {return ESP_OK;}
static esp_err_t esp_mesh_lite_transmit_file_start(esp_mesh_lite_file_transmit_config_t *cfg) {
    assert(cfg->type==ESP_MESH_LITE_OTA_TRANSMIT_BINARY);sent_config=*cfg;transmit_count++;return fail_transmit?ESP_FAIL:ESP_OK;
}
static void esp_mesh_lite_ota_register_file_transfer_cb(esp_mesh_lite_lan_ota_file_transfer_cb_t *cb) {assert(cb && cb->provide_file_cb && cb->get_file_done);}
static esp_err_t esp_event_handler_register(esp_event_base_t b,int id,void (*fn)(void *,esp_event_base_t,int32_t,void *),void *arg) {
    (void)b;(void)id;(void)fn;(void)arg;return ESP_OK;
}

typedef SHA256_CTX mbedtls_sha256_context;
static void mbedtls_sha256_init(mbedtls_sha256_context *ctx) {memset(ctx,0,sizeof(*ctx));}
static int mbedtls_sha256_starts(mbedtls_sha256_context *ctx,int is224) {assert(!is224);return SHA256_Init(ctx)==1?0:-1;}
static int mbedtls_sha256_update(mbedtls_sha256_context *ctx,const unsigned char *p,size_t n) {return SHA256_Update(ctx,p,n)==1?0:-1;}
static int mbedtls_sha256_finish(mbedtls_sha256_context *ctx,unsigned char *out) {return SHA256_Final(out,ctx)==1?0:-1;}
static void mbedtls_sha256_free(mbedtls_sha256_context *ctx) {(void)ctx;}

#define HTTP_EVENT_ERROR 0
#define HTTP_EVENT_ON_CONNECTED 1
#define HTTP_EVENT_HEADER_SENT 2
#define HTTP_EVENT_ON_HEADER 3
#define HTTP_EVENT_ON_DATA 4
#define HTTP_EVENT_ON_FINISH 5
#define HTTP_EVENT_DISCONNECTED 6
#define HTTP_EVENT_REDIRECT 7
typedef struct {int event_id;void *user_data,*data;int data_len;char *header_key,*header_value;} esp_http_client_event_t;
typedef struct {
    const char *url,*cert_pem;void *user_data,*crt_bundle_attach;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    int timeout_ms;bool keep_alive_enable,skip_cert_common_name_check;
} esp_http_client_config_t;
typedef struct {esp_http_client_config_t config;size_t offset;} *esp_http_client_handle_t;
static char server_metadata[1024]="{\"version\":\"1.1.0\",\"file\":\"fw_v1.1.0.bin\"}";
static unsigned metadata_reads,body_downloads;
static int http_status=200;
static bool incomplete_body,unknown_size;
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    esp_http_client_handle_t c=calloc(1,sizeof(*c));c->config=*config;return c;
}
static esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    metadata_reads++;esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_DATA,.user_data=c->config.user_data,
        .data=server_metadata,.data_len=(int)strlen(server_metadata)};return c->config.event_handler(&e);
}
static int esp_http_client_get_status_code(esp_http_client_handle_t c) {(void)c;return http_status;}
static esp_err_t esp_http_client_open(esp_http_client_handle_t c,int n) {(void)n;if(c->config.keep_alive_enable)body_downloads++;return ESP_OK;}
static int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {(void)c;return unknown_size?-1:(int64_t)fixture_size;}
static int esp_http_client_read(esp_http_client_handle_t c,char *data,int cap) {
    size_t size=fixture_size-(incomplete_body?1:0);size_t n=size-c->offset;if(n>(size_t)cap)n=cap;
    memcpy(data,fixture+c->offset,n);c->offset+=n;return (int)n;
}
static bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {(void)c;return !incomplete_body;}
static esp_err_t esp_http_client_close(esp_http_client_handle_t c) {(void)c;return ESP_OK;}
static esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {free(c);return ESP_OK;}
esp_err_t lg_url_build(const char *fmt,char *out,size_t cap) {(void)fmt;strlcpy(out,"http://host/download?fileName=",cap);return ESP_OK;}
#endif
'''
TESTS = r'''
static void reset_case(bool root) {
    reset_receiver_locked();s_ready_file=(mesh_ready_file_t){0};s_finalization_pending=false;
    s_root_downloading=false;s_root_timeout_logged=false;s_finish_wait_logged=false;
    s_retry_count=0;s_retry_waiting=false;s_metadata_valid=false;s_firmware_size_bytes=0;
    s_candidate_record=(ota_cache_record_t){0};s_candidate_persisted=s_candidate_recovered=false;
    s_recovery_pending=s_bootstrap_pending=s_request_deferred=false;s_bootstrap_attempts=0;s_bootstrap_retry_tick=0;
    s_report_device_id[0]=s_report_aging_number[0]=0;
    nvs_exists=nvs_deleting=false;nvs_blob_size=nvs_staged_size=0;nvs_writes=nvs_commits=nvs_erases=0;
    fail_nvs_open=fail_nvs_set=fail_nvs_commit=fail_nvs_erase=false;
    current_part=boot_part=&running_part;writing_part=NULL;override_image_size=0;fail_image_verify=false;
    block_read=read_entered=release_read=start_entered=start_finished=false;
    strlcpy(running_image_version,"1.0.0",sizeof(running_image_version));
    s_otacp_pending=false;s_otacp_payload_ready=false;s_otacp_retry_tick=0;s_otacp_seq=0;
    mqtt_connected=true;mqtt_publish_result=42;mqtt_publish_attempts=mqtt_accepted=0;
    reported_topic[0]=reported_payload[0]=0;wall_time=1782373476;DEVICE_ID="100000010";AgingNumber="2";
    s_metadata=(simple_ota_metadata_t){0};s_download_result_valid=false;
    simple_ota_set_state(SIMPLE_OTA_STATE_IDLE);s_download_error=ESP_OK;
    IsRoot=root;level=root?1:2;now=0;s_cmd_events->bits=0;s_dl_events->bits=0;
    fail_end=fail_read=fail_boot=fail_register=fail_resume=fail_transmit=false;
    incomplete_body=unknown_size=false;http_status=200;
    begin_count=end_count=abort_count=boot_count=ready_logs=restart_count=0;
    metadata_reads=body_downloads=transmit_count=registration_count=resume_count=0;
    fixture_size=sizeof(fixture);strlcpy(image_version,"1.1.0",sizeof(image_version));
    strlcpy(server_metadata,"{\"version\":\"1.1.0\",\"file\":\"fw_v1.1.0.bin\"}",sizeof(server_metadata));
    memset(flash_bytes,0xA5,sizeof(flash_bytes));
    for(size_t i=0;i<sizeof(fixture);i++)fixture[i]=(unsigned char)(i*31u+17u);
    memcpy(running_flash_bytes,fixture,sizeof(fixture));
}

/* 模拟硬复位：Flash/NVS/启动分区保留，所有 OTA RAM 和 SDK 会话丢失。 */
static void simulate_boot(const esp_partition_t *running) {
    handle_live=false;s_ota_handle=0;s_update_partition=NULL;
    s_session_open=s_rx_accepting=s_rx_done=s_rx_validated=s_rx_failed=s_finish_received=false;
    s_ready_file=(mesh_ready_file_t){0};memset(s_registered_file_name,0,sizeof(s_registered_file_name));
    s_candidate_record=(ota_cache_record_t){0};s_candidate_persisted=s_candidate_recovered=false;
    s_retry_count=0;s_retry_waiting=s_finalization_pending=s_root_downloading=false;
    s_root_timeout_logged=s_finish_wait_logged=false;s_metadata_valid=false;s_firmware_size_bytes=0;
    s_metadata=(simple_ota_metadata_t){0};s_download_result_valid=false;
    s_otacp_pending=s_otacp_payload_ready=false;s_otacp_seq=0;s_otacp_retry_tick=0;
    s_report_device_id[0]=s_report_aging_number[0]=0;s_otacp_topic[0]=s_otacp_payload[0]=0;
    s_recovery_pending=true;s_bootstrap_pending=s_request_deferred=false;s_bootstrap_attempts=0;
    s_bootstrap_retry_tick=0;current_part=running;now=0;wall_time+=300;
    simple_ota_set_state(SIMPLE_OTA_STATE_IDLE);s_cmd_events->bits=s_dl_events->bits=0;
    ready_logs=registration_count=resume_count=mqtt_publish_attempts=mqtt_accepted=0;
    begin_count=end_count=abort_count=boot_count=metadata_reads=body_downloads=0;
    nvs_writes=nvs_commits=nvs_erases=0;nvs_staged_size=0;nvs_deleting=false;
    fail_nvs_open=fail_nvs_set=fail_nvs_commit=fail_nvs_erase=false;
    fail_boot=fail_register=fail_resume=fail_read=fail_image_verify=false;
}
static void run_root_download(void) {
    mesh_ota_service_poll(CMD_EVT_REQUEST);assert(s_root_downloading);
    s_download_error=simple_ota_check_and_update_default();
    mesh_ota_service_poll(CMD_EVT_DOWNLOAD_DONE);
}
static void terminal(esp_mesh_lite_ota_finish_reason_t reason) {
    esp_mesh_lite_event_ota_finish_t e={.reason=reason};
    mesh_ota_event_handler(NULL,ESP_MESH_LITE_EVENT,ESP_MESH_LITE_EVENT_OTA_FINISH,&e);
    mesh_ota_service_poll(CMD_EVT_FINISH);
}
static esp_mesh_lite_lan_ota_file_transfer_param_t packet(char *data,size_t offset,size_t n) {
    return (esp_mesh_lite_lan_ota_file_transfer_param_t){.fw_version=s_metadata.file,
        .filesize=(int)aligned_file_size(fixture_size),.data=data,.offset=offset,.data_size=n};
}
static void receive_body(void) {
    mesh_ota_service_poll(CMD_EVT_REQUEST);assert(transmit_count==1 && body_downloads==0);
    /* 接收乱序：后半先到，重复同一块不破坏结果。 */
    size_t half=fixture_size/2;
    esp_mesh_lite_lan_ota_file_transfer_param_t p=packet((char *)fixture+half,half,fixture_size-half);
    assert(get_file_cb(&p)==ESP_OK);assert(get_file_cb(&p)==ESP_OK);
    p=packet((char *)fixture,0,half);assert(get_file_cb(&p)==ESP_OK);
}
static void check_hash(void) {
    unsigned char hash[32];char hex[65];SHA256(fixture,fixture_size,hash);
    for(size_t i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",hash[i]);
    assert(strcmp(s_ready_file.sha256_hex,hex)==0 && strstr(last_ready,hex));
}
static void check_report(const char *device_id,const char *aging_number) {
    char topic[64];snprintf(topic,sizeof(topic),"device/%s/event/OTACP",device_id);
    assert(mqtt_accepted==1 && !strcmp(reported_topic,topic));
    cJSON *root=cJSON_Parse(reported_payload);assert(root && cJSON_GetArraySize(root)==2);
    cJSON *seq=cJSON_GetObjectItem(root,"Seq"),*data=cJSON_GetObjectItem(root,"Data");
    assert(cJSON_IsNumber(seq) && seq->valuedouble==1782373476 && cJSON_IsObject(data));
    assert(cJSON_GetArraySize(data)==2 && cJSON_GetObjectItem(data,"IsComplete")->valueint==1);
    cJSON *aging=cJSON_GetObjectItem(data,"AgingNumber");assert(cJSON_IsString(aging) && !strcmp(aging->valuestring,aging_number));
    cJSON_Delete(root);
}

static void *concurrent_provider(void *arg) {
    esp_mesh_lite_lan_ota_file_transfer_param_t p={.fw_version="fw_v1.0.0.bin",.filesize=65536,
        .offset=0,.data=arg,.data_size=16};assert(provide_file_cb(&p)==ESP_OK);return NULL;
}
static void *concurrent_withdrawal(void *arg) {
    (void)arg;pthread_mutex_lock(&read_gate);start_entered=true;pthread_cond_broadcast(&read_condition);
    pthread_mutex_unlock(&read_gate);mesh_ota_service_poll(CMD_EVT_REQUEST);
    pthread_mutex_lock(&read_gate);start_finished=true;pthread_cond_broadcast(&read_condition);pthread_mutex_unlock(&read_gate);return NULL;
}
int main(void) {
    assert(mesh_ota_init()==ESP_OK);assert(task_count==2);assert(mesh_ota_init()==ESP_OK && task_count==2);

    reset_case(true);run_root_download();
    assert(ready_logs==1 && s_ready_file.valid && begin_count==1 && boot_count==1 && restart_count==0);
    assert(metadata_reads==1 && body_downloads==1 && strcmp(registered_name,"fw_v1.1.0.bin")==0);
    for(size_t i=strlen(registered_name);i<32;i++)assert(registered_name[i]==0);
    check_hash();simple_ota_download_result_t r;assert(simple_ota_get_download_result(&r)==ESP_OK);
    check_report("100000010","2");
    assert(r.size_bytes==fixture_size && r.partition==&update_part && !strcmp(r.metadata.version,"1.1.0"));
    strlcpy(server_metadata,"{\"version\":\"9.0.0\",\"file\":\"changed_v9.0.0.bin\"}",sizeof(server_metadata));
    assert(simple_ota_get_download_result(&r)==ESP_OK && !strcmp(r.metadata.file,"fw_v1.1.0.bin"));
    assert(metadata_reads==1);mesh_ota_service_poll(CMD_EVT_REQUEST);assert(begin_count==1 && ready_logs==1);
    puts("PASS root stages exact download snapshot, hashes full file, no reboot or duplicate erase");

    char out[64];memset(out,0,sizeof(out));
    esp_mesh_lite_lan_ota_file_transfer_param_t p={.fw_version=registered_name,.filesize=65536,
        .offset=fixture_size-7,.data=out,.data_size=sizeof(out)};
    memset(flash_bytes+fixture_size,0xA5,100);assert(provide_file_cb(&p)==ESP_OK);
    assert(!memcmp(out,fixture+fixture_size-7,7));for(unsigned i=7;i<sizeof(out);i++)assert((unsigned char)out[i]==0xFF);
    p.fw_version="wrong.bin";assert(provide_file_cb(&p)==ESP_ERR_NOT_FOUND);
    p.fw_version=registered_name;p.offset=SIZE_MAX-8;assert(provide_file_cb(&p)==ESP_ERR_INVALID_SIZE);
    p.offset=65530;p.data_size=7;assert(provide_file_cb(&p)==ESP_ERR_INVALID_SIZE);
    p.offset=0;p.data_size=7;p.filesize=4103;assert(provide_file_cb(&p)==ESP_ERR_INVALID_SIZE);
    puts("PASS provide exact filename, padded range bounds and FF tail instead of stale flash");

    reset_case(true);strlcpy(server_metadata,"{\"version\":\"1.0.0\",\"file\":\"fw_v1.0.0.bin\"}",sizeof(server_metadata));
    run_root_download();assert(simple_ota_get_state()==SIMPLE_OTA_STATE_IDLE && ready_logs==1 && registration_count==1);
    assert(s_ready_file.partition==current_part && !begin_count && !body_downloads && !boot_count);
    assert(!mqtt_publish_attempts && !s_otacp_pending);
    memset(&r,0xA5,sizeof(r));assert(simple_ota_get_download_result(&r)==ESP_ERR_NOT_FOUND && r.partition==NULL);
    puts("PASS no-new-version bootstraps only verified running image without download or fake OTACP");

    reset_case(true);incomplete_body=true;run_root_download();
    assert(!ready_logs && !registration_count && abort_count==1 && !handle_live && s_retry_waiting);
    assert(simple_ota_get_download_result(&r)==ESP_ERR_NOT_FOUND);
    assert(!mqtt_publish_attempts && !s_otacp_pending);
    reset_case(true);strlcpy(image_version,"9.0.0",sizeof(image_version));run_root_download();
    assert(!ready_logs && !boot_count && !handle_live && !abort_count);
    puts("PASS incomplete HTTP and wrong embedded version cannot select boot or publish READY");

    reset_case(true);fail_register=true;run_root_download();assert(!ready_logs && !s_ready_file.valid && s_finalization_pending);
    fail_register=false;now=s_retry_deadline;mesh_ota_service_poll(0);
    assert(ready_logs==1 && begin_count==1 && body_downloads==1);
    reset_case(true);fail_resume=true;run_root_download();assert(!ready_logs && !s_ready_file.valid);
    fail_resume=false;now=s_retry_deadline;mesh_ota_service_poll(0);assert(ready_logs==1 && begin_count==1);
    puts("PASS registration/resume failures retry finalization without redownload or early READY");

    reset_case(false);receive_body();assert(get_file_done_cb()==ESP_OK);assert(!ready_logs);
    terminal(ESP_MESH_LITE_EVENT_OTA_SUCCESS);assert(ready_logs==1 && s_ready_file.valid && !restart_count);check_hash();
    check_report("100000010","2");
    assert(!memcmp(flash_bytes,fixture,fixture_size) && body_downloads==0);
    p=packet((char *)fixture,0,16);assert(get_file_cb(&p)==ESP_ERR_INVALID_STATE);
    mesh_ota_service_poll(CMD_EVT_REQUEST);assert(begin_count==1);
    puts("PASS child accepts out-of-order/repeated blocks, validates locally, becomes provider without reboot");

    reset_case(false);receive_body();terminal(ESP_MESH_LITE_EVENT_OTA_SUCCESS);
    assert(!ready_logs && !registration_count && simple_ota_get_state()==SIMPLE_OTA_STATE_RUNNING);
    assert(get_file_done_cb()==ESP_OK);mesh_ota_service_poll(CMD_EVT_FINISH);assert(ready_logs==1);
    reset_case(false);mesh_ota_service_poll(CMD_EVT_REQUEST);terminal(ESP_MESH_LITE_EVENT_OTA_SUCCESS);
    assert(!ready_logs);now+=LG_MESH_OTA_DONE_WAIT_MS;mesh_ota_service_poll(0);
    assert(!registration_count && simple_ota_get_state()==SIMPLE_OTA_STATE_FAILED);
    puts("PASS SDK success cannot bypass missing local validation; late done is handled");

    reset_case(false);receive_body();fail_end=true;assert(get_file_done_cb()==ESP_ERR_OTA_VALIDATE_FAILED);
    terminal(ESP_MESH_LITE_EVENT_OTA_SUCCESS);assert(!ready_logs && !boot_count && abort_count==0);
    reset_case(false);receive_body();strlcpy(image_version,"9.0.0",sizeof(image_version));
    assert(get_file_done_cb()==ESP_ERR_INVALID_VERSION);terminal(ESP_MESH_LITE_EVENT_OTA_SUCCESS);
    assert(!ready_logs && !boot_count && !registration_count);
    puts("PASS failed image/version validation never becomes ready and never aborts a consumed handle");

    reset_case(false);receive_body();terminal(ESP_MESH_LITE_EVENT_OTA_FAIL);
    assert(abort_count==1 && !handle_live && !s_ready_file.valid && s_retry_waiting);
    now=s_retry_deadline;mesh_ota_service_poll(0);assert(transmit_count==2 && metadata_reads==1);
    p=packet((char *)fixture,0,16);assert(get_file_cb(&p)==ESP_OK && begin_count==2);
    terminal(ESP_MESH_LITE_EVENT_OTA_FAIL);now=s_retry_deadline;mesh_ota_service_poll(0);
    terminal(ESP_MESH_LITE_EVENT_OTA_FAIL);assert(s_retry_count==2 && !s_retry_waiting);
    puts("PASS failed session aborts before retry, metadata reused and retry count bounded");

    reset_case(false);mesh_ota_service_poll(CMD_EVT_REQUEST);
    p=packet((char *)fixture,65530,20);assert(get_file_cb(&p)==ESP_ERR_INVALID_SIZE && !begin_count);
    terminal(ESP_MESH_LITE_EVENT_OTA_FAIL);assert(!ready_logs);
    reset_case(false);mesh_ota_service_poll(CMD_EVT_REQUEST);
    p=packet((char *)fixture,0,16);p.fw_version="wrong.bin";assert(get_file_cb(&p)==ESP_ERR_INVALID_ARG && !begin_count);
    terminal(ESP_MESH_LITE_EVENT_OTA_FAIL);
    puts("PASS receive rejects mismatched name and out-of-bounds block before flash erase");

    reset_case(false);strlcpy(server_metadata,"{\"version\":\"1.1.0\",\"file\":\"long_filename_exceeds_31bytes_v1.1.0.bin\"}",sizeof(server_metadata));
    mesh_ota_service_poll(CMD_EVT_REQUEST);assert(!transmit_count && !begin_count && !ready_logs);
    reset_case(true);strlcpy(server_metadata,"{\"version\":\"1.1.0\",\"file\":\"long_filename_exceeds_31bytes_v1.1.0.bin\"}",sizeof(server_metadata));
    run_root_download();assert(!body_downloads && !begin_count && !ready_logs);
    puts("PASS overlong filename rejected without truncation by both root and child");

    reset_case(true);
    snprintf(server_metadata,sizeof(server_metadata),"{\"version\":\"1.1.0\",\"file\":\"%s_v1.1.0.bin\"}","xxxxxxxxxxxxxxxxxxxx");
    run_root_download();assert(ready_logs==1 && strlen(registered_name)==31 && registered_name[31]==0);
    reset_case(true);unknown_size=true;run_root_download();
    assert(ready_logs==1 && simple_ota_get_download_result(&r)==ESP_OK && r.size_bytes==sizeof(fixture));
    puts("PASS exact 31-byte filename and unknown HTTP Content-Length keep exact download size");

    reset_case(false);receive_body();assert(get_file_done_cb()==ESP_OK);fail_read=true;
    terminal(ESP_MESH_LITE_EVENT_OTA_SUCCESS);assert(!ready_logs && !registration_count && s_finalization_pending);
    fail_read=false;now=s_retry_deadline;mesh_ota_service_poll(0);assert(ready_logs==1 && begin_count==1);
    puts("PASS SHA read failure cannot advertise READY; finalization retry preserves downloaded file");

    reset_case(false);receive_body();fail_boot=true;assert(get_file_done_cb()==ESP_OK && !boot_count);
    terminal(ESP_MESH_LITE_EVENT_OTA_SUCCESS);assert(!ready_logs && !registration_count && abort_count==0 && nvs_exists);
    assert(s_finalization_pending && simple_ota_get_state()==SIMPLE_OTA_STATE_RUNNING);
    reset_case(true);fail_end=true;run_root_download();
    assert(!ready_logs && !registration_count && !boot_count && abort_count==0);
    assert(simple_ota_get_download_result(&r)==ESP_ERR_NOT_FOUND);
    puts("PASS boot-selection and HTTP image-validation failures do not leave a ready file or consumed handle");

    reset_case(true);mesh_ota_service_poll(CMD_EVT_REQUEST);now+=LG_MESH_OTA_ROOT_DL_TIMEOUT_MS;
    mesh_ota_service_poll(0);assert(s_root_downloading && !s_retry_waiting);
    mesh_ota_service_poll(CMD_EVT_REQUEST);assert(!body_downloads && !s_retry_waiting);
    s_download_error=simple_ota_check_and_update_default();mesh_ota_service_poll(CMD_EVT_DOWNLOAD_DONE);
    assert(ready_logs==1 && begin_count==1);
    puts("PASS slow root worker cannot race a second download; late completion remains valid");

    reset_case(false);level=0;mesh_ota_service_poll(CMD_EVT_REQUEST);
    assert(transmit_count==1 && !s_root_downloading && !body_downloads);
    assert(mesh_ota_extern_disabled()==ESP_FAIL);mesh_ota_service_poll(CMD_EVT_FINISH);
    assert(simple_ota_get_state()==SIMPLE_OTA_STATE_FAILED && s_retry_waiting);
    puts("PASS orphan child never takes root HTTP path; external fallback failure is observable");

    reset_case(false);DEVICE_ID="100000011";AgingNumber="3";receive_body();
    assert(!mqtt_publish_attempts && get_file_done_cb()==ESP_OK && !mqtt_publish_attempts);
    terminal(ESP_MESH_LITE_EVENT_OTA_SUCCESS);check_report("100000011","3");
    mesh_ota_service_poll(CMD_EVT_REQUEST);now+=10000;mesh_ota_service_poll(0);
    assert(mqtt_publish_attempts==1 && begin_count==1);
    puts("PASS root and child OTACP use own device/rack identity, exact schema and no duplicate erase/report");

    reset_case(true);mqtt_connected=false;run_root_download();
    assert(s_otacp_pending && !mqtt_publish_attempts && s_ready_file.valid);
    char saved_payload[256];strcpy(saved_payload,s_otacp_payload);
    wall_time+=100;DEVICE_ID="changed";AgingNumber="7";mqtt_connected=true;now=s_otacp_retry_tick;
    mesh_ota_service_poll(0);check_report("100000010","2");
    assert(!s_otacp_pending && !strcmp(saved_payload,reported_payload) && begin_count==1);
    puts("PASS disconnected completion is reported after reconnect with original timestamp and prepared identity");

    reset_case(true);mqtt_publish_result=-2;run_root_download();
    assert(mqtt_publish_attempts==1 && !mqtt_accepted && s_otacp_pending);
    strcpy(saved_payload,reported_payload);wall_time+=100;mesh_ota_service_poll(0);assert(mqtt_publish_attempts==1);
    mqtt_publish_result=42;now=s_otacp_retry_tick;mesh_ota_service_poll(0);
    check_report("100000010","2");assert(mqtt_publish_attempts==2 && !strcmp(saved_payload,reported_payload));
    now+=10000;mesh_ota_service_poll(0);assert(mqtt_publish_attempts==2 && body_downloads==1);
    puts("PASS outbox rejection retries same OTACP payload every 5 seconds until SDK accepts once");

    reset_case(false);AgingNumber="rack\"2\n";receive_body();assert(get_file_done_cb()==ESP_OK);
    terminal(ESP_MESH_LITE_EVENT_OTA_SUCCESS);check_report("100000010","rack\"2\n");

    puts("PASS OTACP AgingNumber is a JSON-escaped string");

    reset_case(true);fail_nvs_set=true;run_root_download();
    assert(!nvs_exists && !boot_count && !ready_logs && !mqtt_publish_attempts && s_finalization_pending);
    assert(simple_ota_get_state()==SIMPLE_OTA_STATE_RUNNING);
    fail_nvs_set=false;now=s_retry_deadline;mesh_ota_service_poll(0);
    assert(nvs_exists && boot_count==1 && ready_logs==1 && begin_count==1 && body_downloads==1);
    puts("PASS NVS capacity failure holds boot selection and OTACP; retry persists without redownload");

    reset_case(false);receive_body();assert(get_file_done_cb()==ESP_OK && !boot_count && !nvs_exists);
    fail_nvs_commit=true;terminal(ESP_MESH_LITE_EVENT_OTA_SUCCESS);
    assert(!nvs_exists && !boot_count && !ready_logs && !mqtt_publish_attempts);
    simulate_boot(&running_part);mesh_ota_service_poll(0);
    assert(!ready_logs && !boot_count && !s_ready_file.valid && !mqtt_publish_attempts);
    puts("PASS power loss before NVS commit leaves old boot and cannot advertise an unrecorded staged image");

    reset_case(true);fail_boot=true;run_root_download();
    ota_cache_record_t saved;assert(ota_cache_load(&saved)==ESP_OK && !strcmp(saved.version,"1.1.0"));
    assert(saved.completed_seq==1782373476 && boot_part==&running_part && !ready_logs);
    simulate_boot(&running_part);mesh_ota_service_poll(0);
    assert(ready_logs==1 && s_ready_file.partition==&update_part && boot_part==&update_part && boot_count==1);
    assert(simple_ota_get_state()==SIMPLE_OTA_STATE_PENDING_RESTART && !body_downloads && !metadata_reads && !nvs_writes);
    check_report("100000010","2");
    puts("PASS committed cache survives cut before boot selection, resumes staged supply and original OTACP");

    reset_case(true);fail_register=true;run_root_download();assert(nvs_exists && boot_part==&update_part && !ready_logs);
    simulate_boot(boot_part);mqtt_connected=false;DEVICE_ID="changed";AgingNumber="7";mesh_ota_service_poll(0);
    assert(ready_logs==1 && s_ready_file.partition==current_part && simple_ota_get_state()==SIMPLE_OTA_STATE_IDLE);
    assert(!boot_count && !nvs_writes && !metadata_reads && !body_downloads && s_otacp_pending);
    p=(esp_mesh_lite_lan_ota_file_transfer_param_t){.fw_version="fw_v1.1.0.bin",.filesize=65536,.offset=0,.data=out,.data_size=sizeof(out)};
    assert(provide_file_cb(&p)==ESP_OK && !memcmp(out,fixture,sizeof(out)));
    mqtt_connected=true;now=s_otacp_retry_tick;mesh_ota_service_poll(0);check_report("100000010","2");
    puts("PASS rebooted new firmware serves running partition offline and replays original device/Seq notification");

    /* 已运行 B 的缓存恢复不得把状态误留为 pending，后续 C 写回 ota_0。 */
    strcpy(server_metadata,"{\"version\":\"1.2.0\",\"file\":\"fw_v1.2.0.bin\"}");
    strcpy(running_image_version,"1.2.0");DEVICE_ID="100000010";AgingNumber="2";
    run_root_download();assert(begin_count==1 && boot_part==&running_part && s_ready_file.partition==&running_part);
    assert(simple_ota_get_state()==SIMPLE_OTA_STATE_PENDING_RESTART && !strcmp(s_ready_file.version,"1.2.0"));
    puts("PASS recovery of running firmware permits next upgrade into the opposite slot");

    reset_case(true);run_root_download();simulate_boot(boot_part);
    nvs_blob[offsetof(ota_cache_record_t,version)]^=1;
    strcpy(server_metadata,"{\"version\":\"1.2.0\",\"file\":\"fw_v1.2.0.bin\"}");mesh_ota_service_poll(0);
    assert(!ready_logs && !s_ready_file.valid && !mqtt_publish_attempts && nvs_erases==1 && !nvs_exists);
    puts("PASS CRC-corrupt record is rejected and only OTA key is invalidated, with no false completion");

    reset_case(true);run_root_download();simulate_boot(boot_part);flash_bytes[100]^=1;fail_image_verify=true;
    mesh_ota_service_poll(0);assert(!ready_logs && !s_ready_file.valid && !mqtt_publish_attempts && !nvs_exists);
    puts("PASS damaged image cannot restore supply even with valid NVS metadata");

    reset_case(true);run_root_download();simulate_boot(boot_part);flash_bytes[100]^=1;
    strcpy(server_metadata,"{\"version\":\"1.2.0\",\"file\":\"fw_v1.2.0.bin\"}");mesh_ota_service_poll(0);
    assert(!ready_logs && !nvs_exists && !mqtt_publish_attempts);
    puts("PASS full BIN SHA mismatch rejects a record even when image parser reports success");

    reset_case(true);run_root_download();strcpy(running_image_version,"1.2.0");simulate_boot(&running_part);
    strcpy(server_metadata,"{\"version\":\"1.2.0\",\"file\":\"fw_v1.2.0.bin\"}");mesh_ota_service_poll(0);
    assert(ready_logs==1 && s_ready_file.partition==&running_part && !strcmp(s_ready_file.version,"1.2.0"));
    assert(!boot_count && !mqtt_publish_attempts && !body_downloads && simple_ota_get_state()==SIMPLE_OTA_STATE_IDLE);
    assert(ota_cache_load(&saved)==ESP_OK && saved.origin==OTA_CACHE_ORIGIN_BOOTSTRAP);
    puts("PASS newer running version discards stale cache and bootstraps current image without downgrade or OTACP");

    reset_case(true);strcpy(server_metadata,"{\"version\":\"1.0.0\",\"file\":\"fw_v1.0.0.bin\"}");
    s_recovery_pending=true;override_image_size=sizeof(fixture)-1;mesh_ota_service_poll(0);
    assert(!ready_logs && !nvs_exists && !boot_count && !body_downloads);
    override_image_size=0;now=s_bootstrap_retry_tick;mesh_ota_service_poll(0);
    assert(ready_logs==1 && s_ready_file.partition==current_part && !mqtt_publish_attempts && nvs_exists);
    assert(!strcmp(s_ready_file.file_name,"fw_v1.0.0.bin"));
    simulate_boot(current_part);http_status=503;mesh_ota_service_poll(0);
    assert(ready_logs==1 && !metadata_reads && !body_downloads && !mqtt_publish_attempts);
    puts("PASS direct-flash bootstrap requires exact image size and then recovers without server or fake OTACP");

    reset_case(true);fail_nvs_open=true;run_root_download();
    assert(!begin_count && !boot_count && !ready_logs && !mqtt_publish_attempts && !nvs_erases);
    puts("PASS unreadable NVS prevents erasing a potentially cached partition");

    reset_case(true);run_root_download();s_ready_file.valid=false;
    fail_nvs_commit=true;unsigned erases_before=nvs_erases;
    assert(ota_cache_before_write(&update_part)!=ESP_OK && nvs_exists && nvs_erases==erases_before+1);
    fail_nvs_commit=false;assert(ota_cache_before_write(&update_part)==ESP_OK && !nvs_exists);
    puts("PASS overlapping cache must be invalidated and committed before overwrite admission");

    reset_case(true);run_root_download();simulate_boot(&running_part);mqtt_connected=false;mesh_ota_service_poll(CMD_EVT_REQUEST);
    assert(ready_logs==1 && s_request_deferred && !s_root_downloading && !body_downloads && s_otacp_pending);
    mqtt_connected=true;now=s_otacp_retry_tick;mesh_ota_service_poll(0);assert(!s_otacp_pending && !s_request_deferred);
    check_report("100000010","2");

    puts("PASS startup recovery and original completion report precede a deferred upgrade command");

    reset_case(false);receive_body();assert(get_file_done_cb()==ESP_OK);terminal(ESP_MESH_LITE_EVENT_OTA_SUCCESS);
    simulate_boot(boot_part);mqtt_connected=false;mesh_ota_service_poll(0);
    assert(IsRoot==0 && ready_logs==1 && s_ready_file.partition==current_part && !s_root_downloading);
    p=(esp_mesh_lite_lan_ota_file_transfer_param_t){.fw_version="fw_v1.1.0.bin",.filesize=65536,.offset=0,.data=out,.data_size=sizeof(out)};
    assert(provide_file_cb(&p)==ESP_OK && !memcmp(out,fixture,sizeof(out)) && !metadata_reads && !body_downloads);
    puts("PASS intermediate child recovers current-slot supply without taking the root download path");

    reset_case(false);fail_nvs_open=true;mesh_ota_service_poll(CMD_EVT_REQUEST);
    assert(!transmit_count && !begin_count && !boot_count && !ready_logs);
    puts("PASS child cannot start LAN transfer when persistent invalidation cannot be checked");

    reset_case(true);run_root_download();simulate_boot(boot_part);nvs_blob_size=12;
    strcpy(server_metadata,"{\"version\":\"1.2.0\",\"file\":\"fw_v1.2.0.bin\"}");mesh_ota_service_poll(0);
    assert(!ready_logs && !nvs_exists && !mqtt_publish_attempts);
    puts("PASS truncated persistent record is rejected before dereferencing partition or strings");

    reset_case(true);run_root_download();simulate_boot(boot_part);mesh_ota_service_poll(0);
    strcpy(server_metadata,"{\"version\":\"1.2.0\",\"file\":\"fw_v1.2.0.bin\"}");
    strcpy(running_image_version,"1.2.0");incomplete_body=true;run_root_download();
    assert(ota_cache_load(&saved)==ESP_OK && !strcmp(saved.version,"1.1.0") && saved.partition_address==update_part.address);
    incomplete_body=false;simulate_boot(&update_part);mesh_ota_service_poll(0);
    assert(ready_logs==1 && !strcmp(s_ready_file.version,"1.1.0") && s_ready_file.partition==current_part);
    p=(esp_mesh_lite_lan_ota_file_transfer_param_t){.fw_version="fw_v1.2.0.bin",.filesize=65536,.offset=0,.data=out,.data_size=16};
    assert(provide_file_cb(&p)==ESP_ERR_NOT_FOUND);
    puts("PASS interrupted next-slot upgrade preserves prior running cache but never serves it as the newer file");

    reset_case(true);strcpy(server_metadata,"{\"version\":\"1.0.0\",\"file\":\"fw_v1.0.0.bin\"}");
    s_recovery_pending=true;mesh_ota_service_poll(0);assert(ready_logs==1 && !s_otacp_pending);
    block_read=true;pthread_t reader,writer;char thread_data[16];
    pthread_create(&reader,NULL,concurrent_provider,thread_data);
    pthread_mutex_lock(&read_gate);while(!read_entered)pthread_cond_wait(&read_condition,&read_gate);pthread_mutex_unlock(&read_gate);
    assert(pthread_mutex_trylock(&s_ota_mutex->lock)!=0);
    pthread_create(&writer,NULL,concurrent_withdrawal,NULL);
    pthread_mutex_lock(&read_gate);while(!start_entered)pthread_cond_wait(&read_condition,&read_gate);
    assert(!start_finished && !s_root_downloading);release_read=true;pthread_cond_broadcast(&read_condition);pthread_mutex_unlock(&read_gate);
    pthread_join(reader,NULL);pthread_join(writer,NULL);block_read=false;
    assert(!memcmp(thread_data,fixture,16) && s_root_downloading && !s_ready_file.valid && !body_downloads);
    puts("PASS concurrent supply read finishes before next-upgrade withdrawal can admit a writer");
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='mesh-binary-test-') as directory:
        tmp = Path(directory)
        (tmp / 'mock_sdk.h').write_text(MOCK)
        for name in ['esp_err.h', 'esp_partition.h', 'esp_app_format.h', 'esp_http_client.h',
                     'esp_log.h', 'esp_ota_ops.h', 'esp_system.h', 'esp_event.h', 'esp_mesh_lite.h',
                     'ConfigData.h', 'task_config.h', 'mqtt_app.h', 'freertos/FreeRTOS.h', 'freertos/task.h',
                     'freertos/semphr.h', 'freertos/event_groups.h', 'mbedtls/sha256.h',
                     'nvs.h', 'esp_rom_crc.h', 'esp_image_format.h']:
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "mock_sdk.h"\n')
        source = tmp / 'test.c'
        source.write_text('#include "mock_sdk.h"\n#define TAG SIMPLE_OTA_TAG\n'
                          f'#include "{ROOT / "main/APP/OTA/ota.c"}"\n#undef TAG\n'
                          f'#include "{ROOT / "main/APP/OTA/ota_cache.c"}"\n'
                          '#define TAG MESH_OTA_TAG\n'
                          f'#include "{ROOT / "main/APP/OTA/mesh_ota.c"}"\n#undef TAG\n' + TESTS)
        cjson = IDF / 'components/json/cJSON'
        command = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Wno-unused-function',
                   '-Wno-deprecated-declarations', '-Werror=implicit-function-declaration',
                   f'-I{tmp}', f'-I{ROOT / "main/Task"}', f'-I{cjson}', str(source),
                   str(cjson / 'cJSON.c'), '-lcrypto', '-pthread', '-lm', '-o', str(tmp / 'test')]
        if os.getenv('TEST_SANITIZE') == '1':
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g', '-no-pie']
        subprocess.run(command, check=True)
        subprocess.run([str(tmp / 'test')], check=True, timeout=30)


if __name__ == '__main__':
    main()
