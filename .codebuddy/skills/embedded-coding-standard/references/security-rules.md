# 嵌入式安全检测规则

面向嵌入式/固件项目的安全审查规则。覆盖密钥管理、调试接口、固件更新、侧信道、输入验证和运行时保护。

## 密钥与凭证

### SEC-KEY-01: 硬编码密钥/凭证

检测：源码中出现 AES key、密码、token、证书私钥等字面量。

```c
// BAD
static const uint8_t aes_key[16] = {
    0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
    0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C
};

static const char wifi_password[] = "MySecretPass123";

// GOOD — 从安全存储读取
uint8_t aes_key[16];
secure_storage_read(KEY_SLOT_AES, aes_key, sizeof(aes_key));
// 使用后清零
explicit_bzero(aes_key, sizeof(aes_key));
```

关键词扫描：`key`、`secret`、`password`、`passwd`、`token`、`credential`、`private`、连续的十六进制字节数组（16/24/32 字节）。

风险：固件可被提取（JTAG/flash dump），密钥泄露后影响所有设备。严重级别：critical。

### SEC-KEY-02: 密钥/敏感数据未及时清零

检测：密钥、密码、会话密钥使用完后未从内存中擦除。

```c
// BAD
void authenticate(void) {
    uint8_t session_key[32];
    derive_session_key(session_key);
    encrypt_message(session_key, msg);
    // session_key 留在栈上，下次函数调用可能泄露
}

// GOOD
void authenticate(void) {
    uint8_t session_key[32];
    derive_session_key(session_key);
    encrypt_message(session_key, msg);
    explicit_bzero(session_key, sizeof(session_key));
}
```

注意：`memset(key, 0, len)` 可能被编译器优化掉。必须使用 `explicit_bzero`、`SecureZeroMemory`、volatile 写入或平台专用安全擦除函数。

风险：内存残留的密钥可通过冷启动攻击、crash dump、调试接口提取。严重级别：high。

### SEC-KEY-03: 明文存储敏感数据

检测：密钥、配置、凭证存储在 NVS/EEPROM/flash 中未加密。

```c
// BAD
nvs_set_blob(handle, "wifi_key", psk, 32);  // 明文写入 NVS

// GOOD
uint8_t encrypted[32 + 16];  // ciphertext + tag
encrypt_with_device_key(psk, 32, encrypted);
nvs_set_blob(handle, "wifi_key", encrypted, sizeof(encrypted));
```

风险：flash dump 直接获得所有凭证。严重级别：high。

## 调试接口

### SEC-DBG-01: 生产固件未禁用调试接口

检测：SWD/JTAG/debug UART 在生产构建中仍然启用；readout protection 未配置。

```c
// BAD — 生产固件
// 没有以下任何保护措施

// GOOD — 在生产配置中
#if !defined(DEBUG_BUILD)
// STM32 示例：设置读保护
FLASH_OBProgramInitTypeDef ob;
HAL_FLASHEx_OBGetConfig(&ob);
if (ob.RDPLevel == OB_RDP_LEVEL_0) {
    ob.OptionType = OPTIONBYTE_RDP;
    ob.RDPLevel = OB_RDP_LEVEL_1;
    HAL_FLASH_Unlock();
    HAL_FLASH_OB_Unlock();
    HAL_FLASHEx_OBProgram(&ob);
    HAL_FLASH_OB_Launch();
}
#endif
```

检查点：
- `RDP`（STM32）/ `APPROTECT`（nRF）/ `SECURE_BOOT`（ESP32）是否启用
- Debug UART 的 TX/RX pin 是否仍配置为 UART alternate function
- 是否存在 `#if DEBUG` 但实际生产构建中 `DEBUG` 仍被定义

风险：攻击者通过调试接口读取/修改固件和内存。严重级别：critical。

### SEC-DBG-02: 调试日志泄露敏感信息

检测：日志输出包含密钥、token、内存地址、内部状态等敏感信息。

