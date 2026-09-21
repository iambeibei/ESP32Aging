#include "app_enc.h"
#include <string.h>
#include "mbedtls/md5.h"
#include "mbedtls/aes.h"

#include "mbedtls/ecdsa.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/platform.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"

#define TAG "App_Enc"

/// @brief AES根密钥
const uint8_t rootkey[16] = {
    0x45, 0x9f, 0xc5, 0x35,
    0x80, 0x89, 0x41, 0xf1,
    0x70, 0x91, 0xe0, 0x99,
    0x3e, 0xe3, 0xe9, 0x3d};

/// @brief 本地基础私钥，交换数据时，需要用于对目标数据的签名
uint8_t privateKey[32] = {0x4F, 0x19, 0xA1, 0x6E, 0x3E, 0x87, 0xBD, 0xD9, 0xBD, 0x24, 0xD3, 0xE5, 0x49, 0x5B, 0x88, 0x04, 0x15, 0x11, 0x94, 0x3C, 0xBC, 0x8B, 0x96, 0x9A, 0xDE, 0x96, 0x41, 0xD0, 0xF5, 0x6A, 0xF3, 0x37};

/// @brief 本地基础公钥，交换数据时，需要用于对目标数据的验签
uint8_t publicKey[64] = {0xA7, 0x3A, 0xBF, 0x5D, 0x22, 0x32, 0xC8, 0xC1, 0xC7, 0x2E, 0x68, 0x30, 0x43, 0x43, 0xC2, 0x72, 0x49, 0x5E, 0x3A, 0x8F, 0xD6, 0xF3, 0x0E, 0xA9, 0x6D, 0xE2, 0xF4, 0xB3, 0xCE, 0x60, 0xB2, 0x51,
                               0xEE, 0x21, 0xAC, 0x66, 0x7C, 0xF8, 0xA7, 0x1E, 0x18, 0xB4, 0x6B, 0x66, 0x4E, 0xAE, 0xFF, 0xE3, 0xC4, 0x89, 0xF2, 0x4F, 0x69, 0x5B, 0x64, 0x11, 0xDB, 0x7E, 0x22, 0xCC, 0xC8, 0x5A, 0x85, 0x94};
/// @brief 每次AES参与的IV
uint8_t iv[16]; // md5

uint8_t SendPublicKey[64];

/// @brief 交换拿到的公钥
uint8_t publicKeyA1[64];

/// @brief 共享Key
uint8_t shartKey[32];

int ecdsa_verify(uint8_t *data, uint16_t len, uint8_t *sign_data, uint8_t *public_key);
void server_gen_public_key(uint8_t *public_key);
void ecdsa_sign(uint8_t *data, uint16_t len, uint8_t *private_key, uint8_t *sign_out);
static uint16_t calcute_sum(uint8_t *data, uint16_t len);
void iot_aes_cbc_encrypt_data(const uint8_t *scr, uint16_t slen, uint8_t *dst, uint16_t *olen, uint8_t *key, uint8_t *inv, uint8_t key_type);
void iot_aes_cbc_decrypt_data(const uint8_t *src, uint16_t slen, uint8_t *dst, uint16_t *olen, uint8_t *key, uint8_t *iv, uint8_t key_type);
void server_calculate_secret(uint8_t *public_key, uint8_t *share_key);

void printData(const uint8_t *text, size_t text_len, const char *tip)
{
    char hex_str[2 * text_len + 1]; // 为每个字节分配两位表示，加上结束符'\0'
    for (size_t i = 0; i < text_len; i++)
    {
        sprintf(hex_str + i * 2, "%02x", text[i]);
    }
    hex_str[text_len * 2] = '\0'; // 添加结束符

    // 使用ESP_LOGE输出resend数组的十六进制表示
    ESP_LOGI(TAG, "%s: %s", tip, hex_str);
}

// 计算MD5秘钥
static void md5_calculate_key(uint32_t value, uint8_t *out_md5)
{
    mbedtls_md5_context md5_ctx;
    uint8_t encrypt[4] = {0};
    encrypt[0] = (uint8_t)value;
    encrypt[1] = (uint8_t)(value >> 8);
    encrypt[2] = (uint8_t)(value >> 16);
    encrypt[3] = (uint8_t)(value >> 24);
    mbedtls_md5_init(&md5_ctx);
    mbedtls_md5_starts(&md5_ctx);  

    mbedtls_md5_update(&md5_ctx, encrypt, sizeof(encrypt));
    mbedtls_md5_finish(&md5_ctx, out_md5);
    mbedtls_md5_free(&md5_ctx);
}

