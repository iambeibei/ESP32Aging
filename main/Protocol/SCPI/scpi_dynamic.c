#include "scpi_dynamic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "scpi_dynamic";


static void *sd_malloc_prefer_psram(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

static void *sd_calloc_prefer_psram(size_t n, size_t size)
{
    if (n == 0 || size == 0) {
        return NULL;
    }

    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
    }
    return p;
}

static char *sd_strdup(const char *s)
{
    if (!s) {
        return NULL;
    }

    size_t len = strlen(s);
    char *p = (char *)sd_malloc_prefer_psram(len + 1);
    if (!p) {
        return NULL;
    }

    memcpy(p, s, len + 1);
    return p;
}

static char *json_get_string_dup(cJSON *obj, const char *key)
{
    if (!obj || !key) {
        return NULL;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return NULL;
    }

    return sd_strdup(item->valuestring);
}

static int json_get_int_default(cJSON *obj, const char *key, int def)
{
    if (!obj || !key) {
        return def;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }

    return def;
}

static void free_param(scpi_param_def_t *param)
{
    if (!param) {
        return;
    }

    free(param->name);
    free(param->unit);
    free(param->default_value);

    if (param->limits) {
        for (size_t i = 0; i < param->limit_count; i++) {
            free(param->limits[i].key);
            free(param->limits[i].desc);
        }
        free(param->limits);
    }

    memset(param, 0, sizeof(*param));
}

static void free_cmd_item(scpi_cmd_item_t *item)
{
    if (!item) {
        return;
    }

    free(item->id);
    free(item->name);
    free(item->rw);
    free(item->cmd_template);
    free(item->rsp_template);

    free_param(&item->param);

    memset(item, 0, sizeof(*item));
}

static bool rw_contains(const char *rw, char ch)
{
    if (!rw) {
        return false;
    }

    for (const char *p = rw; *p; p++) {
        if (*p == ch) {
            return true;
        }
    }

    return false;
}

static size_t count_object_items(cJSON *obj)
{
    if (!cJSON_IsObject(obj)) {
        return 0;
    }

    size_t count = 0;
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, obj) {
        count++;
    }

    return count;
}

static esp_err_t parse_limit_object(cJSON *limit_obj, scpi_param_def_t *param)
{
    if (!cJSON_IsObject(limit_obj) || !param) {
        return ESP_OK;
    }

    size_t count = count_object_items(limit_obj);
    if (count == 0) {
        return ESP_OK;
    }

    param->limits = (scpi_limit_item_t *)sd_calloc_prefer_psram(count, sizeof(scpi_limit_item_t));
    if (!param->limits) {
        return ESP_ERR_NO_MEM;
    }

    param->limit_count = 0;

    cJSON *child = NULL;
    cJSON_ArrayForEach(child, limit_obj) {
        if (!child->string) {
            continue;
        }

        param->limits[param->limit_count].key = sd_strdup(child->string);

        if (cJSON_IsString(child) && child->valuestring) {
            param->limits[param->limit_count].desc = sd_strdup(child->valuestring);
        } else if (cJSON_IsNumber(child)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%g", child->valuedouble);
            param->limits[param->limit_count].desc = sd_strdup(tmp);
        } else {
            param->limits[param->limit_count].desc = sd_strdup("");
        }

        if (!param->limits[param->limit_count].key ||
            !param->limits[param->limit_count].desc) {
            return ESP_ERR_NO_MEM;
        }

        param->limit_count++;
    }

    return ESP_OK;
}

