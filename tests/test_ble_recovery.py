#!/usr/bin/env python3
"""真实老化状态机/BLE恢复/采样回归；模拟时钟和空口，TEST_SANITIZE=1启用ASan/UBSan。"""
import os
from pathlib import Path
import subprocess
import tempfile
from test_root_uplink import function_source

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', '/home/wbb/espidf/esp-idf-mesh-lite'))
MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <time.h>
#include "aging_config.h"
#define ESP_OK 0
#define ESP_FAIL -1
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define portTICK_PERIOD_MS 1
#define portMAX_DELAY UINT32_MAX
#define LG_STACK_BLE_DATA 6144
#define LG_PRIO_BLE_DATA 8
#define LG_STACK_AGING_GET 6144
#define LG_PRIO_AGING_GET 7
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define AGING_UPLOAD_VALUE_COUNT 32
#define DB_VALUE_DATA_MAX_LEN 2048
#define BLE_AGING_RECOVERY_TIMEOUT_MS 600000u
#define BLE_AGING_RETRY_DELAY_MS 3000u
#define BLE_RECOVERY_TRANSACTION_BUDGET_MS 2500u
#define BLE_AGING_SCAN_SECONDS 20
#define BLE_TARGET_POLL_INTERVAL_MS 500
#define BLE_SCAN_WAIT_TIMEOUT_MS 30000
#define BLE_CONNECT_WAIT_TIMEOUT_MS 15000
#define BLE_GATT_SETUP_TIMEOUT_MS 10000
#define AGING_CANCEL_WAIT_MS 10000
#define AGING_WAIT_SLICE_MS 100u
#define AGING_HANDOVER_POLL_MS 20u
#define BLE_IO_LOCK_WAIT_MS 1200u
#define BLE_INIT_GRACE_MS 20000u
#define AGING_DEVICE_CHECK_MAX_ATTEMPTS 25
static const char *TAG="test", *AgingNumber="rack";
typedef unsigned TickType_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef int esp_err_t;
typedef enum { AgingIdle, AgingStartCheck, AgingDeviceCheck, AgingAction, AgingDataCheck, AgingComplete } AgingProcessState;
typedef enum { IdleState, StandingState, RechargeState, DischargeState } AgingDataAcquisitionMode;
typedef struct { char name[32], CommunicationMode[16]; int ProtoType;
 struct {unsigned item_count;} canprotocol,modbusprotocol;
 AgingStep *steps;size_t step_count;
} Externaldevice;
typedef struct {bool use_mac;uint8_t bda[6],addr_type;} BleTarget;
typedef struct {char pn[64],record_id[64];uint32_t session_token;int current_step,timestamp;uint8_t value_count;char *value_json;} AgingUploadPacket;
static Externaldevice dut,external,*Aging_device=&dut,*read_devices;
static int read_device_count;
static AgingConfig agingcfg;
static AgingProcessState agingState;
static AgingDataAcquisitionMode agingDataAMode;
static struct {unsigned aging_valid,current_step,CycleIndex;uint64_t Deadline;} agingResumeState;
static atomic_bool s_aging_cancel,s_aging_ctrl_exited=true,s_aging_sample_exited=true;
static atomic_bool s_ble_recovering,s_ble_auth_owner=true,s_sample_reading,s_ble_abnormal_disconnect;
static atomic_uint s_ble_ready_session,s_ble_disconnect_reason,s_ble_disconnect_session,s_ble_disconnect_tick;
static AgingStep *s_ble_resume_step;
static bool s_ble_replay_pre,s_ble_recovery_active;
static TickType_t s_ble_recovery_start_tick;
static unsigned s_ble_recovery_attempt;
static volatile bool ScanComplete,BleConnectComplete,BleConnectSuccess;
static unsigned CurrentAgingStep, CycleIndex=1, AgingCMode,Cannextstep;
static unsigned s_consecutive_data_read_failures,s_last_data_read_error_report_tick;
static unsigned s_consecutive_data_upload_failures,s_last_data_upload_error_report_tick;
static bool AgingLogActive=true;
static unsigned BTDisConnect;
static char PN_Code[64]="PN-1",RecordId[28]="record-1";
static BleTarget s_ble_target_current;
static TaskHandle_t Aging_test_task_handle=(void*)1,AgingData_task_handle,BleData_task_handle=(void*)3;
static int s_aging_cmd_state,s_pending_start_seq;
#define AGING_CMD_IDLE 0
static void *Upload_data_queue;
static bool io_held;
static void *s_ble_io_mutex=&io_held;
static int xSemaphoreTake(void *h,unsigned ticks) {(void)h;(void)ticks;if(io_held)return 0;io_held=true;return 1;}
static int xSemaphoreGive(void *h) {(void)h;assert(io_held);io_held=false;return 1;}
static uint32_t now_ticks,sid=2;
static bool connected,encrypted,plain_device,scan_failed,setup_failed,connect_failed,read_failed,pre_failed,post_failed;
static unsigned connect_failures,scan_calls,connect_calls,name_connects,mac_connects,gets,sets,pre_sets,post_sets,external_sets;
static unsigned close_count,idle_count,error_count,abort_count,taskcp_count,led_off_count,samples;
static bool identity_check=true,drop_on_condition,drop_on_post,drop_in_group,group_dropped,cancel_on_delay,cancel_on_probe;
static bool fail_sample_second,drop_sample_second,compete_probe;
static uint64_t saved_deadline;
static uint32_t first_post_failure_tick;
static void (*delay_hook)(void);
static jmp_buf task_exit;
static unsigned xTaskGetTickCount(void){return now_ticks;}
static time_t test_time(time_t *out){time_t t=1000+now_ticks/1000;if(out)*out=t;return t;}
#define time test_time
static void vTaskDelay(unsigned ticks) {
 now_ticks+=ticks;
 if(cancel_on_delay) {atomic_store(&s_aging_cancel,true);cancel_on_delay=false;}
 if(delay_hook)delay_hook();
 if(connected && atomic_load(&s_ble_auth_owner) && !plain_device)encrypted=true;
 if(drop_in_group && taskcp_count && !group_dropped){connected=false;sid+=2;group_dropped=true;}
 if(drop_in_group && group_dropped && !atomic_load(&s_ble_recovering) && connected)Cannextstep=1;
}
static void vTaskDelete(void *h){assert(h==NULL);longjmp(task_exit,1);}
static bool ble_session_is_current(uint32_t s){return connected && s==sid;}
static uint32_t ble_session_id(void){return sid;}
static bool ble_is_connected(void){return connected;}
static bool ble_session_is_encrypted(void){return encrypted;}
static void ble_disconnect(void){connected=false;encrypted=false;sid+=2;BTDisConnect=0;}
static void ble_control_set_budget_ms(unsigned ms){(void)ms;}
static int ble_control_pause(void){return ESP_OK;}
static void ble_restore_coex_balance(void){}
static int ble_start_target_scan(const BleTarget *t,unsigned seconds){assert(t==&s_ble_target_current && seconds==20);scan_calls++;ScanComplete=true;return scan_failed?ESP_FAIL:ESP_OK;}
static bool ble_scanned_has_address(uint8_t *bda,unsigned type){assert(bda==s_ble_target_current.bda && type==1);return true;}
static bool ble_scanned_has_name(const char *name){assert(!strcmp(name,"PN-1"));return true;}
static int ble_stop_scan_wait(void){return ESP_OK;}
static bool connect_mock(void){
 connect_calls++;BleConnectComplete=true;
 if(connect_failed || connect_failures){if(connect_failures)connect_failures--;BleConnectSuccess=false;return true;}
 BleConnectSuccess=true;connected=true;encrypted=false;sid+=2;return true;
}
static bool ble_connect_by_address(uint8_t *bda,unsigned type){assert(bda==s_ble_target_current.bda && type==1);mac_connects++;return connect_mock();}
static bool ble_connect_by_name(const char *name){assert(!strcmp(name,"PN-1"));name_connects++;return connect_mock();}
static bool ble_gatt_setup_done(void){return !setup_failed;}
static bool Data_Set_Impl(Externaldevice *d,const char *p,const char *v,uint8_t addr,uint8_t named){
 (void)v;(void)addr;(void)named;assert(!atomic_load(&s_aging_cancel));sets++;
 if(d==&external){external_sets++;return true;}
 assert(io_held);if(!strcmp(p,"pre")){pre_sets++;if(pre_failed){connected=false;sid+=2;return false;}}
 if(!strcmp(p,"post")){post_sets++;if(post_failed){if(post_sets==1)first_post_failure_tick=now_ticks;return false;}if(drop_on_post){drop_on_post=false;connected=false;sid+=2;return false;}}
 return true;
}
static bool Data_Get_Method(Externaldevice*,const char*,uint8_t,double*,uint8_t);
static bool Data_Get_Impl(Externaldevice *d,const char *p,uint8_t addr,double *v,uint8_t named){
 (void)d;(void)addr;(void)named;gets++;*v=5;
 if(compete_probe){compete_probe=false;double other;assert(!Data_Get_Method(&dut,"sample",1,&other,0));}
 if(cancel_on_probe){atomic_store(&s_aging_cancel,true);return false;}
 if((fail_sample_second || drop_sample_second) && gets==2){
  if(drop_sample_second){connected=false;sid+=2;}return !fail_sample_second;
 }
 if(!strcmp(p,"condition") && drop_on_condition){drop_on_condition=false;connected=false;sid+=2;*v=-1;return false;}
 return !read_failed;
}
static void aging_runtime_log(const char *fmt,...) {(void)fmt;if(identity_check){assert(!strcmp(PN_Code,"PN-1"));assert(!strcmp(RecordId,"record-1"));}}
static void aging_error_log(const char *fmt,...) {(void)fmt;}
static void local_error_log(const char *fmt,...) {(void)fmt;}
static void aging_state_publish(const char *s){if(!strcmp(s,"agingError"))error_count++;if(!strcmp(s,"agingIdle"))idle_count++;}
static void aging_state_publish_StepId(const char *s){(void)s;}
static int publish_aging_stage(const char *s,const char *r){(void)s;(void)r;return 0;}
static int publish_aging_complete(int c,const char *r){(void)c;(void)r;taskcp_count++;return 0;}
static int SelfRecovery_Write_uint16(const char *k,unsigned v){(void)k;(void)v;return ESP_OK;}
static int SelfRecovery_Write_uint64(const char *k,uint64_t v){assert(!strcmp(k,"Deadline"));saved_deadline=v;return ESP_OK;}
static void SelfRecovery_Erase_aging_valid(void){}
static const char *esp_err_to_name(int e){(void)e;return "mock";}
static void aging_gap_session_close(void){close_count++;}
static void aging_gap_idle(void){}
static void app_mqtt_event_cancel_taskcp(void){}
static void AgingDevice_RuntimeFree(void){}
void aging_config_free(AgingConfig *cfg){(void)cfg;}
static void spiffs_file_delete(const char *f){assert(!strcmp(f,"Aging.json"));}
static void status_led_all_off(void){led_off_count++;}
static void clear_ready_pn_runtime(void){}
static void clear_aging_command_runtime(void){}
static bool ExDevice_Check(void){return true;}
static void app_ble_data_handle(void *arg){(void)arg;}
static void app_AgingData_Get_handle(void *arg);
static int create_cpu1_task(TaskFunction_t f,const char *n,unsigned stack,unsigned prio,TaskHandle_t *h){(void)f;(void)n;(void)stack;(void)prio;*h=(void*)2;return pdPASS;}
static const char *aging_report_device_name(void){return "DUT";}
static const char *AgingDataName(const char *id){return id;}
static void aging_data_read_result(bool ok,Externaldevice *d,const char *p){(void)ok;(void)d;(void)p;}
static uint32_t aging_gap_sample_snapshot(char *pn,size_t pn_bytes,char *record,size_t record_bytes){snprintf(pn,pn_bytes,"PN-1");snprintf(record,record_bytes,"record-1");return 123;}
static void *app_calloc_prefer_psram(size_t n,size_t bytes){return calloc(n,bytes);}
static bool aging_gap_sample_submit(uint32_t token,void *q,const void *item){(void)q;assert(token==123);AgingUploadPacket *p=*(AgingUploadPacket*const*)item;assert(p->value_json && !strstr(p->value_json,"-1.00"));samples++;free(p->value_json);free(p);return true;}
'''
TESTS = r'''
static AgingJudgingCondition condition={.Name="condition",.value_num=10,.valid=true};
static AgingSampleData sample_items[2]={{.Value="sample"},{.Value="sample2"}};
static AgingActionItem pre={.ParaID="pre",.ParaName="pre",.ParaValue="1"};
static AgingActionItem post={.ParaID="post",.ParaName="post",.ParaValue="0"};
static AgingStep step,ext_step;
static void reset(void){
 now_ticks=0;sid=2;connected=encrypted=plain_device=scan_failed=setup_failed=connect_failed=read_failed=pre_failed=post_failed=false;
 connect_failures=scan_calls=connect_calls=name_connects=mac_connects=gets=sets=pre_sets=post_sets=external_sets=0;
 close_count=idle_count=error_count=abort_count=taskcp_count=led_off_count=samples=0;
 drop_on_condition=drop_on_post=drop_in_group=group_dropped=cancel_on_delay=cancel_on_probe=false;
 fail_sample_second=drop_sample_second=compete_probe=false;delay_hook=NULL;saved_deadline=0;identity_check=true;io_held=false;
 atomic_store(&s_aging_cancel,false);atomic_store(&s_aging_ctrl_exited,false);atomic_store(&s_aging_sample_exited,true);
 atomic_store(&s_ble_recovering,false);atomic_store(&s_ble_auth_owner,true);atomic_store(&s_sample_reading,false);
 atomic_store(&s_ble_abnormal_disconnect,false);atomic_store(&s_ble_ready_session,0);
 atomic_store(&s_ble_disconnect_tick,0);atomic_store(&s_ble_disconnect_session,0);atomic_store(&s_ble_disconnect_reason,0);
 s_ble_resume_step=NULL;s_ble_replay_pre=s_ble_recovery_active=false;s_ble_recovery_start_tick=0;s_ble_recovery_attempt=0;
 ScanComplete=BleConnectComplete=BleConnectSuccess=false;CurrentAgingStep=0;CycleIndex=1;AgingCMode=Cannextstep=0;
 BTDisConnect=0;agingState=AgingDeviceCheck;agingDataAMode=IdleState;memset(&agingResumeState,0,sizeof(agingResumeState));
 strcpy(PN_Code,"PN-1");strcpy(RecordId,"record-1");memset(&s_ble_target_current,0,sizeof(s_ble_target_current));
 Aging_test_task_handle=(void*)1;AgingData_task_handle=NULL;BleData_task_handle=(void*)3;
 condition.value_num=10;step=(AgingStep){.method="Discharge",.StepId="step-1",.judging_conditions=&condition,.judging_condition_count=1,
 .sample_data=sample_items,.sample_data_count=2,.pre_actions=&pre,.pre_action_count=1,.after_actions=&post,.after_action_count=1};
 ext_step=step;dut=(Externaldevice){.ProtoType=1,.steps=&step,.step_count=1};strcpy(dut.CommunicationMode,"Bluetooth");
 external=dut;strcpy(external.CommunicationMode,"RS485");read_devices=NULL;read_device_count=0;Aging_device=&dut;
 agingcfg=(AgingConfig){.steps=&step,.step_count=1};
}
static void ready(void){connected=true;encrypted=true;atomic_store(&s_ble_auth_owner,false);atomic_store(&s_ble_ready_session,sid);}
static void run_control(void){if(!setjmp(task_exit))Aging_Test_Task(NULL);assert(!io_held);}
static void sample_stop(void){atomic_store(&s_aging_cancel,true);}
static bool standing_dropped;
static void standing_drop(void){
 if(saved_deadline && !standing_dropped){standing_dropped=true;connected=false;sid+=2;connect_failures=2;}
}
static void acknowledge_cancel(void){
 assert(atomic_load(&s_aging_cancel));atomic_store(&s_aging_ctrl_exited,true);atomic_store(&s_aging_sample_exited,true);
}
static void main_tests(void){
 reset();connect_failures=2;compete_probe=true;
 assert(aging_ble_ensure_ready() && connect_calls==3 && name_connects==3 && !atomic_load(&s_ble_auth_owner));
 assert(close_count==0 && error_count==0 && !s_ble_recovery_active);
 uint32_t first=sid;now_ticks+=500;connected=false;atomic_store(&s_ble_disconnect_tick,now_ticks);
 atomic_store(&s_ble_disconnect_session,first);atomic_store(&s_ble_abnormal_disconnect,true);
 assert(aging_ble_ensure_ready() && sid!=first && connect_calls==4 && !strcmp(RecordId,"record-1"));
 puts("PASS preparation failures and repeated disconnect recover with frozen identity and exclusive transactions");

 reset();plain_device=true;s_ble_target_current.use_mac=true;s_ble_target_current.addr_type=1;
 assert(aging_ble_ensure_ready() && now_ticks>=20000 && mac_connects==1 && !encrypted);
 puts("PASS plaintext device initializes without authentication; MAC and address type remain frozen");

 reset();agingState=AgingAction;assert(aging_begin_step(&step) && pre_sets==1);
 puts("PASS disconnect at a new step boundary replays the new pre-actions exactly once");

 reset();scan_failed=true;assert(!aging_ble_ensure_ready());
 assert(now_ticks==600000 && scan_calls==200 && close_count==0 && error_count==0);
 reset();setup_failed=true;assert(!aging_ble_ensure_ready() && now_ticks==600000 && connect_calls>1);
 reset();read_failed=true;assert(!aging_ble_ensure_ready() && now_ticks==600000 && gets>1);
 reset();step.sample_data_count=0;assert(!aging_ble_ensure_ready() && now_ticks==600000 && gets==0);
 reset();pre_failed=true;s_ble_resume_step=&step;s_ble_replay_pre=true;
 assert(!aging_ble_ensure_ready() && now_ticks==600000 && pre_sets>1);
 reset();cancel_on_delay=true;assert(!aging_ble_ensure_ready() && now_ticks<1000 && close_count==0);
 reset();cancel_on_probe=true;assert(!aging_ble_ensure_ready() && close_count==0);
 puts("PASS scan/GATT/action failures share one 10-minute deadline; cancellation during handshake or probe never closes the session");

 reset();ready();agingState=AgingAction;read_devices=&external;read_device_count=1;
 drop_on_condition=true;run_control();
 assert(idle_count==1 && error_count==0 && pre_sets==2 && external_sets==3 && post_sets==1);
 puts("PASS running discharge reconnects, replays all DUT/external pre-actions, and completes original step");

 reset();ready();agingState=AgingAction;AgingStep pair[2]={step,step};
 agingcfg.steps=pair;agingcfg.step_count=2;dut.steps=pair;dut.step_count=2;CycleIndex=2;
 drop_on_condition=true;run_control();
 assert(idle_count==1 && error_count==0 && pre_sets==5 && post_sets==4 && close_count==1 && connect_calls==1);
 puts("PASS reconnect retains both cycle and step cursors without restarting the round or closing sampling early");

 reset();ready();agingState=AgingAction;step.method="Recharge";condition.value_num=1;drop_on_condition=true;
 run_control();assert(idle_count==1 && error_count==0 && pre_sets==2);
 reset();ready();agingState=AgingAction;drop_on_post=true;run_control();
 assert(idle_count==1 && error_count==0 && pre_sets==1 && post_sets==2);
 puts("PASS recharge ignores failed reads; interrupted post-action retries without reopening pre-actions");

 reset();ready();agingState=AgingAction;post_failed=true;run_control();
 assert(error_count==1 && idle_count==0 && now_ticks-first_post_failure_tick==600000 && pre_sets==1 && post_sets>1 && post_sets<200);
 puts("PASS repeated post-action timeouts share one deadline even if connection probes succeed");

 reset();ready();agingState=AgingAction;step.method="Standing";condition.value_num=0.05;
 standing_dropped=false;delay_hook=standing_drop;run_control();
 assert(standing_dropped && idle_count==1 && error_count==0 && saved_deadline==1003 && now_ticks>6000 && pre_sets==2 && post_sets==1);
 puts("PASS standing deadline is unchanged and link recovery completes before an elapsed step can finish");

 reset();ready();agingState=AgingAction;step.method="Standing";condition.value_num=0.05;
 drop_in_group=true;AgingCMode=2;run_control();
 assert(idle_count==1 && error_count==0 && taskcp_count==1 && pre_sets==1 && post_sets==1 && group_dropped);
 puts("PASS group wait resumes with one completion event and no pre-action replay");

 reset();scan_failed=true;run_control();assert(error_count==1 && idle_count==0 && close_count>0 && now_ticks==600000);
 reset();cancel_on_delay=true;run_control();assert(error_count==0 && close_count==0 && atomic_load(&s_aging_ctrl_exited));
 puts("PASS timeout aborts once; human cancellation exits the control task before caller cleanup");

 reset();atomic_store(&s_aging_sample_exited,false);delay_hook=acknowledge_cancel;
 assert(aging_cancel_tasks() && atomic_load(&s_aging_ctrl_exited) && atomic_load(&s_aging_sample_exited) && !close_count);
 reset();assert(!aging_cancel_tasks() && now_ticks==10000 && atomic_load(&s_aging_cancel) && !close_count);
 puts("PASS stop waits for both tasks; a busy task retains cancellation and context for a later stop retry");

 reset();ready();agingState=AgingAction;agingDataAMode=DischargeState;AgingUploadPacket packet;
 fail_sample_second=true;assert(ProcessAgingStepData(&packet)<0 && !packet.value_json);
 reset();ready();agingDataAMode=DischargeState;drop_sample_second=true;assert(ProcessAgingStepData(&packet)<0 && !packet.value_json);
 reset();ready();agingDataAMode=DischargeState;assert(ProcessAgingStepData(&packet)==2 && packet.session_token==123);
 assert(!strcmp(packet.pn,"PN-1") && !strcmp(packet.record_id,"record-1"));free(packet.value_json);
 reset();ready();agingDataAMode=DischargeState;delay_hook=sample_stop;
 if(!setjmp(task_exit))app_AgingData_Get_handle(NULL);
 assert(samples==1 && atomic_load(&s_aging_sample_exited) && !atomic_load(&s_sample_reading));
 puts("PASS failed or cross-session samples are discarded; valid samples keep session identity and sampler exits cooperatively");
}
int main(void){main_tests();return 0;}
'''


def main():
    app = (ROOT / 'main/Task/appTask.c').read_text()
    signatures = [
        'static bool aging_device_uses_ble(', 'static bool Data_Set_Method(Externaldevice *device, const char *idorname,',
        'static bool Data_Get_Method(Externaldevice *device, const char *idorname,',
        'static bool aging_task_wait_ms(uint32_t wait_ms)\n{', 'static bool aging_cancel_tasks(void)\n{',
        'static void aging_finish_sampling(', 'static bool Aging_Execute_ActionList(',
    ]
    source = MOCKS + '\nstatic bool aging_ble_ensure_ready(void);\nstatic uint32_t aging_ble_remaining_ms(void);\n'
    for sig in signatures:
        # Data_Set_Method has a prototype with the same leading signature.
        if sig.startswith('static bool Data_Set_Method'):
            sig = 'static bool Data_Set_Method(Externaldevice *device, const char *idorname, const char *param, uint8_t slave_addr, uint8_t is_name)\n{'
        source += '\n' + function_source(app, sig)
    start = app.index('static uint32_t aging_ble_remaining_ms(void)\n{')
    end = app.index('\n#pragma endregion', app.index('void Aging_Test_Task(void *arg)\n{', start))
    source += '\n' + app[start:end]
    for sig in ['static int find_external_step_index(', 'static bool add_aging_value_item(',
                'static void AgingUploadPacket_Free(', 'static int ProcessAgingStepData(',
                'void app_AgingData_Get_handle(void *arg)\n{']:
        source += '\n' + function_source(app, sig)
    # The mock forward declaration must match the production task's external linkage.
    source = source.replace('static void app_AgingData_Get_handle(void *arg);', 'void app_AgingData_Get_handle(void *arg);')
    with tempfile.TemporaryDirectory(prefix='ble-recovery-test-') as directory:
        temp = Path(directory)
        (temp / 'test.c').write_text(source + TESTS)
        cjson = IDF / 'components/json/cJSON'
        binary = temp / 'test'
        args = [os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter', '-Wno-sign-compare',
                '-I', str(ROOT / 'main/ConfigData'), '-I', str(cjson),
                str(temp / 'test.c'), str(cjson / 'cJSON.c'), '-lm', '-o', str(binary)]
        if os.environ.get('TEST_SANITIZE') == '1':
            args[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(args, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)

if __name__ == '__main__':
    main()
