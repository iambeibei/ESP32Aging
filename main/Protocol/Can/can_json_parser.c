#include "can_json_parser.h"
#include "can_protocol_ext.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "can_json_parser";

static void *can_calloc_prefer_psram(size_t n, size_t size)
{
    if (n == 0 || size == 0)
    {
        return NULL;
    }

    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL)
    {
        p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
    }
    return p;
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
    {
        return;
    }

    if (!src)
    {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static const char *json_get_string(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring)
    {
        return item->valuestring;
    }
    return NULL;
}

static int json_get_int(cJSON *obj, const char *key, int default_value)
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

    return default_value;
}

static double json_get_double(cJSON *obj, const char *key, double default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsNumber(item))
    {
        return item->valuedouble;
    }

    if (cJSON_IsString(item) && item->valuestring)
    {
        return atof(item->valuestring);
    }

    return default_value;
}

static CanItemRW parse_rw(const char *rw)
{
    if (!rw)
    {
        return CAN_ITEM_RW_UNKNOWN;
    }

    if (strcmp(rw, "R") == 0)
    {
        return CAN_ITEM_RW_READ;
    }

    if (strcmp(rw, "W") == 0)
    {
        return CAN_ITEM_RW_WRITE;
    }

    if (strcmp(rw, "RW") == 0 || strcmp(rw, "R/W") == 0)
    {
        return CAN_ITEM_RW_READ_WRITE;
    }

    return CAN_ITEM_RW_UNKNOWN;
}

static CanItemValueType parse_value_type(const char *type)
{
    if (!type)
    {
        return CAN_ITEM_TYPE_UNKNOWN;
    }

    if (strcasecmp(type, "NUM") == 0)
    {
        return CAN_ITEM_TYPE_NUM;
    }

    if (strcasecmp(type, "String") == 0 || strcasecmp(type, "STRING") == 0)
    {
        return CAN_ITEM_TYPE_STRING;
    }

    return CAN_ITEM_TYPE_UNKNOWN;
}

