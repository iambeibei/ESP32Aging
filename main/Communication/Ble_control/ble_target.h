#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

typedef struct {
    bool use_mac;
    uint8_t bda[6];
    uint8_t addr_type;
} BleTarget;

/* 任务上下文，非 ISR；不阻塞、可重入。缺少 MAC 表示名称模式；失败不修改 out。 */
esp_err_t ble_target_parse(const cJSON *data, BleTarget *out);

/* 任务上下文，非 ISR；可能阻塞于 NVS。pn/target 仅在调用期间借用。
 * ready=true 为待消费目标，否则为运行目标；单条 blob 原子保存 PN 与地址。 */
esp_err_t ble_target_save(bool ready, const char *pn, const BleTarget *target);

/* 任务上下文，非 ISR；可能阻塞于 NVS。缺少旧记录按名称模式兼容；
 * 存在但 PN 不匹配、格式损坏或读失败则返回错误，禁止退回名称模式。 */
esp_err_t ble_target_load(bool ready, const char *pn, BleTarget *out);
