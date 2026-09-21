#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "esp_timer.h"
#include "sqlite3.h"
#include "sqllib1.h"
#include "cJSON.h"
#include <time.h>

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

static void scaled_int_to_string(int scaled_value, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0)
    {
        return;
    }

    int abs_value = scaled_value >= 0 ? scaled_value : -scaled_value;
    int integer_part = abs_value / 100;
    int decimal_part = abs_value % 100;

    if (scaled_value < 0)
    {
        snprintf(out, out_size, "-%d.%02d", integer_part, decimal_part);
    }
    else
    {
        snprintf(out, out_size, "%d.%02d", integer_part, decimal_part);
    }
}

/*
 * 兼容 schema v3 时代留下的 value_data：
 *   "SOC=7400,Voltage=5266" 或 "7400,5266"
 * 迁移到 v4 后这些旧记录仍然可以按新 Value 数组格式补发。
 */
static cJSON *build_legacy_value_array(const char *legacy_value_data)
{
    cJSON *value_array = cJSON_CreateArray();
    if (value_array == NULL)
    {
        return NULL;
    }

    if (legacy_value_data == NULL || legacy_value_data[0] == '\0')
    {
        return value_array;
    }

    char temp[DB_VALUE_DATA_MAX_LEN + 1];
    safe_copy(temp, sizeof(temp), legacy_value_data);

    cJSON *group = cJSON_CreateObject();
    cJSON *data_array = cJSON_CreateArray();
    if (group == NULL || data_array == NULL)
    {
        cJSON_Delete(group);
        cJSON_Delete(data_array);
        cJSON_Delete(value_array);
        return NULL;
    }

    cJSON_AddStringToObject(group, "Name", "Legacy");
    cJSON_AddItemToObject(group, "Data", data_array);

    char *saveptr = NULL;
    char *token = strtok_r(temp, ",", &saveptr);
    int index = 0;

    while (token != NULL)
    {
        char *equal = strchr(token, '=');
        const char *name = NULL;
        const char *scaled_text = token;
        char generated_name[24];

        if (equal != NULL)
        {
            *equal = '\0';
            name = token;
            scaled_text = equal + 1;
        }
        else
        {
            snprintf(generated_name, sizeof(generated_name), "Value%d", index);
            name = generated_name;
        }

        cJSON *item = cJSON_CreateObject();
        if (item == NULL)
        {
            cJSON_Delete(value_array);
            cJSON_Delete(group);
            return NULL;
        }

        char value_string[32];
        scaled_int_to_string(atoi(scaled_text), value_string, sizeof(value_string));
        cJSON_AddStringToObject(item, "name", name);
        cJSON_AddStringToObject(item, "value", value_string);
        cJSON_AddItemToArray(data_array, item);

        index++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    if (index > 0)
    {
        cJSON_AddItemToArray(value_array, group);
    }
    else
    {
        cJSON_Delete(group);
    }

    return value_array;
}

static cJSON *parse_or_convert_value_array(const char *value_data)
{
    if (value_data == NULL || value_data[0] == '\0')
    {
        return NULL;
    }

    cJSON *value_array = cJSON_Parse(value_data);
    if (cJSON_IsArray(value_array))
    {
        return value_array;
    }

    cJSON_Delete(value_array);
    return build_legacy_value_array(value_data);
}

int db_build_data_report_json(const QueryResult *record,
                              char *out_json,
                              size_t out_size)
{
    if (record == NULL || out_json == NULL || out_size == 0)
    {
        return -1;
    }

    out_json[0] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *value_array = parse_or_convert_value_array(record->value_data);
    if (root == NULL || data == NULL || value_array == NULL)
    {
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(value_array);
        return -1;
    }

    cJSON_AddNumberToObject(root, "Seq", (double)time(NULL));
    cJSON_AddItemToObject(root, "Data", data);
    cJSON_AddNumberToObject(data, "IDNUM", record->seq_no);
    cJSON_AddNumberToObject(data, "TIMESTAMP", record->timestamp);
    cJSON_AddStringToObject(data, "SN", record->SN);
    cJSON_AddNumberToObject(data, "CurrentStep", record->current_step);
    cJSON_AddItemToObject(data, "Value", value_array);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
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

    memcpy(out_json, json, len + 1);
    free(json);
    return 0;
}

static void fill_query_result_from_stmt(sqlite3_stmt *stmt, QueryResult *result)
{
    if (stmt == NULL || result == NULL)
    {
        return;
    }

    memset(result, 0, sizeof(QueryResult));

    result->seq_no = sqlite3_column_int(stmt, 0);

    const unsigned char *sn = sqlite3_column_text(stmt, 1);
    safe_copy(result->SN, sizeof(result->SN), (const char *)sn);

    result->current_step = (short)sqlite3_column_int(stmt, 2);
    result->timestamp = sqlite3_column_int(stmt, 3);
    result->pushed = sqlite3_column_int(stmt, 4) ? true : false;

    const unsigned char *value_data = sqlite3_column_text(stmt, 5);
    safe_copy(result->value_data, sizeof(result->value_data), (const char *)value_data);

    result->created_at = sqlite3_column_int(stmt, 6);

    /* legacy compatibility */
    result->IDNUM = result->seq_no;
    

    (void)db_build_data_report_json(result, result->json_data, sizeof(result->json_data));
}

int db_open(const char *filename, sqlite3 **db)
{
    if (filename == NULL || db == NULL)
    {
        printf("Invalid parameters to db_open\n");
        return SQLITE_MISUSE;
    }

    *db = NULL;
    int rc = sqlite3_open(filename, db);
    if (rc != SQLITE_OK)
    {
        printf("Can't open database %s: rc=%d, errmsg=%s\n",
               filename,
               rc,
               (*db != NULL) ? sqlite3_errmsg(*db) : "unknown");

        /* sqlite3_open失败时仍可能返回需要close的句柄，必须释放。 */
        if (*db != NULL)
        {
            sqlite3_close(*db);
            *db = NULL;
        }
        return rc;
    }

    sqlite3_extended_result_codes(*db, 1);
    sqlite3_busy_timeout(*db, 5000);
    return SQLITE_OK;
}

int db_exec(sqlite3 *db, const char *sql)
{
    if (db == NULL || sql == NULL)
    {
        printf("Invalid parameters to db_exec\n");
        return SQLITE_ERROR;
    }

    char *zErrMsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);

    if (rc != SQLITE_OK)
    {
        int err = sqlite3_errcode(db);
        int exterr = sqlite3_extended_errcode(db);

        printf("SQL error:\n");
        printf("  sql     = %s\n", sql);
        printf("  rc      = %d\n", rc);
        printf("  err     = %d\n", err);
        printf("  exterr  = %d\n", exterr);
        printf("  errmsg  = %s\n", zErrMsg ? zErrMsg : sqlite3_errmsg(db));

        if (zErrMsg)
        {
            sqlite3_free(zErrMsg);
        }
    }

    return rc;
}

