#include "spiffs_config.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

static const char *TAG = "spiffs_config";
static bool spiffs_initialized = false;

esp_vfs_spiffs_conf_t current_conf = {
    .base_path = SPIFFS_BASE_PATH,
    .partition_label = "storage",
    .max_files = 10,
    .format_if_mount_failed = true,
};

static esp_err_t build_full_path(const char *filename, char *full_path, size_t full_path_size)
{
    if (filename == NULL || full_path == NULL || full_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (filename[0] == '/') {
        ESP_LOGE(TAG, "filename must be relative path, not absolute path: %s", filename);
        return ESP_ERR_INVALID_ARG;
    }

    int ret = snprintf(full_path, full_path_size, "%s/%s", current_conf.base_path, filename);
    if (ret < 0 || (size_t)ret >= full_path_size) {
        ESP_LOGE(TAG, "path too long: %s/%s", current_conf.base_path, filename);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t spiffs_init(void)
{
    if (spiffs_initialized) {
        ESP_LOGW(TAG, "SPIFFS already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_err_t ret = esp_vfs_spiffs_register(&current_conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info(current_conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS initialized: total=%u KB, used=%u KB",
                 (unsigned int)(total / 1024),
                 (unsigned int)(used / 1024));
    }

    spiffs_initialized = true;
    return ESP_OK;
}

esp_err_t spiffs_deinit(void)
{
    if (!spiffs_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Unmounting SPIFFS");

    esp_err_t ret = esp_vfs_spiffs_unregister(current_conf.partition_label);
    if (ret == ESP_OK) {
        spiffs_initialized = false;
    }

    return ret;
}

esp_err_t spiffs_file_write(const char *filename, const void *data, size_t len)
{
    if (!spiffs_initialized) {
        ESP_LOGE(TAG, "SPIFFS not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (filename == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[SPIFFS_PATH_MAX_LEN];
    esp_err_t ret = build_full_path(filename, full_path, sizeof(full_path));
    if (ret != ESP_OK) {
        return ret;
    }

    errno = 0;
    FILE *f = fopen(full_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s errno=%d", full_path, errno);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, len, f);
    fflush(f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "File write incomplete: %s written=%u expected=%u",
                 filename,
                 (unsigned int)written,
                 (unsigned int)len);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t spiffs_file_read(const char *filename, void *buffer, size_t *len, size_t max_len)
{
    if (!spiffs_initialized) {
        ESP_LOGE(TAG, "SPIFFS not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (filename == NULL || buffer == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[SPIFFS_PATH_MAX_LEN];
    esp_err_t ret = build_full_path(filename, full_path, sizeof(full_path));
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *f = fopen(full_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", full_path);
        return ESP_FAIL;
    }

    size_t read_len = fread(buffer, 1, max_len, f);
    fclose(f);

    if (len != NULL) {
        *len = read_len;
    }

    return ESP_OK;
}

esp_err_t spiffs_file_delete(const char *filename)
{
    if (!spiffs_initialized) {
        ESP_LOGE(TAG, "SPIFFS not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (filename == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[SPIFFS_PATH_MAX_LEN];
    esp_err_t ret = build_full_path(filename, full_path, sizeof(full_path));
    if (ret != ESP_OK) {
        return ret;
    }

    if (unlink(full_path) == 0) {
        ESP_LOGI(TAG, "Deleted file: %s", filename);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Failed to delete file: %s", filename);
    return ESP_FAIL;
}

bool spiffs_file_exists(const char *filename)
{
    if (!spiffs_initialized || filename == NULL) {
        return false;
    }

    char full_path[SPIFFS_PATH_MAX_LEN];
    if (build_full_path(filename, full_path, sizeof(full_path)) != ESP_OK) {
        return false;
    }

    struct stat st;
    return (stat(full_path, &st) == 0);
}

esp_err_t save_json_to_spiffs(const char *filename, const char *json_str)
{
    if (!spiffs_initialized) {
        ESP_LOGE(TAG, "SPIFFS not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (filename == NULL || json_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = spiffs_file_write(filename, json_str, strlen(json_str));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save JSON: %s", filename);
        return ret;
    }

    ESP_LOGI(TAG, "JSON saved: %s", filename);
    return ESP_OK;
}

char *read_json_from_spiffs(const char *filename)
{
    if (!spiffs_initialized) {
        ESP_LOGE(TAG, "SPIFFS not initialized");
        return NULL;
    }

    if (filename == NULL) {
        return NULL;
    }

    char full_path[SPIFFS_PATH_MAX_LEN];
    if (build_full_path(filename, full_path, sizeof(full_path)) != ESP_OK) {
        return NULL;
    }

    FILE *f = fopen(full_path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "JSON file not found: %s", full_path);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long file_len = ftell(f);
    if (file_len <= 0) {
        fclose(f);
        ESP_LOGW(TAG, "JSON file is empty: %s", filename);
        return NULL;
    }

    rewind(f);

    char *buffer = malloc((size_t)file_len + 1);
    if (buffer == NULL) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to allocate JSON buffer, len=%ld", file_len);
        return NULL;
    }

    size_t read_len = fread(buffer, 1, (size_t)file_len, f);
    fclose(f);

    if (read_len != (size_t)file_len) {
        ESP_LOGE(TAG, "Failed to read complete JSON file: %s", filename);
        free(buffer);
        return NULL;
    }

    buffer[file_len] = '\0';
    return buffer;
}

esp_err_t spiffs_list_files(void)
{
    if (!spiffs_initialized) {
        ESP_LOGE(TAG, "SPIFFS not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(current_conf.base_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open SPIFFS directory: %s", current_conf.base_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "========== SPIFFS file list ==========");

    struct dirent *entry = NULL;
    int file_count = 0;

    while ((entry = readdir(dir)) != NULL) {
        char full_path[SPIFFS_PATH_MAX_LEN];
        struct stat file_stat;

        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", current_conf.base_path, entry->d_name);
        if (ret < 0 || (size_t)ret >= sizeof(full_path)) {
            ESP_LOGW(TAG, "Path too long, skip: %s", entry->d_name);
            continue;
        }

        if (stat(full_path, &file_stat) == 0) {
            ESP_LOGI(TAG, "File: %s, Size: %ld bytes", entry->d_name, (long)file_stat.st_size);
        } else {
            ESP_LOGW(TAG, "File: %s, stat failed", entry->d_name);
        }

        file_count++;
    }

    closedir(dir);

    ESP_LOGI(TAG, "Total files: %d", file_count);
    ESP_LOGI(TAG, "=====================================");

    return ESP_OK;
}

esp_err_t spiffs_get_file_names(char file_names[][SPIFFS_FILE_NAME_MAX_LEN],
                                size_t max_files,
                                size_t *file_count)
{
    if (!spiffs_initialized) {
        ESP_LOGE(TAG, "SPIFFS not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (file_names == NULL || file_count == NULL || max_files == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *file_count = 0;

    DIR *dir = opendir(current_conf.base_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open SPIFFS directory: %s", current_conf.base_path);
        return ESP_FAIL;
    }

    struct dirent *entry = NULL;

    while ((entry = readdir(dir)) != NULL) {
        if (*file_count >= max_files) {
            ESP_LOGW(TAG, "File name buffer full, max_files=%d", (int)max_files);
            break;
        }

        size_t name_len = strlen(entry->d_name);
        if (name_len >= SPIFFS_FILE_NAME_MAX_LEN) {
            ESP_LOGW(TAG, "File name too long, skip: %s", entry->d_name);
            continue;
        }

        memcpy(file_names[*file_count], entry->d_name, name_len + 1);
        (*file_count)++;
    }

    closedir(dir);
    return ESP_OK;
}

esp_err_t spiffs_delete_all_files(void)
{
    if (!spiffs_initialized) {
        ESP_LOGE(TAG, "SPIFFS not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(current_conf.base_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open SPIFFS directory: %s", current_conf.base_path);
        return ESP_FAIL;
    }

    struct dirent *entry = NULL;
    int deleted_count = 0;
    int failed_count = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[SPIFFS_PATH_MAX_LEN];

        int ret = snprintf(full_path,
                           sizeof(full_path),
                           "%s/%s",
                           current_conf.base_path,
                           entry->d_name);

        if (ret < 0 || (size_t)ret >= sizeof(full_path)) {
            ESP_LOGW(TAG, "Path too long, skip: %s", entry->d_name);
            failed_count++;
            continue;
        }

        struct stat file_stat;
        if (stat(full_path, &file_stat) != 0) {
            ESP_LOGW(TAG, "stat failed, skip: %s errno=%d", full_path, errno);
            failed_count++;
            continue;
        }

        if (S_ISDIR(file_stat.st_mode)) {
            ESP_LOGW(TAG, "Skip directory: %s", full_path);
            continue;
        }

        if (unlink(full_path) == 0) {
            ESP_LOGI(TAG, "Deleted file: %s", entry->d_name);
            deleted_count++;
        } else {
            ESP_LOGW(TAG, "Failed to delete file: %s errno=%d", full_path, errno);
            failed_count++;
        }
    }

    closedir(dir);

    ESP_LOGI(TAG, "Delete all SPIFFS files finished, deleted=%d, failed=%d",
             deleted_count,
             failed_count);

    return (failed_count == 0) ? ESP_OK : ESP_FAIL;
}

