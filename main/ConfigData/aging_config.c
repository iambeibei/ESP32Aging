#include "aging_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ESP-IDF 中如果这里报错，可以改成：
 * #include "cjson/cJSON.h"
 */
#include "cJSON.h"
#include "app_mem.h"
#include "ConfigData.h"
#include "appTask.h"

static char *aging_strdup(const char *s)
{
    if (!s)
    {
        return NULL;
    }

    size_t len = strlen(s);
    char *p = (char *)app_malloc_prefer_psram(len + 1);
    if (!p)
    {
        return NULL;
    }

    memcpy(p, s, len + 1);
    return p;
}

static char *json_get_strdup(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsString(item) && item->valuestring)
    {
        return aging_strdup(item->valuestring);
    }

    return NULL;
}

static int json_get_int(cJSON *obj, const char *key, int def)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsNumber(item))
    {
        return item->valueint;
    }

    if (cJSON_IsString(item) && item->valuestring)
    {
        return atoi(item->valuestring);
    }

    return def;
}

static long long json_get_ll(cJSON *obj, const char *key, long long def)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsNumber(item))
    {
        return (long long)item->valuedouble;
    }

    if (cJSON_IsString(item) && item->valuestring)
    {
        return atoll(item->valuestring);
    }

    return def;
}

static char *json_item_to_strdup(cJSON *item)
{
    char buf[32];

    if (cJSON_IsString(item) && item->valuestring)
    {
        return aging_strdup(item->valuestring);
    }

    if (cJSON_IsNumber(item))
    {
        snprintf(buf, sizeof(buf), "%g", item->valuedouble);
        return aging_strdup(buf);
    }

    return NULL;
}

static void free_condition(AgingJudgingCondition *cond)
{
    if (!cond)
    {
        return;
    }

    free(cond->Name);
    free(cond->Value);
    memset(cond, 0, sizeof(*cond));
}

static AgingErr parse_condition_object(cJSON *cond_obj, AgingJudgingCondition *cond)
{
    if (!cond_obj || !cond)
    {
        return AGING_ERR_PARAM;
    }

    memset(cond, 0, sizeof(*cond));

    if (!cJSON_IsObject(cond_obj))
    {
        return AGING_OK;
    }

    cond->Name = json_get_strdup(cond_obj, "Name");

    cJSON *value_item = cJSON_GetObjectItemCaseSensitive(cond_obj, "Value");
    cond->Value = json_item_to_strdup(value_item);

    if (cond->Name || cond->Value)
    {
        cond->valid = true;
    }

    if (cond->Value)
    {
        cond->value_num = atof(cond->Value);
    }

    return AGING_OK;
}

static AgingErr parse_judging_condition(cJSON *step_obj, AgingStep *step)
{
    if (!step_obj || !step)
    {
        return AGING_ERR_PARAM;
    }

    cJSON *cond = cJSON_GetObjectItemCaseSensitive(step_obj, "JudgingCondition");
    if (!cJSON_IsArray(cond))
    {
        return AGING_ERR_JSON;
    }

    int count = cJSON_GetArraySize(cond);
    if (count <= 0)
    {
        return AGING_ERR_JSON;
    }

    step->judging_conditions = (AgingJudgingCondition *)app_calloc_prefer_psram((size_t)count, sizeof(AgingJudgingCondition));
    if (!step->judging_conditions)
    {
        return AGING_ERR_NOMEM;
    }

    step->judging_condition_count = (size_t)count;

    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(cond, i);
        if (!cJSON_IsObject(item))
        {
            return AGING_ERR_JSON;
        }

        AgingErr err = parse_condition_object(item, &step->judging_conditions[i]);
        if (err != AGING_OK)
        {
            return err;
        }

        if (!step->judging_conditions[i].valid ||
            !step->judging_conditions[i].Name ||
            !step->judging_conditions[i].Value)
        {
            return AGING_ERR_JSON;
        }
    }

    return AGING_OK;
}

static AgingErr parse_sample_data(cJSON *step_obj, AgingStep *step)
{
    if (!step_obj || !step)
    {
        return AGING_ERR_PARAM;
    }

    cJSON *sample = cJSON_GetObjectItemCaseSensitive(step_obj, "SampleData");

    if (sample == NULL || cJSON_IsNull(sample))
    {
        step->sample_data_count=0;
        return AGING_OK;
    }

    // /* Action 存在但类型不对时才属于协议格式错误。 */
    // if (!cJSON_IsObject(sample))
    // {
    //     return AGING_ERR_JSON;
    // }

    int count = cJSON_GetArraySize(sample);
    if (count <= 0)
    {
        return AGING_OK;
    }

    step->sample_data = (AgingSampleData *)app_calloc_prefer_psram((size_t)count, sizeof(AgingSampleData));
    if (!step->sample_data)
    {
        return AGING_ERR_NOMEM;
    }

    step->sample_data_count = (size_t)count;

    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(sample, i);
        if (!cJSON_IsObject(item))
        {
            return AGING_ERR_JSON;
        }

        step->sample_data[i].Value = json_get_strdup(item, "Value");
        if (!step->sample_data[i].Value)
        {
            return AGING_ERR_JSON;
        }
    }

    return AGING_OK;
}