esp_err_t app_enc_process_data(const uint8_t *buf2, size_t len, uint8_t *resend)
{
    if (len != 10 || buf2[0] != '*' || buf2[1] != '*')
    {
        ESP_LOGE(TAG, "Invalid input data");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t subarray[4] = {buf2[7], buf2[6], buf2[5], buf2[4]};
    resend[0] = '*';
    resend[1] = '*';
    resend[2] = 2;
    resend[3] = 4;

    // 初始化MD5上下文
    mbedtls_md5_context ctx;

    // 开始MD5计算
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    // 更新MD5上下文
    mbedtls_md5_update(&ctx, subarray, 4);
    mbedtls_md5_finish(&ctx, iv);
    mbedtls_md5_free(&ctx);
    memcpy(resend + 4, iv + 8, 4);

    uint16_t sum = 0;
    for (uint8_t i = 2; i < 8; i++)
    {
        sum = (sum + resend[i]) % 0xffff;
    }
    resend[8] = sum >> 8;   // 存储高字节
    resend[9] = sum & 0xff; // 存储低字节
    return ESP_OK;
}

esp_err_t app_enc_process_data4(uint8_t *buf2, size_t len, uint8_t *resend)
{

    if (len < 10 || buf2[0] != 0 || buf2[1] != 0x86)
    {
        ESP_LOGI(TAG, "Invalid input data");
        return ESP_ERR_INVALID_ARG;
    }
    // 假设buf2从某个位置开始存储的是已加密的payload，需要解密部分的长度为plen
    size_t plen = len - 2; // 替换PAYLOAD_START_INDEX为实际的偏移量
    uint8_t *ciphertext = buf2 + 2;
    uint8_t plaintext[plen];

    // 调用解密函数
    // esp_err_t ret = app_dec_data(ciphertext, plen, plaintext);
    // if (ret != ESP_OK) {
    //     return ret;
    // }
    uint16_t outlen = 0;
    uint8_t key[16] = {0};
    // 异或运算得第一次密钥
    for (int i = 0; i < 16; i++)
    {
        key[i] = iv[i] ^ rootkey[i];
    }
    iot_aes_cbc_decrypt_data(ciphertext, plen, plaintext, &outlen, key, iv, 1);

    // 将plaintext数组内容转换为十六进制字符串用于打印
    printData(plaintext, plen, " 0x04 Des data");

    if (outlen < 134 || plaintext[0] != '*' || plaintext[1] != '*')
    {
        ESP_LOGE(TAG, "Invalid input data ,  0x04 ECDH Error:len:%d", outlen);
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(publicKeyA1, plaintext + 4, 64);

    // 内容
    uint8_t content[80] = {0};
    memcpy(content, plaintext + 4, 64);
    memcpy(content + 64, iv, 16);

    // 签名
    uint8_t signature[64] = {0};
    // 内容
    memcpy(signature, plaintext + 68, 64);

    if (ecdsa_verify(content, sizeof(content), signature, publicKey) != 1)
    {
        ESP_LOGI(TAG, "Data signature fial--------------------------------");
        return ESP_FAIL;
    }

    // 发送公钥组装回复数据
    // uint8_t sendPublicKey[64];426571213

    printData(SendPublicKey, 64, "server_gen_public_key PublicKey");
    memcpy(content, SendPublicKey, 64);
    memcpy(content + 64, iv, 16);

    uint8_t sign_data[64];
    // 签名
    ecdsa_sign(content, sizeof(content), privateKey, sign_data);

    uint8_t sendData[134] = {0};
    sendData[0] = '*';
    sendData[1] = '*';
    sendData[2] = 5;
    sendData[3] = 0x80;
    memcpy(sendData + 4, SendPublicKey, sizeof(SendPublicKey));
    memcpy(sendData + 68, sign_data, sizeof(sign_data));

    // 计算sum
    uint16_t sum = calcute_sum(sendData + 2, sizeof(sendData) - 4);

    // sendData[132] = (uint8_t)(sum>>8);
    // sendData[133] = (uint8_t)sum;

    sendData[132] = sum >> 8;
    sendData[133] = sum & 0xff;

    // 加密
    iot_aes_cbc_encrypt_data(sendData, 134, resend + 2, &outlen, key, iv, 1);

    ESP_LOGI(TAG, "iot_aes_cbc_encrypt_data  deat  len:%d", outlen);

    resend[0] = 0x00;
    resend[1] = 0x86;

    printData(resend, 146, "resend data");
    printf("\n");
    return ESP_OK;
}

esp_err_t app_enc_process_data6(uint8_t *buf2, size_t len, uint8_t *resend)
{
    // 假设buf2从某个位置开始存储的是已加密的payload，需要解密部分的长度为plen
    size_t plen = len - 2; // 替换PAYLOAD_START_INDEX为实际的偏移量
    uint8_t *ciphertext = buf2 + 2;
    uint8_t plaintext[plen];

    // 调用解密函数
    // esp_err_t ret = app_dec_data(ciphertext, plen, plaintext);
    // if (ret != ESP_OK) {
    //     return ret;
    // }
    uint16_t outlen = 0;
    uint8_t key[16] = {0};
    // 异或运算得第一次密钥
    for (int i = 0; i < 16; i++)
    {
        key[i] = iv[i] ^ rootkey[i];
    }
    iot_aes_cbc_decrypt_data(ciphertext, plen, plaintext, &outlen, key, iv, 1);

    // 将resend数组内容转换为十六进制字符串用于打印
    printData(plaintext, plen, "Data6 Des data");
    if (plaintext[0] != '*' || plaintext[1] != '*' || plaintext[4] != 0)
    {
        ESP_LOGI(TAG, "Invalid input data");
        return ESP_ERR_INVALID_ARG;
    }

    // 计算本次会话的共享加密key
    server_calculate_secret(publicKeyA1, shartKey);

    resend[0] = 0;
    resend[1] = 1;
    return ESP_OK;
}

// 会话解密
esp_err_t app_enc_process_Decrypt(const uint8_t *buf2, size_t in_data_len, uint8_t *data, uint16_t *out_data_len)
{
    // 明文长度
    uint16_t len = 0;
    len = in_data_len - 6;
    // len = ((uint16_t)buf2[0] << 8) + buf2[1]; // 获取明文长度 这里有问题应该取到后6的数组长度
    printData(buf2, sizeof(*buf2) / sizeof(buf2[0]), "buf2data");
    printData(buf2, strlen((char *)buf2) / 2, "data");
    // len = sizeof(*buf2) / sizeof(buf2[0]) - 6; // 得到明文长度
    // len = strlen((char *)buf2) / 2 - 6;
    ESP_LOGI(TAG, "%d", in_data_len);
    ESP_LOGI(TAG, "%d", len);
    ESP_LOGI(TAG, "Len_before:%d", len);
    ESP_LOGI(TAG, "buf[0]:%d", buf2[0]);
    ESP_LOGI(TAG, "buf[0]:%d", buf2[1]);
    ESP_LOGI(TAG, "buf[0]:%d", buf2[2]);
    ESP_LOGI(TAG, "buf[0]:%d", buf2[3]);
    ESP_LOGI(TAG, "buf[0]:%d", buf2[4]);
    ESP_LOGI(TAG, "buf[0]:%d", buf2[5]);
    if (len > (in_data_len - 6)) // 检查明文长度标识和整体长度关系，非法则过滤处理
    {
        ESP_LOGE(TAG, "invalid aec-cbc len");
        return ESP_FAIL;
    }

    // 取出随机数
    uint8_t subarray[4] = {buf2[2], buf2[3], buf2[4], buf2[5]};

    // 初始化MD5上下文
    mbedtls_md5_context ctx;
    // 开始MD5计算
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    // 更新MD5上下文
    mbedtls_md5_update(&ctx, subarray, 4);
    mbedtls_md5_finish(&ctx, iv); // 幅值到IV中
    mbedtls_md5_free(&ctx);

    printData(buf2, in_data_len, "Enc ble Data"); // 这里buf2解密前的值
    // 获取明文
    iot_aes_cbc_decrypt_data(buf2 + 6, len, data, out_data_len, shartKey, iv, 0); // data 赋值?

    printData(data, len, "Nurmal ble Data:");
    // memcpy(resend + 4, iv + 8, 4);
    // data[2]+5
    // *out_data_len = len;
    if (data[1] == 6)
    {
        *out_data_len = 8; // 如果是写的话固定为8位
    }
    else
    {
        *out_data_len = data[2] + 5; // 5表示crc2: 01 03 再加上数据位 +数据 data[2]表示数据
    }

    return ESP_OK;
}

// 会话加密
esp_err_t   app_enc_process_Encrypt(const uint8_t *in_data, uint16_t in_data_len, uint8_t *out_Data, uint16_t *out_data_len)
{

    uint32_t random = 0;
    uint8_t once_ramdom[4];
    uint8_t once_md5[16];
    uint16_t len = 0;
    // 标记明文的数量
    out_Data[0] = (uint8_t)(in_data_len >> 8); // 高8位在前
    out_Data[1] = (uint8_t)in_data_len;        // 低8位在后
    random = esp_random();                     // 产生一个32位随机数
    // printf("send ramdom:0x%x\n", random);
    once_ramdom[0] = (uint8_t)random;  
    once_ramdom[1] = (uint8_t)(random >> 8);
    once_ramdom[2] = (uint8_t)(random >> 16);
    once_ramdom[3] = (uint8_t)(random >> 24);
    md5_calculate_key(random, once_md5); // 计算MD5
    // dump_buf("send MD5", once_md5, sizeof(once_md5));
    memcpy(out_Data + 2, once_ramdom, 4);// 复制随机数到输出数组
    // iot_communciate_aes_cbc_encrypt(in_data, in_data_len, out_Data+6, &len, once_md5);

    iot_aes_cbc_encrypt_data(in_data, in_data_len, out_Data + 6, &len, shartKey, once_md5, 0);

    *out_data_len = len + 6; // 密文数量+2个明文表示长度+4个随机数

    return ESP_OK;

}

static uint16_t calcute_sum(uint8_t *data, uint16_t len)
{
    uint16_t sum = 0;
    uint8_t *ptr = data;
    for (uint16_t i = 0; i < len; i++)
    {
        sum += *(ptr + i);
    }
    return sum;
}

// 计算sha256
void calculate_sha256(uint8_t *data, uint16_t len, uint8_t *hash)
{
    mbedtls_md_context_t md_ctx;
    mbedtls_md_init(&md_ctx);                                                  // 初始化MD结构体
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), data, len, hash); // 使用MD接口计算消息熵要
    mbedtls_md_free(&md_ctx);
}

