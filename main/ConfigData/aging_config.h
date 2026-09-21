#ifndef AGING_CONFIG_H
#define AGING_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include <cJSON.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AGING_OK = 0,
    AGING_ERR_PARAM,
    AGING_ERR_JSON,
    AGING_ERR_NOMEM
} AgingErr;

/* AgingDevices.Devices[] */
typedef struct {
    char *Type;
    char *Communication;
    int64_t ProtoID;
} AgingDevice;

/* JudgingCondition */
typedef struct {
    char *Name;        // Time / SOC
    char *Value;       // "30" / "10" / "90"
    double value_num;  // 30 / 10 / 90
    bool valid;
} AgingJudgingCondition;

/* SampleData[] */
typedef struct {
    char *Value;       // 查询SOC / 读取输入电流
} AgingSampleData;

/* Action.PreAction[] / Action.AfterAction[] */
typedef struct {
    
    char *ParaID;
    char *ParaName;
    char *ParaValue;   
    double para_value_num;
} AgingActionItem;

/* AgingSteps.AgingFunctionList[] */
typedef struct {
    char *method;

    AgingJudgingCondition *judging_conditions;
    size_t judging_condition_count;

    AgingSampleData *sample_data;
    size_t sample_data_count;

    AgingActionItem *pre_actions;
    
    size_t pre_action_count;

    AgingActionItem *after_actions;
    size_t after_action_count;
} AgingStep;

/* 总配置 */
typedef struct {
    int seq;

    AgingDevice *devices;
    size_t device_count;

    AgingStep *steps;
    size_t step_count;
} AgingConfig;

AgingErr aging_config_parse(const char *json_text, AgingConfig *out);

void aging_config_free(AgingConfig *cfg);

const AgingDevice *aging_config_find_device_by_name(const AgingConfig *cfg, const char *name);

const AgingDevice *aging_config_find_device_by_id(const AgingConfig *cfg, int id);

void aging_config_print(const AgingConfig *cfg);

AgingErr parse_steps_Ex(cJSON *aging_steps_obj, AgingStep **out_steps, size_t *out_count);
void aging_steps_free(AgingStep *steps, size_t step_count);



#ifdef __cplusplus
}
#endif

#endif