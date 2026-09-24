#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "SelfRecovery.h"

static const char *TAG = "LogStorage";

static const char *BASE_PATH = "/log";

static const char *DATA_FILE = "/log/records.dat";

static uint16_t s_current_index = 0; // 当前要写入的位置

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static SemaphoreHandle_t s_log_mutex = NULL;

void Get_Index_From_Flash(uint16_t index)
{
    if (s_log_mutex != NULL)
    {
        xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    }

    s_current_index = (index < MAX_RECORDS) ? index : 0;

    if (s_log_mutex != NULL)
    {
        xSemaphoreGive(s_log_mutex);
    }
}

// 辅助函数：通过索引计算文件偏移
uint16_t log_get_offset(uint16_t index)
{
    return index * RECORD_SIZE;
}

esp_err_t log_storage_mount(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 1,
        .format_if_mount_failed = true};

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(BASE_PATH, LOG_PARTITION_LABEL,
                                               &mount_config, &s_wl_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Mounted successfully");

    if (s_log_mutex == NULL)
    {
        s_log_mutex = xSemaphoreCreateMutex();
        if (s_log_mutex == NULL)
        {
            ESP_LOGE(TAG, "Failed to create log mutex");
            esp_vfs_fat_spiflash_unmount_rw_wl(BASE_PATH, s_wl_handle);
            s_wl_handle = WL_INVALID_HANDLE;
            return ESP_ERR_NO_MEM;
        }
    }

    // 检查文件是否存在，不存在则创建
    FILE *f = fopen(DATA_FILE, "rb");
    if (f == NULL)
    {
        ESP_LOGI(TAG, "Creating new data file");
        f = fopen(DATA_FILE, "wb");
        if (f == NULL)
        {
            ESP_LOGE(TAG, "Failed to create file");
            return ESP_FAIL;
        }
        fclose(f);
    }
    else
    {
        fclose(f);
    }
    return ESP_OK;
}