static int entropy_source(void *data, uint8_t *output, size_t len, size_t *olen)
{
    uint32_t seed;
    size_t offset = 0;
    size_t sum = len / 4;

    for (size_t i = 0; i < sum; i++)
    {
        seed = esp_random(); // 产生一个32位随机数
        memcpy(output + offset, &seed, 4);
        offset += 4;
    }
    if (len % 4 != 0)
    {
        seed = esp_random(); // 产生一个32位随机数
        memcpy(output + offset, &seed, 4);
        offset += len % 4;
    }
    *olen = offset;
    return 0;
}

/// @brief 验签数据
/// @param data 数据
/// @param len 数据长度
/// @param sign_data 签名值
/// @param public_key 公钥
/// @return 1-ok  0-fail
int ecdsa_verify(uint8_t *data, uint16_t len, uint8_t *sign_data, uint8_t *public_key)
{
    uint8_t ret = 0;
    uint8_t pub[65]; // 65个字节公钥，第一个字节0x04表示公钥未压缩
    uint8_t hash[32];
    mbedtls_mpi r, s;
    mbedtls_ecdsa_context ctx;

    mbedtls_mpi_init(&r); // 初始化mpi结构体
    mbedtls_mpi_init(&s);
    mbedtls_ecdsa_init(&ctx); // 初始化ECDSA结构体

    mbedtls_ecp_group_load(&ctx.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1);
    pub[0] = 0x04; // 未压缩标志
    memcpy(pub + 1, public_key, 64);
    // dump_buf("ecdh public_key:", public_key, 64);
    mbedtls_ecp_point_read_binary(&ctx.MBEDTLS_PRIVATE(grp), &ctx.MBEDTLS_PRIVATE(Q), pub, 65); // 加载公钥

    mbedtls_mpi_read_binary(&r, sign_data, 32); // 加载签名
    mbedtls_mpi_read_binary(&s, sign_data + 32, 32);

    calculate_sha256(data, len, hash); // 计算哈希值sha256
    // dump_buf("verify hash:", hash, 32);
    // ECDSA验签接口
    if (!mbedtls_ecdsa_verify(&ctx.MBEDTLS_PRIVATE(grp), hash, 32, &ctx.MBEDTLS_PRIVATE(Q), &r, &s))
    {
        // mbedtls_printf("ecdsa verify signature ... ok\n\n");
        ret = 1;
    }
    else
    {
        ESP_LOGI(TAG, "ecdsa verify signature ... fail\n\n");
    }

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecdsa_free(&ctx);
    return ret;
}

