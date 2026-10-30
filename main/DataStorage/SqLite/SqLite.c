#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "sqlite3.h"
#include "cJSON.h"
#include "SqLite.h"
#include "app_mem.h"
#include "sqllib1.h"
#include "SelfRecovery.h"

static const char *TAG = "SQLITE";

/*
 * mount 偶发失败重试：吸收 flash 读取延迟、wear-levelling 句柄竞争等
 * 瞬时失败。仅覆盖 mount 阶段——DB 文件级损坏是持久性的，重试无意义，
 * 直接进入重建流程（见 SqLite_Init）。
 */
#define SQLITE_MOUNT_RETRY_COUNT        3
#define SQLITE_MOUNT_RETRY_INTERVAL_MS  2000

/* 重置结果 NVS 标志值（SqLite_Init 写、Init_ByNetwork_Flag 读后上报并清除）。
 * key 字符串必须与 appTask.c 的 NVS_KEY_SQLITE_RESET 一致。 */
#define NVS_KEY_SQLITE_RESET "sqlite_reset_flag"

#define SQLITE_RESET_FLAG_MOUNT_FAIL   "sqlite_mount_fail"
#define SQLITE_RESET_FLAG_DB_REBUILT   "sqlite_db_rebuilt"
#define SQLITE_RESET_FLAG_REBUILD_FAIL "sqlite_rebuild_fail"

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static SemaphoreHandle_t s_sqlite_mutex = NULL;
static sqlite3 *s_db = NULL;
static bool s_db_ready = false;
/*
 * v7 新增 record_id 列的运行期探测结果：init 成功后为 true。
 * false（迁移失败但旧库保留）时查询不带该列，record_id 留空串，
 * 由补传路径按"旧记录隔离"处理，保证旧库仍可读可用。
 */
static bool s_record_id_column_ready = false;

/*
 * 每上电重置守卫：mount 重建分区与 DB 重建各只允许一次，防止"重置后再次
 * 失败→再重置"的循环（分区硬件级损坏时重启也无济于事，保持禁用更诚实）。
 * SqLite_Init 在 app_task_init 早期只被调用一次，本标志为防御性约束。
 */
static bool s_reset_done_this_boot = false;

char db1_name[96];
QueryResult *g_db1_result=NULL;

/* -------------------------------------------------------------------------- */
/* 基础工具                                                                    */
/* -------------------------------------------------------------------------- */

static int sqlite_now_seconds(void)
{
    time_t now = time(NULL);
    if (now > 0)
    {
        return (int)now;
    }
    return (int)(esp_timer_get_time() / 1000000LL);
}

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0)
    {
        return;
    }

    dst[0] = '\0';
    if (src == NULL)
    {
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static float scaled_int_to_float(int value)
{
    return ((float)value) / 100.0f;
}

static void fill_legacy_float_fields(QueryResult *result)
{
    if (result == NULL || result->value_data[0] == '\0' || result->value_data[0] == '[')
    {
        return;
    }

    /* 只读遍历，保留 strtok_r 跳过空项的语义，不为立即返回的新格式预留 2KiB 栈。 */
    const char *cursor = result->value_data;
    float values[4] = {0};
    int index = 0;

    while (*cursor != '\0' && index < 4)
    {
        cursor += strspn(cursor, ",");
        if (*cursor == '\0') break;
        const char *end = strchr(cursor, ',');
        if (end == NULL) end = cursor + strlen(cursor);
        const char *equal = memchr(cursor, '=', (size_t)(end - cursor));
        long number = strtol(equal != NULL ? equal + 1 : cursor, NULL, 10);
        int bounded = number > INT_MAX ? INT_MAX : number < INT_MIN ? INT_MIN : (int)number;
        values[index++] = scaled_int_to_float(bounded);
        cursor = end;
    }

    result->Equipmentvoltage = values[0];
    result->EquipmentCurrent = values[1];
    result->Testvoltage = values[2];
    result->TestCurrent = values[3];
}

static void fill_query_result_from_stmt(sqlite3_stmt *stmt, QueryResult *result)
{
    if (stmt == NULL || result == NULL)
    {
        return;
    }

    memset(result, 0, sizeof(QueryResult));

    /* 查询列顺序固定：seq_no, pn, current_step, timestamp, pushed, value_data, created_at */
    result->seq_no = sqlite3_column_int(stmt, 0);

    const unsigned char *pn = sqlite3_column_text(stmt, 1);
    safe_copy(result->PN, sizeof(result->PN), (const char *)pn);

    result->current_step = (short)sqlite3_column_int(stmt, 2);
    result->timestamp = sqlite3_column_int(stmt, 3);
    result->pushed = sqlite3_column_int(stmt, 4) ? true : false;

    const unsigned char *value_data = sqlite3_column_text(stmt, 5);
    safe_copy(result->value_data, sizeof(result->value_data), (const char *)value_data);

    result->created_at = sqlite3_column_int(stmt, 6);

    /* v7：可选第 8 列 record_id（7 列兼容模式或旧库时留空串）。
     * 列数守卫保证旧 SELECT 契约（7 列）不受影响。 */
    if (sqlite3_column_count(stmt) > 7)
    {
        const unsigned char *record_id = sqlite3_column_text(stmt, 7);
        safe_copy(result->record_id, sizeof(result->record_id), (const char *)record_id);
    }

    /* 兼容旧业务字段 */
    result->IDNUM = result->seq_no;
    fill_legacy_float_fields(result);
    (void)db_build_data_report_json(result, result->json_data, sizeof(result->json_data));
}

static bool sqlite_lock(void)
{
    if (s_sqlite_mutex == NULL)
    {
        ESP_LOGE(TAG, "SQLite mutex is not initialized");
        return false;
    }

    if (xSemaphoreTake(s_sqlite_mutex, pdMS_TO_TICKS(10000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "SQLite mutex timeout");
        return false;
    }

    return true;
}

static void sqlite_unlock(void)
{
    if (s_sqlite_mutex != NULL)
    {
        xSemaphoreGive(s_sqlite_mutex);
    }
}

static bool sqlite_ready_locked(void)
{
    if (!s_db_ready || s_db == NULL)
    {
        ESP_LOGE(TAG, "SQLite database is not ready");
        return false;
    }
    return true;
}

static void sqlite_log_file_state(void)
{
    struct stat st = {0};

    if (stat(FATFS_BASE_PATH, &st) == 0)
    {
        ESP_LOGI(TAG, "%s is accessible", FATFS_BASE_PATH);
    }
    else
    {
        ESP_LOGE(TAG, "%s is not accessible, errno=%d", FATFS_BASE_PATH, errno);
    }

    if (stat(DB_FILE_PATH, &st) == 0)
    {
        ESP_LOGI(TAG, "%s exists, size=%ld", DB_FILE_PATH, (long)st.st_size);
    }
    else
    {
        ESP_LOGW(TAG, "%s does not exist/access failed, errno=%d", DB_FILE_PATH, errno);
    }

    if (stat(DB_FILE_PATH "-journal", &st) == 0)
    {
        ESP_LOGW(TAG, "Hot/leftover journal present: %s-journal, size=%ld",
                 DB_FILE_PATH, (long)st.st_size);
    }
}

/* -------------------------------------------------------------------------- */
/* FATFS                                                                        */
/* -------------------------------------------------------------------------- */

/*
 * 判断 data_storage 是否仍是“出厂/擦除后的空白分区”。
 *
 * 只有整个分区全部为 0xFF 才返回 true。这样首次烧录后的空白分区可以
 * 安全地创建 FATFS；一旦分区曾经写入过任何数据，哪怕 FATFS 后续损坏，
 * 这里也会返回 false，从而禁止自动格式化，避免掉电恢复数据被误删。
 */
static bool fatfs_partition_is_fully_erased(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        FATFS_PARTITION_LABEL);

    if (partition == NULL)
    {
        ESP_LOGE(TAG, "FATFS partition '%s' not found", FATFS_PARTITION_LABEL);
        return false;
    }

    const size_t chunk_size = 4096;
    uint8_t *buffer = (uint8_t *)malloc(chunk_size);
    if (buffer == NULL)
    {
        ESP_LOGE(TAG, "No memory for FATFS blank-partition check");
        return false;
    }

    bool fully_erased = true;

    for (size_t offset = 0; offset < partition->size; offset += chunk_size)
    {
        size_t read_len = partition->size - offset;
        if (read_len > chunk_size)
        {
            read_len = chunk_size;
        }

        esp_err_t ret = esp_partition_read(partition, offset, buffer, read_len);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "Failed to inspect FATFS partition at offset=0x%lx: %s",
                     (unsigned long)offset,
                     esp_err_to_name(ret));
            fully_erased = false;
            break;
        }

        for (size_t i = 0; i < read_len; ++i)
        {
            if (buffer[i] != 0xFF)
            {
                fully_erased = false;
                break;
            }
        }

        if (!fully_erased)
        {
            break;
        }
    }

    free(buffer);

    if (fully_erased)
    {
        ESP_LOGW(TAG,
                 "FATFS partition '%s' is completely erased; first-time format is allowed",
                 FATFS_PARTITION_LABEL);
    }
    else
    {
        ESP_LOGW(TAG,
                 "FATFS partition '%s' contains existing data; automatic format is forbidden",
                 FATFS_PARTITION_LABEL);
    }

    return fully_erased;
}

