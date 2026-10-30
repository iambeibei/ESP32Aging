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
 * schema v7：local_id 为数据库内部主键，(pn, record_id, seq_no) 唯一。
 * record_id 是老化会话身份（start_aging 下发）：同一 PN 重新开轮会写入新行，
 * 不再覆盖上一轮记录；补传 JSON 用记录自身 record_id，避免跨轮串批。
 * seq_no/IDNUM 可以在每个新的 PN 中从 0 重新开始，不会覆盖其他 PN。
 * pushed 三态：0=pending 待发；1=inflight 已交 SDK 等 PUBACK；2=acked 已确认。
 * 缓存满时淘汰顺序 pushed DESC（acked 先淘汰，pending 最后），待发记录最受保护。
 */
#define MAX_RECORDS_COUNT          1200
#define DB_MIN_FREE_BYTES          (512 * 1024)

#define SQLITE_CACHE_TABLE_NAME "data_cache"
#define SQLITE_SCHEMA_VERSION 7

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
     * record_id 为采样时固化的老化会话 RecordId（可传 NULL，按空串处理）。
     */
    int InsertStructuredRecord(int seq_no,
                               const char *pn,
                               int current_step,
                               int timestamp,
                               bool pushed,
                               const char *value_data,
                               const char *record_id);

    /* 兼容接口：从 double 数组生成 Legacy 结构化 Value JSON 后保存。 */
    int InsertDataReportValues(int seq_no,
                               const char *pn,
                               int current_step,
                               int timestamp,
                               bool pushed,
                               const double *values,
                               int value_count,
                               const char *record_id);

    int QueryStructuredRecordBySeq(int seq_no, QueryResult *out_result);
    /* 精确会话查询；不按 pushed 过滤。out_result 必须由调用方在堆上分配。
     * seq_no=-1 查询本会话最大编号，其余值查询指定编号。0=找到，-1=无记录，
     * -2=参数/数据库错误。任务上下文，内部持 DB 锁。 */
    int QueryStructuredRecordBySession(const char *pn, const char *record_id,
                                       int seq_no, QueryResult *out_result);
    int UpdateRecordPushState(int seq_no, bool pushed);
    int QueryUnpushedRecords(QueryResult *out_array, int max_count, int *out_count);

    /*
     * 兼容接口：传入完整 JSON，内部提取并保存结构化 Value 数组。
     * 同时兼容旧的扁平 Value 数组。
     */
    int InsertJsonRecord(int seq_no, const char *pn, bool pushed, const char *json_data, const char *record_id);
    int QueryJsonRecordBySeq(int seq_no, QueryResult *out_result);



    int QueryStructuredRecordLatestByPN(const char *pn, QueryResult *out_result);
    void query_db1_latest_by_pn_to_global(const char *pn);

    int QueryStructuredRecordsByPNAndPushState(const char *pn,
                                               bool pushed,
                                               QueryResult *out_array,
                                               int max_count,
                                               int *out_count);

    int QueryLatestRecordByPNAndPushState(const char *pn,
                                          bool pushed,
                                          QueryResult *out_result);

    /* record_id 限定更新范围：同一 PN 重开轮后旧轮记录不受影响。
     * push_state 三态：0=pending 待发；1=inflight 已交 SDK 等 PUBACK；2=acked 已确认。 */
    int UpdateRecordPushStateByPNAndID(const char *pn, const char *record_id, int seq_no, int push_state);

    void query_db1_to_global(int target_id);

#ifdef __cplusplus
}
#endif

#endif
