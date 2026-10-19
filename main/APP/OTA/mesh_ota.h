/**
 * @file mesh_ota.h
 * @brief Mesh-Lite 原生 OTA 分发适配层接口（main/APP/OTA/mesh_ota.c 的实现入口）
 *
 * 分层：本层是 Mesh-Lite OTA 组件（esp_mesh_lite_*_ota_*）与项目 OTA 模块
 * （ota.c 外网下载 + 状态机）之间的唯一耦合点。设计决策、闭源库风险与
 * 实机验收记录见 MESH_LITE_OTA_INTEGRATION.md。
 *
 * 角色分流（esp_mesh_lite_get_level()，根=1，子>1）：
 *   - 子节点：transmit_file_start() 走 mesh 内逐级请求/分发，
 *     extern_url_ota_cb 为禁用桩（mesh 内拿不到即失败，无外网兜底）；
 *   - 根节点：notify_child_node_pause() → dl_task 外网下载唯一 1 份 →
 *     notify_child_node_restart()。
 *
 * 可靠性补偿（无兜底后的必要措施）：
 *   - 层级错峰：(level-1)×LG_MESH_OTA_LEVEL_STAGGER_SEC + rand(0,15s)；
 *   - 失败有限重试：LG_MESH_OTA_RETRY_MAX 次 @LG_MESH_OTA_RETRY_INTERVAL_SEC。
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "ota.h" /* simple_ota_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 Mesh-Lite 原生 OTA 适配层
 *
 * 注册 LAN OTA 文件传输三回调（provide/get/get_done）+ OTA 事件处理器，
 * 并创建 cmd_task / dl_task 两个 CPU1 任务（所有节点统一创建，子节点的
 * dl_task 空闲待命，仅根节点路径唤醒）。
 *
 * 调用上下文：app_task_init，mesh_init_Custom() 成功后调用一次；不可重入
 * （内部 init-once 静态守卫）。未启用 CONFIG_ESP_MESH_LITE_OTA_ENABLE 时
 * 返回 ESP_ERR_NOT_SUPPORTED，ota 命令将不被处理（需检查 sdkconfig）。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_SUPPORTED 组件开关未启用；
 *         其他错误码见实现（任务/事件注册失败）
 */
esp_err_t mesh_ota_init(void);

/**
 * @brief MQTT ota 命令入口（appTask.c parse_jsonCommand_MQTT 的 ota 分支）
 *
 * 调用上下文：mqtt_rx 任务。非阻塞，立即返回——只做状态预检
 * （PENDING_RESTART 忽略 / RUNNING 拒绝）后通知 cmd_task，下载与 mesh
 * 请求均在 cmd_task/dl_task 执行，mqtt_rx 不再被阻塞。
 */
void mesh_ota_handle_mqtt_command(void);

/**
 * @brief 查询 OTA 状态（透传 ota.c 状态机当前值）
 *
 * 调用上下文：任意任务；无阻塞。
 *
 * @return 当前 simple_ota_state_t（IDLE/RUNNING/PENDING_RESTART/FAILED）
 */
simple_ota_state_t mesh_ota_get_state(void);

#ifdef __cplusplus
}
#endif