```c
// BAD
printf("Auth key: ");
for (int i = 0; i < 16; i++) printf("%02X ", key[i]);
printf("Session token: %s\n", token);
printf("Buffer at %p, size %d\n", buf, size);  // 地址泄露

// GOOD
LOG_INFO("Auth: started (key_id=%d)", key_slot_id);
LOG_DEBUG("Session established");  // 不打印 token 本身
```

风险：串口输出被监听，泄露密钥或可用于绕过 ASLR。严重级别：high。

## 固件更新

### SEC-OTA-01: OTA/FOTA 无签名校验

检测：固件更新流程中缺少数字签名验证，仅依赖 CRC 或 checksum。

```c
// BAD — 只有 CRC
uint32_t crc = calculate_crc32(fw_data, fw_len);
if (crc == expected_crc) {
    flash_write(FW_ADDR, fw_data, fw_len);  // CRC 不防篡改
}

// GOOD — 签名验证
if (verify_ecdsa_signature(fw_data, fw_len, signature, public_key) != 0) {
    return FW_UPDATE_INVALID_SIGNATURE;
}
// 验证版本号防回滚
if (fw_header->version <= get_current_version()) {
    return FW_UPDATE_ROLLBACK_REJECTED;
}
flash_write(FW_ADDR, fw_data, fw_len);
```

检查点：
- 是否有签名验证（ECDSA、RSA、Ed25519）
- 验证用的公钥是否硬编码在安全位置（OTP/secure element）
- 是否有防回滚（anti-rollback）机制

风险：攻击者推送恶意固件。严重级别：critical。

### SEC-OTA-02: 更新过程中断后设备变砖

检测：固件更新过程中没有 A/B 分区、没有回退机制。

```c
// BAD — 直接覆盖当前固件
flash_erase(FIRMWARE_START, fw_len);
flash_write(FIRMWARE_START, new_fw, fw_len);  // 断电 = 变砖

// GOOD — A/B 分区
flash_erase(INACTIVE_PARTITION, fw_len);
flash_write(INACTIVE_PARTITION, new_fw, fw_len);
if (verify_partition(INACTIVE_PARTITION) == OK) {
    set_boot_partition(INACTIVE_PARTITION);
    system_reset();
}
// bootloader 验证失败时自动回退到旧分区
```

风险：更新中断（断电/通信错误）导致设备永久不可用。严重级别：high。

## 侧信道与故障注入

### SEC-SIDE-01: 密码/密钥比较非恒定时间

检测：使用 `memcmp`、`strcmp`、逐字节提前返回的方式比较密钥或密码。

```c
// BAD — 时间侧信道
if (memcmp(input_key, stored_key, 16) == 0) {
    grant_access();
}

// GOOD — 恒定时间比较
int result = 0;
for (int i = 0; i < 16; i++) {
    result |= input_key[i] ^ stored_key[i];
}
if (result == 0) {
    grant_access();
}
```

注意：编译器可能优化掉恒定时间循环。使用 `volatile` 或平台专用的 `secure_memcmp`。

风险：通过测量比较时间逐字节猜出密钥。严重级别：high。

### SEC-SIDE-02: 安全关键分支可被故障注入跳过

检测：安全检查只有单次 if 判断，没有冗余验证。

```c
// BAD — 单个跳转指令可被 glitch 跳过
if (verify_signature(fw) == OK) {
    boot_firmware(fw);
}

// GOOD — 冗余检查
volatile int check1 = verify_signature(fw);
volatile int check2 = verify_signature(fw);
if (check1 == OK && check2 == OK && check1 == check2) {
    boot_firmware(fw);
}
```

风险：电压毛刺或时钟毛刺跳过认证/签名验证。严重级别：high（安全关键设备为 critical）。

## 输入验证

### SEC-INPUT-01: 外部输入直接用于内存操作

检测：来自 UART、SPI、BLE、USB、网络的数据长度字段直接用于 memcpy/malloc/数组下标。

```c
// BAD
void handle_ble_packet(uint8_t *data, uint16_t raw_len) {
    uint16_t payload_len = data[0] | (data[1] << 8);  // 从包中提取
    uint8_t buf[256];
    memcpy(buf, &data[2], payload_len);  // payload_len 未校验
}

// GOOD
void handle_ble_packet(uint8_t *data, uint16_t raw_len) {
    if (raw_len < 2) return;
    uint16_t payload_len = data[0] | (data[1] << 8);
    if (payload_len > raw_len - 2 || payload_len > sizeof(buf)) return;
    uint8_t buf[256];
    memcpy(buf, &data[2], payload_len);
}
```

