#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 本次启动内的不可变计划快照；调用方拥有，无 SDK/任务指针。 */
typedef struct {
    int32_t command_seq;
    uint8_t level;
    uint32_t delay_seconds;
} system_restart_plan_t;

/** 仅启动任务串行调用，幂等；创建 CPU1 服务失败可重试，不降级为同步重启。非 ISR。 */
esp_err_t system_restart_init(void);
/** 任务上下文、短临界区提交，无网络 I/O/延时。已排程则返回原计划，不推迟截止时间。
 * out_plan 必须非空；未初始化/自愈已提交时返回 ESP_ERR_INVALID_STATE。非 ISR。 */
esp_err_t system_restart_schedule(int32_t command_seq, system_restart_plan_t *out_plan);
/** 任意任务上下文查询，无阻塞，非 ISR。计划从接受起至重启一直有效。 */
bool system_restart_is_planned(void);
/** 任意任务上下文查询，无阻塞，非 ISR。保护窗结束后禁止应用重新连接 Mesh。 */
bool system_restart_mesh_paused(void);
/** 网络自愈重启的最终仲裁：仅在尚无计划/其他自愈时提交成功。
 * 任务上下文、无阻塞；成功后调用方必须直接完成重启，不可取消/继续运行。非 ISR。 */
bool system_restart_claim_recovery(void);

#ifdef __cplusplus
}
#endif
