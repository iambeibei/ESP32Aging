#!/usr/bin/env python3
"""Exercise real PSRAM helpers, cJSON hooks and IDF TLS allocator with constrained heaps.

SDK/RTOS boundaries are mocked; no firmware/network hardware is simulated.
Run: python3 tests/test_psram_memory.py; TEST_SANITIZE=1 enables ASan/UBSan.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', '/home/wbb/espidf/esp-idf-mesh-lite'))

MOCKS = r'''
#ifndef MEMORY_TEST_MOCKS_H
#define MEMORY_TEST_MOCKS_H
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#define CONFIG_SPIRAM 1
#define CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC 1
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_INTERNAL 4U
#define MALLOC_CAP_DMA 8U
#define ESP_OK 0
typedef int esp_err_t;
typedef struct {size_t total_free_bytes, minimum_free_bytes, largest_free_block;} multi_heap_info_t;
typedef void (*FailureCallback)(size_t,uint32_t,const char *);
static FailureCallback failed_callback;
static unsigned callback_registrations, allocations, live, frees, log_count;
static bool fail_external, fail_internal, fail_queue_create;
static int fail_after=-1;
static size_t internal_used, internal_limit=7680;
typedef struct {void *p;size_t n;uint32_t caps;} Allocation;
static Allocation blocks[2048];
static Allocation *lookup(const void *p) {
    for(unsigned i=0;i<2048;i++) if(blocks[i].p==p && p!=NULL) return &blocks[i];
    return NULL;
}
static bool esp_ptr_internal(const void *p) {
    Allocation *a=lookup(p);return p!=NULL && (a==NULL || (a->caps&MALLOC_CAP_SPIRAM)==0);
}
static bool esp_ptr_external_ram(const void *p) {Allocation *a=lookup(p);return a && (a->caps&MALLOC_CAP_SPIRAM);}
static void test_free(void *p) {
    if(!p) return;
    Allocation *a=lookup(p);assert(a);
    if(!(a->caps&MALLOC_CAP_SPIRAM)) internal_used-=a->n;
    a->p=NULL;live--;frees++;free(p);
}
static void *heap_caps_malloc(size_t n,uint32_t caps) {
    allocations++;
    bool ext=(caps&MALLOC_CAP_SPIRAM)!=0;
    if((ext ? fail_external : fail_internal) || fail_after==0 ||
       (!ext && n>internal_limit-internal_used)) {
        if(failed_callback) failed_callback(n,caps,"heap_caps_malloc");
        return NULL;
    }
    if(fail_after>0) fail_after--;
    void *p=malloc(n);assert(p);
    for(unsigned i=0;i<2048;i++) if(!blocks[i].p) {
        blocks[i]=(Allocation){p,n,caps};live++;if(!ext) internal_used+=n;return p;
    }
    abort();
}
static void *heap_caps_calloc(size_t count,size_t size,uint32_t caps) {
    if(size && count>SIZE_MAX/size) return NULL;
    void *p=heap_caps_malloc(count*size,caps);if(p) memset(p,0,count*size);return p;
}
static void *heap_caps_realloc(void *p,size_t n,uint32_t caps) {
    if(!p) return heap_caps_malloc(n,caps);
    Allocation *old=lookup(p);assert(old);
    size_t previous=old->n;
    void *next=heap_caps_malloc(n,caps);
    if(next) {memcpy(next,p,previous<n?previous:n);test_free(p);}return next;
}
static void heap_caps_free(void *p) {test_free(p);}
static esp_err_t heap_caps_register_failed_alloc_callback(FailureCallback cb) {
    failed_callback=cb;callback_registrations++;return ESP_OK;
}
static void heap_caps_get_info(multi_heap_info_t *info,uint32_t caps) {
    *info=(caps&MALLOC_CAP_SPIRAM)?(multi_heap_info_t){8000000,7900000,7800000}:
        (multi_heap_info_t){internal_limit-internal_used,3547,internal_limit-internal_used};
}
static size_t esp_psram_get_size(void) {return 8*1024*1024;}
static char last_log[1024];
static void mock_log(const char *tag,const char *fmt,...) {
    (void)tag;log_count++;va_list ap;va_start(ap,fmt);vsnprintf(last_log,sizeof(last_log),fmt,ap);va_end(ap);
}
#define ESP_LOGI mock_log
#define ESP_LOGW mock_log
#define ESP_LOGE mock_log
typedef size_t UBaseType_t;
typedef unsigned TickType_t;
typedef struct {
    uint8_t *data;size_t capacity,item_size,count,head;pthread_mutex_t mutex;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
static QueueHandle_t xQueueCreateStatic(UBaseType_t n,UBaseType_t size,uint8_t *data,StaticQueue_t *q) {
    assert(esp_ptr_internal(q) && esp_ptr_external_ram(data));
    if(fail_queue_create) return NULL;
    *q=(StaticQueue_t){.data=data,.capacity=n,.item_size=size};
    assert(pthread_mutex_init(&q->mutex,NULL)==0);return q;
}
static void vQueueDelete(QueueHandle_t q) {assert(pthread_mutex_destroy(&q->mutex)==0);}
static int xQueueSend(QueueHandle_t q,const void *p,TickType_t wait) {
    (void)wait;pthread_mutex_lock(&q->mutex);bool ok=q->count<q->capacity;
    if(ok) {memcpy(q->data+((q->head+q->count)%q->capacity)*q->item_size,p,q->item_size);q->count++;}
    pthread_mutex_unlock(&q->mutex);return ok;
}
static int xQueueReceive(QueueHandle_t q,void *p,TickType_t wait) {
    (void)wait;pthread_mutex_lock(&q->mutex);bool ok=q->count>0;
    if(ok) {memcpy(p,q->data+q->head*q->item_size,q->item_size);q->count--;q->head=(q->head+1)%q->capacity;}
    pthread_mutex_unlock(&q->mutex);return ok;
}
#endif
'''

TESTS = r'''
typedef struct {uint16_t len;char topic[64];char payload[1024];} Packet;
static AppPsramQueue control;
static void *producer(void *arg) {
    QueueHandle_t q=arg;Packet p={.len=1024};strcpy(p.topic,"device/test/data");
    for(unsigned i=0;i<2000;i++) {
        memset(p.payload,(int)(i%251),sizeof(p.payload));memcpy(p.payload,&i,sizeof(i));
        while(!xQueueSend(q,&p,0)) sched_yield();
    }return NULL;
}
static void *consumer(void *arg) {
    QueueHandle_t q=arg;Packet p;
    for(unsigned i=0;i<2000;i++) {
        while(!xQueueReceive(q,&p,0)) sched_yield();
        unsigned actual;memcpy(&actual,p.payload,sizeof(actual));
        assert(actual==i && p.len==1024 && strcmp(p.topic,"device/test/data")==0);
        for(unsigned j=sizeof(actual);j<sizeof(p.payload);j++) assert((uint8_t)p.payload[j]==i%251);
    }return NULL;
}
int main(void) {
    app_memory_init();app_memory_init();assert(callback_registrations==1);
    assert(strstr(last_log,"MEM_STAGE=startup") && strstr(last_log,"internal_free=7680"));
    assert(strstr(last_log,"dma_free=7680") && strstr(last_log,"dma_largest=7680"));
    assert(strstr(last_log,"psram_total=8388608") && strstr(last_log,"largest=7800000"));
    cJSON *json=cJSON_Parse("{\"名称\":\"电池 A\",\"values\":[1,2,3]}");assert(json);
    assert(esp_ptr_external_ram(json) && esp_ptr_external_ram(json->child->string));
    char *text=cJSON_PrintUnformatted(json);assert(text && esp_ptr_external_ram(text));
    assert(strstr(text,"电池 A"));cJSON_free(text);cJSON_Delete(json);assert(live==0 && internal_used==0);
    puts("PASS real cJSON parse/print/delete use PSRAM from the first allocation; hooks install once");

    for(int i=0;i<20;i++) {
        fail_after=i;json=cJSON_Parse("{\"a\":[1,2,3],\"b\":\"test\"}");
        if(json) {text=cJSON_Print(json);if(text) cJSON_free(text);cJSON_Delete(json);}
        assert(live==0);
    }fail_after=-1;
    unsigned before=allocations, logs=log_count;
    failed_callback(1234,MALLOC_CAP_INTERNAL,"injected");
    assert(allocations==before && log_count==logs && atomic_load(&s_last_failed_bytes)==1234);
    puts("PASS cJSON failures free partial trees; failed-allocation hook never allocates or logs");

    char *p=app_calloc_prefer_psram(32,4);assert(p && esp_ptr_external_ram(p));
    for(int i=0;i<128;i++) assert(p[i]==0);strcpy(p,"keep-data");
    fail_external=fail_internal=true;
    assert(app_realloc_prefer_psram(p,256)==NULL && strcmp(p,"keep-data")==0);
    fail_internal=false;
    unsigned fallback_before=atomic_load(&s_internal_fallbacks);
    p=app_realloc_prefer_psram(p,256);assert(p && esp_ptr_internal(p) && strcmp(p,"keep-data")==0);
    assert(atomic_load(&s_internal_fallbacks)==fallback_before+1);
    fail_external=false;
    p=app_realloc_prefer_psram(p,512);assert(p && esp_ptr_external_ram(p) && strcmp(p,"keep-data")==0);
    assert(app_realloc_prefer_psram(p,0)==NULL && live==0);
    assert(app_malloc_prefer_psram(0)==NULL && app_calloc_prefer_psram(SIZE_MAX,2)==NULL);
    puts("PASS realloc preserves ownership/data on failure and moves fallback data back to PSRAM");

    assert(heap_caps_calloc(1,16384,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)==NULL);
    void *tls=esp_mbedtls_mem_calloc(1,16384);assert(tls && esp_ptr_external_ram(tls) && internal_used==0);
    esp_mbedtls_mem_free(tls);assert(live==0);
    puts("PASS real IDF TLS allocator obtains 16 KiB in PSRAM while internal heap only has 7.5 KiB");

    assert(app_psram_queue_create(NULL,32,sizeof(Packet),"invalid")==NULL);
    assert(app_psram_queue_create(&control,0,sizeof(Packet),"invalid")==NULL);
    assert(app_psram_queue_create(&control,SIZE_MAX,2,"overflow")==NULL);
    AppPsramQueue *external=heap_caps_calloc(1,sizeof(control),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    assert(external && app_psram_queue_create(external,32,sizeof(Packet),"invalid-control")==NULL);
    test_free(external);
    fail_external=true;before=allocations;
    assert(app_psram_queue_create(&control,32,sizeof(Packet),"no-memory")==NULL);
    assert(allocations==before+1 && live==0 && !control.handle && !control.storage);
    fail_external=false;fail_queue_create=true;
    assert(app_psram_queue_create(&control,32,sizeof(Packet),"rtos-failure")==NULL);
    assert(live==0 && !control.handle && !control.storage);fail_queue_create=false;
    puts("PASS queue failures never consume internal fallback; rollback frees storage and permits retry");

    QueueHandle_t q=app_psram_queue_create(&control,32,sizeof(Packet),"mqtt_rx");assert(q);
    assert(esp_ptr_internal(q) && esp_ptr_external_ram(control.storage));
    assert(lookup(control.storage)->n==34880 && live==1 && internal_used==0);
    before=allocations;assert(app_psram_queue_create(&control,32,sizeof(Packet),"mqtt_rx")==q);
    assert(app_psram_queue_create(&control,31,sizeof(Packet),"mismatch")==NULL && allocations==before);
    Packet packet={.len=1024};memset(packet.payload,'x',1024);
    for(unsigned i=0;i<32;i++) assert(xQueueSend(q,&packet,0));
    assert(!xQueueSend(q,&packet,0));
    for(unsigned i=0;i<32;i++) {Packet out;assert(xQueueReceive(q,&out,0));assert(memcmp(&out,&packet,sizeof(out))==0);}
    assert(!xQueueReceive(q,&packet,0));
    pthread_t a,b;assert(!pthread_create(&a,NULL,producer,q));assert(!pthread_create(&b,NULL,consumer,q));
    pthread_join(a,NULL);pthread_join(b,NULL);
    app_psram_queue_delete(&control);app_psram_queue_delete(&control);assert(live==0 && !control.storage && !control.handle);
    assert(app_psram_queue_create(&control,3,1026,"uart_data"));app_psram_queue_delete(&control);assert(live==0);
    puts("PASS 32-message FIFO capacity, concurrent data integrity, idempotent init, delete and re-init");
    app_memory_log("final");assert(strstr(last_log,"MEM_STAGE=final") && strstr(last_log,"internal_fallbacks=1"));
    assert(internal_used==0 && allocations>frees);
    puts("All PSRAM memory regression scenarios passed");return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='psram-memory-test-') as directory:
        temp = Path(directory)
        (temp / 'mocks.h').write_text(MOCKS)
        for header in ['sdkconfig.h', 'esp_heap_caps.h', 'esp_log.h', 'esp_memory_utils.h',
                       'esp_psram.h', 'esp_attr.h', 'esp_mem.h', 'freertos/FreeRTOS.h', 'freertos/queue.h']:
            path = temp / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "mocks.h"\n')
        unit = temp / 'test.c'
        unit.write_text('#include "mocks.h"\n#define free test_free\n'
                        f'#include "{ROOT / "main/Tool/app_mem.c"}"\n'
                        '#undef TAG\n#define TAG queue_tag\n'
                        f'#include "{ROOT / "main/Tool/app_queue.c"}"\n'
                        '#undef TAG\n'
                        f'#include "{IDF / "components/mbedtls/port/esp_mem.c"}"\n'
                        '#undef free\n' + TESTS)
        cjson = IDF / 'components/json/cJSON'
        binary = temp / 'test'
        args = [os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-pthread',
                '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                '-I', str(temp), '-I', str(cjson), str(unit), str(cjson / 'cJSON.c'),
                '-lm', '-o', str(binary)]
        if os.environ.get('TEST_SANITIZE') == '1':
            args[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(args, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == '__main__':
    main()