风险：缓冲区溢出，代码执行。严重级别：critical。

### SEC-INPUT-02: 命令解析缺少边界检查

检测：协议命令解析器对命令码、参数数量、参数范围缺少完整校验。

```c
// BAD
void handle_command(uint8_t cmd, uint8_t *params) {
    command_table[cmd](params);  // cmd 未检查范围
}

// GOOD
void handle_command(uint8_t cmd, uint8_t *params, uint16_t params_len) {
    if (cmd >= NUM_COMMANDS || command_table[cmd] == NULL) {
        send_error(ERR_INVALID_CMD);
        return;
    }
    if (params_len < command_min_len[cmd]) {
        send_error(ERR_INVALID_PARAMS);
        return;
    }
    command_table[cmd](params, params_len);
}
```

风险：跳转到非法地址、参数越界。严重级别：high。

## 运行时保护

### SEC-RT-01: 未启用栈保护

检测：编译选项中缺少栈保护相关标志。

```makefile
# BAD — 无栈保护
CFLAGS += -O2

# GOOD
CFLAGS += -O2 -fstack-protector-strong  # GCC/Clang 栈金丝雀
# 或
CFLAGS += -O2 -fstack-protector-all     # 更全面但开销更大
```

检查点：
- `-fstack-protector-strong` 或 `-fstack-protector-all`
- FreeRTOS: `configCHECK_FOR_STACK_OVERFLOW` 设为 2
- MPU 是否配置了栈保护区域

风险：栈溢出无法检测，可能被利用执行任意代码。严重级别：medium。

### SEC-RT-02: MPU 未配置或配置不当

检测：有 MPU 的 MCU（Cortex-M3/M4/M7/M33）未启用或未合理配置 MPU。

```c
// 应检查的 MPU 配置项
// 1. 空指针保护：地址 0 区域设为不可访问
// 2. 栈保护：栈底设置 guard region
// 3. flash 不可写：防止代码注入
// 4. 外设区域：限制 task 对外设的访问权限
// 5. DMA buffer：设为 non-cacheable（同时解决 cache coherence）

// FreeRTOS MPU 支持
#define configENABLE_MPU  1
// 每个 task 可定义自己的 MPU region
```

风险：缺少硬件级隔离，一个组件的 bug 影响整个系统。严重级别：medium。

### SEC-RT-03: 看门狗在 ISR 中喂狗

检测：看门狗 feed 操作在定时器中断或其他 ISR 中执行，而非在主循环/task 中。

```c
// BAD — ISR 中喂狗，主循环死锁也检测不到
void TIM6_IRQHandler(void) {
    HAL_IWDG_Refresh(&hiwdg);  // 主循环卡死时 ISR 照常喂狗
}

// GOOD — 在被监控的 task/循环中喂狗
void main_task(void *param) {
    for (;;) {
        process_events();
        HAL_IWDG_Refresh(&hiwdg);  // 只有正常执行到这里才喂狗
    }
}
```

风险：主逻辑死锁/挂死但看门狗不触发复位。严重级别：high。

### SEC-RT-04: 异常/HardFault handler 为空或仅死循环

检测：HardFault_Handler、BusFault_Handler 等异常处理函数为空或只有 `while(1)`。

```c
// BAD
void HardFault_Handler(void) {
    while (1);  // 设备静默死掉，无任何诊断信息
}

// GOOD
void HardFault_Handler(void) {
    // 保存关键寄存器到 retained RAM
    __ASM volatile("MRS r0, MSP");
    save_crash_info(r0);  // 保存 PC、LR、SP、CFSR
    // 记录到 NVS/retained RAM 供下次启动读取
    store_crash_dump_to_nvs();
    NVIC_SystemReset();  // 复位而非死锁
}
```

风险：设备静默死亡，无法诊断现场问题。严重级别：medium。
