#ifndef SQLITE_H
#define SQLITE_H

#include <stdbool.h>
#include "esp_err.h"
#include "sqllib1.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define FATFS_BASE_PATH "/fatfs"
#define FATFS_PARTITION_LABEL "data_storage"
#define DB_FILE_PATH FATFS_BASE_PATH "/data.db"

/*
 * data_storage 分区约 2 MB。Value 为变长 JSON，单条最大 2 KB。
 * 记录数上限取 1200，同时保留至少约 512 KB 文件系统余量；
 * 缓存满时优先淘汰 pushed=1 的最旧记录。
 *
 * schema v5：local_id 为数据库内部主键，(sn, seq_no) 唯一。
 * seq_no/IDNUM 可以在每个新的 SN 中从 0 重新开始，不会覆盖其他 SN。
 */
#define MAX_RECORDS_COUNT          1200
#define DB_MIN_FREE_BYTES          (512 * 1024)

#define SQLITE_CACHE_TABLE_NAME "data_cache"
#define SQLITE_SCHEMA_VERSION 5

    extern QueryResult *g_db1_result;

    esp_err_t mount_fatfs_storage(void);
    void unmount_fatfs_storage(void);
    void print_fatfs_usage(void);
    /* 仅人工维修/调试使用；正常初始化不会自动删除数据库。 */
    void cleanup_database_files(void);
    void print_db_files(void);
    void SqLite_Init(void);

    /*
     * 推荐新版接口：结构化保存 DataReport。
     * value_data 传入完整 `Value` 数组 JSON。
     */
    int InsertStructuredRecord(int seq_no,
                               const char *sn,
                               int current_step,
                               int timestamp,
                               bool pushed,
                               const char *value_data);

    /* 兼容接口：从 double 数组生成 Legacy 结构化 Value JSON 后保存。 */
    int InsertDataReportValues(int seq_no,
                               const char *sn,
                               int current_step,
                               int timestamp,
                               bool pushed,
                               const double *values,
                               int value_count);

    int QueryStructuredRecordBySeq(int seq_no, QueryResult *out_result);
    int UpdateRecordPushState(int seq_no, bool pushed);
    int QueryUnpushedRecords(QueryResult *out_array, int max_count, int *out_count);

    /*
     * 兼容接口：传入完整 JSON，内部提取并保存结构化 Value 数组。
     * 同时兼容旧的扁平 Value 数组。
     */
    int InsertJsonRecord(int seq_no, const char *sn, bool pushed, const char *json_data);
    int QueryJsonRecordBySeq(int seq_no, QueryResult *out_result);



    int QueryStructuredRecordLatestBySN(const char *sn, QueryResult *out_result);
    void query_db1_latest_by_sn_to_global(const char *sn);

    int QueryStructuredRecordsBySNAndPushState(const char *sn,
                                               bool pushed,
                                               QueryResult *out_array,
                                               int max_count,
                                               int *out_count);

    int QueryLatestRecordBySNAndPushState(const char *sn,
                                          bool pushed,
                                          QueryResult *out_result);

    int UpdateRecordPushStateBySNAndID(const char *sn, int seq_no, bool pushed);

    void query_db1_to_global(int target_id);

#ifdef __cplusplus
}
#endif

#endif