static void free_action_items_partial(AgingActionItem *items, size_t count)
{
    if (items == NULL)
    {
        return;
    }

    for (size_t i = 0; i < count; i++)
    {
        free(items[i].ParaID);
        free(items[i].ParaName);
        free(items[i].ParaValue);
    }
    free(items);
}

static AgingErr parse_action_array(cJSON *array, AgingActionItem **out_items, size_t *out_count, uint8_t is_name)
{
    if (!out_items || !out_count)
    {
        return AGING_ERR_PARAM;
    }

    *out_items = NULL;
    *out_count = 0;

    if (!array || cJSON_IsNull(array))
    {
        return AGING_OK;
    }

    if (!cJSON_IsArray(array))
    {
        return AGING_ERR_JSON;
    }

    int count = cJSON_GetArraySize(array);
    if (count <= 0)
    {
        return AGING_OK;
    }

    AgingActionItem *items = (AgingActionItem *)app_calloc_prefer_psram((size_t)count, sizeof(AgingActionItem));
    if (!items)
    {
        return AGING_ERR_NOMEM;
    }

    for (int i = 0; i < count; i++)
    {
        cJSON *obj = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsObject(obj))
        {
            free_action_items_partial(items, (size_t)count);
            return AGING_ERR_JSON;
        }

        if (is_name == 1)
        {
            items[i].ParaName = json_get_strdup(obj, "ParaName");
        }
        else
        {
            items[i].ParaID = json_get_strdup(obj, "ParaID");
        }

        cJSON *para_value_item = cJSON_GetObjectItemCaseSensitive(obj, "ParaValue");
        items[i].ParaValue = json_item_to_strdup(para_value_item);

        if ((is_name == 1 && !items[i].ParaName) ||
            (is_name == 0 && !items[i].ParaID) ||
            !items[i].ParaValue)
        {
            free_action_items_partial(items, (size_t)count);
            return AGING_ERR_JSON;
        }

        items[i].para_value_num = atof(items[i].ParaValue);
    }

    *out_items = items;
    *out_count = (size_t)count;

    return AGING_OK;
}

static AgingErr parse_action(cJSON *step_obj, AgingStep *step, uint8_t is_name)
{
    if (!step_obj || !step)
    {
        return AGING_ERR_PARAM;
    }

    cJSON *action = cJSON_GetObjectItemCaseSensitive(step_obj, "Action");

    /* Action 是可选字段；缺失、null、{} 都表示当前步骤没有动作。 */
    if (action == NULL || cJSON_IsNull(action))
    {
        return AGING_OK;
    }

    /* Action 存在但类型不对时才属于协议格式错误。 */
    if (!cJSON_IsObject(action))
    {
        return AGING_ERR_JSON;
    }

    cJSON *pre_action = cJSON_GetObjectItemCaseSensitive(action, "PreAction");
    AgingErr err = parse_action_array(pre_action,
                                      &step->pre_actions,
                                      &step->pre_action_count, is_name);
    if (err != AGING_OK)
    {
        return err;
    }

    cJSON *after_action = cJSON_GetObjectItemCaseSensitive(action, "AfterAction");
    err = parse_action_array(after_action,
                             &step->after_actions,
                             &step->after_action_count, is_name);
    if (err != AGING_OK)
    {
        return err;
    }

    return AGING_OK;
}

