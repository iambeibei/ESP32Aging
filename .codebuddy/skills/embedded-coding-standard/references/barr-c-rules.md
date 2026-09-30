# BARR-C 检测规则

基于 Barr Group Embedded C Coding Standard 的公开定位和工程目标（减少缺陷、提升可维护性、提升可移植性），转换为模型可直接匹配的检测模式。不复述标准原文，使用"BARR-C 导向 finding"措辞。

## 寄存器与硬件访问

### BARR-HW-01: 寄存器地址未用 volatile 修饰

检测：MMIO 寄存器地址通过裸指针访问，没有 volatile 限定。

```c
// BAD
#define STATUS_REG (*(uint32_t *)0x40001000)
while (STATUS_REG & BUSY) {}  // 编译器可能优化为只读一次

// GOOD
#define STATUS_REG (*(volatile uint32_t *)0x40001000)
while (STATUS_REG & BUSY) {}
```

BARR-C 原则：所有硬件寄存器访问必须通过 volatile 指针。严重级别：critical。

### BARR-HW-02: 魔法数直接用于寄存器配置

检测：寄存器值、位偏移、掩码直接用十六进制或十进制数字面量，没有命名常量。

```c
// BAD
*(volatile uint32_t *)0x40021018 |= 0x00000010;  // 无法理解含义

// GOOD
#define RCC_APB2ENR  (*(volatile uint32_t *)0x40021018)
#define RCC_APB2ENR_IOPAEN  (1U << 4)
RCC_APB2ENR |= RCC_APB2ENR_IOPAEN;
```

BARR-C 原则：所有寄存器地址、位域、掩码必须有描述性命名。严重级别：medium。

### BARR-HW-03: 对状态寄存器做 read-modify-write

检测：包含 W1C（Write-1-to-Clear）或 sticky 位的状态寄存器被 `|=` 或 `&=` 操作。

```c
// BAD — 状态寄存器包含 W1C 位
USART1->SR &= ~USART_SR_TC;  // 读 SR 时读到其他 W1C 位为 1
                               // 写回时把它们也清了

// GOOD — 只写要清的位
USART1->SR = ~USART_SR_TC;   // 或使用硬件提供的专门清除寄存器
// 具体写法取决于硬件，需查手册
```

BARR-C 原则：状态寄存器访问必须理解每个位的清除语义。严重级别：high。

### BARR-HW-04: 外设初始化顺序不正确

检测：使能外设时钟和配置外设的顺序错误，或多个互相依赖的配置步骤缺少必要的等待/序列化。

```c
// BAD — 先配置再使能时钟
USART1->CR1 = USART_CR1_TE | USART_CR1_RE;  // 时钟还没开
RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

// GOOD
RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
__DSB();  // 确保时钟使能生效
USART1->CR1 = USART_CR1_TE | USART_CR1_RE;
```

BARR-C 原则：外设初始化必须遵循硬件要求的顺序。严重级别：high。

### BARR-HW-05: 轮询循环无超时

检测：while 循环等待硬件状态变化，没有超时机制。

```c
// BAD
while (!(SPI1->SR & SPI_SR_TXE)) {}  // 如果硬件故障，永远卡死

// GOOD
uint32_t timeout = get_tick() + TIMEOUT_MS;
while (!(SPI1->SR & SPI_SR_TXE)) {
    if (get_tick() > timeout) {
        return HAL_TIMEOUT;
    }
}
```

BARR-C 原则：所有硬件等待必须有超时保护。严重级别：high。

## 命名与类型

### BARR-NAME-01: 布尔变量/函数命名不明确

检测：返回布尔值的函数或布尔变量没有表达是/否语义的命名。

```c
// BAD
int sensor(void);        // 返回什么？状态？值？
uint8_t uart;            // 是计数？是标志？是索引？

// GOOD
bool is_sensor_ready(void);
bool uart_tx_complete;
```

BARR-C 原则：布尔值命名应以 is_/has_/can_/should_ 开头或以形容词结尾。严重级别：low。

### BARR-NAME-02: 单位歧义

检测：超时、长度、频率、电压等物理量的变量名或函数参数名不包含单位。

```c
// BAD
uint32_t timeout = 1000;     // ms? ticks? us?
uint16_t length = 256;       // bytes? words? elements?
void set_frequency(uint32_t freq);  // Hz? kHz? MHz?

// GOOD
uint32_t timeout_ms = 1000;
uint16_t length_bytes = 256;
void set_frequency_hz(uint32_t freq_hz);
```

BARR-C 原则：变量名必须包含单位，或类型本身编码单位。严重级别：medium。

### BARR-NAME-03: 类型宽度不明确

检测：使用 `int`、`short`、`long`、`char` 而非固定宽度类型（`uint8_t`、`int16_t`、`uint32_t`）。

```c
// BAD
int counter;
short adc_value;
unsigned long timestamp;

// GOOD
uint32_t counter;
int16_t adc_value;
uint32_t timestamp;
```

例外：`main` 的返回类型、标准库接口参数可以使用标准类型。

