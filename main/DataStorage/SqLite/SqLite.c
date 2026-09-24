#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <errno.h>
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

static const char *TAG = "SQLITE";

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static SemaphoreHandle_t s_sqlite_mutex = NULL;
static sqlite3 *s_db = NULL;
static bool s_db_ready = false;

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

    char temp[DB_VALUE_DATA_MAX_LEN + 1];
    safe_copy(temp, sizeof(temp), result->value_data);

    char *saveptr = NULL;
    char *token = strtok_r(temp, ",", &saveptr);
    float values[4] = {0};
    int index = 0;

    while (token != NULL && index < 4)
    {
        char *equal = strchr(token, '=');
        const char *number = equal ? equal + 1 : token;
        values[index++] = scaled_int_to_float(atoi(number));
        token = strtok_r(NULL, ",", &saveptr);
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

    /* 查询列顺序固定：seq_no, sn, current_step, timestamp, pushed, value_data, created_at */
    result->seq_no = sqlite3_column_int(stmt, 0);

    const unsigned char *sn = sqlite3_column_text(stmt, 1);
    safe_copy(result->SN, sizeof(result->SN), (const char *)sn);

    result->current_step = (short)sqlite3_column_int(stmt, 2);
    result->timestamp = sqlite3_column_int(stmt, 3);
    result->pushed = sqlite3_column_int(stmt, 4) ? true : false;

    const unsigned char *value_data = sqlite3_column_text(stmt, 5);
    safe_copy(result->value_data, sizeof(result->value_data), (const char *)value_data);

    result->created_at = sqlite3_column_int(stmt, 6);

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

static int create_schema_v5(sqlite3 *db)
{
    if (db == NULL)
    {
        return SQLITE_MISUSE;
    }

    /*
     * local_id 是数据库内部主键；IDNUM(seq_no)只在同一个SN内唯一。
     * 这修复了“第二台设备IDNUM重新从0开始时覆盖第一台设备记录”的问题。
     */
    int rc = db_exec(db,
                     "CREATE TABLE IF NOT EXISTS " SQLITE_CACHE_TABLE_NAME " ("
                     "local_id INTEGER PRIMARY KEY,"
                     "seq_no INTEGER NOT NULL,"
                     "sn TEXT NOT NULL CHECK(length(sn) < 64),"
                     "current_step INTEGER NOT NULL,"
                     "timestamp INTEGER NOT NULL,"
                     "pushed INTEGER NOT NULL DEFAULT 0 CHECK(pushed IN (0,1)),"
                     "value_data TEXT NOT NULL CHECK(length(CAST(value_data AS BLOB)) <= 2048),"
                     "created_at INTEGER NOT NULL,"
                     "UNIQUE(sn, seq_no)"
                     ");");
    if (rc != SQLITE_OK) return rc;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_sn_seq_v5 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (sn, seq_no DESC);");
    if (rc != SQLITE_OK) return rc;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_sn_push_seq_v5 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (sn, pushed, seq_no);");
    if (rc != SQLITE_OK) return rc;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_push_local_v5 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (pushed, local_id);");
    if (rc != SQLITE_OK) return rc;

    return db_exec(db, "PRAGMA user_version = 5;");
}

static bool sqlite_schema_is_v5(sqlite3 *db)
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
        "sn",
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

static int create_v5_temp_table(sqlite3 *db)
{
    if (db == NULL)
    {
        return SQLITE_MISUSE;
    }

    int rc = db_exec(db, "DROP TABLE IF EXISTS data_cache_v5_tmp;");
    if (rc != SQLITE_OK)
    {
        return rc;
    }

    return db_exec(db,
                   "CREATE TABLE data_cache_v5_tmp ("
                   "local_id INTEGER PRIMARY KEY,"
                   "seq_no INTEGER NOT NULL,"
                   "sn TEXT NOT NULL CHECK(length(sn) < 64),"
                   "current_step INTEGER NOT NULL,"
                   "timestamp INTEGER NOT NULL,"
                   "pushed INTEGER NOT NULL DEFAULT 0 CHECK(pushed IN (0,1)),"
                   "value_data TEXT NOT NULL CHECK(length(CAST(value_data AS BLOB)) <= 2048),"
                   "created_at INTEGER NOT NULL,"
                   "UNIQUE(sn, seq_no)"
                   ");");
}

static int migrate_legacy_schema_to_v5(sqlite3 *db)
{
    if (db == NULL)
    {
        return SQLITE_MISUSE;
    }

    /*
     * 老版本 v3/v4 没有 local_id；不使用 ALTER TABLE RENAME。
     * 当前嵌入式 SQLite 组件实测 ALTER TABLE ... RENAME 可能返回
     * SQLITE_ERROR，因此改成事务内“临时表复制 -> 重建主表 -> 回填”。
     */
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

    rc = create_v5_temp_table(db);
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "INSERT OR REPLACE INTO data_cache_v5_tmp "
                 "(seq_no, sn, current_step, timestamp, pushed, value_data, created_at) "
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
                 "sn TEXT NOT NULL CHECK(length(sn) < 64),"
                 "current_step INTEGER NOT NULL,"
                 "timestamp INTEGER NOT NULL,"
                 "pushed INTEGER NOT NULL DEFAULT 0 CHECK(pushed IN (0,1)),"
                 "value_data TEXT NOT NULL CHECK(length(CAST(value_data AS BLOB)) <= 2048),"
                 "created_at INTEGER NOT NULL,"
                 "UNIQUE(sn, seq_no)"
                 ");");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "INSERT OR REPLACE INTO " SQLITE_CACHE_TABLE_NAME " "
                 "(seq_no, sn, current_step, timestamp, pushed, value_data, created_at) "
                 "SELECT seq_no, sn, current_step, timestamp, pushed, value_data, created_at "
                 "FROM data_cache_v5_tmp;");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db, "DROP TABLE data_cache_v5_tmp;");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_sn_seq_v5 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (sn, seq_no DESC);");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_sn_push_seq_v5 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (sn, pushed, seq_no);");
    if (rc != SQLITE_OK) goto rollback;

    rc = db_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_cache_push_local_v5 "
                 "ON " SQLITE_CACHE_TABLE_NAME " (pushed, local_id);");
    if (rc != SQLITE_OK) goto rollback;

    /* 仅作为版本标记尝试写入；后续启动不依赖它判断 schema。 */
    (void)db_exec(db, "PRAGMA user_version = 5;");

    rc = db_exec(db, "COMMIT;");
    if (rc == SQLITE_OK)
    {
        ESP_LOGI(TAG, "Legacy SQLite schema migrated to v5");
    }
    return rc;

