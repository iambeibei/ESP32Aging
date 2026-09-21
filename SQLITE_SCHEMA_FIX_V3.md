# SQLite schema recovery fix v3

## Root cause found from full test log

The first aging session successfully persisted seq_no 0..4. After reboot, `/fatfs/data.db` still existed and retained the same 24576-byte size, but `PRAGMA user_version` reported 0. The previous code required both `user_version >= 5` and the `local_id` column to accept the table as v5. It therefore misclassified an already-v5 table as legacy and attempted `ALTER TABLE data_cache RENAME ...`, which failed on the embedded SQLite build. SQLite initialization was then disabled.

## Fix

1. Schema version is now determined primarily from the real table columns, not `PRAGMA user_version`.
2. If `data_cache` contains all v5 columns (`local_id`, `seq_no`, `sn`, `current_step`, `timestamp`, `pushed`, `value_data`, `created_at`), it is accepted as v5 even when `user_version` is 0.
3. `PRAGMA user_version` is kept only as a diagnostic/version hint and is no longer trusted as the migration decision source.
4. Legacy v3/v4 migration no longer uses `ALTER TABLE ... RENAME`. It uses a transaction with a temporary v5 table, copies the data, rebuilds the main table, copies data back, creates indexes, and commits.
5. No automatic database deletion or FATFS formatting is added by this fix.

## Expected reboot log

On the board from the supplied test, the next boot should ideally show:

- FATFS mounted at /fatfs
- /fatfs/data.db exists, size=24576
- SQLite current user_version=0, target=5
- SQLite table structure is already v5 but user_version=0; ignoring version mismatch and keeping existing data
- SQLite cache record count=5
- Database initialized successfully

When aging recovery starts for `AC702336910071810`, the upload task should query the latest record and continue from seq_no 5.

Do NOT erase `data_storage` before this verification; the prior failed migration used a rollback path and the database was preserved.
