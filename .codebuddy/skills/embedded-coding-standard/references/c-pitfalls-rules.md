# C 语言陷阱检测规则

覆盖嵌入式场景中不属于 CERT C/MISRA/BARR-C 标准范畴，但实际高频出错的 C 语言和工具链问题。包括链接器/启动代码、编译器优化、C++ 互操作。

## 链接器与内存布局

### CPIT-LINK-01: 栈和堆区域重叠

检测：链接脚本中 stack 和 heap 没有独立区域定义，或 `_Min_Heap_Size` + `_Min_Stack_Size` > 可用 RAM。

```ld
/* BAD — 只定义了最小值，没有 guard */
_Min_Heap_Size = 0x2000;
_Min_Stack_Size = 0x4000;
/* 如果 .bss + .data 增长，heap 和 stack 会无声重叠 */

/* GOOD — 显式分离 */
.heap (NOLOAD) :
{
    . = ALIGN(8);
    _heap_start = .;
    . += _Min_Heap_Size;
    _heap_end = .;
} > RAM

.stack (NOLOAD) :
{
    . = ALIGN(8);
    . += _Min_Stack_Size;
    _estack = .;
} > RAM

/* 或用 ASSERT 检测溢出 */
ASSERT(_heap_end <= _estack - _Min_Stack_Size, "Heap/stack overlap!")
```

风险：堆增长覆盖栈或栈增长覆盖堆，数据损坏且极难排查。严重级别：critical。

### CPIT-LINK-02: section 放置错误

检测：性能关键数据/代码放在错误的 memory region（flash vs SRAM vs DTCM/ITCM）。

```c
// BAD — 频繁访问的 lookup table 放在 flash
static const uint16_t sin_table[4096] = { ... };
// 在 Cortex-M7 上，flash 访问有 wait state，DTCM 是零等待

// GOOD — 放在 DTCM/快速 SRAM
__attribute__((section(".dtcm_data")))
static const uint16_t sin_table[4096] = { ... };

// BAD — DMA buffer 放在 DTCM（Cortex-M7 的 DTCM 不连接到 DMA 总线）
__attribute__((section(".dtcm_data")))
uint8_t dma_rx_buf[256];  // DMA 无法访问

// GOOD — DMA buffer 放在普通 SRAM
__attribute__((section(".sram1")))
uint8_t dma_rx_buf[256];
```

风险：性能问题或 DMA 无法访问目标内存。严重级别：high。

### CPIT-LINK-03: .bss 初始化依赖缺失

检测：startup 代码未清零 .bss 段，或自定义 section 的零初始化被遗漏。

```c
// 风险场景：自定义 section 不在默认 .bss 中
__attribute__((section(".my_bss")))
static uint32_t my_data[64];
// 如果 startup 代码没有显式清零 .my_bss，my_data 初始值不确定

// 检查 startup.s 中是否包含：
// 1. .bss 段清零循环
// 2. .data 段从 flash 拷贝到 RAM 的循环
// 3. 自定义 section 的处理
```

风险：全局变量初始值不确定，行为随机。严重级别：high。

### CPIT-LINK-04: weak symbol 静默吞错误

检测：中断向量表中的 handler 使用 `__weak` 默认实现（通常是死循环或空函数），实际需要的 handler 因拼写错误或链接问题没有覆盖 weak 版本。

```c
// startup.s 中的默认 handler
.weak USART1_IRQHandler
.thumb_set USART1_IRQHandler, Default_Handler
// Default_Handler 通常是 while(1)

// BAD — 拼写错误，实际 handler 未链接
void USART_IRQHandler(void) {  // 少了 "1"
    // 这个函数永远不会被调用
    // 实际中断触发 Default_Handler → 死循环
}

// GOOD — 名称与向量表完全匹配
void USART1_IRQHandler(void) {
    // 正确的 handler
}
```

检查方法：对比 `.map` 文件中实际链接的 handler 地址，是否都指向用户代码而非 Default_Handler。