mbedtls_ecp_group *srv_grp = NULL;
mbedtls_mpi *srv_pri = NULL; // 私钥存放的对象
mbedtls_ctr_drbg_context *srv_ctr_drbg = NULL;

// 生成密钥
void server_gen_public_key(uint8_t *public_key)
{
    int ret = 0;
    size_t olen;
    unsigned char buf[65];
    mbedtls_entropy_context entropy;
    mbedtls_ecp_point pub;
    char pers[] = "simple_ecdh";
    srv_grp = NULL;
    srv_pri = NULL;
    srv_ctr_drbg = NULL;
    // mbedtls_platform_set_printf(printf);
    if (srv_grp == NULL)
    {
        srv_grp = (mbedtls_ecp_group *)heap_caps_malloc(sizeof(mbedtls_ecp_group) * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
        if (srv_grp == NULL)
        {
            printf("malloc fail!");
            goto cleanup;
            ;
        }
    }

    if (srv_pri == NULL)
    {
        srv_pri = (mbedtls_mpi *)heap_caps_malloc(sizeof(mbedtls_mpi) * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
        if (srv_pri == NULL)
        {
            printf("malloc fail!");
            goto cleanup;
            ;
        }
    }

    if (srv_ctr_drbg == NULL)
    {
        srv_ctr_drbg = (mbedtls_ctr_drbg_context *)heap_caps_malloc(sizeof(mbedtls_ctr_drbg_context) * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
        if (srv_ctr_drbg == NULL)
        {
            printf("malloc fail!");
            goto cleanup;
            ;
        }
    }

    mbedtls_mpi_init(srv_pri);
    mbedtls_ecp_group_init(srv_grp); // 初始化椭圆曲线群结构体
    mbedtls_ecp_point_init(&pub);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(srv_ctr_drbg);

    mbedtls_entropy_add_source(&entropy, entropy_source, NULL,
                               MBEDTLS_ENTROPY_MAX_GATHER, MBEDTLS_ENTROPY_SOURCE_STRONG);
    mbedtls_ctr_drbg_seed(srv_ctr_drbg, mbedtls_entropy_func, &entropy,
                          (const uint8_t *)pers, strlen(pers));
    // mbedtls_printf("\n  . setup rng ... ok\n");

    ret = mbedtls_ecp_group_load(srv_grp, MBEDTLS_ECP_DP_SECP256R1); // 加载椭圆曲线SECP256R1
    if (ret != 0)
    {
        mbedtls_printf("ecp_group_load fail\n");
        goto cleanup;
    }
    // mbedtls_printf("\n  . select ecp group SECP256R1 ... ok\n");

    ret = mbedtls_ecdh_gen_public(srv_grp, srv_pri, &pub, mbedtls_ctr_drbg_random, srv_ctr_drbg); // 服务端生成公开参数
    if (ret != 0)
    {
        mbedtls_printf("ecdh_gen_public fail\n");
        goto cleanup;
    }
    // assert_exit(ret == 0, ret);
    mbedtls_ecp_point_write_binary(srv_grp, &pub,
                                   MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, buf, sizeof(buf));
    // dump_buf("  2. ecdh server generate public parameter:", buf, olen);
    memcpy(public_key, buf + 1, 64); // 复制公钥，第一个字节是压缩标志，需要剔除。

cleanup:
    mbedtls_entropy_free(&entropy);
    mbedtls_ecp_point_free(&pub);
}

// 用自己的私钥对数据做ECDSA签名
void ecdsa_sign(uint8_t *data, uint16_t len, uint8_t *private_key, uint8_t *sign_out)
{
    int ret = 0;
    uint8_t hash[32];
    char pers[] = "Poweroak";
    size_t rlen, slen;
    mbedtls_mpi r, s;
    mbedtls_ecdsa_context ctx;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    mbedtls_mpi_init(&r); // 初始化mpi结构体
    mbedtls_mpi_init(&s);
    mbedtls_ecdsa_init(&ctx);         // 初始化ECDSA结构体
    mbedtls_entropy_init(&entropy);   // 初始化熵结构体
    mbedtls_ctr_drbg_init(&ctr_drbg); // 初始化随机数结构体
                                      // 添加熵源接口，设置熵源属性
    mbedtls_entropy_add_source(&entropy, entropy_source, NULL,
                               MBEDTLS_ENTROPY_MAX_GATHER, MBEDTLS_ENTROPY_SOURCE_STRONG);
    // 根据个性化字符串更新种子
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                (const uint8_t *)pers, strlen(pers));
    if (ret != 0)
    {
        printf("drbg_seedfail\n");
        goto cleanup;
    }
    // assert_exit(ret == 0, ret);
    // mbedtls_printf("\n  . setup rng ... ok\n\n");
    mbedtls_ecp_group_load(&ctx.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1); // 椭圆曲线算法为SECP256R1
    mbedtls_mpi_read_binary(&ctx.MBEDTLS_PRIVATE(d), private_key, 32);           // 加载私钥
    // dlen = mbedtls_mpi_size(&ctx.d);
    // printf("dlen:%d\n", dlen);
    calculate_sha256(data, len, hash);
    // dump_buf("hash:", hash, 32);
    ret = mbedtls_ecdsa_sign(&ctx.MBEDTLS_PRIVATE(grp), &r, &s, &ctx.MBEDTLS_PRIVATE(d), hash, sizeof(hash), mbedtls_ctr_drbg_random, &ctr_drbg);
    rlen = mbedtls_mpi_size(&r);
    slen = mbedtls_mpi_size(&s);
    // printf("rlen:%d, slen:%d\n", rlen, slen);
    mbedtls_mpi_write_binary(&r, sign_out, rlen);
    mbedtls_mpi_write_binary(&s, sign_out + rlen, slen);
    // dump_buf("  3. ecdsa generate signature:", sign_out, rlen + slen);

cleanup:
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecdsa_free(&ctx);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
}

/*iot数据AES-CBC加密*/
void iot_aes_cbc_encrypt_data(const uint8_t *scr, uint16_t slen, uint8_t *dst, uint16_t *olen, uint8_t *key, uint8_t *inv, uint8_t key_type)
{
    uint16_t count = 0;
    uint8_t iv_buf[16];
    uint8_t aes_in[16];
    uint16_t aes_encrypt_len = 0;
    mbedtls_aes_context aes_ctx;
    memcpy(iv_buf, inv, sizeof(iv_buf));
    memset(aes_in, 0, sizeof(aes_in)); // 清零
    mbedtls_aes_init(&aes_ctx);
    if (!key_type) // 密钥类型
    {
        mbedtls_aes_setkey_enc(&aes_ctx, key, 256); // 设置AES-CBC加密密钥256
    }
    else
    {
        mbedtls_aes_setkey_enc(&aes_ctx, key, 128); // 设置AES-CBC加密密钥128
    }
    if (slen < 16) // 不足16字节
    {
        memcpy(aes_in, scr, slen);
        mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_ENCRYPT, 16, iv_buf, aes_in, dst);
        aes_encrypt_len += 16;
    }
    else // 超过16字节
    {
        count = slen / 16;
        for (uint16_t i = 0; i < count; i++)
        {
            // AES-CBC加密，每次加密16字节
            mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_ENCRYPT, 16, iv_buf, scr + aes_encrypt_len, dst + aes_encrypt_len);
            memcpy(iv_buf, dst + aes_encrypt_len, sizeof(iv_buf)); // 本次目标密文作为下一次AES-CBC加密的初始IV向量
            aes_encrypt_len += 16;
        }

        if (slen % 16 != 0) // 最后不足16字节部分
        {
            memcpy(aes_in, scr + aes_encrypt_len, slen % 16); // 后面补充0
            mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_ENCRYPT, 16, iv_buf, aes_in, dst + aes_encrypt_len);
            aes_encrypt_len += 16;
        }
        else // 如果刚好16字节整数倍，则需要再加一个包
        {
            // mbedtls_aes_crypt_cbc( &aes_ctx, MBEDTLS_AES_ENCRYPT, 16, iv_buf, aes_in, dst+aes_encrypt_len);
            // aes_encrypt_len += 16;
        }
    }
    *olen = aes_encrypt_len;
    mbedtls_aes_free(&aes_ctx); // 释放并清除指定的 AES 上下文
}