BARR-C 原则：嵌入式代码必须使用固定宽度类型以确保跨平台一致性。严重级别：medium。

## 模块与分层

### BARR-MOD-01: 应用逻辑直接操作寄存器

检测：非驱动文件中直接访问硬件寄存器地址。

```c
// BAD — 在应用层
void app_process_data(void) {
    while (!(USART1->SR & USART_SR_TXE)) {}  // 应用层直接碰寄存器
    USART1->DR = data;
}

// GOOD
void app_process_data(void) {
    uart_send_byte(data);  // 通过 HAL/驱动接口
}
```

BARR-C 原则：硬件访问必须封装在驱动/HAL 层。严重级别：medium。

### BARR-MOD-02: 头文件缺少 include guard

检测：头文件没有 `#ifndef` / `#pragma once` 保护。

```c
// BAD — 多次 include 导致重复定义
// sensor.h
typedef struct { ... } SensorConfig;

// GOOD
// sensor.h
#ifndef SENSOR_H
#define SENSOR_H
typedef struct { ... } SensorConfig;
#endif /* SENSOR_H */
```

BARR-C 原则：每个头文件必须有 include guard。严重级别：medium。

### BARR-MOD-03: 头文件暴露内部实现细节

检测：头文件中包含 static 函数声明、内部数据结构、内部宏。

```c
// BAD — sensor.h
static void calibrate_internal(void);  // 内部函数不应在头文件
#define INTERNAL_CALIBRATION_OFFSET 42  // 内部常量

// GOOD — 只暴露公开接口
// sensor.h
HAL_StatusTypeDef sensor_init(const SensorConfig *cfg);
HAL_StatusTypeDef sensor_read(int16_t *value);
```

BARR-C 原则：头文件只暴露模块的公开接口。严重级别：low。

### BARR-MOD-04: 全局变量跨模块直接访问

检测：模块的全局变量（非 static）被其他模块直接 `extern` 引用，而不是通过 getter/setter 函数。

```c
// BAD
// uart.c
uint32_t uart_baud_rate = 115200;  // 非 static，任何人可改

// app.c
extern uint32_t uart_baud_rate;
uart_baud_rate = 9600;  // 直接改，无验证

// GOOD
// uart.c
static uint32_t baud_rate = 115200;
HAL_StatusTypeDef uart_set_baud(uint32_t rate) {
    if (rate < 1200 || rate > 921600) return HAL_ERROR;
    baud_rate = rate;
    return reconfigure_uart();
}
```

BARR-C 原则：模块数据必须私有（static），通过函数接口访问。严重级别：medium。

## 错误处理与防御

### BARR-ERR-01: 返回值被忽略

检测：可能失败的函数返回值被丢弃（void cast 除外）。嵌入式中尤其注意 HAL 函数、外设操作、内存分配。

```c
// BAD
HAL_I2C_Mem_Read(&hi2c, addr, reg, I2C_MEMADD_SIZE_8BIT, buf, len, 100);
// 总线错误？NACK？超时？全部被吞掉

// GOOD
HAL_StatusTypeDef status = HAL_I2C_Mem_Read(&hi2c, addr, reg,
    I2C_MEMADD_SIZE_8BIT, buf, len, 100);
if (status != HAL_OK) {
    log_error(ERR_I2C_READ, status);
    return status;
}
```

BARR-C 原则：每个可能失败的函数调用的返回值必须检查。严重级别：high。

### BARR-ERR-02: 错误码信息不足

检测：函数对多种不同故障情况返回同一个错误码。

```c
// BAD
int sensor_init(void) {
    if (!check_id()) return -1;
    if (!calibrate()) return -1;  // 和上面同一个 -1
    if (!self_test()) return -1;  // 还是 -1
    return 0;
}
// 调用者无法区分哪一步失败

// GOOD
typedef enum {
    SENSOR_OK = 0,
    SENSOR_ERR_ID,
    SENSOR_ERR_CALIBRATE,
    SENSOR_ERR_SELF_TEST,
} SensorStatus;

SensorStatus sensor_init(void) {
    if (!check_id())   return SENSOR_ERR_ID;
    if (!calibrate())  return SENSOR_ERR_CALIBRATE;
    if (!self_test())  return SENSOR_ERR_SELF_TEST;
    return SENSOR_OK;
}
```

BARR-C 原则：错误码必须能区分故障类型，支持排障。严重级别：medium。

### BARR-ERR-03: 清理路径不完整

检测：函数在错误路径中途返回，但没有释放已获取的资源（锁、内存、外设使能状态）。

```c
// BAD
HAL_StatusTypeDef transfer(void) {
    uint8_t *buf = pvPortMalloc(256);
    xSemaphoreTake(spi_mutex, portMAX_DELAY);
    HAL_StatusTypeDef status = HAL_SPI_Transmit(&hspi, buf, 256, 1000);
    if (status != HAL_OK) {
        return status;  // mutex 未释放，buf 未 free
    }
    xSemaphoreGive(spi_mutex);
    vPortFree(buf);
    return HAL_OK;
}

// GOOD
HAL_StatusTypeDef transfer(void) {
    HAL_StatusTypeDef status;
    uint8_t *buf = pvPortMalloc(256);
    if (buf == NULL) return HAL_ERROR;

    xSemaphoreTake(spi_mutex, portMAX_DELAY);
    status = HAL_SPI_Transmit(&hspi, buf, 256, 1000);
    xSemaphoreGive(spi_mutex);
    vPortFree(buf);
    return status;
}
```

