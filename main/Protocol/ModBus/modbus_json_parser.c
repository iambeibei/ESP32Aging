#include "modbus_json_parser.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>


static const char *TAG = "modbus_json_parser";


static void *mb_calloc_prefer_psram(size_t n, size_t size)
{
    if (n == 0 || size == 0) {
        return NULL;
    }

    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);//
    if (p == NULL) {
        p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
    }
    return p;
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;

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
        return item->valuestring;

    return NULL;
}

static double json_get_double(cJSON *obj, const char *key, double default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsNumber(item))
        return item->valuedouble;

    if (cJSON_IsString(item) && item->valuestring)
        return atof(item->valuestring);

    return default_value;
}

static int json_get_int(cJSON *obj, const char *key, int default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsNumber(item))
        return item->valueint;

    if (cJSON_IsString(item) && item->valuestring)
        return atoi(item->valuestring);

    return default_value;
}

static ProtocolItemRW parse_rw(const char *rw)
{
    if (!rw)
        return ITEM_RW_UNKNOWN;

    if (strcmp(rw, "R") == 0)
        return ITEM_RW_READ;

    if (strcmp(rw, "W") == 0)
        return ITEM_RW_WRITE;

    if (strcmp(rw, "RW") == 0 || strcmp(rw, "R/W") == 0)
        return ITEM_RW_READ_WRITE;

    return ITEM_RW_UNKNOWN;
}

bool ModbusJson_Parse(const char *json_text, ProtocolInfo *out_info)
{
    if (!json_text || !out_info)
        return false;

    memset(out_info, 0, sizeof(ProtocolInfo));

    cJSON *root = cJSON_Parse(json_text);
    if (!root)
    {
        printf("JSON parse failed: %s\n", cJSON_GetErrorPtr());
        return false;
    }

    copy_string(out_info->id, sizeof(out_info->id), json_get_string(root, "id"));
    copy_string(out_info->uuid, sizeof(out_info->uuid), json_get_string(root, "uuid"));
    copy_string(out_info->name, sizeof(out_info->name), json_get_string(root, "name"));
    copy_string(out_info->description, sizeof(out_info->description), json_get_string(root, "description"));
    copy_string(out_info->protocol_type_str, sizeof(out_info->protocol_type_str), json_get_string(root, "protocolTypeStr"));

    out_info->protocol_type = (ProtocolType)json_get_int(root, "protocolType", PROTOCOL_TYPE_UNKNOWN);

    cJSON *contents = cJSON_GetObjectItemCaseSensitive(root, "contents");
    if (!cJSON_IsArray(contents))
    {
        printf("JSON field 'contents' is not array\n");
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(contents);
    if (count <= 0)
    {
        out_info->items = NULL;
        out_info->item_count = 0;
        cJSON_Delete(root);
        return true;
    }

    out_info->items = (ProtocolItem *)mb_calloc_prefer_psram((size_t)count, sizeof(ProtocolItem));
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
            continue;

        ProtocolItem *item = &out_info->items[i];

        copy_string(item->id, sizeof(item->id), json_get_string(node, "id"));
        copy_string(item->name, sizeof(item->name), json_get_string(node, "名称"));
        copy_string(item->rw_str, sizeof(item->rw_str), json_get_string(node, "读写状态"));
        copy_string(item->unit, sizeof(item->unit), json_get_string(node, "单位"));
        copy_string(item->data_type, sizeof(item->data_type), json_get_string(node, "类型"));

        item->rw = parse_rw(item->rw_str);

        item->reg_addr = (uint16_t)json_get_int(node, "寄存器地址", 0);
        item->reg_len = (uint16_t)json_get_int(node, "长度", 1);

        item->precision = json_get_double(node, "精度", 1.0);
        item->compensation = json_get_double(node, "补偿", 0.0);

        item->is_unsigned = (uint8_t)json_get_int(node, "无符号", 1);

        if (item->reg_len == 0)
            item->reg_len = 1;

        if (item->precision == 0)
            item->precision = 1.0;
    }

    cJSON_Delete(root);
    return true;
}