esp_err_t mount_fatfs_storage(void)
{
    if (s_wl_handle != WL_INVALID_HANDLE)
    {
        return ESP_OK;
    }

    /*
     * 必须在第一次 mount 之前判断分区是否全空白。
     * wear-levelling 初始化本身可能写入元数据；如果等 mount 失败后再检查，
     * 一个原本全 0xFF 的首次使用分区可能已经不再表现为“全空白”。
     */
    const bool partition_was_blank = fatfs_partition_is_fully_erased();

    /*
     * 第一阶段：永远先保护性挂载，不允许自动格式化。
     * 正常运行、断电重启、已有数据库时都只走这一条路径。
     */
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 4096,
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(
        FATFS_BASE_PATH,
        FATFS_PARTITION_LABEL,
        &mount_config,
        &s_wl_handle);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "FATFS mounted at %s", FATFS_BASE_PATH);
        return ESP_OK;
    }

    s_wl_handle = WL_INVALID_HANDLE;

    /*
     * 第二阶段：仅首次使用、且确认整个物理分区仍为全 0xFF 时，
     * 才允许格式化一次。
     *
     * 不能仅凭 ESP_FAIL/FR_NO_FILESYSTEM 就格式化，因为损坏的旧 FATFS
     * 也可能返回同类挂载错误。是否允许格式化由“分区是否全空白”决定。
     */
    if (partition_was_blank)
    {
        ESP_LOGW(TAG,
                 "FATFS mount failed on a blank partition (%s); performing one-time initialization",
                 esp_err_to_name(ret));

        esp_vfs_fat_mount_config_t first_boot_config = {
            .format_if_mount_failed = true,
            .max_files = 8,
            .allocation_unit_size = 4096,
        };

        ret = esp_vfs_fat_spiflash_mount_rw_wl(
            FATFS_BASE_PATH,
            FATFS_PARTITION_LABEL,
            &first_boot_config,
            &s_wl_handle);

        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG,
                     "FATFS first-time format/mount completed successfully at %s",
                     FATFS_BASE_PATH);
            return ESP_OK;
        }

        s_wl_handle = WL_INVALID_HANDLE;
        ESP_LOGE(TAG,
                 "FATFS first-time initialization failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /*
     * 非空分区挂载失败：很可能存在旧数据或文件系统损坏。
     * 为了保证掉电恢复数据不被误删，绝不自动格式化。
     */
    ESP_LOGE(TAG,
             "Failed to mount FATFS: %s; partition contains existing data, so it will NOT be formatted",
             esp_err_to_name(ret));
    return ret;
}

void unmount_fatfs_storage(void)
{
    if (!sqlite_lock())
    {
        return;
    }

    s_db_ready = false;

    if (s_db != NULL)
    {
        int rc = sqlite3_close(s_db);
        if (rc != SQLITE_OK)
        {
            ESP_LOGE(TAG, "sqlite3_close failed before unmount: rc=%d; FATFS remains mounted", rc);
            sqlite_unlock();
            return;
        }
        s_db = NULL;
    }

    if (s_wl_handle != WL_INVALID_HANDLE)
    {
        esp_vfs_fat_spiflash_unmount_rw_wl(FATFS_BASE_PATH, s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
        ESP_LOGI(TAG, "FATFS unmounted");
    }

    sqlite_unlock();
}

/*
 * 强制重建分区（format_if_mount_failed=true）。
 *
 * 仅由 SqLite_Init 在 mount 重试耗尽后调用一次（每上电守卫保证）。
 * 语义：放弃"非空分区绝不格式化"的保护哲学——mount 持续失败说明文件
 * 系统级损坏、数据已不可读，保留分区只会让节点永久禁用 SQLite，
 * 丢失后续全部老化记录。用户明确决策接受该损失以恢复存储能力。
 *
 * 调用上下文：SqLite_Init（app_task_init 早期，任务上下文），不可重入。
 */
static esp_err_t mount_fatfs_storage_reset(void)
{
    esp_vfs_fat_mount_config_t reset_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(
        FATFS_BASE_PATH,
        FATFS_PARTITION_LABEL,
        &reset_config,
        &s_wl_handle);

    if (ret != ESP_OK)
    {
        s_wl_handle = WL_INVALID_HANDLE;
        ESP_LOGE(TAG, "FATFS reset mount (format) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGW(TAG,
             "FATFS partition was reformatted after persistent mount failure; "
             "all previous cache records are lost");
    return ESP_OK;
}

void print_fatfs_usage(void)
{
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;

    esp_err_t ret = esp_vfs_fat_info(FATFS_BASE_PATH, &total_bytes, &free_bytes);

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        FATFS_PARTITION_LABEL);

    printf("\n=== FATFS Partition Usage ===\n");

    if (partition != NULL)
    {
        printf("Partition Label:   %s\n", partition->label);
        printf("Partition Offset:  0x%08lX\n", (unsigned long)partition->address);
        printf("Partition Size:    %lu bytes (%.2f MB)\n",
               (unsigned long)partition->size,
               partition->size / (1024.0 * 1024.0));
    }

    if (ret == ESP_OK)
    {
        uint64_t used_bytes = total_bytes - free_bytes;
        printf("File System Total: %llu bytes\n", (unsigned long long)total_bytes);
        printf("File System Used:  %llu bytes\n", (unsigned long long)used_bytes);
        printf("File System Free:  %llu bytes\n", (unsigned long long)free_bytes);
    }
    else
    {
        printf("esp_vfs_fat_info failed: %s\n", esp_err_to_name(ret));
    }
}

void print_db_files(void)
{
    struct stat st = {0};
    if (stat(DB_FILE_PATH, &st) == 0)
    {
        printf("%s size: %ld bytes (%.2f KB)\n",
               DB_FILE_PATH,
               (long)st.st_size,
               st.st_size / 1024.0);
    }
    else
    {
        printf("%s not found, errno=%d\n", DB_FILE_PATH, errno);
    }
}

/*
 * 仅保留为人工调试/维修接口。
 * 正常初始化、quick_check失败、运行时错误都绝不会自动调用本函数。
 */
void cleanup_database_files(void)
{
    if (!sqlite_lock())
    {
        return;
    }

    s_db_ready = false;

    if (s_db != NULL)
    {
        (void)sqlite3_close(s_db);
        s_db = NULL;
    }

    unlink(DB_FILE_PATH);
    unlink(DB_FILE_PATH "-journal");
    unlink(DB_FILE_PATH "-wal");
    unlink(DB_FILE_PATH "-shm");

    ESP_LOGW(TAG, "SQLite database files were manually removed");
    sqlite_unlock();
}

/* -------------------------------------------------------------------------- */
/* SQLite初始化、完整性与schema                                                 */
/* -------------------------------------------------------------------------- */

static int apply_sqlite_pragmas(sqlite3 *db)
{
    if (db == NULL)
    {
        return SQLITE_MISUSE;
    }

    /*
     * DELETE journal + FULL synchronous：
     * 先把回滚日志持久化，再提交数据库页。突然断电后由 SQLite 自动恢复 hot journal。
     */
    if (db_exec(db, "PRAGMA journal_mode = DELETE;") != SQLITE_OK) return SQLITE_ERROR;
    if (db_exec(db, "PRAGMA synchronous = FULL;") != SQLITE_OK) return SQLITE_ERROR;
    if (db_exec(db, "PRAGMA temp_store = MEMORY;") != SQLITE_OK) return SQLITE_ERROR;
    if (db_exec(db, "PRAGMA locking_mode = NORMAL;") != SQLITE_OK) return SQLITE_ERROR;
    if (db_exec(db, "PRAGMA cache_size = -64;") != SQLITE_OK) return SQLITE_ERROR;
    if (db_exec(db, "PRAGMA auto_vacuum = NONE;") != SQLITE_OK) return SQLITE_ERROR;
    if (db_exec(db, "PRAGMA busy_timeout = 5000;") != SQLITE_OK) return SQLITE_ERROR;

    return SQLITE_OK;
}

static int sqlite_basic_check(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master;", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "SQLite basic check prepare failed: rc=%d, errmsg=%s",
                 rc, sqlite3_errmsg(db));
        return rc;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        return SQLITE_OK;
    }

    ESP_LOGE(TAG, "SQLite basic check failed: rc=%d, errmsg=%s",
             rc, sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    return rc;
}

/*
 * 某些嵌入式 SQLite 编译配置下 PRAGMA quick_check 可能不返回结果行而直接 DONE。
 * SQLITE_DONE(101) 是正常结束，不是 SQLITE_CORRUPT(11)。
 * 无结果行时退化为 sqlite_master 基础可读性检查。
 */
static int sqlite_quick_check(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA quick_check;", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "SQLite quick_check prepare failed: rc=%d, errmsg=%s",
                 rc, sqlite3_errmsg(db));
        return rc;
    }

    bool got_row = false;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        got_row = true;
        const unsigned char *text = sqlite3_column_text(stmt, 0);

        if (text == NULL || strcmp((const char *)text, "ok") != 0)
        {
            ESP_LOGE(TAG, "SQLite quick_check reported: %s",
                     text ? (const char *)text : "NULL");
            sqlite3_finalize(stmt);
            return SQLITE_CORRUPT;
        }
    }

    if (rc != SQLITE_DONE)
    {
        ESP_LOGE(TAG, "SQLite quick_check execution failed: rc=%d, errmsg=%s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return rc;
    }

    sqlite3_finalize(stmt);

    if (got_row)
    {
        ESP_LOGI(TAG, "SQLite quick_check OK");
        return SQLITE_OK;
    }

    ESP_LOGW(TAG, "SQLite quick_check returned no rows; using basic database check");
    return sqlite_basic_check(db);
}

static bool sqlite_table_exists(sqlite3 *db, const char *table_name)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        return false;
    }

    sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_TRANSIENT);
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