static AgingErr parse_devices(cJSON *aging_devices_obj, AgingConfig *out)
{
    if (!aging_devices_obj || !out)
    {
        return AGING_ERR_PARAM;
    }

    cJSON *devices = cJSON_GetObjectItemCaseSensitive(aging_devices_obj, "Devices");
    if (!cJSON_IsArray(devices))
    {
        return AGING_ERR_JSON;
    }

    int count = cJSON_GetArraySize(devices);
    if (count <= 0)
    {
        return AGING_ERR_JSON;
    }

    out->devices = (AgingDevice *)app_calloc_prefer_psram((size_t)count, sizeof(AgingDevice));
    if (!out->devices)
    {
        return AGING_ERR_NOMEM;
    }

    out->device_count = (size_t)count;

    for (int i = 0; i < count; i++)
    {
        cJSON *dev = cJSON_GetArrayItem(devices, i);
        if (!cJSON_IsObject(dev))
        {
            return AGING_ERR_JSON;
        }

        out->devices[i].Type = json_get_strdup(dev, "Type");
        out->devices[i].Communication = json_get_strdup(dev, "Communication");
        out->devices[i].ProtoID = json_get_int(dev, "ProtoID", 0);

        if (!out->devices[i].Type || !out->devices[i].Communication || out->devices[i].ProtoID == 0)
        {
            return AGING_ERR_JSON;
        }
    }

    return AGING_OK;
}

static AgingErr parse_steps(cJSON *aging_steps_obj, AgingConfig *out)
{
    if (!aging_steps_obj || !out)
    {
        return AGING_ERR_PARAM;
    }

    cJSON *list = cJSON_GetObjectItemCaseSensitive(aging_steps_obj, "AgingFunctionList");
    if (!cJSON_IsArray(list))
    {
        return AGING_ERR_JSON;
    }

    int count = cJSON_GetArraySize(list);
    if (count <= 0)
    {
        return AGING_ERR_JSON;
    }

    out->steps = (AgingStep *)app_calloc_prefer_psram((size_t)count, sizeof(AgingStep));
    if (!out->steps)
    {
        return AGING_ERR_NOMEM;
    }

    out->step_count = (size_t)count;

    for (int i = 0; i < count; i++)
    {
        cJSON *step_obj = cJSON_GetArrayItem(list, i);
        if (!cJSON_IsObject(step_obj))
        {
            return AGING_ERR_JSON;
        }

        AgingStep *step = &out->steps[i];

        step->method = json_get_strdup(step_obj, "Method");
        if (!step->method)
        {
            return AGING_ERR_JSON;
        }

        AgingErr err = parse_judging_condition(step_obj, step);
        if (err != AGING_OK)
        {
            return err;
        }

        err = parse_sample_data(step_obj, step);
        if (err != AGING_OK)
        {
            return err;
        }

        err = parse_action(step_obj, step, 0);
        if (err != AGING_OK)
        {
            return err;
        }
    }

    return AGING_OK;
}

void aging_steps_free(AgingStep *steps, size_t step_count)
{
    if (steps == NULL)
    {
        return;
    }

    for (size_t i = 0; i < step_count; i++)
    {
        AgingStep *step = &steps[i];

        if (step->method)
        {
            free(step->method);
            step->method = NULL;
        }

        if (step->judging_conditions)
        {
            for (size_t j = 0; j < step->judging_condition_count; j++)
            {
                free(step->judging_conditions[j].Name);
                free(step->judging_conditions[j].Value);

                step->judging_conditions[j].Name = NULL;
                step->judging_conditions[j].Value = NULL;
                step->judging_conditions[j].valid = false;
                step->judging_conditions[j].value_num = 0;
            }

            free(step->judging_conditions);
            step->judging_conditions = NULL;
            step->judging_condition_count = 0;
        }

        if (step->sample_data)
        {
            for (size_t j = 0; j < step->sample_data_count; j++)
            {
                free(step->sample_data[j].Value);
                step->sample_data[j].Value = NULL;
            }

            free(step->sample_data);
            step->sample_data = NULL;
            step->sample_data_count = 0;
        }

        if (step->pre_actions)
        {
            for (size_t j = 0; j < step->pre_action_count; j++)
            {
                free(step->pre_actions[j].ParaID);
                free(step->pre_actions[j].ParaName);
                free(step->pre_actions[j].ParaValue);

                step->pre_actions[j].ParaID = NULL;
                step->pre_actions[j].ParaName = NULL;
                step->pre_actions[j].ParaValue = NULL;
                step->pre_actions[j].para_value_num = 0;
            }

            free(step->pre_actions);
            step->pre_actions = NULL;
            step->pre_action_count = 0;
        }

        if (step->after_actions)
        {
            for (size_t j = 0; j < step->after_action_count; j++)
            {
                free(step->after_actions[j].ParaID);
                free(step->after_actions[j].ParaName);
                free(step->after_actions[j].ParaValue);

                step->after_actions[j].ParaID = NULL;
                step->after_actions[j].ParaName = NULL;
                step->after_actions[j].ParaValue = NULL;
                step->after_actions[j].para_value_num = 0;
            }

            free(step->after_actions);
            step->after_actions = NULL;
            step->after_action_count = 0;
        }
    }

    free(steps);
}
AgingErr parse_steps_Ex(cJSON *aging_steps_obj, AgingStep **out_steps, size_t *out_count)
{
    if (!aging_steps_obj || !out_steps || !out_count)
    {
        return AGING_ERR_PARAM;
    }

    *out_steps = NULL;
    *out_count = 0;

    cJSON *list = cJSON_GetObjectItemCaseSensitive(aging_steps_obj, "AgingFunctionList");
    if (!cJSON_IsArray(list))
    {
        return AGING_ERR_JSON;
    }

    int count = cJSON_GetArraySize(list);
    if (count <= 0)
    {
        return AGING_ERR_JSON;
    }

    AgingStep *steps = (AgingStep *)app_calloc_prefer_psram((size_t)count, sizeof(AgingStep));
    if (!steps)
    {
        return AGING_ERR_NOMEM;
    }

    for (int i = 0; i < count; i++)
    {
        cJSON *step_obj = cJSON_GetArrayItem(list, i);
        if (!cJSON_IsObject(step_obj))
        {
            aging_steps_free(steps, (size_t)count);
            return AGING_ERR_JSON;
        }

        AgingStep *step = &steps[i];

        step->method = json_get_strdup(step_obj, "Method");
        if (!step->method)
        {
            aging_steps_free(steps, (size_t)count);
            return AGING_ERR_JSON;
        }

        AgingErr err = parse_sample_data(step_obj, step);
        if (err != AGING_OK)
        {
            aging_steps_free(steps, (size_t)count);
            return err;
        }

        err = parse_action(step_obj, step, 1);
        if (err != AGING_OK)
        {
            aging_steps_free(steps, (size_t)count);
            return err;
        }
    }

    *out_steps = steps;
    *out_count = (size_t)count;
    return AGING_OK;
}