bool CanJson_Parse(const char *json_text, CanProtocolInfo *out_info)
{
    if (!json_text || !out_info)
    {
        return false;
    }

    memset(out_info, 0, sizeof(CanProtocolInfo));

    cJSON *root = cJSON_Parse(json_text);
    if (!root)
    {
        ESP_LOGE(TAG, "JSON parse failed: %s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");
        return false;
    }

    copy_string(out_info->id, sizeof(out_info->id), json_get_string(root, "id"));
    copy_string(out_info->uuid, sizeof(out_info->uuid), json_get_string(root, "uuid"));
    copy_string(out_info->name, sizeof(out_info->name), json_get_string(root, "name"));
    copy_string(out_info->description, sizeof(out_info->description), json_get_string(root, "description"));
    copy_string(out_info->protocol_type_str, sizeof(out_info->protocol_type_str), json_get_string(root, "protocolTypeStr"));
    out_info->protocol_type = json_get_int(root, "protocolType", 4);

    cJSON *contents = cJSON_GetObjectItemCaseSensitive(root, "contents");
    if (!cJSON_IsArray(contents))
    {
        ESP_LOGE(TAG, "JSON field 'contents' is not array");
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(contents);
    if (count <= 0)
    {
        cJSON_Delete(root);
        return true;
    }

    out_info->items = (CanProtocolItem *)can_calloc_prefer_psram((size_t)count, sizeof(CanProtocolItem));
    if (!out_info->items)
    {
        cJSON_Delete(root);
        return false;
    }
    out_info->item_count = (size_t)count;

    for (int i = 0; i < count; i++)
    {
        cJSON *node = cJSON_GetArrayItem(contents, i);
        if (!cJSON_IsObject(node))
        {
            continue;
        }

        CanProtocolItem *item = &out_info->items[i];

        copy_string(item->id, sizeof(item->id), json_get_string(node, "id"));
        copy_string(item->name, sizeof(item->name), json_get_string(node, "名称"));
        copy_string(item->rw_str, sizeof(item->rw_str), json_get_string(node, "读写状态"));
        copy_string(item->unit, sizeof(item->unit), json_get_string(node, "单位"));
        copy_string(item->type_str, sizeof(item->type_str), json_get_string(node, "类型"));

        item->rw = parse_rw(item->rw_str);
        item->value_type = parse_value_type(item->type_str);

        item->data_type = (uint16_t)json_get_int(node, "数据类型", 0);
        item->start = (uint16_t)json_get_int(node, "起始", 0);
        item->len = (uint16_t)json_get_int(node, "长度", 1);
        item->precision = json_get_double(node, "精度", 1.0);
        item->compensation = json_get_double(node, "补偿", 0.0);
        item->is_unsigned = (uint8_t)json_get_int(node, "无符号", 1);
        item->bit_state = (uint8_t)json_get_int(node, "位状态", 0);

        if (item->len == 0)
        {
            item->len = 1;
        }

        if (item->value_type == CAN_ITEM_TYPE_NUM && item->precision == 0.0)
        {
            item->precision = 1.0;
        }
    }

    cJSON_Delete(root);
    return true;
}

void CanJson_Free(CanProtocolInfo *info)
{
    if (!info)
    {
        return;
    }

    if (info->items)
    {
        free(info->items);
        info->items = NULL;
    }

    info->item_count = 0;
    memset(info, 0, sizeof(CanProtocolInfo));
}

void CanJson_Print(const CanProtocolInfo *info)
{
    if (!info)
    {
        return;
    }

    printf("CAN Protocol: %s, type=%d, str=%s, item_count=%u\n",
           info->name,
           info->protocol_type,
           info->protocol_type_str,
           (unsigned int)info->item_count);

    for (size_t i = 0; i < info->item_count; i++)
    {
        const CanProtocolItem *item = &info->items[i];
        printf("[%u] name=%s rw=%s data_type=%u start=%u len=%u type=%s precision=%.6f unsigned=%u compensation=%.6f\n",
               (unsigned int)i,
               item->name,
               item->rw_str,
               item->data_type,
               item->start,
               item->len,
               item->type_str,
               item->precision,
               item->is_unsigned,
               item->compensation);
    }
}

const CanProtocolItem *CanJson_FindItemByName(const CanProtocolInfo *info, const char *name)
{
    if (!info || !name)
    {
        return NULL;
    }

    for (size_t i = 0; i < info->item_count; i++)
    {
        if (strcmp(info->items[i].name, name) == 0)
        {
            return &info->items[i];
        }
    }

    return NULL;
}

const CanProtocolItem *CanJson_FindItemByID(const CanProtocolInfo *info, const char *id)
{
    if (!info || !id)
    {
        return NULL;
    }

    for (size_t i = 0; i < info->item_count; i++)
    {
        if (strcmp(info->items[i].id, id) == 0)
        {
            return &info->items[i];
        }
    }

    return NULL;
}

bool CanJson_ItemReadable(const CanProtocolItem *item)
{
    return item && (item->rw == CAN_ITEM_RW_READ || item->rw == CAN_ITEM_RW_READ_WRITE);
}

bool CanJson_ItemWritable(const CanProtocolItem *item)
{
    return item && (item->rw == CAN_ITEM_RW_WRITE || item->rw == CAN_ITEM_RW_READ_WRITE);
}

double CanJson_RawToPhysical(const CanProtocolItem *item, int64_t raw_value)
{
    if (!item)
    {
        return 0.0;
    }

    /*
     * 若你的平台字段“补偿”定义为 value = raw * precision + compensation，
     * 把这里的 - 改成 + 即可。
     * Bluetti/BMS 温度常见为 raw - 40，所以这里默认采用减补偿。
     */
    return ((double)raw_value) * item->precision - item->compensation;
}

int64_t CanJson_PhysicalToRaw(const CanProtocolItem *item, double physical_value)
{
    if (!item || item->precision == 0.0)
    {
        return 0;
    }

    return (int64_t)llround((physical_value + item->compensation) / item->precision);
}

static void fill_start_len(uint8_t data[8], const CanProtocolItem *item)
{
    memset(data, 0, 8);
    data[0] = (uint8_t)(item->data_type & 0xFF);
    data[1] = (uint8_t)(item->start & 0xFF);
    data[2] = (uint8_t)((item->start >> 8) & 0xFF);
    data[3] = (uint8_t)(item->len & 0xFF);
    data[4] = (uint8_t)((item->len >> 8) & 0xFF);
}

esp_err_t CanProtocol_BuildReadStart(const CanProtocolItem *item, uint8_t target_addr, can_message_ext_t *out_msg)
{
    if (!item || !out_msg)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->ext_id.id = can_ext_create_id(PRIORITY_NORMAL, EDP, DP,
                                           READ_START, target_addr, ADDR_MASTER);
    out_msg->data_len = 8;
    out_msg->is_remote = false;
    fill_start_len(out_msg->data, item);

    return ESP_OK;
}

esp_err_t CanProtocol_BuildWriteStart(const CanProtocolItem *item,
                                      uint8_t target_addr,
                                      can_message_ext_t *out_msg)
{
    if (!item || !out_msg)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->ext_id.id = can_ext_create_id(PRIORITY_HIGH, EDP, DP,
                                           WRITE_START, target_addr, ADDR_MASTER);
    out_msg->data_len = 8;
    out_msg->is_remote = false;
    fill_start_len(out_msg->data, item);

    return ESP_OK;
}

static bool encode_numeric_payload(const CanProtocolItem *item, const char *param, uint8_t *payload, size_t payload_size)
{
    if (!item || !param || !payload || payload_size < item->len)
    {
        return false;
    }

    int64_t raw = CanJson_PhysicalToRaw(item, atof(param));

    memset(payload, 0, payload_size);
    for (uint16_t i = 0; i < item->len; i++)
    {
        payload[i] = (uint8_t)((raw >> (8 * i)) & 0xFF);
    }

    return true;
}

esp_err_t CanProtocol_BuildWriteData(const CanProtocolItem *item,
                                     uint8_t target_addr,
                                     const char *param,
                                     can_message_ext_t *out_msg)
{
    if (!item || !param || !out_msg)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[6] = {0};
    uint8_t payload_len = 0;

    if (item->value_type == CAN_ITEM_TYPE_STRING)
    {
        size_t str_len = strlen(param);
        if (str_len > sizeof(payload))
        {
            str_len = sizeof(payload);
        }
        memcpy(payload, param, str_len);
        payload_len = (uint8_t)str_len;
    }
    else
    {
        if (item->len > sizeof(payload))
        {
            ESP_LOGE(TAG, "one WRITE_DATA frame only supports len <= 6, item=%s len=%u", item->name, item->len);
            return ESP_ERR_INVALID_SIZE;
        }
        if (!encode_numeric_payload(item, param, payload, sizeof(payload)))
        {
            return ESP_FAIL;
        }
        payload_len = (uint8_t)item->len;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->ext_id.id = can_ext_create_id(PRIORITY_HIGH, EDP, DP,
                                           WRITE_DATA, target_addr, ADDR_MASTER);
    out_msg->is_remote = false;

    /*
     * 与你现有 READ_DATA_LOAD 解析保持一致：data[2..7] 是有效负载。
     * data[0] 作为片序号，data[1] 作为本帧负载长度。
     */
    out_msg->data[0] = 0;
    out_msg->data[1] = payload_len;
    memcpy(&out_msg->data[2], payload, payload_len);
    out_msg->data_len = (uint8_t)(2 + payload_len);

    return ESP_OK;
}

static int64_t sign_extend(int64_t raw, uint16_t byte_len)
{
    if (byte_len == 0 || byte_len >= 8)
    {
        return raw;
    }

    int bits = byte_len * 8;
    int64_t sign_bit = (int64_t)1 << (bits - 1);
    int64_t mask = ((int64_t)1 << bits) - 1;
    raw &= mask;

    if (raw & sign_bit)
    {
        raw |= ~mask;
    }

    return raw;
}

bool CanProtocol_DecodeNumValue(const CanProtocolItem *item,
                                const uint8_t *payload,
                                size_t payload_len,
                                double *out_value)
{
    if (!item || !payload || !out_value)
    {
        return false;
    }

    if (item->value_type != CAN_ITEM_TYPE_NUM)
    {
        ESP_LOGW(TAG, "item is not NUM: %s", item->name);
        return false;
    }

    if (item->len == 0 || item->len > 8 || payload_len < item->len)
    {
        ESP_LOGE(TAG, "invalid payload len, item=%s item_len=%u payload_len=%u",
                 item->name, item->len, (unsigned int)payload_len);
        return false;
    }

    int64_t raw = 0;
    for (uint16_t i = 0; i < item->len; i++)
    {
        raw |= ((int64_t)payload[i]) << (8 * i);
    }

    if (!item->is_unsigned)
    {
        raw = sign_extend(raw, item->len);
    }

    *out_value = CanJson_RawToPhysical(item, raw);
    return true;
}