/*
 * IMPORTANT:
 * Do not use PRAGMA table_info() to probe columns on this ESP32 SQLite build.
 * Runtime testing showed several PRAGMA result sets are unreliable here:
 *   - PRAGMA quick_check may return SQLITE_DONE without a row
 *   - PRAGMA user_version may read back as 0 after reboot
 *   - PRAGMA table_info may likewise return no rows
 *
 * A normal SELECT is much more reliable: sqlite3_prepare_v2() resolves column
 * names against the schema even when LIMIT 0 means no data rows are read.
 */
static bool sqlite_table_has_column(sqlite3 *db, const char *table_name, const char *column_name)
{
    if (db == NULL || table_name == NULL || column_name == NULL)
    {
        return false;
    }

    char sql[192];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT %s FROM %s LIMIT 0;",
                     column_name, table_name);
    if (n <= 0 || n >= (int)sizeof(sql))
    {
        return false;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        ESP_LOGW(TAG,
                 "SQLite schema probe failed: table=%s column=%s rc=%d errmsg=%s",
                 table_name, column_name, rc, sqlite3_errmsg(db));
        if (stmt != NULL)
        {
            sqlite3_finalize(stmt);
        }
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

static bool sqlite_table_has_columns(sqlite3 *db,
                                     const char *table_name,
                                     const char *const *columns,
                                     size_t column_count)
{
    if (db == NULL || table_name == NULL || columns == NULL || column_count == 0)
    {
        return false;
    }

    for (size_t i = 0; i < column_count; ++i)
    {
        if (!sqlite_table_has_column(db, table_name, columns[i]))
        {
            return false;
        }
    }

    return true;
}

static void sqlite_log_table_definition(sqlite3 *db, const char *table_name)
{
    if (db == NULL || table_name == NULL)
    {
        return;
    }

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT sql FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;";

    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        ESP_LOGW(TAG, "Failed to prepare sqlite_master schema log: rc=%d", rc);
        return;
    }

    sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        const unsigned char *definition = sqlite3_column_text(stmt, 0);
        ESP_LOGI(TAG, "SQLite table definition [%s]: %s",
                 table_name,
                 definition != NULL ? (const char *)definition : "<NULL>");
    }
    else
    {
        ESP_LOGW(TAG, "SQLite table definition not found for %s, rc=%d",
                 table_name, rc);
    }

    sqlite3_finalize(stmt);
}

static int get_user_version(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    int version = 0;

    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL) != SQLITE_OK)
    {
        return 0;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        version = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return version;
}

static int create_schema_v7(sqlite3 *db)
{
    if (db == NULL)
    {
        return SQLITE_MISUSE;
    }

    /*
     * local_id 是数据库内部主键；IDNUM(seq_no)只在同一个PN内唯一。
     * 这修复了“第二台设备IDNUM重新从0开始时覆盖第一台设备记录”的问题。
     *
     * v7：唯一键加入 record_id（老化会话身份）。同一 PN 重新开轮时
     * RecordId 不同 → 写入新行，不再覆盖上一轮记录；补传 JSON 用
     * 记录自身 record_id，服务端可正确归属轮次。
     */
    int rc = db_exec(db,
                     "CREATE TABLE IF NOT EXISTS " SQLITE_CACHE_TABLE_NAME " ("
                     "local_id INTEGER PRIMARY KEY,"
                     "seq_no INTEGER NOT NULL,"
                     "pn TEXT NOT NULL CHECK(length(pn) < 64),"
                     "record_id TEXT NOT NULL DEFAULT '' CHECK(length(record_id) < 40),"
                     "current_step INTEGER NOT NULL,"
                     "timestamp INTEGER NOT NULL,"
                     "pushed INTEGER NOT NULL DEFAULT 0 CHECK(pushed IN (0,1,2)),"
                     "value_data TEXT NOT NULL CHECK(length(CAST(value_data AS BLOB)) <= 2048),"
                     "created_at INTEGER NOT NULL,"
                     "UNIQUE(pn, record_id, seq_no)"
                     ");");
    if (rc != SQLITE_OK) return rc;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_pn_seq_v6 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (pn, seq_no DESC);");
    if (rc != SQLITE_OK) return rc;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_pn_push_seq_v6 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (pn, pushed, seq_no);");
    if (rc != SQLITE_OK) return rc;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_push_local_v6 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (pushed, local_id);");
    if (rc != SQLITE_OK) return rc;

    return db_exec(db, "PRAGMA user_version = 7;");
}

static bool sqlite_schema_is_v7(sqlite3 *db)
{
    if (db == NULL || !sqlite_table_exists(db, SQLITE_CACHE_TABLE_NAME))
    {
        return false;
    }

    /*
     * 不依赖 PRAGMA user_version 判断真实表版本。
     * 当前 ESP32 SQLite 组件实测 user_version 在重启后可能仍返回 0，
     * 因此必须以实际列结构为准。
     */
    static const char *required_columns[] = {
        "local_id",
        "seq_no",
        "pn",
        "record_id",
        "current_step",
        "timestamp",
        "pushed",
        "value_data",
        "created_at",
    };

    return sqlite_table_has_columns(
        db,
        SQLITE_CACHE_TABLE_NAME,
        required_columns,
        sizeof(required_columns) / sizeof(required_columns[0]));
}

/* v6 基础列（不含 record_id）：用于识别"只差 record_id 一列"的存量 v6 库。 */
static bool sqlite_schema_is_v6_base(sqlite3 *db)
{
    if (db == NULL || !sqlite_table_exists(db, SQLITE_CACHE_TABLE_NAME))
    {
        return false;
    }

    static const char *base_columns[] = {
        "local_id",
        "seq_no",
        "pn",
        "current_step",
        "timestamp",
        "pushed",
        "value_data",
        "created_at",
    };

    return sqlite_table_has_columns(
        db,
        SQLITE_CACHE_TABLE_NAME,
        base_columns,
        sizeof(base_columns) / sizeof(base_columns[0]));
}

static int create_v7_temp_table(sqlite3 *db)
{
    if (db == NULL)
    {
        return SQLITE_MISUSE;
    }

    int rc = db_exec(db, "DROP TABLE IF EXISTS data_cache_v7_tmp;");
    if (rc != SQLITE_OK)
    {
        return rc;
    }

    return db_exec(db,
                   "CREATE TABLE data_cache_v7_tmp ("
                   "local_id INTEGER PRIMARY KEY,"
                   "seq_no INTEGER NOT NULL,"
                   "pn TEXT NOT NULL CHECK(length(pn) < 64),"
                   "record_id TEXT NOT NULL DEFAULT '' CHECK(length(record_id) < 40),"
                   "current_step INTEGER NOT NULL,"
                   "timestamp INTEGER NOT NULL,"
                   "pushed INTEGER NOT NULL DEFAULT 0 CHECK(pushed IN (0,1,2)),"
                   "value_data TEXT NOT NULL CHECK(length(CAST(value_data AS BLOB)) <= 2048),"
                   "created_at INTEGER NOT NULL,"
                   "UNIQUE(pn, record_id, seq_no)"
                   ");");
}

/*
 * v3/v4/v5 的老表都把电池包编号存放在 sn 列；v6 统一改名为 pn，v7 增加 record_id。
 * 不使用 ALTER TABLE RENAME：当前嵌入式 SQLite 组件实测
 * ALTER TABLE ... RENAME 可能返回 SQLITE_ERROR，
 * 因此改成事务内“临时表复制 -> 重建主表 -> 回填”。
 *
 * 注意：老表可能带 local_id（v5）也可能不带（v3/v4）。
 * local_id 只是数据库内部主键，重建时重新生成不影响业务数据。
 * 老记录没有 record_id，重建后留空串，由补传路径按旧记录隔离处理。
 */
