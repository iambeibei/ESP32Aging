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
 * data_storage 分区 0x260000 = 2 428 928 B（约 2.32 MB，见 partition.csv）。
 * Value 为变长 JSON，单条上限 DB_VALUE_DATA_MAX_LEN = 2048 B。
 *
 * 容量推导：1200 条 × 2048 B = 2 457 600 B，已略大于分区容量；也就是说
 * 记录数上限实际上不可达，真正在前面试图拦住写入的是 DB_MIN_FREE_BYTES
 * （剩余空间低于 512 KB 即触发淘汰/拒绝写入）。实测 Value 平均只有几百字节，
 * 因此 1200 条是"够用且偏保守"的上限，暂不下调；如需调整必须先用
 * print_fatfs_usage() 实测可用空间与 get_record_count_locked() 实测条数再定。
 *
 * 淘汰顺序：只淘汰已上传(pushed=1)中最旧的一条（按 local_id，单调递增）。
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
     * InsertStructuredRecord() 返回码：
     *   0  (SQLITE_INSERT_OK)              成功
     *  -1  (SQLITE_INSERT_ERR_GENERAL)     参数/Prepare/执行/提交失败
     *  -2  (SQLITE_INSERT_ERR_CACHE_FULL)  缓存满且没有已上传(pushed=1)记录可淘汰
     *
     * 契约：未上传(pushed=0)的记录**绝不淘汰**。缓存满时宁可拒绝本次写入（返回 -2），
     * 也不能覆盖尚未上传的数据 —— 断网期间本地库是这些数据的唯一副本。
     * 调用方必须把 -2 当作"可恢复的限流"处理：记录并告警后继续老化，不得中止工艺。
     */
#define SQLITE_INSERT_OK             0
#define SQLITE_INSERT_ERR_GENERAL    (-1)
#define SQLITE_INSERT_ERR_CACHE_FULL (-2)

    /*
     * 推荐新版接口：结构化保存 DataReport。
     * value_data 传入完整 `Value` 数组 JSON。
     * 返回 0 成功；-2 表示缓存满且无已上传记录可淘汰（未上传数据不受影响）。
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
