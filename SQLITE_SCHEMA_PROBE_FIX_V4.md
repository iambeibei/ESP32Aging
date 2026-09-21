# SQLite schema probe fix v4

Root cause from reboot logs:

- First-run CREATE TABLE v5 contains `seq_no` and `local_id`.
- `InsertStructuredRecord()` prepares and commits INSERT statements using `seq_no`, proving the column exists during normal operation.
- After reboot, FATFS mounts and `data.db` is preserved, but `PRAGMA table_info(data_cache)`-based probing reports `seq_no` missing.
- The same embedded SQLite build already shows non-standard/unreliable PRAGMA result behavior (`quick_check` returns no row, `user_version` reads 0 after reboot).

Fix:

1. Stop using `PRAGMA table_info()` for column detection.
2. Probe columns with a normal prepared SELECT, e.g. `SELECT seq_no FROM data_cache LIMIT 0;`.
   `sqlite3_prepare_v2()` validates column existence without reading any rows.
3. Continue treating `PRAGMA user_version` as advisory only.
4. Log the persisted `CREATE TABLE` SQL from `sqlite_master` at boot.
5. Do not delete/reformat the database on schema-probe failure.

Expected reboot behavior for an existing v5 database:

- FATFS mounts.
- `/fatfs/data.db` exists.
- `SQLite table definition [data_cache]: CREATE TABLE ... local_id ... seq_no ...`
- `SQLite table structure is already v5 but user_version=0; ... keeping existing data`
- cache record count is retained.
- database ready=1.
- aging recovery queries the last record by SN and continues IDNUM from last+1.