static int migrate_legacy_schema_to_v7(sqlite3 *db)
{
    if (db == NULL)
    {
        return SQLITE_MISUSE;
    }

    /* 这些是“老表”的列名，必须保持 sn 不变。 */
    static const char *legacy_columns[] = {
        "seq_no",
        "sn",
        "current_step",
        "timestamp",
        "pushed",
        "value_data",
        "created_at",
    };

    for (size_t i = 0; i < sizeof(legacy_columns) / sizeof(legacy_columns[0]); ++i)
    {
        if (!sqlite_table_has_column(db, SQLITE_CACHE_TABLE_NAME, legacy_columns[i]))
        {
            ESP_LOGE(TAG,
                     "Legacy SQLite table is missing required column '%s'; database is preserved",
                     legacy_columns[i]);
            sqlite_log_table_definition(db, SQLITE_CACHE_TABLE_NAME);
            return SQLITE_SCHEMA;
        }
    }

    int rc = db_exec(db, "BEGIN IMMEDIATE TRANSACTION;");
    if (rc != SQLITE_OK)
    {
        return rc;
    }

    rc = create_v7_temp_table(db);
    if (rc != SQLITE_OK) goto rollback;

    /* 源列是老表的 sn，目标列是新表的 pn；record_id 取默认空串。 */
    rc = db_exec(db,
                 "INSERT OR REPLACE INTO data_cache_v7_tmp "
                 "(seq_no, pn, current_step, timestamp, pushed, value_data, created_at) "
                 "SELECT seq_no, sn, current_step, timestamp, pushed, value_data, created_at "
                 "FROM " SQLITE_CACHE_TABLE_NAME " "
                 "WHERE length(sn) < 64 "
                 "AND length(CAST(value_data AS BLOB)) <= 2048;");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db, "DROP TABLE " SQLITE_CACHE_TABLE_NAME ";");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "CREATE TABLE " SQLITE_CACHE_TABLE_NAME " ("
                 "local_id INTEGER PRIMARY KEY,"
                 "seq_no INTEGER NOT NULL,"
                 "pn TEXT NOT NULL CHECK(length(pn) < 64),"
                 "record_id TEXT NOT NULL DEFAULT '' CHECK(length(record_id) < 40),"
                 "current_step INTEGER NOT NULL,"
                 "timestamp INTEGER NOT NULL,"
                 "pushed INTEGER NOT NULL DEFAULT 0 CHECK(pushed IN (0,1,2)),"
                 "value_data TEXT NOT NULL CHECK(length(CAST(value_data AS BLOB)) <= 2048),"
                 "created_at INTEGER NOT NULL,"
                 "UNIQUE(pn, record_id, seq_no)"
                 ");");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "INSERT OR REPLACE INTO " SQLITE_CACHE_TABLE_NAME " "
                 "(seq_no, pn, current_step, timestamp, pushed, value_data, created_at) "
                 "SELECT seq_no, pn, current_step, timestamp, pushed, value_data, created_at "
                 "FROM data_cache_v7_tmp;");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db, "DROP TABLE data_cache_v7_tmp;");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_pn_seq_v6 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (pn, seq_no DESC);");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_pn_push_seq_v6 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (pn, pushed, seq_no);");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_push_local_v6 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (pushed, local_id);");
    if (rc != SQLITE_OK) goto rollback;

    /* 仅作为版本标记尝试写入；后续启动不依赖它判断 schema。 */
    (void)db_exec(db, "PRAGMA user_version = 7;");

    rc = db_exec(db, "COMMIT;");
    if (rc == SQLITE_OK)
    {
        ESP_LOGI(TAG, "SQLite legacy schema migrated to v7 (column sn renamed to pn, record_id added)");
    }
    return rc;

rollback:
    (void)db_exec(db, "ROLLBACK;");
    ESP_LOGE(TAG, "SQLite legacy schema migration to v7 failed: rc=%d, errmsg=%s",
             rc, sqlite3_errmsg(db));
    return rc;
}

/*
 * 存量 v6 库（结构完整、只差 record_id 列与轮次唯一键）升级到 v7。
 * 不能只用 ALTER TABLE ADD COLUMN：唯一键要从 UNIQUE(pn, seq_no) 改为
 * UNIQUE(pn, record_id, seq_no)，ADD COLUMN 无法修改约束，
 * 因此沿用“临时表复制 -> 重建主表 -> 回填”的事务模式。
 * 老记录 record_id 置空串；失败时 ROLLBACK，旧库结构与数据原样保留。
 */
static int migrate_v6_to_v7(sqlite3 *db)
{
    if (db == NULL)
    {
        return SQLITE_MISUSE;
    }

    int rc = db_exec(db, "BEGIN IMMEDIATE TRANSACTION;");
    if (rc != SQLITE_OK)
    {
        return rc;
    }

    rc = create_v7_temp_table(db);
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "INSERT OR REPLACE INTO data_cache_v7_tmp "
                 "(seq_no, pn, current_step, timestamp, pushed, value_data, created_at) "
                 "SELECT seq_no, pn, current_step, timestamp, pushed, value_data, created_at "
                 "FROM " SQLITE_CACHE_TABLE_NAME " "
                 "WHERE length(pn) < 64 "
                 "AND length(CAST(value_data AS BLOB)) <= 2048;");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db, "DROP TABLE " SQLITE_CACHE_TABLE_NAME ";");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "CREATE TABLE " SQLITE_CACHE_TABLE_NAME " ("
                 "local_id INTEGER PRIMARY KEY,"
                 "seq_no INTEGER NOT NULL,"
                 "pn TEXT NOT NULL CHECK(length(pn) < 64),"
                 "record_id TEXT NOT NULL DEFAULT '' CHECK(length(record_id) < 40),"
                 "current_step INTEGER NOT NULL,"
                 "timestamp INTEGER NOT NULL,"
                 "pushed INTEGER NOT NULL DEFAULT 0 CHECK(pushed IN (0,1,2)),"
                 "value_data TEXT NOT NULL CHECK(length(CAST(value_data AS BLOB)) <= 2048),"
                 "created_at INTEGER NOT NULL,"
                 "UNIQUE(pn, record_id, seq_no)"
                 ");");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "INSERT OR REPLACE INTO " SQLITE_CACHE_TABLE_NAME " "
                 "(seq_no, pn, current_step, timestamp, pushed, value_data, created_at) "
                 "SELECT seq_no, pn, current_step, timestamp, pushed, value_data, created_at "
                 "FROM data_cache_v7_tmp;");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db, "DROP TABLE data_cache_v7_tmp;");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_pn_seq_v6 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (pn, seq_no DESC);");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_pn_push_seq_v6 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (pn, pushed, seq_no);");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_push_local_v6 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (pushed, local_id);");
    if (rc != SQLITE_OK) goto rollback;

    (void)db_exec(db, "PRAGMA user_version = 7;");

    rc = db_exec(db, "COMMIT;");
    if (rc == SQLITE_OK)
    {
        ESP_LOGI(TAG, "SQLite schema migrated v6 -> v7 (record_id column and per-round unique key added)");
    }
    return rc;

rollback:
    (void)db_exec(db, "ROLLBACK;");
    ESP_LOGE(TAG, "SQLite schema migration v6 -> v7 failed: rc=%d, errmsg=%s",
             rc, sqlite3_errmsg(db));
    return rc;
}

static int ensure_schema_v7(sqlite3 *db, int reported_user_version)
{
    if (db == NULL)
    {
        return SQLITE_MISUSE;
    }

    if (!sqlite_table_exists(db, SQLITE_CACHE_TABLE_NAME))
    {
        ESP_LOGI(TAG, "SQLite data_cache table does not exist; creating v7 schema");
        return create_schema_v7(db);
    }

    if (sqlite_schema_is_v7(db))
    {
        /*
         * 真实结构已经是 v7。即使 PRAGMA user_version 返回 0 也绝不能迁移。
         * 只补齐索引并尝试重新写版本标记。
         */
        if (reported_user_version != SQLITE_SCHEMA_VERSION)
        {
            ESP_LOGW(TAG,
                     "SQLite table structure is already v7 but user_version=%d; "
                     "ignoring version mismatch and keeping existing data",
                     reported_user_version);
        }

        return create_schema_v7(db);
    }

    /*
     * 存量 v6 库：基础列齐全、仅缺 record_id 与轮次唯一键。
     * 走增量迁移补列，不动 legacy 的老表迁移路径。
     */
    if (sqlite_schema_is_v6_base(db) &&
        !sqlite_table_has_column(db, SQLITE_CACHE_TABLE_NAME, "record_id"))
    {
        ESP_LOGW(TAG,
                 "SQLite v6 table detected (user_version=%d); migrating to v7 "
                 "(add record_id, per-round unique key)",
                 reported_user_version);
        return migrate_v6_to_v7(db);
    }

    ESP_LOGW(TAG,
             "SQLite legacy table detected by schema probe (user_version=%d); migrating to v7",
             reported_user_version);

    return migrate_legacy_schema_to_v7(db);
}

static int get_record_count_locked(int *record_count)
{
    if (record_count == NULL || s_db == NULL)
    {
        return -1;
    }

    *record_count = 0;
    sqlite3_stmt *stmt = NULL;

    int rc = sqlite3_prepare_v2(s_db,
                                "SELECT COUNT(*) FROM " SQLITE_CACHE_TABLE_NAME ";",
                                -1,
                                &stmt,
                                NULL);
    if (rc != SQLITE_OK)
    {
        return -1;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        *record_count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    return -1;
}

static void print_sqlite_pragmas(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT "
        "(SELECT page_size FROM pragma_page_size), "
        "(SELECT page_count FROM pragma_page_count), "
        "(SELECT freelist_count FROM pragma_freelist_count);";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        return;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        ESP_LOGI(TAG, "page_size=%d page_count=%d freelist_count=%d",
                 sqlite3_column_int(stmt, 0),
                 sqlite3_column_int(stmt, 1),
                 sqlite3_column_int(stmt, 2));
    }

    sqlite3_finalize(stmt);
}

