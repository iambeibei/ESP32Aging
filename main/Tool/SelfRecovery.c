#include "SelfRecovery.h"
#include "esp_log.h"

#define TAG "SelfRecovery"

static nvs_handle_t my_handle = 0;

esp_err_t SelfRecovery_Init(void)
{
    esp_err_t err = nvs_open("SelfRecovery", NVS_READWRITE, &my_handle);
    if (err != ESP_OK)
    {
        my_handle = 0;
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "NVS initialized");
    }
    return err;
}

void SelfRecovery_Deinit(void)
{
    if (my_handle)
    {
        nvs_close(my_handle);
        my_handle = 0;
        ESP_LOGI(TAG, "NVS closed");
    }
}

esp_err_t SelfRecovery_Write_str(const char *key, const char *value)
{
    if (my_handle == 0)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = nvs_set_str(my_handle, key, value);
    if (err == ESP_OK)
    {
        err = nvs_commit(my_handle);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Written str: %s = %s", key, value);
        }
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Write str failed: %s", esp_err_to_name(err));
    }
    err = nvs_commit(my_handle);
    return err;
}

esp_err_t SelfRecovery_Read_str(const char *key, char *out_value, size_t max_len)
{
    if (my_handle == 0 || out_value == NULL || max_len == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }
    size_t len = max_len;
    esp_err_t err = nvs_get_str(my_handle, key, out_value, &len);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Read str: %s = %s", key, out_value);
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "Key '%s' not found", key);
    }
    else
    {
        ESP_LOGE(TAG, "Read str failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t SelfRecovery_Write_uint16(const char *key, uint16_t value)
{
    esp_err_t err = nvs_set_u16(my_handle, key, value);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Error (%s) writing to NVS!\n", esp_err_to_name(err));
        return err;
    }
    else
    {
        ESP_LOGI(TAG, "Done\n");
    }
    err = nvs_commit(my_handle);
    return err;
}

esp_err_t SelfRecovery_Read_uint16(const char *key, uint16_t *value)
{
    esp_err_t err = nvs_get_u16(my_handle, key, value);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Error (%s) reading from NVS!\n", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "Done\n");
    }
    return err;
}



esp_err_t SelfRecovery_Write_uint64(const char *key, uint64_t val)
{
    esp_err_t err = nvs_set_u64(my_handle, key, val);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Error (%s) writing to NVS!\n", esp_err_to_name(err));
    }
    else
    {
        //ESP_LOGI(TAG, "Done\n");
    }
    err = nvs_commit(my_handle);
    return err;
}

esp_err_t SelfRecovery_Read_uint64(const char *key, uint64_t *val)
{
    esp_err_t err = nvs_get_u64(my_handle, key, val);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Error (%s) reading from NVS!\n", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "Done\n");
    }
    return err;
}


// 老化结束，删除所有键
esp_err_t SelfRecovery_Erase_all(void)
{

    esp_err_t err = nvs_erase_all(my_handle);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "All keys in namespace 'storage' have been erased.\n");
    }
    else
    {
        ESP_LOGE(TAG, "Error (%s) erasing all keys!\n", esp_err_to_name(err));
    }

    // 3. 提交更改，确保删除操作写入 Flash
    err = nvs_commit(my_handle);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Changes committed.\n");
    }
    else
    {
        ESP_LOGE(TAG, "Error (%s) committing changes!\n", esp_err_to_name(err));
    }

    return err;
}

//直接将老化有效标志置为0
esp_err_t SelfRecovery_Erase_aging_valid(void)
{
    esp_err_t err = SelfRecovery_Write_uint16("aging_valid", 0);
    return err;
}