static esp_err_t parse_param_def(cJSON *param_obj, scpi_cmd_item_t *cmd)
{
    if (!cmd || !cJSON_IsObject(param_obj)) {
        return ESP_OK;
    }

    /*
     * 你的 JSON 里面类似：
     *
     * "命令参数": {
     *     "动作": {
     *         "unit": null,
     *         "defaultValue": null,
     *         "limit": {
     *             "0": "关闭",
     *             "1": "开启"
     *         }
     *     }
     * }
     *
     * 这里不把“动作”写死，而是解析第一个参数定义。
     */
    cJSON *first_param = param_obj->child;
    if (!first_param || !first_param->string) {
        return ESP_OK;
    }

    cmd->has_param_def = true;
    cmd->param.name = sd_strdup(first_param->string);
    if (!cmd->param.name) {
        return ESP_ERR_NO_MEM;
    }

    if (cJSON_IsObject(first_param)) {
        cJSON *unit = cJSON_GetObjectItemCaseSensitive(first_param, "unit");
        if (cJSON_IsString(unit) && unit->valuestring) {
            cmd->param.unit = sd_strdup(unit->valuestring);
        }

        cJSON *def = cJSON_GetObjectItemCaseSensitive(first_param, "defaultValue");
        if (cJSON_IsString(def) && def->valuestring) {
            cmd->param.default_value = sd_strdup(def->valuestring);
        } else if (cJSON_IsNumber(def)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%g", def->valuedouble);
            cmd->param.default_value = sd_strdup(tmp);
        }

        cJSON *limit = cJSON_GetObjectItemCaseSensitive(first_param, "limit");
        esp_err_t ret = parse_limit_object(limit, &cmd->param);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

static bool string_has_placeholder(const char *s)
{
    if (!s) {
        return false;
    }

    return strstr(s, "<NRf>") ||
           strstr(s, "<NP1>") ||
           strstr(s, "<function>");
}

static const char *find_first_placeholder(const char *s, size_t *placeholder_len)
{
    if (!s || !placeholder_len) {
        return NULL;
    }

    const char *p_nrf = strstr(s, "<NRf>");
    const char *p_np1 = strstr(s, "<NP1>");
    const char *p_func = strstr(s, "<function>");

    const char *best = NULL;
    size_t len = 0;

    if (p_nrf) {
        best = p_nrf;
        len = strlen("<NRf>");
    }

    if (p_np1 && (!best || p_np1 < best)) {
        best = p_np1;
        len = strlen("<NP1>");
    }

    if (p_func && (!best || p_func < best)) {
        best = p_func;
        len = strlen("<function>");
    }

    if (!best) {
        return NULL;
    }

    *placeholder_len = len;
    return best;
}

static esp_err_t replace_placeholders_once_or_more(const char *tpl,
                                                   const char *param,
                                                   char *out,
                                                   size_t out_size)
{
    if (!tpl || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!param) {
        param = "";
    }

    size_t out_pos = 0;
    const char *cur = tpl;

    while (*cur) {
        size_t ph_len = 0;
        const char *ph = find_first_placeholder(cur, &ph_len);

        if (!ph) {
            size_t remain_len = strlen(cur);
            if (out_pos + remain_len >= out_size) {
                return ESP_ERR_NO_MEM;
            }

            memcpy(out + out_pos, cur, remain_len);
            out_pos += remain_len;
            break;
        }

        size_t plain_len = (size_t)(ph - cur);
        if (out_pos + plain_len >= out_size) {
            return ESP_ERR_NO_MEM;
        }

        memcpy(out + out_pos, cur, plain_len);
        out_pos += plain_len;

        size_t param_len = strlen(param);
        if (out_pos + param_len >= out_size) {
            return ESP_ERR_NO_MEM;
        }

        memcpy(out + out_pos, param, param_len);
        out_pos += param_len;

        cur = ph + ph_len;
    }

    out[out_pos] = '\0';
    return ESP_OK;
}

static void trim_inplace(char *s)
{
    if (!s) {
        return;
    }

    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

static size_t expected_response_value_count(const char *rsp_template)
{
    if (!rsp_template) {
        return 0;
    }

    size_t count = 0;
    const char *p = rsp_template;

    while ((p = strstr(p, "<NRf>")) != NULL) {
        count++;
        p += strlen("<NRf>");
    }

    return count;
}

esp_err_t scpi_dynamic_import_json(const char *json_text, scpi_protocol_t **out_protocol)
{
    if (!json_text || !out_protocol) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_protocol = NULL;

    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_ERR_INVALID_ARG;
    }

    scpi_protocol_t *protocol = (scpi_protocol_t *)sd_calloc_prefer_psram(1, sizeof(scpi_protocol_t));
    if (!protocol) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    protocol->id = json_get_string_dup(root, "id");
    protocol->uuid = json_get_string_dup(root, "uuid");
    protocol->name = json_get_string_dup(root, "name");
    protocol->description = json_get_string_dup(root, "description");
    protocol->protocol_type = json_get_int_default(root, "protocolType", -1);
    protocol->protocol_type_str = json_get_string_dup(root, "protocolTypeStr");
    protocol->version = json_get_int_default(root, "version", 0);

    cJSON *contents = cJSON_GetObjectItemCaseSensitive(root, "contents");
    if (!cJSON_IsArray(contents)) {
        ESP_LOGE(TAG, "JSON field contents is not array");
        scpi_dynamic_free(protocol);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int arr_size = cJSON_GetArraySize(contents);
    if (arr_size <= 0) {
        ESP_LOGW(TAG, "contents is empty");
        cJSON_Delete(root);
        *out_protocol = protocol;
        return ESP_OK;
    }

    protocol->items = (scpi_cmd_item_t *)sd_calloc_prefer_psram((size_t)arr_size, sizeof(scpi_cmd_item_t));
    if (!protocol->items) {
        scpi_dynamic_free(protocol);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    protocol->item_count = 0;

    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, contents) {
        if (!cJSON_IsObject(entry)) {
            continue;
        }

        scpi_cmd_item_t *cmd = &protocol->items[protocol->item_count];

        cmd->id = json_get_string_dup(entry, "id");
        cmd->name = json_get_string_dup(entry, "名称");
        cmd->rw = json_get_string_dup(entry, "读写状态");
        cmd->cmd_template = json_get_string_dup(entry, "命令模板");
        cmd->rsp_template = json_get_string_dup(entry, "响应模板");

        cmd->readable = rw_contains(cmd->rw, 'R');
        cmd->writable = rw_contains(cmd->rw, 'W');

        cJSON *param_obj = cJSON_GetObjectItemCaseSensitive(entry, "命令参数");
        esp_err_t ret = parse_param_def(param_obj, cmd);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "parse param failed");
            scpi_dynamic_free(protocol);
            cJSON_Delete(root);
            return ret;
        }

        if (!cmd->id || !cmd->name || !cmd->rw || !cmd->cmd_template) {
            ESP_LOGW(TAG, "skip invalid item");
            free_cmd_item(cmd);
            continue;
        }

        protocol->item_count++;
    }

    cJSON_Delete(root);
    *out_protocol = protocol;

    ESP_LOGI(TAG, "SCPI protocol imported: %s, items=%u",
             protocol->name ? protocol->name : "unknown",
             (unsigned int)protocol->item_count);

    return ESP_OK;
}

void scpi_dynamic_free(scpi_protocol_t *protocol)
{
    if (!protocol) {
        return;
    }

    free(protocol->id);
    free(protocol->uuid);
    free(protocol->name);
    free(protocol->description);
    free(protocol->protocol_type_str);

    if (protocol->items) {
        for (size_t i = 0; i < protocol->item_count; i++) {
            free_cmd_item(&protocol->items[i]);
        }
        free(protocol->items);
    }

    free(protocol);
}

size_t scpi_dynamic_get_count(const scpi_protocol_t *protocol)
{
    if (!protocol) {
        return 0;
    }

    return protocol->item_count;
}

const scpi_cmd_item_t *scpi_dynamic_find_by_name(const scpi_protocol_t *protocol, const char *name)
{
    if (!protocol || !name) {
        return NULL;
    }

    for (size_t i = 0; i < protocol->item_count; i++) {
        if (protocol->items[i].name && strcmp(protocol->items[i].name, name) == 0) {
            return &protocol->items[i];
        }
    }

    return NULL;
}

const scpi_cmd_item_t *scpi_dynamic_find_by_id(const scpi_protocol_t *protocol, const char *id)
{
    if (!protocol || !id) {
        return NULL;
    }

    for (size_t i = 0; i < protocol->item_count; i++) {
        if (protocol->items[i].id && strcmp(protocol->items[i].id, id) == 0) {
            return &protocol->items[i];
        }
    }

    return NULL;
}

esp_err_t scpi_dynamic_build_command(const scpi_cmd_item_t *cmd,
                                     const char *param,
                                     char *out_buf,
                                     size_t out_size)
{
    if (!cmd || !cmd->cmd_template || !out_buf || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_buf[0] = '\0';

    const char *real_param = param;

    if ((!real_param || strlen(real_param) == 0) &&
        cmd->has_param_def &&
        cmd->param.default_value) {
        real_param = cmd->param.default_value;
    }

    bool need_param = string_has_placeholder(cmd->cmd_template);
    if (need_param && (!real_param || strlen(real_param) == 0)) {
        ESP_LOGE(TAG, "command [%s] needs param, template=%s",
                 cmd->name ? cmd->name : "unknown",
                 cmd->cmd_template);
        return ESP_ERR_INVALID_ARG;
    }

    char temp[256];
    esp_err_t ret = replace_placeholders_once_or_more(cmd->cmd_template,
                                                      real_param,
                                                      temp,
                                                      sizeof(temp));
    if (ret != ESP_OK) {
        return ret;
    }

    size_t need_len = strlen(temp) + strlen(SCPI_DYNAMIC_LINE_ENDING) + 1;
    if (need_len > out_size) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(out_buf, out_size, "%s%s", temp, SCPI_DYNAMIC_LINE_ENDING);
    return ESP_OK;
}

esp_err_t scpi_dynamic_build_by_name(const scpi_protocol_t *protocol,
                                     const char *name,
                                     const char *param,
                                     char *out_buf,
                                     size_t out_size)
{
    const scpi_cmd_item_t *cmd = scpi_dynamic_find_by_name(protocol, name);
    if (!cmd) {
        ESP_LOGE(TAG, "command name not found: %s", name ? name : "NULL");
        return ESP_ERR_NOT_FOUND;
    }

    return scpi_dynamic_build_command(cmd, param, out_buf, out_size);
}

esp_err_t scpi_dynamic_build_by_id(const scpi_protocol_t *protocol,
                                   const char *id,
                                   const char *param,
                                   char *out_buf,
                                   size_t out_size)
{
    const scpi_cmd_item_t *cmd = scpi_dynamic_find_by_id(protocol, id);
    if (!cmd) {
        ESP_LOGE(TAG, "command id not found: %s", id ? id : "NULL");
        return ESP_ERR_NOT_FOUND;
    }

    return scpi_dynamic_build_command(cmd, param, out_buf, out_size);
}

esp_err_t scpi_dynamic_parse_response(const scpi_cmd_item_t *cmd,
                                      const char *response,
                                      double *values,
                                      size_t max_values,
                                      size_t *out_value_count)
{
    if (!cmd || !response || !values || max_values == 0 || !out_value_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_value_count = 0;

    if (!cmd->rsp_template) {
        ESP_LOGW(TAG, "command [%s] has no response template",
                 cmd->name ? cmd->name : "unknown");
        return ESP_ERR_INVALID_STATE;
    }

    size_t expected = expected_response_value_count(cmd->rsp_template);
    if (expected == 0) {
        ESP_LOGW(TAG, "response template has no <NRf>: %s", cmd->rsp_template);
        return ESP_ERR_INVALID_STATE;
    }

    char *copy = sd_strdup(response);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }

    trim_inplace(copy);

    size_t count = 0;
    char *saveptr = NULL;
    char *token = strtok_r(copy, ",", &saveptr);

    while (token && count < max_values) {
        trim_inplace(token);

        char *endptr = NULL;
        float v = strtof(token, &endptr);

        if (endptr == token) {
            free(copy);
            ESP_LOGE(TAG, "response parse failed, token=%s", token);
            return ESP_ERR_INVALID_RESPONSE;
        }

        values[count++] = v;
        token = strtok_r(NULL, ",", &saveptr);
    }

    free(copy);

    *out_value_count = count;

    if (count < expected) {
        ESP_LOGW(TAG, "response value count less than expected, got=%u expected=%u",
                 (unsigned int)count,
                 (unsigned int)expected);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

void scpi_dynamic_dump(const scpi_protocol_t *protocol)
{
    if (!protocol) {
        ESP_LOGI(TAG, "protocol is NULL");
        return;
    }

    ESP_LOGI(TAG, "========== SCPI PROTOCOL ==========");
    ESP_LOGI(TAG, "name: %s", protocol->name ? protocol->name : "");
    ESP_LOGI(TAG, "type: %s", protocol->protocol_type_str ? protocol->protocol_type_str : "");
    ESP_LOGI(TAG, "items: %u", (unsigned int)protocol->item_count);

    for (size_t i = 0; i < protocol->item_count; i++) {
        const scpi_cmd_item_t *cmd = &protocol->items[i];

        ESP_LOGI(TAG, "[%u] id=%s, name=%s, rw=%s, cmd=%s, rsp=%s",
                 (unsigned int)i,
                 cmd->id ? cmd->id : "",
                 cmd->name ? cmd->name : "",
                 cmd->rw ? cmd->rw : "",
                 cmd->cmd_template ? cmd->cmd_template : "",
                 cmd->rsp_template ? cmd->rsp_template : "NULL");

        if (cmd->has_param_def) {
            ESP_LOGI(TAG, "    param: %s", cmd->param.name ? cmd->param.name : "");

            for (size_t j = 0; j < cmd->param.limit_count; j++) {
                ESP_LOGI(TAG, "    limit: %s => %s",
                         cmd->param.limits[j].key ? cmd->param.limits[j].key : "",
                         cmd->param.limits[j].desc ? cmd->param.limits[j].desc : "");
            }
        }
    }

    ESP_LOGI(TAG, "===================================");
}