static int init_database_locked(void)
{
    snprintf(db1_name, sizeof(db1_name), "%s", DB_FILE_PATH);
    s_db_ready = false;

    sqlite_log_file_state();

    struct stat st = {0};
    bool db_existed_before_open = (stat(DB_FILE_PATH, &st) == 0);

    sqlite3_initialize();

    int rc = db_open(DB_FILE_PATH, &s_db);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Failed to open database %s: rc=%d", DB_FILE_PATH, rc);
        sqlite_log_file_state();
        return rc;
    }

    /* 新库创建任何表之前设置为与 FATFS allocation unit 一致的4KB页。 */
    if (!db_existed_before_open)
    {
        rc = db_exec(s_db, "PRAGMA page_size = 4096;");
        if (rc != SQLITE_OK)
        {
            ESP_LOGE(TAG, "Failed to set page_size for new database");
            goto fail;
        }
    }

    rc = apply_sqlite_pragmas(s_db);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Failed to apply SQLite pragmas");
        goto fail;
    }

    /*
     * sqlite3_open + 第一次读取会自动处理 DELETE journal 模式遗留的 hot journal。
     * 此处只检查，不自动删除数据库或 journal。
     */
    rc = sqlite_quick_check(s_db);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG,
                 "SQLite integrity check failed: rc=%d. Database is preserved; no automatic delete/reformat.",
                 rc);
        goto fail;
    }

    int version = get_user_version(s_db);
    ESP_LOGI(TAG, "SQLite current user_version=%d, target=%d",
             version, SQLITE_SCHEMA_VERSION);

    /* Log the real CREATE TABLE SQL from sqlite_master. This does not depend
     * on PRAGMA and is useful when diagnosing persisted schema after reboot. */
    if (sqlite_table_exists(s_db, SQLITE_CACHE_TABLE_NAME))
    {
        sqlite_log_table_definition(s_db, SQLITE_CACHE_TABLE_NAME);
    }

    rc = ensure_schema_v7(s_db, version);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG,
                 "SQLite schema create/migration failed: rc=%d, errmsg=%s. Database is preserved.",
                 rc,
                 sqlite3_errmsg(s_db));
        goto fail;
    }

    /*
     * 迁移成功后再探测一次 record_id 列：为 false 时（理论仅防御路径）
     * 结构化查询走 7 列兼容模式，旧库仍可读可用。
     */
    s_record_id_column_ready =
        sqlite_table_has_column(s_db, SQLITE_CACHE_TABLE_NAME, "record_id");
    if (!s_record_id_column_ready)
    {
        ESP_LOGW(TAG,
                 "SQLite record_id column unavailable after schema ensure; "
                 "queries fall back to 7-column compatibility mode");
    }

    rc = sqlite_quick_check(s_db);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG,
                 "SQLite post-migration integrity check failed: rc=%d. Database is preserved.",
                 rc);
        goto fail;
    }

    int cache_count = 0;
    if (get_record_count_locked(&cache_count) == 0)
    {
        ESP_LOGI(TAG, "SQLite cache record count=%d", cache_count);
    }

    print_sqlite_pragmas(s_db);
    print_db_files();

    s_db_ready = true;
    ESP_LOGI(TAG, "Database initialized successfully: %s", DB_FILE_PATH);
    return SQLITE_OK;

fail:
    s_db_ready = false;
    if (s_db != NULL)
    {
        int close_rc = sqlite3_close(s_db);
        if (close_rc == SQLITE_OK)
        {
            s_db = NULL;
        }
        else
        {
            ESP_LOGE(TAG, "sqlite3_close after init failure returned rc=%d", close_rc);
        }
    }
    return rc;
}

/* -------------------------------------------------------------------------- */
/* Value JSON校验与容量控制                                                     */
/* -------------------------------------------------------------------------- */

static bool is_valid_value_data(const char *value_data)
{
    if (value_data == NULL || value_data[0] == '\0')
    {
        return false;
    }

    size_t len = strlen(value_data);
    if (len > DB_VALUE_DATA_MAX_LEN)
    {
        return false;
    }

    cJSON *value_array = cJSON_Parse(value_data);
    if (!cJSON_IsArray(value_array) || cJSON_GetArraySize(value_array) <= 0)
    {
        cJSON_Delete(value_array);
        return false;
    }

    cJSON *group = NULL;
    cJSON_ArrayForEach(group, value_array)
    {
        if (!cJSON_IsObject(group))
        {
            cJSON_Delete(value_array);
            return false;
        }

        cJSON *name = cJSON_GetObjectItemCaseSensitive(group, "Name");
        cJSON *data = cJSON_GetObjectItemCaseSensitive(group, "Data");

        if (!cJSON_IsString(name) || name->valuestring == NULL || name->valuestring[0] == '\0' ||
            !cJSON_IsArray(data))
        {
            cJSON_Delete(value_array);
            return false;
        }

        cJSON *item = NULL;
        cJSON_ArrayForEach(item, data)
        {
            if (!cJSON_IsObject(item))
            {
                cJSON_Delete(value_array);
                return false;
            }

            cJSON *item_name = cJSON_GetObjectItemCaseSensitive(item, "name");
            cJSON *item_value = cJSON_GetObjectItemCaseSensitive(item, "value");

            if (!cJSON_IsString(item_name) || item_name->valuestring == NULL || item_name->valuestring[0] == '\0' ||
                !(cJSON_IsString(item_value) || cJSON_IsNumber(item_value)))
            {
                cJSON_Delete(value_array);
                return false;
            }
        }
    }

    cJSON_Delete(value_array);
    return true;
}

static bool sqlite_storage_low_on_free_space(void)
{
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;

    if (esp_vfs_fat_info(FATFS_BASE_PATH, &total_bytes, &free_bytes) != ESP_OK)
    {
        return false;
    }

    return free_bytes < DB_MIN_FREE_BYTES;
}

/*
 * v7：存在性判断键为 (pn, seq_no, record_id)。
 * record_id 为空串时退化为旧行为 (pn, seq_no)——兼容尚未开轮的记录。
 */
static int get_existing_local_id_locked(const char *pn,
                                        const char *record_id,
                                        int seq_no,
                                        sqlite3_int64 *local_id)
{
    *local_id = -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT local_id FROM " SQLITE_CACHE_TABLE_NAME " "
        "WHERE pn=? AND seq_no=? AND record_id=? LIMIT 1;";

    int rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return rc;
    }

    sqlite3_bind_text(stmt, 1, pn, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, seq_no);
    sqlite3_bind_text(stmt, 3, record_id != NULL ? record_id : "", -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        *local_id = sqlite3_column_int64(stmt, 0);
        rc = SQLITE_OK;
    }
    else if (rc == SQLITE_DONE)
    {
        rc = SQLITE_NOTFOUND;
    }

    sqlite3_finalize(stmt);
    return rc;
}

static int get_overwrite_local_id_locked(sqlite3_int64 *local_id)
{
    *local_id = -1;

    /* pushed=1优先淘汰；同状态下local_id越小越旧。 */
    const char *sql =
        "SELECT local_id FROM " SQLITE_CACHE_TABLE_NAME " "
        "ORDER BY pushed DESC, local_id ASC LIMIT 1;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return rc;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        *local_id = sqlite3_column_int64(stmt, 0);
        rc = SQLITE_OK;
    }
    else if (rc == SQLITE_DONE)
    {
        rc = SQLITE_NOTFOUND;
    }

    sqlite3_finalize(stmt);
    return rc;
}

static int delete_local_id_locked(sqlite3_int64 local_id)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(s_db,
                                "DELETE FROM " SQLITE_CACHE_TABLE_NAME " WHERE local_id=?;",
                                -1,
                                &stmt,
                                NULL);
    if (rc != SQLITE_OK)
    {
        return rc;
    }

    sqlite3_bind_int64(stmt, 1, local_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

/* -------------------------------------------------------------------------- */
/* 保存                                                                        */
/* -------------------------------------------------------------------------- */

int InsertStructuredRecord(int seq_no,
                           const char *pn,
                           int current_step,
                           int timestamp,
                           bool pushed,
                           const char *value_data,
                           const char *record_id)
{
    if (seq_no < 0 || pn == NULL || pn[0] == '\0' || value_data == NULL)
    {
        ESP_LOGE(TAG, "InsertStructuredRecord invalid parameter");
        return -1;
    }

    if (strlen(pn) >= DB_PN_MAX_LEN)
    {
        ESP_LOGE(TAG, "PN length invalid: %u bytes", (unsigned)strlen(pn));
        return -1;
    }

    if (record_id != NULL && strlen(record_id) >= DB_RECORD_ID_MAX_LEN)
    {
        ESP_LOGE(TAG, "RecordId length invalid: %u bytes", (unsigned)strlen(record_id));
        return -1;
    }

    const char *record_id_value = (record_id != NULL) ? record_id : "";

    if (!is_valid_value_data(value_data))
    {
        ESP_LOGE(TAG, "Value JSON invalid or too long, len=%u",
                 (unsigned)(value_data ? strlen(value_data) : 0));
        return -1;
    }

    if (!sqlite_lock())
    {
        return -1;
    }

    if (!sqlite_ready_locked())
    {
        sqlite_unlock();
        return -1;
    }

    int rc = db_exec(s_db, "BEGIN IMMEDIATE TRANSACTION;");
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "BEGIN transaction failed: rc=%d, errmsg=%s", rc, sqlite3_errmsg(s_db));
        sqlite_unlock();
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    sqlite3_int64 existing_local_id = -1;
    int existing_rc = get_existing_local_id_locked(pn, record_id_value, seq_no, &existing_local_id);

    if (existing_rc != SQLITE_OK && existing_rc != SQLITE_NOTFOUND)
    {
        ESP_LOGE(TAG, "Check existing record failed: rc=%d, errmsg=%s",
                 existing_rc, sqlite3_errmsg(s_db));
        goto rollback;
    }

    if (existing_rc == SQLITE_NOTFOUND)
    {
        int record_count = 0;
        if (get_record_count_locked(&record_count) != 0)
        {
            ESP_LOGE(TAG, "Get record count failed");
            goto rollback;
        }

        bool low_space = sqlite_storage_low_on_free_space();
        if (record_count > 0 && (record_count >= MAX_RECORDS_COUNT || low_space))
        {
            sqlite3_int64 overwrite_local_id = -1;
            rc = get_overwrite_local_id_locked(&overwrite_local_id);
            if (rc != SQLITE_OK || overwrite_local_id < 0)
            {
                ESP_LOGE(TAG, "No record available for cache eviction");
                goto rollback;
            }

            rc = delete_local_id_locked(overwrite_local_id);
            if (rc != SQLITE_OK)
            {
                ESP_LOGE(TAG, "Delete old cache record failed: rc=%d", rc);
                goto rollback;
            }

            ESP_LOGW(TAG,
                     "SQLite cache eviction: local_id=%lld reason=%s",
                     (long long)overwrite_local_id,
                     low_space ? "low_free_space" : "max_record_count");
        }
    }

    int created_at = sqlite_now_seconds();

    if (existing_rc == SQLITE_OK)
    {
        const char *sql =
            "UPDATE " SQLITE_CACHE_TABLE_NAME " "
            "SET current_step=?, timestamp=?, pushed=?, value_data=?, created_at=?, record_id=? "
            "WHERE local_id=?;";

        rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK)
        {
            ESP_LOGE(TAG, "Prepare update failed: %s", sqlite3_errmsg(s_db));
            goto rollback;
        }

        sqlite3_bind_int(stmt, 1, current_step);
        sqlite3_bind_int(stmt, 2, timestamp);
        sqlite3_bind_int(stmt, 3, pushed ? 1 : 0);
        sqlite3_bind_text(stmt, 4, value_data, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, created_at);
        sqlite3_bind_text(stmt, 6, record_id_value, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 7, existing_local_id);
    }
    else
    {
        const char *sql =
            "INSERT INTO " SQLITE_CACHE_TABLE_NAME " "
            "(seq_no, pn, current_step, timestamp, pushed, value_data, created_at, record_id) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";

        rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK)
        {
            ESP_LOGE(TAG, "Prepare insert failed: %s", sqlite3_errmsg(s_db));
            goto rollback;
        }

        sqlite3_bind_int(stmt, 1, seq_no);
        sqlite3_bind_text(stmt, 2, pn, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, current_step);
        sqlite3_bind_int(stmt, 4, timestamp);
        sqlite3_bind_int(stmt, 5, pushed ? 1 : 0);
        sqlite3_bind_text(stmt, 6, value_data, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, created_at);
        sqlite3_bind_text(stmt, 8, record_id_value, -1, SQLITE_TRANSIENT);
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
    {
        ESP_LOGE(TAG, "Insert/update step failed: rc=%d, ext=%d, errmsg=%s",
                 rc,
                 sqlite3_extended_errcode(s_db),
                 sqlite3_errmsg(s_db));
        goto rollback;
    }

    sqlite3_finalize(stmt);
    stmt = NULL;

    rc = db_exec(s_db, "COMMIT;");
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "COMMIT failed: rc=%d, errmsg=%s", rc, sqlite3_errmsg(s_db));
        (void)db_exec(s_db, "ROLLBACK;");
        sqlite_unlock();
        return -1;
    }

    ESP_LOGI(TAG,
             "structured record committed: seq_no=%d, PN=%s, RecordId=%s, step=%d, timestamp=%d, pushed=%d, value_len=%u",
             seq_no,
             pn,
             record_id_value,
             current_step,
             timestamp,
             pushed ? 1 : 0,
             (unsigned)strlen(value_data));

    sqlite_unlock();
    return 0;