rollback:
    (void)db_exec(db, "ROLLBACK;");
    ESP_LOGE(TAG, "Legacy SQLite schema migration failed: rc=%d, errmsg=%s",
             rc, sqlite3_errmsg(db));
    return rc;
}

static int ensure_schema_v5(sqlite3 *db, int reported_user_version)
{
    if (db == NULL)
    {
        return SQLITE_MISUSE;
    }

    if (!sqlite_table_exists(db, SQLITE_CACHE_TABLE_NAME))
    {
        ESP_LOGI(TAG, "SQLite data_cache table does not exist; creating v5 schema");
        return create_schema_v5(db);
    }

    if (sqlite_schema_is_v5(db))
    {
        /*
         * 真实结构已经是 v5。即使 PRAGMA user_version 返回 0 也绝不能迁移。
         * 只补齐索引并尝试重新写版本标记。
         */
        if (reported_user_version != SQLITE_SCHEMA_VERSION)
        {
            ESP_LOGW(TAG,
                     "SQLite table structure is already v5 but user_version=%d; "
                     "ignoring version mismatch and keeping existing data",
                     reported_user_version);
        }

        return create_schema_v5(db);
    }

    ESP_LOGW(TAG,
             "SQLite legacy table detected by schema probe (user_version=%d); migrating to v5",
             reported_user_version);

    return migrate_legacy_schema_to_v5(db);
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

    rc = ensure_schema_v5(s_db, version);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG,
                 "SQLite schema create/migration failed: rc=%d, errmsg=%s. Database is preserved.",
                 rc,
                 sqlite3_errmsg(s_db));
        goto fail;
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

