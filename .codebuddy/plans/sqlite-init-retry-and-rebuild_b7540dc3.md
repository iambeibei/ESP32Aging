---
name: sqlite-init-retry-and-rebuild
overview: SqLite_Init() 启动初始化增加重试与受控重建：FATFS mount 失败重试 3 次（吸收偶发失败）后仍失败则格式化重建分区（用户决策，覆盖"非空绝不格式化"哲学，每上电一次守卫）；DB 文件损坏（quick_check/schema 失败）则 rename 留档 .corrupt 后重建空库。所有重置写 NVS 标志，由 appTask.c Init_ByNetwork_Flag 在网络就绪后 MQTT 上报 event/warn（补发契约：服务器必须知道缓存已丢）。正常路径行为零变化。
todos:
  - id: sqlite-retry-rebuild
    content: SqLite.c：SqLite_Init 增加 mount 重试(3×2s)、两级重置（mount 耗尽→格式化重建分区；DB 损坏→rename .corrupt 留档+重建空库）、每上电守卫与 NVS 标志写入
    status: completed
  - id: mqtt-deferred-report
    content: appTask.c：Init_ByNetwork_Flag 在 MQTT 就绪后读 NVS 重置标志，经 device/%s/event/warn 上报并清除，保证补发契约闭环
    status: completed
  - id: build-and-review
    content: 编译验证零告警、正常路径日志零变化；用 [skill:embedded-coding-standard] 对照本地规范自审本次改动
    status: completed
---

## 产品概述

在设备重启初始化阶段增强 SQLite 存储层的自愈能力：解决"持久损坏后重启永远无法恢复"的问题。用户确认方案为"初始化时尝试几次，不行就重置 SQLite 区域"。

## 核心功能

- **mount 重试**：FATFS 挂载失败时重试 3 次（间隔 2s），吸收 flash 读取延迟、wear-levelling 句柄竞争等偶发失败；正常路径行为零变化
- **两级受控重置**（用户"重置区域"的精确化，最小化破坏面）：
  - mount 重试耗尽仍失败（文件系统级损坏，数据已不可读）→ 格式化重建分区（format_if_mount_failed=true 重挂），覆盖现有"非空分区绝不格式化"哲学（用户明确决策）
  - mount 成功但 DB 文件 init 失败（quick_check/schema 损坏）→ `rename` 留档 `.corrupt`（rename 失败则 unlink）→ 重建空库，不动分区
- **每上电一次守卫**：`s_reset_done_this_boot` 静态标志，防止重置后再次失败触发循环
- **上报闭环**：重置发生时 MQTT 尚未连接（SqLite_Init 在 app_task_init 早期、mesh/MQTT 之前），故重置结果写 NVS 标志，由 `Init_ByNetwork_Flag`（MQTT 就绪后）读取并经 `device/%s/event/warn` 上报服务器后清除——保证"断网补发契约"不被静默破坏（服务器必须知晓该节点缓存已丢）

## 边界

- OTA 不碰 data_storage 分区（既有事实），本计划不改分区表、不改两阶段挂载/迁移/Insert/Query 既有逻辑，只在 SqLite_Init 编排层加重试与重建
- 无测试套件，验证 = 编译 + 串口日志比对（正常路径日志应与现状逐字一致）


## 技术栈

- 语言/环境：C（gnu17）、ESP-IDF v5.4.4、ESP32-S3、FreeRTOS、SQLite3 组件、wear-levelling
- 复用既有能力：`mount_fatfs_storage()` 两阶段挂载（失败时 s_wl_handle 归零可重调）、`init_database_locked()`（fail 路径已 close+置 s_db_ready=false）、`SelfRecovery_Write_str/Read_str`（main/Tool/SelfRecovery.h，INCLUDE_DIRS 已含 main/Tool）、`app_mqtt_publish("device/%s/event/warn", ...)` 上报范式（appTask.c:2294-2297）

## 实现方式（两个文件，SqLite.c 为主）

### 1. main/DataStorage/SqLite/SqLite.c

**新增常量**（置于文件顶部宏区）：

```c
#define SQLITE_MOUNT_RETRY_COUNT 3        /* mount 偶发失败重试次数 */
#define SQLITE_MOUNT_RETRY_INTERVAL_MS 2000
```

**新增静态状态**：`static bool s_reset_done_this_boot = false;`（每上电重置守卫）

**新增 `mount_fatfs_storage_reset()`**：`esp_vfs_fat_spiflash_mount_rw_wl` + `format_if_mount_failed=true` 强制重建分区（仅 mount 重试耗尽后调用一次）。