BARR-C 原则：所有执行路径必须正确释放资源。严重级别：high。

### BARR-ERR-04: 缺少防御性 assert

检测：函数入口对指针参数、范围参数没有 assert 保护，在调试阶段无法提早发现调用错误。

```c
// BAD
void dma_configure(DMA_Channel *ch, uint8_t *buf, uint16_t len) {
    ch->MAR = (uint32_t)buf;  // ch 或 buf 为 NULL 时直接崩溃
    ch->NDTR = len;
}

// GOOD
#include <assert.h>
void dma_configure(DMA_Channel *ch, uint8_t *buf, uint16_t len) {
    assert(ch != NULL);
    assert(buf != NULL);
    assert(len > 0 && len <= DMA_MAX_TRANSFER);
    ch->MAR = (uint32_t)buf;
    ch->NDTR = len;
}
```

BARR-C 原则：在调试构建中使用 assert 保护前置条件。严重级别：low。

## 可移植性

### BARR-PORT-01: sizeof 依赖

检测：代码隐式依赖 `sizeof(int)`、`sizeof(long)`、`sizeof(pointer)` 的具体值。

```c
// BAD
uint8_t buf[4];  // 假设 sizeof(uint32_t) == 4 来存储 uint32_t
memcpy(buf, &value, 4);

// GOOD
uint8_t buf[sizeof(uint32_t)];
memcpy(buf, &value, sizeof(value));
```

BARR-C 原则：使用 sizeof 运算符而非硬编码大小。严重级别：medium。

### BARR-PORT-02: 字节序假设

检测：代码假设特定字节序进行序列化/反序列化，没有显式转换。

```c
// BAD — 假设小端
uint32_t value;
memcpy(&value, rx_buffer, 4);  // 在大端平台上结果错误

// GOOD
uint32_t value = (uint32_t)rx_buffer[0]
              | ((uint32_t)rx_buffer[1] << 8)
              | ((uint32_t)rx_buffer[2] << 16)
              | ((uint32_t)rx_buffer[3] << 24);
```

BARR-C 原则：协议数据和持久化数据必须显式处理字节序。严重级别：high。

### BARR-PORT-03: 编译器扩展未隔离

检测：`__attribute__`、`__packed`、`__weak`、`#pragma`、`__asm` 等编译器扩展散落在代码中，没有通过条件编译宏封装。

```c
// BAD
void __attribute__((weak)) system_error_handler(void) {}

// GOOD
#if defined(__GNUC__)
  #define WEAK __attribute__((weak))
#elif defined(__ICCARM__)
  #define WEAK __weak
#else
  #define WEAK
#endif
void WEAK system_error_handler(void) {}
```

BARR-C 原则：编译器扩展必须通过可移植宏隔离。严重级别：low。

## 代码可读性

### BARR-READ-01: 状态机用零散 flag 隐式表达

检测：多个布尔 flag 组合表达状态，而非使用显式枚举状态机。

```c
// BAD
static bool initialized;
static bool running;
static bool error;
// 8 种组合，哪些是合法的？initialized && !running && error 什么含义？

// GOOD
typedef enum {
    STATE_UNINIT,
    STATE_IDLE,
    STATE_RUNNING,
    STATE_ERROR,
} ModuleState;
static ModuleState state = STATE_UNINIT;
```

BARR-C 原则：复合状态必须用枚举显式表达，非法状态组合应在编译期或运行时不可能出现。严重级别：medium。

### BARR-READ-02: 复杂条件表达式未分解

检测：if/while 条件包含 3 个以上子条件，或条件体超过一行。

```c
// BAD
if (uart->SR & USART_SR_RXNE && !(uart->CR1 & USART_CR1_RXNEIE) && state != STATE_IDLE && retry < MAX_RETRY) {

// GOOD
bool data_available = uart->SR & USART_SR_RXNE;
bool irq_disabled = !(uart->CR1 & USART_CR1_RXNEIE);
bool active = state != STATE_IDLE;
bool can_retry = retry < MAX_RETRY;
if (data_available && irq_disabled && active && can_retry) {
```

BARR-C 原则：复杂条件必须分解为命名的中间变量。严重级别：low。

### BARR-READ-03: 函数过长

检测：函数体超过 75 行（不计空行和注释）。嵌入式中尤其注意初始化函数和状态机处理函数。

说明：这条规则在审查时降低优先级。只有当长函数同时包含多个抽象层级（寄存器操作 + 协议解析 + 业务逻辑）时才标记。单纯因为初始化步骤多而导致的长度不视为问题。

BARR-C 原则：函数应短小，每个函数只做一件事。严重级别：low。
