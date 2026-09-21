#ifndef SQLLIB1_H
#define SQLLIB1_H

#include <stdbool.h>
#include <stddef.h>
#include "sqlite3.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SQLite 只保存 DataReport 中可重构的字段。
 * value_data 现在统一保存完整的结构化 `Value` 数组 JSON，例如：
 * [{"Name":"Device1","Data":[{"name":"SOC","value":"74.00"}]}]
 *
 * 这样实时 MQTT 与 SQLite 补发共用同一份 Value，不再维护第二套字段映射。
 */
#define DB_SN_MAX_LEN             64
#define DB_STEP_MAX_LEN           32
#define DB_VALUE_DATA_MAX_LEN     2048
/* 完整上报 JSON = Value(<=2048) + 外层字段，预留足够余量。 */
#define DB_JSON_DATA_MAX_LEN      2560
#define DB_VALUE_MAX_COUNT        32

/*
 * 为了降低旧 UDP 代码的编译影响，结构体保留少量旧字段：
 *   IDNUM     = seq_no
 *   timestamp = TIMESTAMP
 *   json_data = 根据结构化字段重构出来的完整 DataReport JSON
 * 旧电压/电流字段仅用于兼容旧调用，不作为真实数据库字段保存。
 */
typedef struct
{
    int seq_no;
    char SN[DB_SN_MAX_LEN];
    short current_step;
    int timestamp;
    bool pushed;
    char value_data[DB_VALUE_DATA_MAX_LEN + 1];
    char json_data[DB_JSON_DATA_MAX_LEN + 1];
    int created_at;

    /* legacy compatibility */
    int IDNUM;
    float Equipmentvoltage;
    float EquipmentCurrent;
    float Testvoltage;
    float TestCurrent;
} QueryResult;

int db_open(const char *filename, sqlite3 **db);
int db_exec(sqlite3 *db, const char *sql);
int db_query_to_variable(sqlite3 *db, const char *sql, QueryResult *result);
void db_print_result(const char *db_name, const QueryResult *result);
int db_print_query(sqlite3 *db, const char *sql);

/* 工具函数：把结构化 Value JSON 重构成完整 DataReport JSON */
int db_build_data_report_json(const QueryResult *record, char *out_json, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