rollback:
    if (stmt != NULL)
    {
        sqlite3_finalize(stmt);
    }
    (void)db_exec(s_db, "ROLLBACK;");
    sqlite_unlock();
    return -1;
}

int InsertDataReportValues(int seq_no,
                           const char *pn,
                           int current_step,
                           int timestamp,
                           bool pushed,
                           const double *values,
                           int value_count,
                           const char *record_id)
{
    if (values == NULL || value_count <= 0 || value_count > DB_VALUE_MAX_COUNT)
    {
        return -1;
    }

    cJSON *value_array = cJSON_CreateArray();
    cJSON *group = cJSON_CreateObject();
    cJSON *data = cJSON_CreateArray();
    if (value_array == NULL || group == NULL || data == NULL)
    {
        cJSON_Delete(value_array);
        cJSON_Delete(group);
        cJSON_Delete(data);
        return -1;
    }

    cJSON_AddStringToObject(group, "Name", "Legacy");
    cJSON_AddItemToObject(group, "Data", data);
    cJSON_AddItemToArray(value_array, group);

    for (int i = 0; i < value_count; i++)
    {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL)
        {
            cJSON_Delete(value_array);
            return -1;
        }

        char name[24];
        char value_string[32];
        snprintf(name, sizeof(name), "Value%d", i);
        snprintf(value_string, sizeof(value_string), "%.2f", values[i]);
        cJSON_AddStringToObject(item, "name", name);
        cJSON_AddStringToObject(item, "value", value_string);
        cJSON_AddItemToArray(data, item);
    }

    char *value_json = cJSON_PrintUnformatted(value_array);
    cJSON_Delete(value_array);
    if (value_json == NULL)
    {
        return -1;
    }

    int ret = InsertStructuredRecord(seq_no, pn, current_step, timestamp, pushed, value_json, record_id);
    free(value_json);
    return ret;
}

/* -------------------------------------------------------------------------- */
/* 更新                                                                        */
/* -------------------------------------------------------------------------- */

/*
 * push_state 三态（v7）：0=pending 待发；1=inflight 已交 SDK 等 PUBACK；
 * 2=acked broker 已确认。传 0/1/2 之外的值按 0 处理。
 * record_id 限定轮次：同一 PN 重开轮后不会误更新上一轮记录。
 * 传 NULL/空串时退化为旧行为（仅 pn+seq_no）。
 */
int UpdateRecordPushStateByPNAndID(const char *pn, const char *record_id, int seq_no, int push_state)
{
    if (pn == NULL || pn[0] == '\0' || seq_no < 0)
    {
        return -1;
    }

    int state = (push_state == 1 || push_state == 2) ? push_state : 0;

    if (!sqlite_lock()) return -1;
    if (!sqlite_ready_locked())
    {
        sqlite_unlock();
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        (record_id != NULL && record_id[0] != '\0')
            ? "UPDATE " SQLITE_CACHE_TABLE_NAME " SET pushed=? WHERE pn=? AND seq_no=? AND record_id=?;"
            : "UPDATE " SQLITE_CACHE_TABLE_NAME " SET pushed=? WHERE pn=? AND seq_no=?;";

    int rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Prepare pushed update failed: %s", sqlite3_errmsg(s_db));
        sqlite_unlock();
        return -1;
    }

    sqlite3_bind_int(stmt, 1, state);
    sqlite3_bind_text(stmt, 2, pn, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, seq_no);
    if (record_id != NULL && record_id[0] != '\0')
    {
        sqlite3_bind_text(stmt, 4, record_id, -1, SQLITE_TRANSIENT);
    }

    rc = sqlite3_step(stmt);
    int changed = (rc == SQLITE_DONE) ? sqlite3_changes(s_db) : 0;
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE || changed <= 0)
    {
        ESP_LOGW(TAG, "Push-state update failed/not found: PN=%s seq_no=%d state=%d rc=%d",
                 pn, seq_no, state, rc);
        sqlite_unlock();
        return -1;
    }

    ESP_LOGI(TAG, "Push-state updated: PN=%s seq_no=%d state=%d", pn, seq_no, state);
    sqlite_unlock();
    return 0;
}

int UpdateRecordPushState(int seq_no, bool pushed)
{
    if (seq_no < 0) return -1;
    if (!sqlite_lock()) return -1;
    if (!sqlite_ready_locked())
    {
        sqlite_unlock();
        return -1;
    }

    /* legacy：若多个PN拥有相同seq_no，只更新最近插入的一条。 */
    const char *sql =
        "UPDATE " SQLITE_CACHE_TABLE_NAME " SET pushed=? "
        "WHERE local_id=(SELECT local_id FROM " SQLITE_CACHE_TABLE_NAME " "
        "WHERE seq_no=? ORDER BY local_id DESC LIMIT 1);";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        sqlite_unlock();
        return -1;
    }

    sqlite3_bind_int(stmt, 1, pushed ? 1 : 0);
    sqlite3_bind_int(stmt, 2, seq_no);
    rc = sqlite3_step(stmt);
    int changed = (rc == SQLITE_DONE) ? sqlite3_changes(s_db) : 0;
    sqlite3_finalize(stmt);
    sqlite_unlock();

    return (rc == SQLITE_DONE && changed > 0) ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
/* 查询                                                                        */
/* -------------------------------------------------------------------------- */

/*
 * 结构化查询列清单：record_id 列可用时作为第 8 列返回。
 * 调用方用 snprintf 拼完整 SQL（缓冲区 >= 256 字节）；
 * fill_query_result_from_stmt 按列数守卫读取，7 列旧库自动兼容。
 */
static const char *structured_select_columns(void)
{
    static const char with_record_id[] =
        "SELECT seq_no, pn, current_step, timestamp, pushed, value_data, created_at, record_id ";
    static const char without_record_id[] =
        "SELECT seq_no, pn, current_step, timestamp, pushed, value_data, created_at ";

    return s_record_id_column_ready ? with_record_id : without_record_id;
}

static int query_one_locked(const char *sql,
                            const char *pn,
                            int seq_no,
                            int pushed_filter,
                            QueryResult *out_result)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Prepare query failed: %s", sqlite3_errmsg(s_db));
        return -1;
    }

    int bind_index = 1;
    if (pn != NULL)
    {
        sqlite3_bind_text(stmt, bind_index++, pn, -1, SQLITE_TRANSIENT);
    }
    if (seq_no >= 0)
    {
        sqlite3_bind_int(stmt, bind_index++, seq_no);
    }
    if (pushed_filter >= 0)
    {
        sqlite3_bind_int(stmt, bind_index++, pushed_filter);
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        fill_query_result_from_stmt(stmt, out_result);
        sqlite3_finalize(stmt);
        return 0;
    }

    if (rc != SQLITE_DONE)
    {
        ESP_LOGE(TAG, "Query step failed: rc=%d, errmsg=%s", rc, sqlite3_errmsg(s_db));
    }

    sqlite3_finalize(stmt);
    return -1;
}