/*iot数据AES解密*/
void iot_aes_cbc_decrypt_data(const uint8_t *src, uint16_t slen, uint8_t *dst, uint16_t *olen, uint8_t *key, uint8_t *iv, uint8_t key_type) // dst赋值
{
    mbedtls_aes_context aes_ctx;
    uint8_t iv_buf[16];
    size_t aes_decrypt_len = 0;
    ESP_LOGI(TAG, "len_slen:%d", slen);
    uint16_t count = slen / 16;

    memcpy(iv_buf, iv, sizeof(iv_buf)); // 复制第一次初始化IV向量
    mbedtls_aes_init(&aes_ctx);
    if (!key_type) // 256
    {
        // ESP_LOGI(TAG, "aes256");
        // ESP_LOGI(TAG, "slen:%d", slen);
        mbedtls_aes_setkey_dec(&aes_ctx, key, 256); // 设置AES-CBC解密密钥256
    }
    else
    {
        // ESP_LOGI(TAG, "aes128");
        mbedtls_aes_setkey_dec(&aes_ctx, key, 128); // 设置AES-CBC解密密钥128
    }
    ESP_LOGI(TAG, "count:%d", count);
    for (uint16_t i = 0; i < count; i++)
    {
        ESP_LOGI(TAG, "ENTER");
        // 每一次解密16字节
        mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_DECRYPT, 16, iv_buf, src + aes_decrypt_len, dst + aes_decrypt_len);
        memcpy(iv_buf, src + aes_decrypt_len, sizeof(iv_buf)); // 本次源密文作为下一次AES-CBC解密的初始IV向量
        aes_decrypt_len += 16;
        printData(dst, 16, "every dec");
    }
    ESP_LOGE(TAG, "dst:%d", *dst);
    //*dst=dst;
    *olen = aes_decrypt_len;
    mbedtls_aes_free(&aes_ctx); // 释放并清除指定的 AES 上下文
}

