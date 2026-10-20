---
name: remove-child-ota-stagger
overview: 去掉 mesh_ota.c 子节点 OTA 的层级错峰延时（(level-1)×6s + 0~15s 抖动），升级时序改由上位机分级下发完全控制；保留 pause/restart 排队、90s×2 失败重试、库穿透三层兜底。删除三个错峰专用常量、错峰计算块、启动日志参数及死代码状态复查，同步更新 mesh_ota.h 注释与 MESH_LITE_OTA_INTEGRATION.md。
todos:
  - id: remove-stagger-core
    content: 删除 mesh_ota.c 错峰三常量与 handle_child_path 错峰块（含死代码状态复查、陈旧注释）
    status: completed
  - id: update-logs-comments
    content: 更新 mesh_ota_init 启动日志与 mesh_ota.c/mesh_ota.h 注释为“上位机控制时序”表述
    status: completed
    dependencies:
      - remove-stagger-core
  - id: update-doc
    content: 同步 MESH_LITE_OTA_INTEGRATION.md（3.3 节、常量表、架构图、6.2 验收）
    status: completed
    dependencies:
      - update-logs-comments
  - id: build-and-review
    content: 编译验证零告警；用 [skill:embedded-coding-standard] 对照本地规范自审本次改动
    status: completed
    dependencies:
      - update-doc
---

## 产品概述

去掉 mesh_ota.c 子节点 OTA 的层级错峰延时（(level-1)×6s + 0~15s 抖动），OTA 升级时序改由上位机分级下发完全控制（先根→根重启上线→逐层下发子节点）；设备侧保留 pause/restart 库内排队与 90s×2 失败重试两层兜底。

## 核心功能

- 删除子节点错峰：`handle_child_path()` 中层级防御下限、stagger/jitter 计算、stagger 日志、`vTaskDelay`，以及错峰去掉后成为死代码的状态复查段（`state != RUNNING → abort`）
- 删除三个错峰专用常量：`LG_MESH_OTA_LEVEL_STAGGER_SEC`（当前值 6，注释"60s 依据"已陈旧，一并清理）、`LG_MESH_OTA_STAGGER_JITTER_SEC`、`LG_MESH_OTA_MIN_CHILD_LEVEL`
- 更新 `mesh_ota_init()` 启动日志：删 stagger/jitter 参数项，保留 retry 参数
- 注释与文档同步：mesh_ota.c 文件头、mesh_ota.h 头注释（“可靠性补偿”改为“失败重试 + pause/restart 兜底，时序由上位机控制”）、MESH_LITE_OTA_INTEGRATION.md（3.3 节、第 4 节常量表、架构图、6.2 验收标准）

## 保留不动（非错峰）

- 根路径 pause/restart、cmd_task 重试（CMD_EVT_RETRY + 90s 间隔 + metadata 复用）、完成即重启 3s 落盘、库穿透周期

## 取舍（已与用户确认）

- 服务器分级下发：子节点零失败直达，全场最快（预期用法）
- 服务器同时全层下发：首次请求撞根未就绪→禁用桩 FAIL→90s 重试兜底命中（功能正确，多一轮失败日志）
- 同层射频峰值由库窗口流控（OTA_WND=8256）+ SoftAP 调度吸收

## 技术栈

- 语言/框架：C（gnu17）、FreeRTOS、ESP-IDF v5.4.4、ESP32-S3、esp_mesh_lite
- 改动范围：mesh_ota.c / mesh_ota.h 单模块 + 设计文档 MESH_LITE_OTA_INTEGRATION.md，无新文件、无新依赖

## 实现方式

### 1. mesh_ota.c：删除错峰

- **常量区**（66-91 行）：删除 `LG_MESH_OTA_LEVEL_STAGGER_SEC`（含陈旧“60s 依据”注释块）、`LG_MESH_OTA_STAGGER_JITTER_SEC`、`LG_MESH_OTA_MIN_CHILD_LEVEL` 三个宏及各自注释；保留 RETRY/RETRY_INTERVAL/ROOT_DL_TIMEOUT/RESTART_DELAY/PROGRESS_LOG 常量
- **`handle_child_path()`**（569 行起）：删除 level 防御、stagger_ms/jitter_ms/delay_ms 计算、`Child OTA: level=..., stagger ... before mesh request` 日志、`vTaskDelay(delay_ms)`，以及紧随其后的状态复查段（错峰去掉后主循环置 RUNNING 到本函数同步执行无 yield，该段为死代码，YAGNI 删除）；函数保留 update.json 获取 → size 探测 → `transmit_file_start` 主链，日志可精简为 `Child OTA: level=%u, requesting firmware via mesh`（保留 level 诊断值，`esp_mesh_lite_get_level()` 调用保留）
- **`mesh_ota_init()` 启动日志**（829-833 行）：改为 `mesh_ota initialized: retry=%ux@%us`，删 stagger/jitter 参数

### 2. 注释与文档同步

- mesh_ota.c 文件头“完成即重启”注释段保留；如有错峰相关表述（如 3.3 设计引用）一并修正为“时序由上位机分级下发控制，设备侧仅重试兜底”
- mesh_ota.h 头注释“可靠性补偿”段改为：失败有限重试 2 次 @90s + pause/restart 库内排队；升级时序由上位机分级下发控制（2026-10-20 起设备侧不做层级错峰）
- MESH_LITE_OTA_INTEGRATION.md：3.3 节“三层时序保护”→“服务器分级下发控制时序 + 两层设备兜底（pause/restart + 重试）”；第 4 节常量表删 stagger 两行；架构图子节点路径去“错峰 +”；6.2 验收“观察错峰打散”→“验证服务器分级下发时序 + 重试兜底行为”

## 关键取舍

- **只删错峰不删重试**：重试是失败恢复语义（服务器漏判时设备自愈），非错峰；90s 间隔覆盖“根还在下载/重启”窗口，与服务器控制不冲突
- **删死代码而非留防御**：状态复查依赖“延时期间状态被改变”这一前提，前提消失即删除，避免误导后续读者
- **保留 level 日志值**：诊断价值（现场判断请求发自哪层），但不再参与计算

## 性能与可靠性

- 时间复杂度 O(1)；无内存/锁变化；删除后 cmd_task 收到子路径命令直达 `transmit_file_start`，无 vTaskDelay 阻塞
- 风险面：仅子节点请求时序变化；服务器若同时全层下发，多一轮 90s 重试（功能正确），已在取舍中告知

## Skill

- **embedded-coding-standard**
  - 用途：改动完成后对照 `.ai/coding-standard.md` 自审（删除后无死代码/未用常量、注释与实现一致、日志规范）
  - 预期结果：确认改动合规，无规范回归
