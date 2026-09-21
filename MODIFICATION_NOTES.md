# Light_Gateway 老化数据链路修改说明

## 本次修改

1. 老化数据实时 MQTT 与 SQLite 共用同一份结构化 `Value` JSON，不再使用 `build_aging_named_value_data()`。
2. 老化设备始终存在：
   - 外层 `Name` 使用老化设备 `Type`（带运行时名称 fallback）。
   - 内层参数 `name` 使用 `SampleData.Value` 中的参数 ID 调用 `AgingDataName()` 反查协议名称；查询失败时保留原始 ID。
3. 外接设备为可选（0~N）：
   - 外层 `Name` 使用外接设备自己的 `name`。
   - 内层参数 `name` 直接使用其 `SampleData.Value`。
   - 当前步骤无匹配采样项时跳过该外接设备，不生成空 `Data`。
4. 上传队列改为 `AgingUploadPacket *`：采样时固化 SN、CurrentStep、TIMESTAMP 和 Value，避免异步上传时步骤/SN串数据。
5. 最大单次采样值数量由 10 提高到 32。
6. SQLite：
   - schema 升级到 v4；旧 v3 自动迁移。
   - `value_data` 改为保存完整 `Value` 数组 JSON，最大 2048 bytes。
   - 最多 1200 条记录。
   - FATFS 剩余空间低于约 512 KiB 时提前采用覆盖策略。
   - 仍然保存每一条数据：MQTT publish 接受成功写 `pushed=1`，否则 `pushed=0`；覆盖时优先淘汰旧的 `pushed=1`。
   - 历史旧格式补发时会转换成新的 `Value` 数组格式。
7. `parse_action()`：Action 缺失/null/{} 均视为合法“无动作”；Action 存在但类型错误仍报错；实际动作项缺必要参数仍报错。
8. 修复外接设备 Action 执行时错误地只检查/使用 `ParaID` 的问题；`is_name=1` 时改用 `ParaName`。
9. 修复部分 Linux/CI 下大小写敏感的源文件/include 名称。

## 新的 Value 示例

```json
[
  {
    "Name": "Device1",
    "Data": [
      {"name": "SOC", "value": "74.00"},
      {"name": "Voltage", "value": "52.66"}
    ]
  },
  {
    "Name": "Device2",
    "Data": [
      {"name": "SOC", "value": "74.00"},
      {"name": "Voltage", "value": "52.66"}
    ]
  }
]
```

没有外接设备时，`Value` 仅包含第一组老化设备。

## 说明

当前 `pushed=1` 仍沿用原工程语义：以 `app_mqtt_publish() > 0` 为成功条件，即 MQTT 客户端接受了 QoS1 publish 请求；本次没有改成等待 `MQTT_EVENT_PUBLISHED` PUBACK 后再更新 pushed。

## SQLite corruption hardening

针对运行时 `SQLITE: insert/update step failed: rc=11, errmsg=database disk image is malformed`：

- `PRAGMA journal_mode` 从 `MEMORY` 改为 `DELETE`。
- `PRAGMA synchronous` 从 `NORMAL` 改为 `FULL`。
- `PRAGMA locking_mode` 从 `EXCLUSIVE` 改为 `NORMAL`。
- 启动时在 schema migration 前后执行 `PRAGMA quick_check`。
- 发现旧 `data.db` 损坏/NOTADB 时，只删除 SQLite 数据库相关文件并重建 schema v4，不格式化整个 data_storage 分区。
- 修正 push-state 更新接口，使其也通过统一 `db_open()` 并应用一致的 SQLite PRAGMA。

注意：已经损坏的 SQLite 数据无法保证恢复；自动重建会丢失该损坏库中的历史/未补发记录，但不会清除 NVS、SPIFFS 等其他分区数据。

## SQLite schema detection fix v3
- Do not trust `PRAGMA user_version` as the sole/primary schema detector on the embedded SQLite component.
- Detect v5 by actual columns; an existing v5 table is retained even if user_version reports 0.
- Remove `ALTER TABLE ... RENAME` from legacy migration; use transactional temporary-table copy/rebuild instead.
- This specifically fixes the reboot failure observed after seq_no 0..4 were successfully persisted.