static int get_existing_local_id_locked(const char *sn, int seq_no, sqlite3_int64 *local_id)
{
    *local_id = -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT local_id FROM " SQLITE_CACHE_TABLE_NAME " "
        "WHERE sn=? AND seq_no=? LIMIT 1;";

    int rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        return rc;
    }

    sqlite3_bind_text(stmt, 1, sn, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, seq_no);

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
                           const char *sn,
                           int current_step,
                           int timestamp,
                           bool pushed,
                           const char *value_data)
{
    if (seq_no < 0 || sn == NULL || sn[0] == '\0' || value_data == NULL)
    {
        ESP_LOGE(TAG, "InsertStructuredRecord invalid parameter");
        return -1;
    }

    if (strlen(sn) >= DB_SN_MAX_LEN)
    {
        ESP_LOGE(TAG, "SN length invalid: %u bytes", (unsigned)strlen(sn));
        return -1;
    }

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
    int existing_rc = get_existing_local_id_locked(sn, seq_no, &existing_local_id);

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
            "SET current_step=?, timestamp=?, pushed=?, value_data=?, created_at=? "
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
        sqlite3_bind_int64(stmt, 6, existing_local_id);
    }
    else
    {
        const char *sql =
            "INSERT INTO " SQLITE_CACHE_TABLE_NAME " "
            "(seq_no, sn, current_step, timestamp, pushed, value_data, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?);";

        rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK)
        {
            ESP_LOGE(TAG, "Prepare insert failed: %s", sqlite3_errmsg(s_db));
            goto rollback;
        }

        sqlite3_bind_int(stmt, 1, seq_no);
        sqlite3_bind_text(stmt, 2, sn, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, current_step);
        sqlite3_bind_int(stmt, 4, timestamp);
        sqlite3_bind_int(stmt, 5, pushed ? 1 : 0);
        sqlite3_bind_text(stmt, 6, value_data, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, created_at);
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
             "structured record committed: seq_no=%d, sn=%s, step=%d, timestamp=%d, pushed=%d, value_len=%u",
             seq_no,
             sn,
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
                           const char *sn,
                           int current_step,
                           int timestamp,
                           bool pushed,
                           const double *values,
                           int value_count)
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

    int ret = InsertStructuredRecord(seq_no, sn, current_step, timestamp, pushed, value_json);
    free(value_json);
    return ret;
}

/* -------------------------------------------------------------------------- */
/* 更新                                                                        */
/* -------------------------------------------------------------------------- */

int UpdateRecordPushStateBySNAndID(const char *sn, int seq_no, bool pushed)
{
    if (sn == NULL || sn[0] == '\0' || seq_no < 0)
    {
        return -1;
    }

    if (!sqlite_lock()) return -1;
    if (!sqlite_ready_locked())
    {
        sqlite_unlock();
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "UPDATE " SQLITE_CACHE_TABLE_NAME " SET pushed=? WHERE sn=? AND seq_no=?;";

    int rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Prepare pushed update failed: %s", sqlite3_errmsg(s_db));
        sqlite_unlock();
        return -1;
    }

    sqlite3_bind_int(stmt, 1, pushed ? 1 : 0);
    sqlite3_bind_text(stmt, 2, sn, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, seq_no);

    rc = sqlite3_step(stmt);
    int changed = (rc == SQLITE_DONE) ? sqlite3_changes(s_db) : 0;
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE || changed <= 0)
    {
        ESP_LOGW(TAG, "Push-state update failed/not found: SN=%s seq_no=%d rc=%d",
                 sn, seq_no, rc);
        sqlite_unlock();
        return -1;
    }

    ESP_LOGI(TAG, "Push-state updated: SN=%s seq_no=%d pushed=%d",
             sn, seq_no, pushed ? 1 : 0);
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

    /* legacy：若多个SN拥有相同seq_no，只更新最近插入的一条。 */
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