// 计算共享密钥
void server_calculate_secret(uint8_t *public_key, uint8_t *share_key)
{
    uint8_t out_public[65];
    int ret = 0;
    unsigned char buf[65];
    // char secret_buf[33];
    mbedtls_mpi secret;
    mbedtls_ecp_point pub;
    memset(out_public, 0, sizeof(out_public));
    // memset(secret_buf, 0 ,sizeof(secret_buf));
    mbedtls_ecp_point_init(&pub);
    mbedtls_mpi_init(&secret);

    buf[0] = 0x04;
    memcpy(buf + 1, public_key, 64);
    mbedtls_ecp_point_read_binary(srv_grp, &pub, buf, sizeof(buf)); // 加载公钥

    ret = mbedtls_ecdh_compute_shared(srv_grp, &secret, &pub, srv_pri, mbedtls_ctr_drbg_random, srv_ctr_drbg); // 客户端生成会话秘钥
    if (ret != 0)
    {
        printf("ecdh_compute_shared fail\n");
        goto cleanup;
    }
    // printf("server secret,len:%d\n", mbedtls_mpi_size(&secret));
    mbedtls_mpi_write_binary(&secret, share_key, mbedtls_mpi_size(&secret));
    // dump_buf("server generate secret:", share_key, mbedtls_mpi_size(&secret));

cleanup:
    mbedtls_ecp_group_free(srv_grp);
    mbedtls_mpi_free(srv_pri);
    mbedtls_mpi_free(&secret);
    mbedtls_ecp_point_free(&pub);
    mbedtls_ctr_drbg_free(srv_ctr_drbg);

    if (srv_grp != NULL)
    {
        free(srv_grp);
        // srv_grp = NULL;
    }

    if (srv_pri != NULL)
    {
        free(srv_pri);
        //  srv_pri = NULL;
    }

    if (srv_ctr_drbg != NULL)
    {
        free(srv_ctr_drbg);
        // srv_ctr_drbg = NULL;
    }
}

void RandomSendPKey()
{
    server_gen_public_key(SendPublicKey);
}