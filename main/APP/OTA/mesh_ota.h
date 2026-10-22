/**
 * @file mesh_ota.h
 * @brief Mesh-Lite 原生 OTA 分发适配层接口（main/APP/OTA/mesh_ota.c 的实现入口）
 *
 * 分层：本层是 Mesh-Lite OTA 组件（esp_mesh_lite_*_ota_*）与项目 OTA 模块
 * （ota.c 外网下载 + 状态机）之间的唯一耦合点。设计决策、闭源库风险与
 * 实机验收记录见 MESH_LITE_OTA_INTEGRATION.md。
 *
 * 角色分流依据 IsRoot，层级仅供日志；失去父链路的子仍不能走根下载路径：
 *   - 子节点：transmit_file_start(BINARY) 以文件名走 mesh 内逐级请求/分发，
 *     extern_url_ota_cb 为禁用桩（mesh 内拿不到即失败，无外网兜底）；
 *   - 根节点：notify_child_node_pause() → dl_task 外网下载唯一 1 份 →
 *     完成后登记备用分区中的已校验文件，保持旧固件运行，不自动重启。
 *   完成本地校验、文件名登记和恢复下级 OTA 后输出 OTA_BINARY_READY。
 *   就绪后以 QoS1 发布 device/<DEVICE_ID>/event/OTACP，包含完成时间戳、
 *   IsComplete=1 与本机 AgingNumber。断线/入队失败保留通知、每 5 秒重试。
 *   不调整 CanRestart，也不承诺断电后继续运行旧固件。
 *
 * 可靠性补偿（无外网兜底后的必要措施）：
 *   - 失败有限重试：最多 2 次、间隔 90 秒；子复用首次 metadata/size；
 *   - 根下载期间 pause/restart 让子节点请求在库内排队。
 *   升级时序由上位机分级下发控制（先根 READY，再逐层下发子节点），
 *   设备侧不做层级错峰（2026-10-20 移除）；服务器若同时全层下发，子首次
 *   请求失败后由重试兜底命中。
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
 * 并创建 CPU1 任务：cmd_task 所有节点创建；dl_task 仅根节点创建
 * （DL_EVT_START 唯一置位点在根节点路径，子节点无外网下载需求）。
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
 * 调用上下文：mqtt_rx 任务。非阻塞，立即返回——只通知 cmd_task，下载与 mesh
 * 请求均在 cmd_task/dl_task 执行，mqtt_rx 不再被阻塞。重复命令由服务任务
 * 合并；已就绪不会重复擦写，登记/恢复失败仅重试收尾。
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
