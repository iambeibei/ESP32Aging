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
 *     cmd_task 校验/计算 SHA、提交 NVS、设置启动分区、登记已校验文件；
 *     保持旧固件运行，不自动重启。
 *   启动先恢复 NVS 供数记录并核验完整镜像及 SHA；允许备用槽和当前运行槽供数。
 *   无记录时仅在服务器元数据/长度与当前镜像一致后绑定当前槽，不下载 BIN 正文。
 *   完成本地校验、持久化、文件名登记和恢复下级 OTA 后输出 OTA_BINARY_READY。
 *   就绪后以 QoS1 发布 device/<DEVICE_ID>/event/OTACP，包含完成时间戳、
 *   IsComplete=1 与本机 AgingNumber。断线/入队失败保留通知、每 5 秒重试。
 *   原完成时间和设备身份随镜像持久化，启动恢复后以原 Seq 补报；首次绑定不报完成。
 *   外部 CanRestart 由独立重启服务分层调度，不承诺断电后继续运行旧固件。
 *
 * 可靠性补偿（无外网兜底后的必要措施）：
 *   - 失败有限重试：最多 2 次、间隔 90 秒；子复用首次 metadata/size；
 *   - 子节点父链路/IP 中断取消未完成写入，进入 WAIT_PARENT 保留本次启动内任务；
 *     旧 SDK 传输结束且重新取得 IP 稳定 10s 后，随机 0..3s 从头获取原目标文件。
 *     等待父节点不消耗上述重试次数；MQTT 单独断开不取消，已就绪镜像不重下。
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
 * 并创建 CPU1 任务：cmd_task 所有节点创建（先本地恢复再消费命令）；dl_task 仅根节点创建
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
 * 合并；待重启的新镜像不会重复擦写，持久化/登记失败仅重试收尾。
 * 已运行新镜像并恢复供数的节点可继续接受下一版本升级。
 */
void mesh_ota_handle_mqtt_command(void);

/** 子节点父链路/IP 状态通知；GOT_IP=true，STA_DISCONNECTED/LOST_IP=false。
 * 默认事件任务调用，短临界区保存状态并唤醒服务任务，不等待 OTA 锁、不执行 Flash 操作。
 * 可早于本模块初始化调用；根节点忽略。仅已收到 OTA 命令的未完成任务会自动恢复。
 * 接收期间断链立即禁止后续写入，旧 SDK 传输终态到达后才能重新请求。非 ISR。 */
void mesh_ota_parent_link_changed(bool has_ip);

/**
 * @brief 查询 OTA 状态（透传 ota.c 状态机当前值）
 *
 * 调用上下文：任意任务；无阻塞。
 *
 * @return 当前 simple_ota_state_t（IDLE/RUNNING/PENDING_RESTART/FAILED/WAIT_PARENT）
 */
simple_ota_state_t mesh_ota_get_state(void);

#ifdef __cplusplus
}
#endif
