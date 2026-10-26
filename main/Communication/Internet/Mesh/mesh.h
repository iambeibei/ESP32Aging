#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t mesh_init_Custom(void);

/** 当前节点的 Mesh 层级（根=1，子节点≥2；库未就绪时可能返回 0）。
 * 只读、无阻塞，任意任务上下文可调用。 */
uint8_t mesh_get_current_level(void);

/** 重启排程使用的层数：根固定 1，子优先当前/最近有效层数，未知时取最大层数。
 * 任意任务上下文，无阻塞，非 ISR。 */
uint8_t mesh_get_restart_level(void);

/** MQTT 上线成功时调用：当前层数与 NVS 保存值不一致才写入（层数用于启动错峰）。
 * 层级在下次启动时决定 Mesh 初始化延时（根 0s、每层 +5s）。
 * 任意任务上下文，可能短暂写 flash，非 ISR。 */
void mesh_level_persist_if_changed(void);

/** 仅重启服务在保护窗结束后调用。等待在途应用连接结束并暂停 Mesh 重连；
 * 不关闭 SoftAP、不清连接记录，可能短暂阻塞，非 ISR。失败由服务有限重试。 */
esp_err_t mesh_pause_for_restart(void);

/** 根节点有线出口期间暂停 STA 内部重连；保留 SoftAP、Mesh 配置及根身份。
 * 仅由 net_monitor 任务串行调用，可能阻塞，不可在 ISR/事件回调调用。
 * wired_preferred=false 时恢复此前暂停的重连；已有健康 STA 不主动断开。
 * 计划重启的暂停阶段不再恢复连接；控制锁忙时留待下轮调用。
 */
void mesh_root_manage_sta_reconnect(bool wired_preferred, bool sta_has_ip);