/*
 * 通用查询函数。
 * 传入 SQL 必须按如下列顺序返回：
 *   seq_no, sn, current_step, timestamp, pushed, value_data, created_at
 */
int db_query_to_variable(sqlite3 *db, const char *sql, QueryResult *result)
{
    if (db == NULL || sql == NULL || result == NULL)
    {
        printf("Invalid parameters to db_query_to_variable\n");
        return SQLITE_ERROR;
    }

    memset(result, 0, sizeof(QueryResult));

    printf("Query to variable: %s\n", sql);
    int64_t start = esp_timer_get_time();

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        printf("Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return rc;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        fill_query_result_from_stmt(stmt, result);
        db_print_result("Query", result);
        rc = SQLITE_OK;
    }
    else if (rc == SQLITE_DONE)
    {
        printf("No data found\n");
        rc = SQLITE_NOTFOUND;
    }
    else
    {
        printf("SQLite step error: rc=%d, errmsg=%s\n", rc, sqlite3_errmsg(db));
    }

    sqlite3_finalize(stmt);

    printf("Time taken: %lld us\n", esp_timer_get_time() - start);
    return rc;
}

void db_print_result(const char *db_name, const QueryResult *result)
{
    if (db_name == NULL || result == NULL)
    {
        printf("Invalid parameters to db_print_result\n");
        return;
    }

    printf("\n=== %s Query Result ===\n", db_name);
    printf("seq_no:       %d\n", result->seq_no);
    printf("SN:           %s\n", result->SN);
    printf("current_step: %d\n", result->current_step);
    printf("timestamp:    %d\n", result->timestamp);
    printf("pushed:       %s\n", result->pushed ? "true" : "false");
    printf("value_data:   %s\n", result->value_data);
    printf("created_at:   %d\n", result->created_at);
    printf("json_data:    %s\n", result->json_data);
    printf("========================\n\n");
}

int db_print_query(sqlite3 *db, const char *sql)
{
    if (db == NULL || sql == NULL)
    {
        return SQLITE_ERROR;
    }

    printf("Printing query: %s\n", sql);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        printf("Failed to prepare: %s\n", sqlite3_errmsg(db));
        return rc;
    }

    int row_count = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        row_count++;
        int col_count = sqlite3_column_count(stmt);

        printf("Row %d: ", row_count);
        for (int i = 0; i < col_count; i++)
        {
            const unsigned char *text = sqlite3_column_text(stmt, i);
            printf("%s", text ? (const char *)text : "NULL");
            if (i < col_count - 1)
            {
                printf(", ");
            }
        }
        printf("\n");
    }

    if (rc != SQLITE_DONE)
    {
        printf("Query step error: rc=%d, errmsg=%s\n", rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return rc;
    }

    printf("Total %d row(s) returned\n", row_count);

    sqlite3_finalize(stmt);
    return SQLITE_OK;
}