**重构 `SqLite_Init()`**（保持 mutex 创建与 g_db1_result 分配逻辑不变）：

```
1. mount 重试循环（3 次×2s）：调既有 mount_fatfs_storage()
   └─ 成功 → 进步骤 2
   └─ 耗尽且 !s_reset_done_this_boot：
        s_reset_done_this_boot = true
        WRN 日志 + mount_fatfs_storage_reset()（格式化重建）
        └─ 成功 → 进步骤 2；失败 → 禁用 + NVS 写 "mount_fail" + return
2. rc = init_database_locked()
   └─ SQLITE_OK → 完成（正常路径，日志与现状一致）
   └─ 失败且 !s_reset_done_this_boot：
        s_reset_done_this_boot = true
        rename(DB_FILE_PATH, DB_FILE_PATH ".corrupt")；rename 失败则 unlink 原文件
        （-journal/-wal/-shm 一并清理，防新库打开时回滚旧 journal）
        rc = init_database_locked()（重建空库 + v6 schema）
        └─ OK → NVS 写 "db_rebuilt" + WRN 日志（明确提示未上传记录已丢失）
        └─ 失败 → 禁用 + NVS 写 "rebuild_fail"
3. rc != OK → 保持现状禁用语义（ESP_LOGE + s_db_ready=false）
```

注释标注：调用上下文（app_task_init 早期）、重置语义（覆盖非空保护哲学的决策依据）、每上电守卫原因。

### 2. main/Task/appTask.c

`Init_ByNetwork_Flag()`（mqtt_app_start 成功后、业务拉起前）新增延迟上报块：

```c
/* SQLite 重置延迟上报：SqLite_Init 早于 MQTT，重置结果经 NVS 标志到此地上报 */
char reset_flag[16] = {0};
if (SelfRecovery_Read_str(NVS_KEY_SQLITE_RESET, reset_flag, sizeof(reset_flag)) == ESP_OK
    && reset_flag[0] != '\0') {
    app_mqtt_publish("device/%s/event/warn", reset_flag, DEVICE_ID);
    SelfRecovery_Write_str(NVS_KEY_SQLITE_RESET, "");   /* 上报后清除，避免重启重复报 */
}
```

NVS key 用 `NVS_KEY_SQLITE_RESET` 命名常量（跟随 appTask.c 既有 NVS_KEY_* 宏区）；上报 payload 含重置类型（mount_fail/db_rebuilt/rebuild_fail）与时间戳，便于服务器区分"缓存重建"与"存储禁用"。

## 关键取舍

- **重试只覆盖 mount**：DB 文件级损坏是持久性的，重试无意义，直接进重建；mount 偶发失败（flash/wl 竞争）重试可吸收——避免无谓延长启动时间
- **DB 级损坏用 rename 留档而非直接删**：`.corrupt` 文件可供事后取数；rename 失败（文件系统只读等极端情况）降级 unlink
- **重建后空间**：`.corrupt` 残留占用 FATFS 空间（data_storage 2.44MB），新库从零增长 + 既有 `sqlite_storage_low_on_free_space()` 淘汰机制兜底；.corrupt 仅在人工维修时清理（复用 cleanup_database_files 的人工接口语义）
- **不改 mount 两阶段既有逻辑**：首次空白分区的一次性格式化路径原样保留，新重置路径是其外的编排

## 性能与可靠性

- 时间复杂度：正常路径 O(1) 零增加；最坏路径 mount 重试 3×2s + 重建一次 init（~1s 内），启动延迟有界
- 可靠性：所有失败分支非空（日志 + NVS 标志 + 禁用语义保持）；`esp_restart` 无关，纯初始化期逻辑
- 日志规范：重置动作全部 WRN 级以上，含原因与后果（"unpushed records lost"），串口可定位

## 架构影响

- SqLite_Init 从"一次尝试"升级为"重试 + 受控重建"编排，存储层自愈闭环：本地重建 → NVS 标志 → 网络就绪 MQTT 上报 → 服务器知晓补发缺口
- 不改动数据流其他部分（Insert/Query/补发/老化任务零影响）


## Agent Extensions
### Skill
- **embedded-coding-standard**
  - 用途：改动完成后对照 `.ai/coding-standard.md` 本地规范自审（返回值处理、错误路径非空、常量宏化、调用上下文注释、NVS 键命名一致性）
  - 预期结果：确认 SqLite.c/appTask.c 改动合规，无规范回归