void ModbusJson_Free(ProtocolInfo *info)
{
    if (!info)
        return;

    if (info->items)
    {
        free(info->items);
        info->items = NULL;
    }

    info->item_count = 0;
}

void ModbusJson_Print(const ProtocolInfo *info)
{
    if (!info)
        return;

    printf("Protocol Name: %s\n", info->name);
    printf("Protocol Type: %d, %s\n", info->protocol_type, info->protocol_type_str);
    printf("Description  : %s\n", info->description);
    printf("Item Count   : %u\n", (unsigned int)info->item_count);

    for (size_t i = 0; i < info->item_count; i++)
    {
        const ProtocolItem *item = &info->items[i];

        printf("[%u] name=%s, rw=%s, unit=%s, addr=%u, len=%u, type=%s, precision=%.6f, unsigned=%u, compensation=%.6f\n",
               (unsigned int)i,
               item->name,
               item->rw_str,
               item->unit,
               item->reg_addr,
               item->reg_len,
               item->data_type,
               item->precision,
               item->is_unsigned,
               item->compensation);
    }
}

const ProtocolItem *ModbusJson_FindItemByName(const ProtocolInfo *info, const char *name)
{
    if (!info || !name)
        return NULL;

    for (size_t i = 0; i < info->item_count; i++)
    {
        if (strcmp(info->items[i].name, name) == 0)
            return &info->items[i];
    }

    return NULL;
}

const ProtocolItem *ModbusJson_FindItemByID(const ProtocolInfo *info, const char *id)
{
    if (!info || !id)
        return NULL;

    for (size_t i = 0; i < info->item_count; i++)
    {
        if (strcmp(info->items[i].id, id) == 0)
            return &info->items[i];
    }

    return NULL;
}

const ProtocolItem *ModbusJson_FindItemByRegAddr(const ProtocolInfo *info, uint16_t reg_addr)
{
    if (!info)
        return NULL;

    for (size_t i = 0; i < info->item_count; i++)
    {
        if (info->items[i].reg_addr == reg_addr)
            return &info->items[i];
    }

    return NULL;
}

double ModbusJson_RawToPhysical(const ProtocolItem *item, int32_t raw_value)
{
    if (!item)
        return 0.0;

    return ((double)raw_value) * item->precision + item->compensation;
}

int32_t ModbusJson_PhysicalToRaw(const ProtocolItem *item, double physical_value)
{
    if (!item)
        return 0;

    return (int32_t)llround((physical_value - item->compensation) / item->precision);
}

/**
 * @brief 通过 Modbus 03 功能码生成读取指定协议项的数据帧
 * 
 * @param modbusprotocol Modbus 协议配置信息
 * @param item_name 要读取的协议项名称（如 "PV充电总功率"）
 * @param slave_addr 从机地址
 * @param out_frame 输出的 Modbus 帧（需要外部释放）
 * @return true 生成成功
 * @return false 生成失败（找不到协议项或生成帧失败）
 */
bool Modbus_Generate03_GetFrame(ProtocolInfo *modbusprotocol, const char *item_name, uint8_t slave_addr,ModbusFrame *out_frame)
{
    if (modbusprotocol == NULL || item_name == NULL || out_frame == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return false;
    }
    
    // 查找协议项
    const ProtocolItem *item = ModbusJson_FindItemByName(modbusprotocol, item_name);
    if (item == NULL) {
        ESP_LOGE(TAG, "Protocol item not found: %s", item_name);
        return false;
    }
    
    // 检查读写权限
    if (item->rw != ITEM_RW_READ && item->rw != ITEM_RW_READ_WRITE) {
        ESP_LOGE(TAG, "Item [%s] is not readable", item_name);
        return false;
    }
    
    // 生成 Modbus 03 读取帧
    if (!Modbus_Generate03(slave_addr, item->reg_addr, item->reg_len, out_frame)) {
        ESP_LOGE(TAG, "Generate Modbus 03 failed for item: %s", item_name);
        return false;
    }
    
    return true;
}