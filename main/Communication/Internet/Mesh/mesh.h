#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t mesh_init_Custom(void);

/** 当前节点的 Mesh 层级（根=1，子节点≥2；库未就绪时可能返回 0）。
 * 只读、无阻塞，任意任务上下文可调用。 */
uint8_t mesh_get_current_level(void);

/** 根节点有线出口期间暂停 STA 内部重连；保留 SoftAP、Mesh 配置及根身份。
 * 仅由 net_monitor 任务串行调用，可能阻塞，不可在 ISR/事件回调调用。
 * wired_preferred=false 时恢复此前暂停的重连；已有健康 STA 不主动断开。
 */
void mesh_root_manage_sta_reconnect(bool wired_preferred, bool sta_has_ip);