int QueryStructuredRecordLatestByPN(const char *pn, QueryResult *out_result)
{
    if (pn == NULL || pn[0] == '\0' || out_result == NULL)
    {
        return -1;
    }

    memset(out_result, 0, sizeof(QueryResult));

    if (!sqlite_lock()) return -1;
    if (!sqlite_ready_locked())
    {
        sqlite_unlock();
        return -1;
    }

    char sql[256];
    (void)snprintf(sql, sizeof(sql),
                   "%s"
                   "FROM " SQLITE_CACHE_TABLE_NAME " "
                   "WHERE pn=? ORDER BY seq_no DESC, local_id DESC LIMIT 1;",
                   structured_select_columns());

    int ret = query_one_locked(sql, pn, -1, -1, out_result);
    if (ret == 0)
    {
        ESP_LOGI(TAG, "Latest record found: PN=%s last_seq=%d pushed=%d",
                 pn, out_result->seq_no, out_result->pushed ? 1 : 0);
    }
    else
    {
        ESP_LOGI(TAG, "No cached record for PN=%s", pn);
    }

    sqlite_unlock();
    return ret;
}

int QueryStructuredRecordBySession(const char *pn, const char *record_id,
                                   int seq_no, QueryResult *out_result)
{
    if (pn == NULL || pn[0] == '\0' || record_id == NULL || record_id[0] == '\0' ||
        seq_no < -1 || out_result == NULL) return -2;
    memset(out_result, 0, sizeof(*out_result));
    if (!sqlite_lock()) return -2;
    int result = -2;
    sqlite3_stmt *stmt = NULL;
    if (!sqlite_ready_locked() || !s_record_id_column_ready) goto done;
    char sql[320];
    snprintf(sql, sizeof(sql), "%s FROM " SQLITE_CACHE_TABLE_NAME
             " WHERE pn=? AND record_id=? %s ORDER BY seq_no DESC, local_id DESC LIMIT 1;",
             structured_select_columns(), seq_no >= 0 ? "AND seq_no=?" : "");
    if (sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL) != SQLITE_OK) goto done;
    if (sqlite3_bind_text(stmt, 1, pn, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, record_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        (seq_no >= 0 && sqlite3_bind_int(stmt, 3, seq_no) != SQLITE_OK)) goto done;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        fill_query_result_from_stmt(stmt, out_result);
        /* 行存在但重构失败（内存不足/输出超长等）必须重试，不能视为本地无数据。 */
        result = out_result->json_data[0] != '\0' ? 0 : -2;
    } else if (rc == SQLITE_DONE) result = -1;
done:
    if (stmt != NULL) sqlite3_finalize(stmt);
    sqlite_unlock();
    return result;
}

int QueryStructuredRecordBySeq(int seq_no, QueryResult *out_result)
{
    if (seq_no < 0 || out_result == NULL) return -1;
    memset(out_result, 0, sizeof(QueryResult));

    if (!sqlite_lock()) return -1;
    if (!sqlite_ready_locked())
    {
        sqlite_unlock();
        return -1;
    }

    /* legacy：跨PN时返回最近插入的同seq_no记录。 */
    char sql[256];
    (void)snprintf(sql, sizeof(sql),
                   "%s"
                   "FROM " SQLITE_CACHE_TABLE_NAME " "
                   "WHERE seq_no=? ORDER BY local_id DESC LIMIT 1;",
                   structured_select_columns());

    int ret = query_one_locked(sql, NULL, seq_no, -1, out_result);
    sqlite_unlock();
    return ret;
}

int QueryJsonRecordBySeq(int seq_no, QueryResult *out_result)
{
    return QueryStructuredRecordBySeq(seq_no, out_result);
}

void query_db1_latest_by_pn_to_global(const char *pn)
{
    if (g_db1_result == NULL) return; /* 初始化内存不足时不解引用未建立的结果缓存。 */
    memset(g_db1_result, 0, sizeof(QueryResult));
    (void)QueryStructuredRecordLatestByPN(pn, g_db1_result);
}

void query_db1_to_global(int target_id)
{
    if (g_db1_result == NULL) return;
    memset(g_db1_result, 0, sizeof(QueryResult));
    (void)QueryStructuredRecordBySeq(target_id, g_db1_result);
}

int QueryStructuredRecordsByPNAndPushState(const char *pn,
                                           bool pushed,
                                           QueryResult *out_array,
                                           int max_count,
                                           int *out_count)
{
    if (out_count != NULL) *out_count = 0;

    if (pn == NULL || pn[0] == '\0' || out_array == NULL || max_count <= 0 || out_count == NULL)
    {
        return -1;
    }

    memset(out_array, 0, sizeof(QueryResult) * max_count);

    if (!sqlite_lock()) return -1;
    if (!sqlite_ready_locked())
    {
        sqlite_unlock();
        return -1;
    }

    char sql[256];
    (void)snprintf(sql, sizeof(sql),
                   "%s"
                   "FROM " SQLITE_CACHE_TABLE_NAME " "
                   "WHERE pn=? AND pushed=? "
                   "ORDER BY seq_no ASC, local_id ASC LIMIT ?;",
                   structured_select_columns());

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Prepare PN/pushed query failed: %s", sqlite3_errmsg(s_db));
        sqlite_unlock();
        return -1;
    }

    sqlite3_bind_text(stmt, 1, pn, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, pushed ? 1 : 0);
    sqlite3_bind_int(stmt, 3, max_count);

    int count = 0;
    while (count < max_count && (rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        fill_query_result_from_stmt(stmt, &out_array[count++]);
    }

    sqlite3_finalize(stmt);
    sqlite_unlock();

    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
    {
        return -1;
    }

    *out_count = count;
    return (count > 0) ? 0 : -1;
}

int QueryLatestRecordByPNAndPushState(const char *pn,
                                      bool pushed,
                                      QueryResult *out_result)
{
    if (pn == NULL || pn[0] == '\0' || out_result == NULL)
    {
        return -1;
    }

    memset(out_result, 0, sizeof(QueryResult));

    if (!sqlite_lock()) return -1;
    if (!sqlite_ready_locked())
    {
        sqlite_unlock();
        return -1;
    }

    /* 补发按IDNUM从小到大，避免乱序。 */
    char sql[256];
    (void)snprintf(sql, sizeof(sql),
                   "%s"
                   "FROM " SQLITE_CACHE_TABLE_NAME " "
                   "WHERE pn=? AND pushed=? "
                   "ORDER BY seq_no ASC, local_id ASC LIMIT 1;",
                   structured_select_columns());

    int ret = query_one_locked(sql, pn, -1, pushed ? 1 : 0, out_result);
    sqlite_unlock();
    return ret;
}

