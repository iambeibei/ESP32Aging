#include "ble_target.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

#define BLE_TARGET_PN_BYTES 64
#define BLE_TARGET_RECORD_VERSION 1

typedef struct {
    uint8_t version;
    uint8_t use_mac;
    uint8_t addr_type;
    uint8_t bda[6];
    char pn[BLE_TARGET_PN_BYTES];
} BleTargetRecord;

static int ble_target_hex(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

esp_err_t ble_target_parse(const cJSON *data, BleTarget *out)
{
    if (!cJSON_IsObject(data) || out == NULL) return ESP_ERR_INVALID_ARG;
    BleTarget parsed = {0};
    const cJSON *mac = cJSON_GetObjectItemCaseSensitive(data, "MAC");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(data, "AddrType");
    if (mac != NULL) {
        if (!cJSON_IsString(mac) || mac->valuestring == NULL || strlen(mac->valuestring) != 17)
            return ESP_ERR_INVALID_ARG;
        for (size_t i = 0; i < sizeof(parsed.bda); ++i) {
            int high = ble_target_hex(mac->valuestring[i * 3]);
            int low = ble_target_hex(mac->valuestring[i * 3 + 1]);
            if (high < 0 || low < 0 || (i < 5 && mac->valuestring[i * 3 + 2] != ':'))
                return ESP_ERR_INVALID_ARG;
            parsed.bda[i] = (uint8_t)((high << 4) | low);
        }
        parsed.use_mac = true;
    }
    if (type != NULL) {
        if (!parsed.use_mac || !cJSON_IsNumber(type) ||
            (type->valuedouble != 0 && type->valuedouble != 1)) return ESP_ERR_INVALID_ARG;
        parsed.addr_type = (uint8_t)type->valueint;
    }
    *out = parsed;
    return ESP_OK;
}

esp_err_t ble_target_save(bool ready, const char *pn, const BleTarget *target)
{
    if (pn == NULL || target == NULL || strlen(pn) >= BLE_TARGET_PN_BYTES || target->addr_type > 1)
        return ESP_ERR_INVALID_ARG;
    BleTargetRecord record = {0};
    record.version = BLE_TARGET_RECORD_VERSION;
    record.use_mac = target->use_mac;
    record.addr_type = target->addr_type;
    memcpy(record.bda, target->bda, sizeof(record.bda));
    memcpy(record.pn, pn, strlen(pn) + 1);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("ble_target", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, ready ? "ready" : "current", &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t ble_target_load(bool ready, const char *pn, BleTarget *out)
{
    if (pn == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    BleTargetRecord record = {0};
    size_t size = sizeof(record);
    esp_err_t err = nvs_open("ble_target", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(out, 0, sizeof(*out));
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    err = nvs_get_blob(handle, ready ? "ready" : "current", &record, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(out, 0, sizeof(*out));
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    if (size != sizeof(record) || record.version != BLE_TARGET_RECORD_VERSION ||
        record.use_mac > 1 || record.addr_type > 1 ||
        memchr(record.pn, '\0', sizeof(record.pn)) == NULL || strcmp(record.pn, pn) != 0)
    {
        ESP_LOGE("ble_target", "蓝牙目标恢复失败：%s 记录损坏或 PN 不匹配", ready ? "待启动" : "运行中");
        return ESP_ERR_INVALID_STATE;
    }
    BleTarget restored = {.use_mac = record.use_mac, .addr_type = record.addr_type};
    memcpy(restored.bda, record.bda, sizeof(restored.bda));
    *out = restored;
    return ESP_OK;
}