void storage_unmount(void)
{
    if (s_wl_handle != WL_INVALID_HANDLE)
    {
        esp_vfs_fat_spiflash_unmount_rw_wl(BASE_PATH, s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
    }

    if (s_log_mutex != NULL)
    {
        vSemaphoreDelete(s_log_mutex);
        s_log_mutex = NULL;
    }
}

esp_err_t storage_write_record(uint16_t index, const char *str)
{
    if (index >= MAX_RECORDS)
    {
        ESP_LOGE(TAG, "Index %d out of range", index);
        return ESP_ERR_INVALID_ARG;
    }

    if (str == NULL)
    {
        ESP_LOGE(TAG, "String pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // 准备要写入的缓冲区，固定长度 RECORD_SIZE
    char buffer[RECORD_SIZE];
    memset(buffer, 0, RECORD_SIZE);

    // 复制字符串到缓冲区，限制长度
    strncpy(buffer, str, MAX_STR_LEN);
    buffer[MAX_STR_LEN] = '\0'; // 确保结束符

    FILE *f = fopen(DATA_FILE, "rb+"); //
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file");
        return ESP_FAIL;
    }

    fseek(f, log_get_offset(index), SEEK_SET);
    size_t written = fwrite(buffer, 1, RECORD_SIZE, f);
    fclose(f);

    if (written != RECORD_SIZE)
    {
        ESP_LOGE(TAG, "Write failed: wrote %d bytes", written);
        return ESP_FAIL;
    }
    esp_err_t ret = SelfRecovery_Write_uint16("LogIndex", index);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Write LogIndex failed");
    }
    return ESP_OK;
}

esp_err_t storage_read_record(uint16_t index, char *buffer, size_t buffer_size)
{
    if (index >= MAX_RECORDS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (buffer == NULL || buffer_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(DATA_FILE, "rb");
    if (f == NULL)
    {
        return ESP_FAIL;
    }

    // 临时读取固定长度
    char temp[RECORD_SIZE];
    fseek(f, log_get_offset(index), SEEK_SET);
    size_t read = fread(temp, 1, RECORD_SIZE, f);
    fclose(f);

    if (read != RECORD_SIZE)
    {
        return ESP_FAIL;
    }

    // 复制字符串到用户缓冲区（遇到空字符自动停止）
    strncpy(buffer, temp, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';

    return ESP_OK;
}

// 添加辅助函数：获取当前索引并递增
static inline uint16_t get_next_index(void)
{
    uint16_t index = s_current_index;
    s_current_index++;
    if (s_current_index >= MAX_RECORDS)
    {
        s_current_index = 0; // 循环回开头
    }
    return index;
}

// 添加新的循环写入函数
esp_err_t storage_write_record_cyclic(const char *pn, const char *str)
{
    if (str == NULL)
    {
        ESP_LOGW(TAG, "String pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // if (pn[0] == '\0' || strlen(pn) == 0)
    // {
    //     sprintf(pn, "%s", "SN");
    // }

    if (s_log_mutex != NULL && xSemaphoreTake(s_log_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    // 获取当前写入索引并自动递增
    uint16_t index = get_next_index();

    time_t now = time(NULL);
    char buffer[RECORD_SIZE];
    memset(buffer, 0, sizeof(buffer));

    /*
     * 单条本地日志固定为 RECORD_SIZE 字节。使用 snprintf 安全截断，
     * 防止较长 PN 与详细错误信息组合后造成栈缓冲区溢出。
     */
    snprintf(buffer,
             sizeof(buffer),
             "%lld,%s,%s",
             (long long)now,
             pn != NULL ? pn : "",
             str);

    esp_err_t ret = storage_write_record(index, buffer);

    if (s_log_mutex != NULL)
    {
        xSemaphoreGive(s_log_mutex);
    }

    return ret;
}

// 获取当前写入索引
uint16_t storage_get_current_index(void)
{
    uint16_t index;
    if (s_log_mutex != NULL)
    {
        xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    }
    index = s_current_index;
    if (s_log_mutex != NULL)
    {
        xSemaphoreGive(s_log_mutex);
    }
    return index;
}

// 重置循环写入位置
void storage_reset_cyclic_position(void)
{
    if (s_log_mutex != NULL)
    {
        xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    }
    s_current_index = 0;
    if (s_log_mutex != NULL)
    {
        xSemaphoreGive(s_log_mutex);
    }
    ESP_LOGI(TAG, "Cyclic position reset to 0");
}

/// @brief 打印FatFS的使用情况
void print_logfatfs_usage(void)
{
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;

    esp_err_t ret = esp_vfs_fat_info(BASE_PATH, &total_bytes, &free_bytes);

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        LOG_PARTITION_LABEL);

    printf("\n=== FATFS Partition Usage ===\n");

    if (partition != NULL)
    {
        printf("Partition Label:   %s\n", partition->label);
        printf("Partition Offset:  0x%08lX\n", (unsigned long)partition->address);
        printf("Partition Size:    %lu bytes (%.2f KB, %.2f MB)\n",
               (unsigned long)partition->size,
               partition->size / 1024.0,
               partition->size / (1024.0 * 1024.0));
        printf("----------------------------\n");
    }
    else
    {
        printf("Partition not found: label=%s, subtype=FAT\n", LOG_PARTITION_LABEL);
    }

    if (ret == ESP_OK)
    {
        uint64_t used_bytes = total_bytes - free_bytes;

        printf("File System Total: %llu bytes (%.2f KB, %.2f MB)\n",
               (unsigned long long)total_bytes,
               total_bytes / 1024.0,
               total_bytes / (1024.0 * 1024.0));

        printf("File System Used:  %llu bytes (%.2f KB, %.2f MB)\n",
               (unsigned long long)used_bytes,
               used_bytes / 1024.0,
               used_bytes / (1024.0 * 1024.0));

        printf("File System Free:  %llu bytes (%.2f KB, %.2f MB)\n",
               (unsigned long long)free_bytes,
               free_bytes / 1024.0,
               free_bytes / (1024.0 * 1024.0));

        if (total_bytes > 0)
        {
            printf("Usage Ratio:       %.2f%%\n",
                   (double)used_bytes / (double)total_bytes * 100.0);
        }
    }
    else
    {
        printf("esp_vfs_fat_info failed: %s\n", esp_err_to_name(ret));
    }
}
