#!/usr/bin/env python3
"""Real aging_gap.c + session SQL + JSON paths with host SQLite and SDK mocks."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_root_uplink import function_source

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', '/home/wbb/espidf/esp-idf-mesh-lite'))
MOCKS = r'''
#ifndef GAP_TEST_MOCKS_H
#define GAP_TEST_MOCKS_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <limits.h>
#include <math.h>
#include "sqlite3.h"
#include "cJSON.h"
typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef unsigned UBaseType_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
#define SQLITE_CACHE_TABLE_NAME "data_cache"
#define AGING_UPLOAD_PN_MAX_LEN 64
#define PUSH_STATE_INFLIGHT 1
#define MQTT_EVENT_REPORT_WARN 0
static uint32_t now_ms;
static TickType_t xTaskGetTickCount(void){return now_ms;}
typedef struct {pthread_mutex_t lock;} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s){assert(!pthread_mutex_init(&s->lock,NULL));return s;}
static int xSemaphoreTake(SemaphoreHandle_t s,uint32_t wait){(void)wait;return pthread_mutex_lock(&s->lock)==0;}
static int xSemaphoreGive(SemaphoreHandle_t s){return pthread_mutex_unlock(&s->lock)==0;}
typedef struct {unsigned count,head;void *items[3];} *QueueHandle_t;
static pthread_mutex_t queue_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond=PTHREAD_COND_INITIALIZER;
static bool block_queue,queue_entered,release_queue;
static int xQueueSend(QueueHandle_t q,const void *item,uint32_t wait){
    assert(wait==50);pthread_mutex_lock(&queue_gate);queue_entered=true;pthread_cond_broadcast(&queue_cond);
    while(block_queue&&!release_queue)pthread_cond_wait(&queue_cond,&queue_gate);
    pthread_mutex_unlock(&queue_gate);
    if(q->count==3)return 0;
    q->items[(q->head+q->count)%3]=*(void *const *)item;q->count++;return 1;
}
static bool fail_alloc,fail_legacy_alloc;
static unsigned legacy_alloc_calls;
static void *app_malloc_prefer_psram(size_t size){assert(size==2049);legacy_alloc_calls++;return fail_alloc||fail_legacy_alloc?NULL:malloc(size);}
static void *app_calloc_prefer_psram(size_t n,size_t size){return fail_alloc?NULL:calloc(n,size);}
static unsigned stack_warn_count,query_watermarks,send_watermarks,complete_watermarks;
static UBaseType_t stack_free_bytes=4096;
static UBaseType_t uxTaskGetStackHighWaterMark(void *task){assert(task==NULL);return stack_free_bytes;}
static void mock_log(const char *tag,const char *fmt,...){
    (void)tag;
    if(strstr(fmt,"补查任务栈水位")){
        va_list args;va_start(args,fmt);const char *phase=va_arg(args,const char*);va_end(args);
        if(!strcmp(phase,"query"))query_watermarks++;
        if(!strcmp(phase,"send"))send_watermarks++;
        if(!strcmp(phase,"complete"))complete_watermarks++;
    }
}
static void mock_warn(const char *tag,const char *fmt,...){(void)tag;if(strstr(fmt,"http_auto 栈余量不足"))stack_warn_count++;}
#define ESP_LOGI mock_log
#define ESP_LOGW mock_warn
#define ESP_LOGE mock_log
static int json_fail_after=-1;
static void *json_malloc(size_t bytes){if(json_fail_after==0)return NULL;if(json_fail_after>0)json_fail_after--;return malloc(bytes);}
static unsigned warn_count,local_count,http_calls,publish_count,tracked;
static bool mqtt_connected,server_accept,fail_publish,fail_db,preempt_http;
static bool backend_received[256];
static int last_request_max;
static char last_request_record[64], last_publish[3000];
static const char *http_override;
static uint32_t published_ticks[256];
static int published_ids[256];
static const char *DEVICE_ID="node";
static int Network_Flag=1;
static int app_mqtt_event_submit(const char *t,const char *p,const char *d,int kind){(void)t;(void)p;(void)d;(void)kind;warn_count++;return 0;}
static void storage_write_record_cyclic(const char *pn,const char *msg){(void)pn;(void)msg;local_count++;}
static void aging_error_log(const char *fmt,...){(void)fmt;local_count++;}
static const char *current_log_pn(void){return "test";}
static void aging_data_upload_result(bool ok,int id){(void)ok;(void)id;}
static bool app_mqtt_is_connected(void){return mqtt_connected;}
static int app_mqtt_publish(char *topic,char *payload,const char *device){
    (void)topic;(void)device;if(!mqtt_connected||fail_publish)return -1;
    assert(publish_count<256);cJSON *root=cJSON_Parse(payload);assert(root);
    cJSON *data=cJSON_GetObjectItem(root,"Data");int id=cJSON_GetObjectItem(data,"IDNUM")->valueint;
    published_ticks[publish_count]=now_ms;published_ids[publish_count++]=id;
    if(server_accept&&id>=0&&id<256)backend_received[id]=true;
    snprintf(last_publish,sizeof(last_publish),"%s",payload);cJSON_Delete(root);return 100+publish_count;
}
static void mqtt_publish_track(int id,const char *pn,const char *record,int seq){(void)id;(void)pn;assert(record[0]);(void)seq;tracked++;}
static int UpdateRecordPushStateByPNAndID(const char *p,const char *r,int seq,int state){(void)p;(void)r;(void)seq;(void)state;return 0;}
static esp_err_t test_http_post_record_query(int32_t seq,int32_t max_id,const char *record,char *out,int size);
static int64_t esp_timer_get_time(void){return now_ms*1000LL;}
#endif
'''
DB_STATE = r'''
static sqlite3 *s_db;
static bool s_record_id_column_ready=true;
static pthread_mutex_t db_mutex=PTHREAD_MUTEX_INITIALIZER;
static bool sqlite_lock(void){return pthread_mutex_lock(&db_mutex)==0;}
static void sqlite_unlock(void){pthread_mutex_unlock(&db_mutex);}
static bool sqlite_ready_locked(void){return s_db!=NULL;}
'''
DB_WRITE = r'''
int InsertStructuredRecord(int seq,const char *pn,int step,int timestamp,bool pushed,const char *value,const char *record){
    (void)pushed;if(fail_db)return -1;
    sqlite3_stmt *stmt=NULL;assert(sqlite3_prepare_v2(s_db,"INSERT OR REPLACE INTO data_cache(seq_no,pn,current_step,timestamp,pushed,value_data,created_at,record_id) VALUES(?,?,?,?,0,?,0,?)",-1,&stmt,NULL)==SQLITE_OK);
    sqlite3_bind_int(stmt,1,seq);sqlite3_bind_text(stmt,2,pn,-1,SQLITE_TRANSIENT);sqlite3_bind_int(stmt,3,step);sqlite3_bind_int(stmt,4,timestamp);
    sqlite3_bind_text(stmt,5,value,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,6,record,-1,SQLITE_TRANSIENT);
    assert(sqlite3_step(stmt)==SQLITE_DONE);sqlite3_finalize(stmt);return 0;
}
'''
TESTS = r'''
static esp_err_t test_http_post_record_query(int32_t seq,int32_t max_id,const char *record,char *out,int size){
    (void)seq;http_calls++;last_request_max=max_id;snprintf(last_request_record,sizeof(last_request_record),"%s",record);
    if(preempt_http){preempt_http=false;assert(aging_gap_session_begin("new","new-run",false));}
    if(http_override){snprintf(out,size,"%s",http_override);return strcmp(http_override,"FAIL")==0?ESP_FAIL:ESP_OK;}
    cJSON *root=cJSON_CreateObject(),*data=cJSON_AddObjectToObject(root,"Data"),*ids=cJSON_AddArrayToObject(data,"Ids");
    for(int i=0;i<=max_id&&i<256;i++)if(!backend_received[i])cJSON_AddItemToArray(ids,cJSON_CreateNumber(i));
    char *json=cJSON_PrintUnformatted(root);assert((int)strlen(json)<size);strcpy(out,json);free(json);cJSON_Delete(root);return ESP_OK;
}
static void reset(void){
    json_fail_after=-1;
    gap_work_clear_ids();free(s_work.response);free(s_work.record);memset(&s_work,0,sizeof(s_work));
    if(s_db)assert(sqlite3_close(s_db)==SQLITE_OK);
    assert(sqlite3_open(":memory:",&s_db)==SQLITE_OK);
    assert(sqlite3_exec(s_db,"CREATE TABLE data_cache(local_id INTEGER PRIMARY KEY,seq_no INTEGER,pn TEXT,current_step INTEGER,timestamp INTEGER,pushed INTEGER,value_data TEXT,created_at INTEGER,record_id TEXT,UNIQUE(pn,record_id,seq_no));",NULL,NULL,NULL)==SQLITE_OK);
    if(!s_sessions)assert(aging_gap_init()==ESP_OK);else memset(s_sessions,0,sizeof(*s_sessions)*GAP_SESSION_SLOTS);
    s_current=s_next_token=0;s_running=false;atomic_store(&s_trigger,0);atomic_store(&s_mqtt_ready,false);
    memset(backend_received,0,sizeof(backend_received));now_ms=0;http_calls=publish_count=warn_count=local_count=tracked=0;
    mqtt_connected=server_accept=true;fail_alloc=fail_publish=fail_db=preempt_http=false;http_override=NULL;
    block_queue=queue_entered=release_queue=false;
    fail_legacy_alloc=false;legacy_alloc_calls=0;
    stack_free_bytes=4096;stack_warn_count=query_watermarks=send_watermarks=complete_watermarks=0;
    s_stack_warned=false;s_stack_last_warn=0;
}
static uint32_t current_token(void){char pn[64],r[29];uint32_t token=aging_gap_sample_snapshot(pn,sizeof(pn),r,sizeof(r));assert(token);return token;}
static int intent_count(void){int n=0;for(unsigned i=0;i<GAP_SESSION_SLOTS;i++)if(s_sessions[i].used&&s_sessions[i].intent)n++;return n;}
static uint32_t session(const char *pn,const char *record,int count){
    assert(aging_gap_session_begin(pn,record,false));uint32_t token=current_token();
    struct {unsigned count,head;void *items[3];} q={0};void *item=(void*)1;
    for(int i=0;i<count;i++){
        q.count=q.head=0;assert(aging_gap_sample_submit(token,(QueueHandle_t)&q,&item));int id=aging_gap_sample_next_id(token);assert(id==i);
        assert(InsertStructuredRecord(id,pn,0,100+i,false,"[]",record)==0);aging_gap_sample_done(token,true);
    }
    aging_gap_session_close();aging_gap_idle();return token;
}
static void online(void){aging_gap_mqtt_ready(true);}
static void run_for(unsigned milliseconds){for(unsigned i=0;i<milliseconds;i+=100){aging_gap_poll();now_ms+=100;}}
static uint32_t thread_token;
static void *submit_thread(void *arg){void *item=(void*)1;assert(aging_gap_sample_submit(thread_token,arg,&item));return NULL;}
static void *close_thread(void *arg){(void)arg;aging_gap_session_close();return NULL;}
int main(void){
    cJSON_Hooks hooks={.malloc_fn=json_malloc,.free_fn=free};cJSON_InitHooks(&hooks);
    reset();uint32_t token=session("pn","run",0);(void)token;assert(!intent_count());online();run_for(120000);assert(!http_calls&&!publish_count);
    puts("PASS idle/reconnect without ended sampled sessions performs no database backlog scan or network query");

    reset();assert(aging_gap_session_begin("pn","run-A",false));token=current_token();
    struct {unsigned count,head;void *items[3];} q={0};void *item=(void*)1;
    for(unsigned i=0;i<3;i++)assert(aging_gap_sample_submit(token,(QueueHandle_t)&q,&item));
    assert(!aging_gap_sample_submit(token,(QueueHandle_t)&q,&item));aging_gap_session_close();aging_gap_idle();online();run_for(10000);assert(!http_calls&&!intent_count());
    for(int i=0;i<3;i++){assert(aging_gap_sample_next_id(token)==i);InsertStructuredRecord(i,"pn",0,100+i,false,"[]","run-A");aging_gap_sample_done(token,true);}
    assert(intent_count()==1);run_for(20000);assert(last_request_max==2&&!intent_count()&&publish_count==3);
    for(unsigned i=1;i<publish_count;i++)assert(published_ticks[i]-published_ticks[i-1]>=1000);
    assert(!aging_gap_sample_submit(token,(QueueHandle_t)&q,&item));
    puts("PASS closing waits for every accepted tail sample; HTTP uses final IDNUM and resend rate is at most one per second");

    reset();assert(aging_gap_session_begin("pn","run",false));thread_token=current_token();q.count=q.head=0;
    block_queue=true;pthread_t producer,closer;assert(!pthread_create(&producer,NULL,submit_thread,&q));
    pthread_mutex_lock(&queue_gate);while(!queue_entered)pthread_cond_wait(&queue_cond,&queue_gate);pthread_mutex_unlock(&queue_gate);
    assert(pthread_mutex_trylock(&s_mutex_storage.lock)!=0);online(); /* SDK上线标志不等待会话锁。 */
    assert(!pthread_create(&closer,NULL,close_thread,NULL));
    pthread_mutex_lock(&queue_gate);release_queue=true;pthread_cond_broadcast(&queue_cond);pthread_mutex_unlock(&queue_gate);
    assert(!pthread_join(producer,NULL)&&!pthread_join(closer,NULL));
    GapSession *closed=gap_find(thread_token);assert(closed&&closed->closed&&closed->accepted==1&&closed->processed==0&&!closed->intent);
    assert(aging_gap_sample_next_id(thread_token)==0);InsertStructuredRecord(0,"pn",0,0,false,"[]","run");aging_gap_sample_done(thread_token,true);aging_gap_idle();assert(intent_count()==1);
    puts("PASS concurrent queue submission/close cannot freeze the cutoff before the accepted tail packet; SDK ready hook never waits");

    reset();session("same","run-A",3);assert(sqlite3_exec(s_db,"UPDATE data_cache SET pushed=seq_no",NULL,NULL,NULL)==SQLITE_OK);
    InsertStructuredRecord(0,"same",0,0,false,"[]","other-run");InsertStructuredRecord(1,"other-pn",0,0,false,"[]","run-A");
    online();run_for(20000);assert(publish_count==3&&!intent_count());
    cJSON *root=cJSON_Parse(last_publish);cJSON *data=cJSON_GetObjectItem(root,"Data");assert(!strcmp(cJSON_GetObjectItem(data,"PN")->valuestring,"same"));assert(!strcmp(cJSON_GetObjectItem(data,"RecordId")->valuestring,"run-A"));cJSON_Delete(root);
    QueryResult *record=calloc(1,sizeof(*record));assert(QueryStructuredRecordBySession("same","run-A",1,record)==0&&record->timestamp==101);
    puts("PASS exact session SQL ignores pushed states and never selects another PN/RecordId with the same IDNUM");

    reset();session("same","A",1);assert(aging_gap_session_begin("same","B",false));uint32_t b=current_token();
    q.count=0;assert(aging_gap_sample_submit(b,(QueueHandle_t)&q,&item));assert(aging_gap_sample_next_id(b)==0);aging_gap_sample_done(b,false);aging_gap_session_close();aging_gap_idle();
    InsertStructuredRecord(7,"same",0,0,false,"[]","B");InsertStructuredRecord(99,"same",0,0,false,"[]","C");
    assert(aging_gap_session_begin("same","B",true));b=current_token();q.count=0;assert(aging_gap_sample_submit(b,(QueueHandle_t)&q,&item));assert(aging_gap_sample_next_id(b)==8);aging_gap_sample_done(b,false);aging_gap_session_close();aging_gap_idle();
    puts("PASS same-PN new runs restart IDs; resumed runs use their own persisted maximum");

    reset();assert(aging_gap_session_begin("pn","old",false));
    AgingUploadPacket packet={.timestamp=123,.value_json="[]"};packet.session_token=aging_gap_sample_snapshot(packet.pn,sizeof(packet.pn),packet.record_id,sizeof(packet.record_id));
    q.count=0;void *packet_ptr=&packet;assert(aging_gap_sample_submit(packet.session_token,(QueueHandle_t)&q,&packet_ptr));
    aging_gap_session_close();assert(aging_gap_session_begin("pn","new",false));
    int id=aging_gap_sample_next_id(packet.session_token);assert(app_DataUpload_Functiong(&packet,id));aging_gap_sample_done(packet.session_token,true);
    assert(tracked==1&&QueryStructuredRecordBySession("pn","old",0,record)==0&&record->timestamp==123);
    root=cJSON_Parse(last_publish);assert(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(root,"Data"),"RecordId")->valuestring,"old"));cJSON_Delete(root);
    puts("PASS real upload/JSON paths persist and publish frozen RecordId after the next run has begun");

    reset();session("pn","run",2);mqtt_connected=false;online();run_for(60000);assert(!http_calls);
    mqtt_connected=true;aging_gap_mqtt_ready(false);run_for(10000);assert(!http_calls);online();run_for(20000);assert(!intent_count());
    puts("PASS offline ending retains intent and reconnect waits for subscribe/online completion");

    reset();session("pn","run",1);online();preempt_http=true;run_for(6000);assert(s_running&&intent_count()==1&&!publish_count);
    run_for(60000);assert(http_calls==1);aging_gap_session_close();aging_gap_idle();run_for(15000);assert(!intent_count());
    puts("PASS new aging preempts an in-progress HTTP result without sending old data or blocking new sampling");

    reset();session("pn","run",1);online();http_override="FAIL";run_for(120000);assert(http_calls==3&&intent_count()==1&&warn_count==1);
    run_for(120000);assert(http_calls==3);aging_gap_mqtt_ready(false);online();http_override="{\"Data\":{\"Ids\":[]}}";run_for(10000);assert(!intent_count());
    puts("PASS three failed rounds stop network retries until a fresh online edge; valid empty Ids alone confirms completion");

    reset();session("pn","run",2);online();server_accept=false;run_for(120000);assert(intent_count()==1&&publish_count==6&&http_calls==4);
    puts("PASS successful enqueue without server ingestion is not completion; three send/verify rounds stay bounded");

    reset();session("pn","run",3);assert(sqlite3_exec(s_db,"DELETE FROM data_cache WHERE seq_no=1",NULL,NULL,NULL)==SQLITE_OK);
    InsertStructuredRecord(1,"different",0,0,false,"[]","run");online();run_for(120000);assert(publish_count==2&&intent_count()==1&&warn_count>=1);
    for(unsigned i=0;i<publish_count;i++)assert(published_ids[i]!=1);
    puts("PASS unrecoverable IDs are reported while other IDs continue; no cross-session fallback");

    reset();for(unsigned i=0;i<17;i++){char r[29];snprintf(r,sizeof(r),"run-%u",i);session("pn",r,1);}assert(intent_count()==16&&warn_count==1);
    assert(aging_gap_session_begin("pn","next",false));aging_gap_session_close();aging_gap_idle();
    puts("PASS 16-intent capacity never overwrites old intents or prevents new aging");

    reset();session("pn","same",1);session("pn","same",2);assert(intent_count()==1);online();run_for(20000);assert(last_request_max==1&&!intent_count());
    puts("PASS repeated close is idempotent and same-session intents merge to the final maximum");

    int *ids=NULL,count=-1;assert(gap_parse_ids("{\"Data\":{\"Ids\":[2,0,2,1]}}",2,&ids,&count)&&count==3&&ids[0]==0&&ids[2]==2);free(ids);
    const char *invalid[]={"{}","{\"Data\":{\"Ids\":null}}","{\"Data\":{\"Ids\":[-1]}}","{\"Data\":{\"Ids\":[0.5]}}","{\"Data\":{\"Ids\":[\"0\"]}}","{\"Data\":{\"Ids\":[3]}}","{\"Data\":{\"Ids\":[]}}trailing"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)assert(!gap_parse_ids(invalid[i],2,&ids,&count));
    fail_alloc=true;assert(!gap_parse_ids("{\"Data\":{\"Ids\":[0]}}",2,&ids,&count));fail_alloc=false;
    puts("PASS strict IDs parsing distinguishes empty, malformed, fractional, negative, out-of-range, duplicates and allocation failure");
    reset();s_record_id_column_ready=false;
    assert(QueryStructuredRecordBySession("pn","run",0,record)==-2);
    assert(!aging_gap_session_begin("pn","run",true)&&!s_running&&s_current==0);
    s_record_id_column_ready=true;
    puts("PASS database failure is distinct from missing records and cannot reset resumed IDs to zero");
    reset();session("pn","run",1);online();fail_alloc=true;run_for(120000);
    assert(!http_calls&&intent_count()==1&&warn_count==3);fail_alloc=false;
    aging_gap_mqtt_ready(false);online();run_for(15000);assert(!intent_count());
    puts("PASS work-buffer allocation failure has finite retries and recovers on a fresh trigger");
    reset();session("pn","run",1);free(s_sessions);s_sessions=NULL;assert(aging_gap_init()==ESP_OK);s_current=0;s_running=false;online();run_for(60000);assert(!http_calls);
    puts("PASS simulated reboot clears RAM intents while retaining the SQLite contract");

    reset();const char *legacy=",,SOC=7400,,5266,Voltage=-1234,0,ignored=999";
    assert(InsertStructuredRecord(0,"pn",0,1,false,legacy,"legacy")==0);
    assert(QueryStructuredRecordBySession("pn","legacy",0,record)==0);
    assert(!strcmp(record->value_data,legacy));
    assert(fabsf(record->Equipmentvoltage-74.0f)<0.001f && fabsf(record->EquipmentCurrent-52.66f)<0.001f);
    assert(fabsf(record->Testvoltage+12.34f)<0.001f && record->TestCurrent==0);
    cJSON *legacy_root=cJSON_Parse(record->json_data);assert(legacy_root);
    cJSON *legacy_values=cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(legacy_root,"Data"),"Value"),0),"Data");
    assert(cJSON_GetArraySize(legacy_values)==5);
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(legacy_values,1),"name")->valuestring,"Value1"));
    cJSON_Delete(legacy_root);
    char boundary[DB_VALUE_DATA_MAX_LEN+1];
    const char *prefix="[{\"Name\":\"D\",\"Data\":[{\"name\":\"V\",\"value\":\"",*suffix="\"}]}]";
    size_t prefix_len=strlen(prefix),suffix_len=strlen(suffix);
    memcpy(boundary,prefix,prefix_len);memset(boundary+prefix_len,'1',DB_VALUE_DATA_MAX_LEN-prefix_len-suffix_len);
    memcpy(boundary+DB_VALUE_DATA_MAX_LEN-suffix_len,suffix,suffix_len+1);
    assert(strlen(boundary)==DB_VALUE_DATA_MAX_LEN);
    assert(InsertStructuredRecord(1,"pn",0,1,false,boundary,"boundary")==0);
    unsigned alloc_before=legacy_alloc_calls;
    assert(QueryStructuredRecordBySession("pn","boundary",1,record)==0 && !strcmp(record->value_data,boundary));
    assert(legacy_alloc_calls==alloc_before && record->Equipmentvoltage==0);
    strcpy(boundary,"Voltage=");memset(boundary+8,' ',DB_VALUE_DATA_MAX_LEN-12);strcpy(boundary+DB_VALUE_DATA_MAX_LEN-4,"1250");
    assert(InsertStructuredRecord(2,"pn",0,1,false,boundary,"boundary")==0);
    assert(QueryStructuredRecordBySession("pn","boundary",2,record)==0 && !strcmp(record->value_data,boundary));
    assert(record->Equipmentvoltage==12.5f && legacy_alloc_calls==alloc_before+1);
    puts("PASS actual SqLite.c fill path: read-only first four legacy fields and full 2048-byte structured/legacy records");

    fail_legacy_alloc=true;assert(QueryStructuredRecordBySession("pn","legacy",0,record)==-2 && !record->json_data[0]);fail_legacy_alloc=false;
    for(int format=0;format<2;format++) {
        const char *r=format?"legacy":"boundary";int seq=format?0:1;bool succeeded=false;
        for(int fail=0;fail<512;fail++) {
            json_fail_after=fail;int rc=QueryStructuredRecordBySession("pn",r,seq,record);json_fail_after=-1;
            if(rc==0){assert(fail>10);succeeded=true;break;}
            assert(rc==-2 && !record->json_data[0]);
        }
        assert(succeeded);
    }
    assert(InsertStructuredRecord(3,"pn",0,1,false,"[broken","bad")==0);
    assert(QueryStructuredRecordBySession("pn","bad",3,record)==-2 && !record->json_data[0]);
    for(unsigned i=0;i<DB_VALUE_DATA_MAX_LEN;i++)boundary[i]=(i%2)?',':'1';
    boundary[DB_VALUE_DATA_MAX_LEN]=0;
    assert(InsertStructuredRecord(4,"pn",0,1,false,boundary,"bad")==0);
    assert(QueryStructuredRecordBySession("pn","bad",4,record)==-2 && !record->json_data[0]);
    puts("PASS scratch PSRAM/each cJSON allocation, malformed new JSON and oversized rebuilt JSON fail as -2, never missing or partial payload");

    reset();session("pn","legacy",1);assert(InsertStructuredRecord(0,"pn",0,1,false,"SOC=7400","legacy")==0);
    online();fail_legacy_alloc=true;run_for(120000);
    assert(intent_count()==1 && !publish_count && http_calls==3 && warn_count==1 && s_work.missing==0);
    fail_legacy_alloc=false;aging_gap_mqtt_ready(false);online();run_for(15000);
    assert(!intent_count() && publish_count==1);
    reset();session("pn","run",1);online();run_for(5100);assert(http_calls==1 && !publish_count);
    json_fail_after=0;aging_gap_poll();json_fail_after=-1;
    assert(intent_count()==1 && !publish_count && s_work.missing==0);
    run_for(25000);assert(!intent_count() && publish_count==1);
    puts("PASS reconstruction failures retain SQLite row and intent with bounded retry; fresh trigger recovers");

    reset();session("EL102611110001759","471634628172587008",105);
    for(unsigned i=0;i<90;i++)backend_received[i]=true;
    online();run_for(30000);
    assert(last_request_max==104 && publish_count==15 && !intent_count());
    for(unsigned i=0;i<15;i++){assert(published_ids[i]==(int)i+90);if(i)assert(published_ticks[i]-published_ticks[i-1]>=1000);}
    assert(query_watermarks>=2 && send_watermarks==15 && complete_watermarks==1 && !stack_warn_count);
    puts("PASS reported last_id=104/missing=15 scenario completes at one per second with query/send/complete stack watermarks");

    reset();stack_free_bytes=1024;gap_log_stack("query");assert(!stack_warn_count);
    now_ms=100;stack_free_bytes=1023;gap_log_stack("send");assert(stack_warn_count==1 && !warn_count && !local_count);
    now_ms=60099;gap_log_stack("send");assert(stack_warn_count==1);
    now_ms=60100;gap_log_stack("complete");assert(stack_warn_count==2);
    puts("PASS stack warning threshold is below 1024 bytes, limited to once per 60 seconds without MQTT/storage recursion");
    free(record);gap_work_clear_ids();free(s_work.response);free(s_work.record);free(s_sessions);sqlite3_close(s_db);return 0;
}
'''

def main():
    db_source = (ROOT / 'main/DataStorage/SqLite/SqLite.c').read_text()
    sql = '\n'.join(function_source(db_source, signature) for signature in [
        'static void safe_copy(', 'static float scaled_int_to_float(',
        'static void fill_legacy_float_fields(', 'static void fill_query_result_from_stmt(',
        'static const char *structured_select_columns(', 'int QueryStructuredRecordBySession(',
    ])
    app = (ROOT / 'main/Task/appTask.c').read_text()
    packet_start = app.index('typedef struct\n{\n    char pn[AGING_UPLOAD_PN_MAX_LEN];')
    packet = app[packet_start:app.index('} AgingUploadPacket;', packet_start) + len('} AgingUploadPacket;')]
    upload = function_source(app, 'static bool app_DataUpload_Functiong(')
    sensor = function_source((ROOT / 'main/Tool/Json_data.c').read_text(), 'char *create_sensor_json(')
    with tempfile.TemporaryDirectory(prefix='aging-gap-test-') as directory:
        tmp = Path(directory)
        (tmp / 'mocks.h').write_text(MOCKS)
        for header in ['esp_err.h', 'esp_log.h', 'app_mem.h', 'GetProto.h', 'mqtt_app.h', 'ConfigData.h',
                       'log.h', 'esp_timer.h', 'appTask.h', 'freertos/FreeRTOS.h', 'freertos/queue.h', 'freertos/semphr.h', 'freertos/task.h']:
            target = tmp / header
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('#include "mocks.h"\n')
        (tmp / 'test.c').write_text('#include "mocks.h"\n#include "SqLite.h"\n' + DB_STATE
            # 两个真实源文件均有同名 static helper，隔离 sqllib1 名称，让会话查询走实际 SqLite.c 填充链。
            + '\n#define safe_copy sqllib_safe_copy\n#define fill_query_result_from_stmt sqllib_fill_query_result_from_stmt\n'
            + f'#include "{ROOT}/main/DataStorage/SqLite/sqllib1.c"\n'
            + '\n#undef safe_copy\n#undef fill_query_result_from_stmt\n' + sql + DB_WRITE
            + f'\n#include "{ROOT}/main/Task/aging_gap.c"\n' + packet + sensor + upload + TESTS)
        cjson = IDF / 'components/json/cJSON'
        args = [os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-format', '-I', str(tmp),
                '-I', str(ROOT / 'main/Task'), '-I', str(ROOT / 'main/DataStorage/SqLite'),
                '-I', str(IDF / 'components/esp32-idf-sqlite3/include'), '-I', str(cjson),
                str(tmp / 'test.c'), str(cjson / 'cJSON.c'), '-l:libsqlite3.so.0', '-lm', '-o', str(tmp / 'test')]
        if os.environ.get('TEST_SANITIZE') == '1':
            args[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g', '-no-pie']
        subprocess.run(args, check=True)
        subprocess.run([str(tmp / 'test')], check=True, timeout=20)

if __name__ == '__main__':
    main()