int QueryUnpushedRecords(QueryResult *out_array, int max_count, int *out_count)
{
    if (out_array == NULL || out_count == NULL || max_count <= 0)
    {
        return -1;
    }

    *out_count = 0;
    memset(out_array, 0, sizeof(QueryResult) * max_count);

    if (!sqlite_lock()) return -1;
    if (!sqlite_ready_locked())
    {
        sqlite_unlock();
        return -1;
    }

    /*
     * 契约（v7）：只返回"可补传"的待发记录——
     *   - pushed=0（pending；inflight/acked 不重传）；
     *   - record_id 非空（迁移前历史记录无法归属轮次，补发即串批）；
     *   - value_data 非空（空值无法重构上报 JSON）。
     * 坏记录被此过滤挡在扫描外，不会每轮顶住队列头部饿死后续数据；
     * 它们由缓存淘汰（pushed=0 最后淘汰）或 AgingDataCheck 退休。
     * record_id 列不可用（防御路径）时退化为仅 pushed=0。
     */
    const char *filter = s_record_id_column_ready
        ? "WHERE pushed=0 AND length(record_id) > 0 AND length(value_data) > 0 "
        : "WHERE pushed=0 ";

    char sql[256];
    (void)snprintf(sql, sizeof(sql),
                   "%s"
                   "FROM " SQLITE_CACHE_TABLE_NAME " "
                   "%s"
                   "ORDER BY local_id ASC LIMIT ?;",
                   structured_select_columns(), filter);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        sqlite_unlock();
        return -1;
    }

    sqlite3_bind_int(stmt, 1, max_count);

    while (*out_count < max_count && (rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        fill_query_result_from_stmt(stmt, &out_array[*out_count]);
        (*out_count)++;
    }

    sqlite3_finalize(stmt);
    sqlite_unlock();

    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
/* JSON兼容接口                                                                 */
/* -------------------------------------------------------------------------- */

static int serialize_value_array_for_storage(cJSON *value_item,
                                             char *out_value_data,
                                             size_t out_size)
{
    if (!cJSON_IsArray(value_item) || out_value_data == NULL || out_size == 0)
    {
        return -1;
    }

    cJSON *storage_array = NULL;
    cJSON *first = cJSON_GetArrayItem(value_item, 0);

    if (first == NULL || cJSON_IsObject(first))
    {
        storage_array = cJSON_Duplicate(value_item, true);
    }
    else
    {
        storage_array = cJSON_CreateArray();
        cJSON *group = cJSON_CreateObject();
        cJSON *data = cJSON_CreateArray();
        if (storage_array == NULL || group == NULL || data == NULL)
        {
            cJSON_Delete(storage_array);
            cJSON_Delete(group);
            cJSON_Delete(data);
            return -1;
        }

        cJSON_AddStringToObject(group, "Name", "Legacy");
        cJSON_AddItemToObject(group, "Data", data);
        cJSON_AddItemToArray(storage_array, group);

        int count = cJSON_GetArraySize(value_item);
        for (int i = 0; i < count; i++)
        {
            cJSON *src = cJSON_GetArrayItem(value_item, i);
            cJSON *item = cJSON_CreateObject();
            if (item == NULL)
            {
                cJSON_Delete(storage_array);
                return -1;
            }

            char name[24];
            char value_string[32];
            snprintf(name, sizeof(name), "Value%d", i);

            if (cJSON_IsString(src) && src->valuestring != NULL)
            {
                snprintf(value_string, sizeof(value_string), "%s", src->valuestring);
            }
            else if (cJSON_IsNumber(src))
            {
                snprintf(value_string, sizeof(value_string), "%.2f", src->valuedouble);
            }
            else
            {
                cJSON_Delete(item);
                cJSON_Delete(storage_array);
                return -1;
            }

            cJSON_AddStringToObject(item, "name", name);
            cJSON_AddStringToObject(item, "value", value_string);
            cJSON_AddItemToArray(data, item);
        }
    }

    char *json = cJSON_PrintUnformatted(storage_array);
    cJSON_Delete(storage_array);
    if (json == NULL)
    {
        return -1;
    }

    size_t len = strlen(json);
    if (len >= out_size)
    {
        free(json);
        return -1;
    }

    memcpy(out_value_data, json, len + 1);
    free(json);
    return 0;
}

int InsertJsonRecord(int seq_no, const char *pn, bool pushed, const char *json_data, const char *record_id)
{
    if (json_data == NULL)
    {
        return -1;
    }

    cJSON *root = cJSON_Parse(json_data);
    if (root == NULL)
    {
        return -1;
    }

    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "Data");
    if (!cJSON_IsObject(payload))
    {
        payload = root;
    }

    const char *final_pn = pn;
    int final_seq_no = seq_no;
    int final_timestamp = 0;
    int final_step = 0;
    char value_data[DB_VALUE_DATA_MAX_LEN + 1] = {0};

    cJSON *pn_item = cJSON_GetObjectItemCaseSensitive(payload, "PN");
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(payload, "IDNUM");
    cJSON *timestamp_item = cJSON_GetObjectItemCaseSensitive(payload, "TIMESTAMP");
    cJSON *step_item = cJSON_GetObjectItemCaseSensitive(payload, "CurrentStep");
    cJSON *value_item = cJSON_GetObjectItemCaseSensitive(payload, "Value");

    if ((final_pn == NULL || final_pn[0] == '\0') &&
        cJSON_IsString(pn_item) && pn_item->valuestring != NULL)
    {
        final_pn = pn_item->valuestring;
    }

    if (final_seq_no < 0 && cJSON_IsNumber(id_item))
    {
        final_seq_no = id_item->valueint;
    }

    if (cJSON_IsNumber(timestamp_item)) final_timestamp = timestamp_item->valueint;
    if (cJSON_IsNumber(step_item)) final_step = step_item->valueint;

    if (serialize_value_array_for_storage(value_item, value_data, sizeof(value_data)) != 0)
    {
        cJSON_Delete(root);
        return -1;
    }

    if (final_pn == NULL || final_pn[0] == '\0' || final_seq_no < 0 || final_timestamp <= 0)
    {
        cJSON_Delete(root);
        return -1;
    }

    int ret = InsertStructuredRecord(final_seq_no,
                                     final_pn,
                                     final_step,
                                     final_timestamp,
                                     pushed,
                                     value_data,
                                     record_id);
    cJSON_Delete(root);
    return ret;
}

/* -------------------------------------------------------------------------- */
/* 初始化入口                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * 重置结果写 NVS 标志。
 * SqLite_Init 远早于 MQTT 建立（app_task_init 早期），此处 publish 必然失败，
 * 故由 Init_ByNetwork_Flag 在网络就绪后读取本标志经 event/warn 上报并清除，
 * 保证"缓存已丢"对服务器可见（断网补发契约不被打断）。
 */
static void sqlite_write_reset_flag(const char *flag)
{
    esp_err_t ret = SelfRecovery_Write_str(NVS_KEY_SQLITE_RESET, flag);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to persist SQLite reset flag '%s': %s",
                 flag, esp_err_to_name(ret));
    }
}

void SqLite_Init(void)
{
    ESP_LOGI(TAG, "Initializing SqLite");

    if (s_sqlite_mutex == NULL)
    {
        s_sqlite_mutex = xSemaphoreCreateMutex();
        if (s_sqlite_mutex == NULL)
        {
            ESP_LOGE(TAG, "Failed to create SQLite mutex");
            return;
        }
    }

    /*
     * 阶段 1：mount，带偶发失败重试。正常路径第一次即成功，行为与历史一致。
     */
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= SQLITE_MOUNT_RETRY_COUNT; ++attempt)
    {
        ret = mount_fatfs_storage();
        if (ret == ESP_OK)
        {
            break;
        }
        ESP_LOGW(TAG, "FATFS mount attempt %d/%d failed: %s",
                 attempt, SQLITE_MOUNT_RETRY_COUNT, esp_err_to_name(ret));
        if (attempt < SQLITE_MOUNT_RETRY_COUNT)
        {
            vTaskDelay(pdMS_TO_TICKS(SQLITE_MOUNT_RETRY_INTERVAL_MS));
        }
    }

    /*
     * 阶段 2a：mount 持续失败 → 文件系统级损坏，数据已不可读。
     * 用户决策：格式化重建分区恢复存储能力（覆盖"非空绝不格式化"哲学，
     * 依据见 mount_fatfs_storage_reset 注释），每上电仅一次。
     */
    if (ret != ESP_OK && !s_reset_done_this_boot)
    {
        s_reset_done_this_boot = true;
        ESP_LOGW(TAG,
                 "FATFS mount failed %d times; reformatting partition as last resort "
                 "(SQLite cache rebuild, user-approved)",
                 SQLITE_MOUNT_RETRY_COUNT);
        ret = mount_fatfs_storage_reset();
        if (ret != ESP_OK)
        {
            sqlite_write_reset_flag(SQLITE_RESET_FLAG_MOUNT_FAIL);
            ESP_LOGE(TAG, "FATFS reset mount failed; SQLite is disabled this boot");
            return;
        }
    }
    else if (ret != ESP_OK)
    {
        /* 守卫已消耗（本次上电已重建过仍失败）：保持禁用，不再重置。 */
        sqlite_write_reset_flag(SQLITE_RESET_FLAG_MOUNT_FAIL);
        ESP_LOGE(TAG, "FATFS mount failed and reset already used this boot; SQLite is disabled");
        return;
    }

    if (g_db1_result == NULL) g_db1_result = app_calloc_prefer_psram(1, sizeof(QueryResult));
    if (g_db1_result == NULL) {
        ESP_LOGE(TAG, "No memory for database result cache; SQLite init stopped without resetting storage");
        return;
    }
    snprintf(db1_name, sizeof(db1_name), "%s", DB_FILE_PATH);
    memset(g_db1_result, 0, sizeof(QueryResult));

    if (!sqlite_lock())
    {
        return;
    }

    int rc = init_database_locked();
    sqlite_unlock();

    /*
     * 阶段 2b：DB 文件级损坏（quick_check/schema/后校验失败）。
     * rename 留档 .corrupt 后重建空库——文件系统完好时不动分区，最小化
     * 破坏面；.corrupt 供事后取数（人工维修时清理）。
     * journal/wal/shm 一并清理：防止新库打开时回滚旧 hot journal。
     */
    if (rc != SQLITE_OK && !s_reset_done_this_boot)
    {
        s_reset_done_this_boot = true;
        ESP_LOGW(TAG,
                 "SQLite init failed (rc=%d); rebuilding database from scratch "
                 "(unpushed cache records are lost)", rc);

        char corrupt_path[128];
        int n = snprintf(corrupt_path, sizeof(corrupt_path), "%s.corrupt", DB_FILE_PATH);
        if (n > 0 && n < (int)sizeof(corrupt_path))
        {
            if (rename(DB_FILE_PATH, corrupt_path) != 0)
            {
                ESP_LOGW(TAG, "rename to .corrupt failed (errno=%d); removing damaged file", errno);
                (void)unlink(DB_FILE_PATH);
            }
            else
            {
                ESP_LOGW(TAG, "Damaged database preserved as %s", corrupt_path);
            }
        }
        else
        {
            ESP_LOGW(TAG, "Corrupt path too long; removing damaged database file");
            (void)unlink(DB_FILE_PATH);
        }
        (void)unlink(DB_FILE_PATH "-journal");
        (void)unlink(DB_FILE_PATH "-wal");
        (void)unlink(DB_FILE_PATH "-shm");

        if (!sqlite_lock())
        {
            sqlite_write_reset_flag(SQLITE_RESET_FLAG_REBUILD_FAIL);
            return;
        }
        rc = init_database_locked();
        sqlite_unlock();

        if (rc == SQLITE_OK)
        {
            sqlite_write_reset_flag(SQLITE_RESET_FLAG_DB_REBUILT);
            ESP_LOGW(TAG, "SQLite database rebuilt empty; pending restart records were lost");
        }
        else
        {
            sqlite_write_reset_flag(SQLITE_RESET_FLAG_REBUILD_FAIL);
        }
    }

    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG,
                 "SQLite initialization failed: rc=%d. No database file was automatically deleted.",
                 rc);
    }

    esp_log_level_set(TAG, ESP_LOG_INFO);
}