AgingErr aging_config_parse(const char *json_text, AgingConfig *out)
{
    if (!json_text || !out)
    {
        return AGING_ERR_PARAM;
    }

    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json_text);
    if (!root)
    {
        return AGING_ERR_JSON;
    }

    AgingErr err = AGING_OK;

    out->seq = json_get_int(root, "Seq", 0);
    if (out->seq == 0)
    {
        err = AGING_ERR_JSON;
        goto done;
    }

    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "Data");
    if (!cJSON_IsObject(data))
    {
        err = AGING_ERR_JSON;
        goto done;
    }

    cJSON *ClMode = cJSON_GetObjectItem(data, "ControlMode");
    if (ClMode)
    {
        AgingCMode = (uint8_t)ClMode->valueint;
        publish_device_mode(ClMode->valueint);
    }
    cJSON *CycleIndexObj = cJSON_GetObjectItem(data, "CycleIndex");
    if (CycleIndexObj && cJSON_IsNumber(CycleIndexObj))
    {
        CycleIndex = (uint8_t)CycleIndexObj->valueint;
    }

    cJSON *aging_devices = cJSON_GetObjectItemCaseSensitive(data, "AgingDevices");
    if (!cJSON_IsObject(aging_devices))
    {
        err = AGING_ERR_JSON;
        goto done;
    }

    err = parse_devices(aging_devices, out);
    if (err != AGING_OK)
    {
        goto done;
    }

    cJSON *aging_steps = cJSON_GetObjectItemCaseSensitive(data, "AgingSteps");
    if (!cJSON_IsObject(aging_steps))
    {
        err = AGING_ERR_JSON;
        goto done;
    }

    err = parse_steps(aging_steps, out);
    if (err != AGING_OK)
    {
        goto done;
    }

    if (out->device_count == 0 || out->step_count == 0)
    {
        err = AGING_ERR_JSON;
        goto done;
    }

done:
    cJSON_Delete(root);

    if (err != AGING_OK)
    {
        aging_config_free(out);
    }

    return err;
}

