#pragma once
#include "stdint.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif
    extern uint8_t iv[16];

    void printData(const uint8_t *text, size_t text_len, const char *tip);

    esp_err_t app_enc_process_data(const uint8_t *buf2, size_t len, uint8_t *resend);

    esp_err_t app_enc_process_data4(uint8_t *buf2, size_t len, uint8_t *resend);

    esp_err_t app_enc_process_data6(uint8_t *buf2, size_t len, uint8_t *resend);

    esp_err_t app_enc_process_Decrypt(const uint8_t *buf2, size_t in_data_len, uint8_t *data, uint16_t *out_data_len);

    esp_err_t app_enc_process_Encrypt(const uint8_t *in_data, uint16_t in_data_len, uint8_t *out_Data, uint16_t *out_data_len);

    void RandomSendPKey();

#ifdef __cplusplus
}
#endif
