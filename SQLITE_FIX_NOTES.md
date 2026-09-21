# SQLite 掉电恢复完整修复

本版本基于 `Light_Gateway_modified_v2`，对 SQLite 从挂载、初始化、保存、查询到断电重启恢复做了整体重构。

## 1. FATFS

- `format_if_mount_failed = false`：挂载失败绝不自动格式化 2 MB `data_storage` 分区。
- `max_files = 8`。
- `allocation_unit_size = 4096`。
- FATFS 挂载失败时 SQLite 停用并保留原数据，不继续创建/删除数据库。

## 2. SQLite 连接与并发

- 改为一个常驻 `sqlite3 *s_db` 连接，不再每次 INSERT/SELECT/UPDATE 都反复 open/close。
- 所有 SQLite 访问由全局 FreeRTOS mutex 串行化。
- 修复 `sqlite3_open()` 失败时失败句柄未 close 的资源泄漏。
- `busy_timeout = 5000 ms`。

## 3. 掉电安全参数

- `journal_mode = DELETE`
- `synchronous = FULL`
- `locking_mode = NORMAL`
- 新建数据库在建表前设置 `page_size = 4096`。
- 不主动删除 `data.db-journal`；掉电后由 SQLite 自己利用 hot journal 回滚未完成事务。

## 4. 完整性检查

- 修复 `SQLITE_DONE(101)` 被误判为 `SQLITE_CORRUPT(11)` 的错误。
- `quick_check` 有结果行时必须为 `ok`；无结果行时回退到 `sqlite_master` 基础可读性检查。
- 检查失败只报错并保留数据库，不再自动 `unlink(data.db)` 或重建数据库。

## 5. Schema v5

旧版将 `seq_no(IDNUM)` 作为全库主键，但业务上每个 SN 都可能从 IDNUM=0 开始，会覆盖其他设备的数据。

v5 改为：

- `local_id INTEGER PRIMARY KEY`：数据库内部唯一键。
- `UNIQUE(sn, seq_no)`：每个 SN 内 IDNUM 唯一。
- 旧 v3/v4 表启动时事务迁移到 v5，保留已有记录。

## 6. 保存顺序

老化数据现在：

1. 先 `InsertStructuredRecord(..., pushed=false)`，事务 COMMIT 完成。
2. 再 MQTT publish。
3. publish 被 MQTT 客户端接受后将对应 `(SN, IDNUM)` 更新为 `pushed=1`。

因此掉电最坏情况是某条已经发过的数据仍保持 `pushed=0`，重启后可能重复补发一次；不会出现 MQTT 已发出但本地还没保存导致的缓存数据丢失窗口。

## 7. 掉电恢复 IDNUM

`app_AgingData_Upload_handle()` 在恢复后的第一包数据到来时：

- 按 SN 查询 SQLite 最新记录。
- `next IDNUM = last seq_no + 1`。
- 查不到历史记录时从 0 开始。
- 新的不同 SN 自动从 0 开始。

## 8. 2 MB 容量策略

- `Value` JSON 最大 2048 B。
- 最大 1200 条记录。
- FATFS 尽量保留至少 512 KB 空闲空间。
- 达到记录上限或低剩余空间时优先淘汰最旧的 `pushed=1`；只有没有已上传数据可淘汰时才会影响 `pushed=0`。

## 9. 推荐断电测试日志

断电前应看到：

```
structured record committed: seq_no=3, sn=..., pushed=0
Push-state updated: SN=..., seq_no=3, pushed=1
```

重新上电应看到：

```
FATFS mounted at /fatfs
SQLite cache record count=N
Database initialized successfully: /fatfs/data.db
Latest record found: SN=..., last_seq=3
Resume aging upload from SQLite: SN=..., last_id=3, next_id=4
```

随后新的记录从 `seq_no=4` 继续。