void aging_config_free(AgingConfig *cfg)
{
    if (!cfg)
    {
        return;
    }

    if (cfg->devices)
    {
        for (size_t i = 0; i < cfg->device_count; i++)
        {
            free(cfg->devices[i].Type);
            free(cfg->devices[i].Communication);
        }

        free(cfg->devices);
    }

    if (cfg->steps)
    {
        for (size_t i = 0; i < cfg->step_count; i++)
        {
            free(cfg->steps[i].method);

            if (cfg->steps[i].judging_conditions)
            {
                for (size_t j = 0; j < cfg->steps[i].judging_condition_count; j++)
                {
                    free_condition(&cfg->steps[i].judging_conditions[j]);
                }

                free(cfg->steps[i].judging_conditions);
            }

            if (cfg->steps[i].sample_data)
            {
                for (size_t j = 0; j < cfg->steps[i].sample_data_count; j++)
                {
                    free(cfg->steps[i].sample_data[j].Value);
                }

                free(cfg->steps[i].sample_data);
            }

            if (cfg->steps[i].pre_actions)
            {
                for (size_t j = 0; j < cfg->steps[i].pre_action_count; j++)
                {
                    free(cfg->steps[i].pre_actions[j].ParaID);
                    free(cfg->steps[i].pre_actions[j].ParaName);
                    free(cfg->steps[i].pre_actions[j].ParaValue);
                }

                free(cfg->steps[i].pre_actions);
            }

            if (cfg->steps[i].after_actions)
            {
                for (size_t j = 0; j < cfg->steps[i].after_action_count; j++)
                {
                    free(cfg->steps[i].after_actions[j].ParaID);
                    free(cfg->steps[i].after_actions[j].ParaName);
                    free(cfg->steps[i].after_actions[j].ParaValue);
                }

                free(cfg->steps[i].after_actions);
            }
        }

        free(cfg->steps);
    }

    memset(cfg, 0, sizeof(*cfg));
}

const AgingDevice *aging_config_find_device_by_name(const AgingConfig *cfg, const char *type)
{
    if (!cfg || !type)
    {
        return NULL;
    }

    for (size_t i = 0; i < cfg->device_count; i++)
    {
        const AgingDevice *dev = &cfg->devices[i];

        if (dev->Type && strcmp(dev->Type, type) == 0)
        {
            return dev;
        }
    }

    return NULL;
}

const AgingDevice *aging_config_find_device_by_id(const AgingConfig *cfg, int id)
{
    if (!cfg)
    {
        return NULL;
    }

    for (size_t i = 0; i < cfg->device_count; i++)
    {
        const AgingDevice *dev = &cfg->devices[i];

        if (dev->ProtoID == id)
        {
            return dev;
        }
    }

    return NULL;
}

void aging_config_print(const AgingConfig *cfg)
{
    if (!cfg)
    {
        return;
    }

    printf("Seq: %d\r\n", cfg->seq);

    printf("Devices: %u\r\n", (unsigned)cfg->device_count);

    for (size_t i = 0; i < cfg->device_count; i++)
    {
        const AgingDevice *d = &cfg->devices[i];

        printf("  [%u] Type=%s, Communication=%s, ProtoID=%lld\r\n",
               (unsigned)i,
               d->Type ? d->Type : "",
               d->Communication ? d->Communication : "",
               d->ProtoID);
    }

    printf("Aging Steps: %u\r\n", (unsigned)cfg->step_count);

    for (size_t i = 0; i < cfg->step_count; i++)
    {
        const AgingStep *s = &cfg->steps[i];

        printf("  [%u] Method=%s\r\n",
               (unsigned)i,
               s->method ? s->method : "");

        printf("      JudgingCondition count=%u\r\n", (unsigned)s->judging_condition_count);
        for (size_t j = 0; j < s->judging_condition_count; j++)
        {
            const AgingJudgingCondition *cond = &s->judging_conditions[j];
            printf("        [%u] Name=%s, Value=%s, value_num=%.2f\r\n",
                   (unsigned)j,
                   cond->Name ? cond->Name : "",
                   cond->Value ? cond->Value : "",
                   cond->value_num);
        }

        printf("      SampleData count=%u\r\n", (unsigned)s->sample_data_count);
        for (size_t j = 0; j < s->sample_data_count; j++)
        {
            printf("        [%u] Value=%s\r\n",
                   (unsigned)j,
                   s->sample_data[j].Value ? s->sample_data[j].Value : "");
        }

        printf("      PreAction count=%u\r\n", (unsigned)s->pre_action_count);
        for (size_t j = 0; j < s->pre_action_count; j++)
        {
            printf("        [%u] ParaID=%s, ParaValue=%s\r\n",
                   (unsigned)j,
                   s->pre_actions[j].ParaID ? s->pre_actions[j].ParaID : "",
                   s->pre_actions[j].ParaValue ? s->pre_actions[j].ParaValue : "");
        }

        printf("      AfterAction count=%u\r\n", (unsigned)s->after_action_count);
        for (size_t j = 0; j < s->after_action_count; j++)
        {
            printf("        [%u] ParaID=%s, ParaValue=%s\r\n",
                   (unsigned)j,
                   s->after_actions[j].ParaID ? s->after_actions[j].ParaID : "",
                   s->after_actions[j].ParaValue ? s->after_actions[j].ParaValue : "");
        }
    }
}