风险：中断触发后系统静默死掉。严重级别：high。

## 编译器优化陷阱

### CPIT-OPT-01: 安全擦除被优化掉

检测：用 `memset` 清零敏感数据，但 buffer 之后未被使用，编译器可能在 -O2 下删除该 memset。

```c
// BAD
void process_key(const uint8_t *key) {
    uint8_t local_key[32];
    memcpy(local_key, key, 32);
    do_crypto(local_key);
    memset(local_key, 0, 32);  // 编译器可能删除
}

// GOOD
#include <string.h>
// 使用 explicit_bzero（POSIX）或自定义 volatile 擦除
void secure_zero(void *ptr, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) *p++ = 0;
}

void process_key(const uint8_t *key) {
    uint8_t local_key[32];
    memcpy(local_key, key, 32);
    do_crypto(local_key);
    secure_zero(local_key, 32);
}
```

风险：密钥残留在栈上。严重级别：high。

### CPIT-OPT-02: 延时循环被优化掉

检测：空循环或无副作用的循环用于硬件延时。

```c
// BAD — 编译器在 -O2 下直接删除
void delay_us(uint32_t us) {
    for (uint32_t i = 0; i < us * 72; i++);  // 被优化掉
}

// GOOD — 使用硬件定时器
void delay_us(uint32_t us) {
    TIM2->CNT = 0;
    while (TIM2->CNT < us);
}

// 或使用 DWT cycle counter（Cortex-M3+）
void delay_us(uint32_t us) {
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000);
    while ((DWT->CYCCNT - start) < ticks);
}

// 或至少用 volatile
for (volatile uint32_t i = 0; i < us * 72; i++);
```

风险：硬件时序不满足，外设初始化失败或通信错误。严重级别：high。

### CPIT-OPT-03: -O2 下整数溢出检查被删除

检测：有符号整数溢出检查在高优化级别下被删除。

```c
// BAD — 编译器认为有符号溢出不会发生（UB），可能删除检查
int32_t safe_add(int32_t a, int32_t b) {
    int32_t result = a + b;
    if (a > 0 && b > 0 && result < 0) return INT32_MAX;  // 可能被删除
    return result;
}

// GOOD — 在溢出前检查
int32_t safe_add(int32_t a, int32_t b) {
    if (a > 0 && b > INT32_MAX - a) return INT32_MAX;
    if (a < 0 && b < INT32_MIN - a) return INT32_MIN;
    return a + b;
}

// 或使用 __builtin_add_overflow（GCC/Clang）
int32_t result;
if (__builtin_add_overflow(a, b, &result)) {
    return INT32_MAX;
}
```

风险：安全检查失效，溢出未被捕获。严重级别：high。

## C++ 互操作

### CPIT-CPP-01: 中断 handler 缺少 extern "C"

检测：C++ 文件中定义的中断处理函数没有 `extern "C"` 包裹，C++ name mangling 导致向量表链接不到实际 handler。

```cpp
// BAD — C++ name mangling，向量表找不到
void USART1_IRQHandler(void) {  // 实际符号是 _Z19USART1_IRQHandlerv
    // ...
}

// GOOD
extern "C" {
void USART1_IRQHandler(void) {
    // ...
}
}

// 或在头文件中统一处理
#ifdef __cplusplus
extern "C" {
#endif
void USART1_IRQHandler(void);
void SysTick_Handler(void);
#ifdef __cplusplus
}
#endif
```

风险：中断触发 Default_Handler（死循环），系统静默死掉。严重级别：critical。

### CPIT-CPP-02: C++ 静态初始化顺序问题

检测：不同编译单元中的全局/静态 C++ 对象互相依赖。

```cpp
// file_a.cpp
Logger logger("system");  // 全局对象

// file_b.cpp
extern Logger logger;
Config config(logger);  // 依赖 logger，但初始化顺序未定义
```

风险：访问未初始化的对象，崩溃或数据损坏。严重级别：high。

