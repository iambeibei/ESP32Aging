# SQLite first-boot FATFS provisioning fix

## Symptom confirmed by the test log

On a freshly erased device, `data_storage` has no FAT filesystem yet. With an unconditional
`format_if_mount_failed = false`, the first mount returns `FR_NO_FILESYSTEM/ESP_FAIL` and
`SqLite_Init()` exits. The later aging task therefore reports `SQLite database is not ready`.

## Final mount policy

1. Before the first mount, inspect the entire `data_storage` partition.
2. Always try a protected mount first (`format_if_mount_failed = false`).
3. If it mounts, use the existing filesystem and never format it.
4. If mount fails AND the partition was completely erased (`0xFF`), allow exactly one first-time
   auto-format/mount (`format_if_mount_failed = true`).
5. If mount fails AND the partition contains any data, preserve it and return an error. Do not
   format the partition automatically.

This separates first-time provisioning from power-loss/corruption handling.

## Expected first-boot log

A truly blank partition should show roughly:

- FATFS partition 'data_storage' is completely erased; first-time format is allowed
- FATFS mount failed on a blank partition ... performing one-time initialization
- Formatting FATFS partition ...
- FATFS first-time format/mount completed successfully at /fatfs
- SQLite current user_version=0, target=5
- SQLite cache record count=0
- Database initialized successfully: /fatfs/data.db

Subsequent boots should show a normal mount with no formatting.

## Important for a board that already ran the previous broken build

The previous failed wear-levelling mount may have written wear-levelling metadata even though no
FAT filesystem/database was created. Such a partition is no longer necessarily all `0xFF`.
Because the supplied log proves SQLite never became ready and therefore never stored aging data,
it is safe on this test board to erase only the `data_storage` partition once before testing this
build. With the current partition table the region is 0x490000..0x68FFFF (size 0x200000).

Do not erase this region on a deployed device that may contain valid SQLite records.
