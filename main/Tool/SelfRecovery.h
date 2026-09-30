
#ifndef SELFRECOVERY_H
#define SELFRECOVERY_H

#include "esp_log.h"
#include "nvs.h"
#include "stdbool.h"

typedef struct {
    uint16_t aging_valid;
    uint16_t current_step;//老化恢复的步骤，三大步中的某一个
    uint64_t Deadline;//截至时间
    uint16_t CycleIndex;//老化循环次数
    bool SOC_HaveData;//SOC是否已经采集到数据
    char* AgingJson;//老化恢复的JSON数据
    
} AgingResumeState;

esp_err_t SelfRecovery_Init(void);

void SelfRecovery_Deinit(void);

esp_err_t SelfRecovery_Erase_all(void);

esp_err_t SelfRecovery_Write_str(const char *key, const char *value);

esp_err_t SelfRecovery_Read_str(const char *key, char *out_value, size_t max_len);

esp_err_t SelfRecovery_Write_uint16(const char *key, uint16_t value);

esp_err_t SelfRecovery_Read_uint16(const char *key, uint16_t *value);

esp_err_t SelfRecovery_Write_uint64(const char *key, uint64_t val);

esp_err_t SelfRecovery_Read_uint64(const char *key, uint64_t *val);

esp_err_t SelfRecovery_Erase_aging_valid(void);

#endif