### CPIT-CPP-03: C++ 全局构造函数在 RTOS 启动前执行

检测：C++ 全局对象的构造函数使用了 RTOS API（mutex、queue、task 创建）。

```cpp
// BAD — 构造函数在 main() 之前执行，RTOS 未启动
class SensorDriver {
public:
    SensorDriver() {
        mutex_ = xSemaphoreCreateMutex();  // RTOS 未启动！
    }
private:
    SemaphoreHandle_t mutex_;
};
SensorDriver g_sensor;  // 全局构造在 __libc_init_array 中

// GOOD — 延迟初始化
class SensorDriver {
public:
    void init() {
        mutex_ = xSemaphoreCreateMutex();  // 在 RTOS 启动后调用
    }
};
```

风险：RTOS 内核未初始化时调用 API，HardFault 或内核数据损坏。严重级别：critical。

## 可移植性陷阱

### CPIT-PORT-01: char 默认符号假设

检测：代码假设 `char` 是有符号的（x86 默认），但 ARM 默认 `char` 是无符号的。

```c
// BAD
char c = 0xFF;
if (c < 0) {  // ARM 上 c = 255 (unsigned)，永远为假
    handle_error();
}

// BAD — 符号扩展问题
char buf[64];
uart_read(buf, len);
int16_t value = buf[0] | (buf[1] << 8);
// 如果 buf[0] = 0xFF，ARM 上 (int)buf[0] = 255
// x86 上 (int)buf[0] = -1 = 0xFFFFFFFF

// GOOD — 显式指定符号
uint8_t buf[64];
uart_read(buf, len);
int16_t value = (int16_t)(buf[0] | (buf[1] << 8));
```

风险：同一代码在 x86 host 测试通过但 ARM target 上行为不同。严重级别：medium。

### CPIT-PORT-02: struct 布局用于协议/硬件映射

检测：struct 直接通过指针映射到网络协议帧或硬件寄存器，依赖特定的 padding 和字节序。

```c
// BAD
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint16_t length;
    uint32_t payload;
} __attribute__((packed)) Packet;

Packet *pkt = (Packet *)rx_buf;
uint32_t data = pkt->payload;  // 字节序问题 + 可能的对齐问题

// GOOD — 显式解析
uint8_t type = rx_buf[0];
uint16_t length = (uint16_t)rx_buf[1] | ((uint16_t)rx_buf[2] << 8);  // 明确小端
uint32_t payload;
memcpy(&payload, &rx_buf[3], sizeof(payload));  // 安全的未对齐访问
payload = le32_to_cpu(payload);  // 显式字节序转换
```

风险：不同平台上 padding/字节序不同，数据解析错误。严重级别：high。

## 危险函数速查表

以下函数在嵌入式代码中出现时应自动标记审查：

| 危险函数 | 风险 | 安全替代 | 严重级别 |
|---------|------|---------|---------|
| `sprintf` | buffer 溢出 | `snprintf` | high |
| `strcpy` | buffer 溢出 | `strncpy` + null 终止 / `strlcpy` | high |
| `strcat` | buffer 溢出 | `strncat` + 剩余大小计算 | high |
| `gets` | 无边界限制 | `fgets` | critical |
| `scanf("%s")` | 无宽度限制 | `scanf("%255s")` | high |
| `atoi` / `atol` | 无错误检测 | `strtol` + errno | medium |
| `strtok` | 非可重入 | `strtok_r` | medium (多任务时 high) |
| `rand` / `srand` | 非密码安全 | 硬件 TRNG / `mbedtls_entropy` | medium (安全场景 high) |
| `alloca` / VLA | 栈溢出不可控 | 固定大小 buffer 或堆分配 | high |
| `memcpy(d,s,n)` | n 未校验 | 校验 n <= dst_size | high (外部输入时 critical) |
| `free` | double free / use after | 设为 NULL / 限制使用 | high |

检测方式：对 diff 中出现的上述函数名做字符串匹配，命中后检查是否有边界保护。