static int query_one_locked(const char *sql,
                            const char *sn,
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
    if (sn != NULL)
    {
        sqlite3_bind_text(stmt, bind_index++, sn, -1, SQLITE_TRANSIENT);
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

int QueryStructuredRecordLatestBySN(const char *sn, QueryResult *out_result)
{
    if (sn == NULL || sn[0] == '\0' || out_result == NULL)
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

    const char *sql =
        "SELECT seq_no, sn, current_step, timestamp, pushed, value_data, created_at "
        "FROM " SQLITE_CACHE_TABLE_NAME " "
        "WHERE sn=? ORDER BY seq_no DESC, local_id DESC LIMIT 1;";

    int ret = query_one_locked(sql, sn, -1, -1, out_result);
    if (ret == 0)
    {
        ESP_LOGI(TAG, "Latest record found: SN=%s last_seq=%d pushed=%d",
                 sn, out_result->seq_no, out_result->pushed ? 1 : 0);
    }
    else
    {
        ESP_LOGI(TAG, "No cached record for SN=%s", sn);
    }

    sqlite_unlock();
    return ret;
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

    /* legacy：跨SN时返回最近插入的同seq_no记录。 */
    const char *sql =
        "SELECT seq_no, sn, current_step, timestamp, pushed, value_data, created_at "
        "FROM " SQLITE_CACHE_TABLE_NAME " "
        "WHERE seq_no=? ORDER BY local_id DESC LIMIT 1;";

    int ret = query_one_locked(sql, NULL, seq_no, -1, out_result);
    sqlite_unlock();
    return ret;
}

int QueryJsonRecordBySeq(int seq_no, QueryResult *out_result)
{
    return QueryStructuredRecordBySeq(seq_no, out_result);
}

void query_db1_latest_by_sn_to_global(const char *sn)
{
    memset(g_db1_result, 0, sizeof(QueryResult));
    (void)QueryStructuredRecordLatestBySN(sn, g_db1_result);
}

void query_db1_to_global(int target_id)
{
    memset(g_db1_result, 0, sizeof(QueryResult));
    (void)QueryStructuredRecordBySeq(target_id, g_db1_result);
}

int QueryStructuredRecordsBySNAndPushState(const char *sn,
                                           bool pushed,
                                           QueryResult *out_array,
                                           int max_count,
                                           int *out_count)
{
    if (out_count != NULL) *out_count = 0;

    if (sn == NULL || sn[0] == '\0' || out_array == NULL || max_count <= 0 || out_count == NULL)
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

    const char *sql =
        "SELECT seq_no, sn, current_step, timestamp, pushed, value_data, created_at "
        "FROM " SQLITE_CACHE_TABLE_NAME " "
        "WHERE sn=? AND pushed=? "
        "ORDER BY seq_no ASC, local_id ASC LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Prepare SN/pushed query failed: %s", sqlite3_errmsg(s_db));
        sqlite_unlock();
        return -1;
    }

    sqlite3_bind_text(stmt, 1, sn, -1, SQLITE_TRANSIENT);
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

int QueryLatestRecordBySNAndPushState(const char *sn,
                                      bool pushed,
                                      QueryResult *out_result)
{
    if (sn == NULL || sn[0] == '\0' || out_result == NULL)
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
    const char *sql =
        "SELECT seq_no, sn, current_step, timestamp, pushed, value_data, created_at "
        "FROM " SQLITE_CACHE_TABLE_NAME " "
        "WHERE sn=? AND pushed=? "
        "ORDER BY seq_no ASC, local_id ASC LIMIT 1;";

    int ret = query_one_locked(sql, sn, -1, pushed ? 1 : 0, out_result);
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

    const char *sql =
        "SELECT seq_no, sn, current_step, timestamp, pushed, value_data, created_at "
        "FROM " SQLITE_CACHE_TABLE_NAME " "
        "WHERE pushed=0 ORDER BY local_id ASC LIMIT ?;";

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

int InsertJsonRecord(int seq_no, const char *sn, bool pushed, const char *json_data)
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

    const char *final_sn = sn;
    int final_seq_no = seq_no;
    int final_timestamp = 0;
    int final_step = 0;
    char value_data[DB_VALUE_DATA_MAX_LEN + 1] = {0};

    cJSON *sn_item = cJSON_GetObjectItemCaseSensitive(payload, "SN");
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(payload, "IDNUM");
    cJSON *timestamp_item = cJSON_GetObjectItemCaseSensitive(payload, "TIMESTAMP");
    cJSON *step_item = cJSON_GetObjectItemCaseSensitive(payload, "CurrentStep");
    cJSON *value_item = cJSON_GetObjectItemCaseSensitive(payload, "Value");

    if ((final_sn == NULL || final_sn[0] == '\0') &&
        cJSON_IsString(sn_item) && sn_item->valuestring != NULL)
    {
        final_sn = sn_item->valuestring;
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

    if (final_sn == NULL || final_sn[0] == '\0' || final_seq_no < 0 || final_timestamp <= 0)
    {
        cJSON_Delete(root);
        return -1;
    }

    int ret = InsertStructuredRecord(final_seq_no,
                                     final_sn,
                                     final_step,
                                     final_timestamp,
                                     pushed,
                                     value_data);
    cJSON_Delete(root);
    return ret;
}

/* -------------------------------------------------------------------------- */
/* 初始化入口                                                                   */
/* -------------------------------------------------------------------------- */

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

    esp_err_t ret = mount_fatfs_storage();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "FATFS mount failed; SQLite is disabled to preserve existing data");
        return;
    }

    g_db1_result=(QueryResult *)app_malloc_prefer_psram(sizeof(QueryResult));
    snprintf(db1_name, sizeof(db1_name), "%s", DB_FILE_PATH);
    memset(g_db1_result, 0, sizeof(QueryResult));

    if (!sqlite_lock())
    {
        return;
    }

    int rc = init_database_locked();
    sqlite_unlock();

    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG,
                 "SQLite initialization failed: rc=%d. No database file was automatically deleted.",
                 rc);
    }

    esp_log_level_set(TAG, ESP_LOG_INFO);
}
