# CERT C 检测规则

基于 CERT C Secure Coding Standard 公开 wiki，筛选嵌入式场景高频规则，转换为模型可直接匹配的检测模式。

## ARR — 数组与边界

### ARR30-C: 不要越界访问数组

检测：数组下标或指针算术可能超出分配范围。常见于外部输入作为下标、循环边界 off-by-one、memcpy/memset 长度未校验。

```c
// BAD
void process(uint8_t *data, uint16_t len) {
    uint8_t buf[256];
    memcpy(buf, data, len);  // len 可能 > 256
}

// GOOD
void process(uint8_t *data, uint16_t len) {
    uint8_t buf[256];
    if (len > sizeof(buf)) { return; }
    memcpy(buf, data, len);
}
```

风险：栈溢出、堆破坏。嵌入式无 ASLR，溢出后果更严重。严重级别：critical。

### ARR36-C: 不要对不同数组的指针做减法或比较

检测：两个指针来自不同 buffer 却做 `p1 - p2` 或 `p1 < p2`。

```c
// BAD
uint8_t bufA[64], bufB[64];
ptrdiff_t diff = &bufA[10] - &bufB[5];  // 未定义行为
```

风险：未定义行为，结果不可预测。严重级别：high。

### ARR38-C: 保证数组下标在有效范围内

检测：枚举值、外部命令码、通道号直接用作数组下标，无范围检查。嵌入式常见于查表操作。

```c
// BAD
const Handler handlers[] = { handler0, handler1, handler2 };
void dispatch(uint8_t cmd) {
    handlers[cmd]();  // cmd 可能 >= 3
}

// GOOD
void dispatch(uint8_t cmd) {
    if (cmd < sizeof(handlers) / sizeof(handlers[0])) {
        handlers[cmd]();
    }
}
```

风险：跳转到非法地址，嵌入式中可能触发 HardFault 或执行随机内存。严重级别：critical。

## INT — 整数问题

### INT30-C: 确保无符号整数运算不回绕

检测：无符号减法结果可能下溢、无符号加法可能回绕，尤其用于 buffer 大小计算、DMA 长度、寄存器配置值。

```c
// BAD
uint16_t remaining = total_len - processed;  // processed > total_len 时回绕到极大值
memcpy(dst, src, remaining);

// GOOD
if (processed > total_len) { return ERROR; }
uint16_t remaining = total_len - processed;
```

风险：回绕后传入 memcpy 导致大面积越界写入。严重级别：critical。

### INT31-C: 确保整数转换不丢失或误解数据

检测：宽类型赋值给窄类型、有符号与无符号隐式转换，尤其在寄存器配置、DMA 长度、协议字段拆包处。

```c
// BAD
uint32_t total = get_file_size();  // 可能 > 65535
uint16_t dma_len = total;          // 截断
HAL_UART_Transmit_DMA(&huart, buf, dma_len);

// GOOD
uint32_t total = get_file_size();
if (total > UINT16_MAX) { return ERROR; }
uint16_t dma_len = (uint16_t)total;
```

风险：DMA 传输长度被截断，只传部分数据或溢出。严重级别：high。

### INT32-C: 确保有符号整数运算不溢出

检测：有符号乘法/加法用于 buffer 大小计算、timeout 计算、ADC 值换算。

```c
// BAD
int16_t offset = base + delta;  // 可能溢出
int16_t scaled = adc_raw * calibration_factor;  // 可能溢出

// GOOD
int32_t tmp = (int32_t)base + delta;
if (tmp > INT16_MAX || tmp < INT16_MIN) { return ERROR; }
int16_t offset = (int16_t)tmp;
```

风险：有符号溢出是未定义行为，编译器可能优化掉后续检查。严重级别：high。

### INT34-C: 不要移位负值或移位量 >= 位宽

检测：移位操作的左操作数可能为负、右操作数可能 >= 类型位宽或为负。嵌入式常见于位域操作和寄存器配置。

```c
// BAD
uint32_t mask = 1 << bit_pos;  // bit_pos >= 32 时未定义行为
                                // 且 1 是 signed int，bit_pos=31 时溢出

// GOOD
uint32_t mask = (uint32_t)1U << bit_pos;  // 1U 避免有符号溢出
// 仍需确保 bit_pos < 32
```

风险：未定义行为，不同编译器/优化级别结果不同。严重级别：high。

### INT36-C: 指针与整数之间的转换

检测：指针转整数或整数转指针，常见于 MMIO 地址定义、DMA 地址配置。

```c
// BAD — 地址截断
uint32_t addr = (uint32_t)large_buffer;  // 在 64 位系统上截断

// 嵌入式中合理用法（需确保平台地址宽度匹配）
#define PERIPH_BASE  ((uint32_t)0x40000000U)
volatile uint32_t *reg = (volatile uint32_t *)PERIPH_BASE;
```

风险：地址截断导致访问错误内存。在 32 位 MCU 上通常安全，但跨平台移植时出错。严重级别：medium。

## MEM — 内存管理

### MEM30-C: 不要访问已释放的内存

检测：free/pool_release 后继续使用指针、callback context 在对象释放后仍被调用。嵌入式中尤其注意 DMA 完成回调和 timer 回调。

```c
// BAD
void start_transfer(void) {
    uint8_t *buf = pvPortMalloc(256);
    HAL_SPI_Transmit_DMA(&hspi, buf, 256);
    vPortFree(buf);  // DMA 还在使用 buf
}

// GOOD
// 在 DMA 完成回调里释放
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi) {
    vPortFree(tx_buf);
    tx_buf = NULL;
}
```

风险：DMA 写入已释放内存，数据损坏或覆盖其他分配。严重级别：critical。

### MEM33-C: 正确分配含柔性数组成员的结构体

检测：`malloc(sizeof(struct))` 没有加上柔性数组的实际大小。

```c
// BAD
typedef struct {
    uint16_t len;
    uint8_t data[];
} Packet;
Packet *p = malloc(sizeof(Packet));  // data[] 大小为 0

// GOOD
Packet *p = malloc(sizeof(Packet) + payload_len);
```

风险：写入 data[] 时越界。严重级别：high。

### MEM35-C: 为对象分配足够的内存

检测：`sizeof` 用错（`sizeof(ptr)` 而非 `sizeof(*ptr)` 或 `sizeof(type)`）、分配大小计算有整数溢出。

```c
// BAD
int *arr = malloc(sizeof(arr) * count);  // sizeof(arr) 是指针大小，不是 int 大小

// GOOD
int *arr = malloc(sizeof(*arr) * count);
```

风险：分配不足导致后续写入越界。严重级别：high。

## EXP — 表达式与未定义行为

### EXP30-C: 不要依赖副作用的求值顺序

检测：同一表达式中对同一变量有多个副作用，或读取与写入未通过序列点分隔。

```c
// BAD
buf[i++] = buf[i];  // i 的读写顺序未定义
a = a++ + ++a;       // 多重副作用

// GOOD
buf[i] = buf[i + 1];
i++;
```

风险：不同编译器/优化级别产生不同结果。严重级别：high。

### EXP32-C: 不要通过非 volatile 引用访问 volatile 对象

检测：volatile 寄存器地址被赋值给普通指针后访问，编译器可能优化掉读取。

```c
// BAD
volatile uint32_t *status_reg = (volatile uint32_t *)0x40001000;
uint32_t *p = (uint32_t *)status_reg;  // 丢失 volatile
while (*p & BUSY_FLAG);  // 编译器可能只读一次

// GOOD
while (*status_reg & BUSY_FLAG);
```

风险：轮询被优化为无限循环或单次读取。严重级别：critical。

### EXP33-C: 不要读取未初始化的内存

检测：局部变量未赋值就使用、结构体部分字段初始化后整体使用、出错路径跳过初始化但后续仍读取。

```c
// BAD
uint8_t status;
if (condition) { status = read_sensor(); }
send_response(status);  // condition 为假时 status 未初始化

// GOOD
uint8_t status = DEFAULT_STATUS;
if (condition) { status = read_sensor(); }
send_response(status);
```

风险：未初始化值在嵌入式中通常是上次栈帧残留，可能泄露信息或导致错误状态。严重级别：high。

### EXP34-C: 不要解引用空指针

检测：函数返回值可能为 NULL 但未检查就解引用，尤其 malloc/pvPortMalloc/查找函数。

```c
// BAD
Config *cfg = find_config(id);
uint32_t val = cfg->value;  // cfg 可能为 NULL

// GOOD
Config *cfg = find_config(id);
if (cfg == NULL) { return ERROR; }
uint32_t val = cfg->value;
```

风险：嵌入式中 NULL 解引用通常触发 HardFault。严重级别：high。

### EXP36-C: 不要将指针转换为对齐要求更严格的类型

检测：`uint8_t *` 强转为 `uint32_t *`、协议 buffer 直接转结构体指针。

```c
// BAD
uint8_t rx_buf[64];
uint32_t *word_ptr = (uint32_t *)&rx_buf[1];  // 未对齐
uint32_t val = *word_ptr;  // ARM Cortex-M 可能 HardFault

// GOOD
uint32_t val;
memcpy(&val, &rx_buf[1], sizeof(val));
```

风险：未对齐访问在某些 ARM 核上触发 HardFault，某些核上静默执行但速度慢或结果错误。严重级别：high。

### EXP39-C: 不要通过不兼容类型的指针访问变量

检测：违反 strict aliasing 规则，如 `uint32_t` 和 `float` 之间通过指针转换互访。

```c
// BAD
uint32_t raw = read_register();
float *fval = (float *)&raw;  // strict aliasing violation
float result = *fval;

// GOOD — 使用 memcpy 或 union
float result;
memcpy(&result, &raw, sizeof(result));
```

风险：编译器在 -O2 下可能基于 strict aliasing 优化掉访问。严重级别：medium。

### EXP45-C: 不要在选择语句中做赋值

检测：`if (x = value)` 而非 `if (x == value)`。

```c
// BAD
if (status = HAL_OK) {  // 赋值而非比较，始终为真
    proceed();
}

// GOOD
if (status == HAL_OK) {
    proceed();
}
```

风险：条件判断永远为真或永远为假，逻辑完全错误。严重级别：high。

## CON — 并发

### CON32-C: 防止多线程/多上下文访问位域时的数据竞争

检测：结构体中的位域被不同 task 或 ISR 和 task 同时访问。编译器对位域的读改写不是原子的。

```c
// BAD
struct {
    uint32_t enabled : 1;
    uint32_t ready   : 1;
    uint32_t error   : 1;
} flags;
// ISR 写 flags.ready，task 写 flags.enabled — 读改写冲突

// GOOD — 用独立变量或原子操作
volatile uint32_t flags;
#define FLAG_ENABLED (1U << 0)
#define FLAG_READY   (1U << 1)
// ISR: atomic_set_bits(&flags, FLAG_READY);
```

风险：位域读改写非原子，ISR 和 task 并发修改会互相覆盖。严重级别：critical。

### CON33-C: 避免库函数的竞态条件

检测：在多任务环境中使用 strtok、localtime、rand 等非可重入标准库函数。

```c
// BAD — 多个 task 调用
void parse_cmd(char *input) {
    char *token = strtok(input, ",");  // 全局内部状态
}

// GOOD
char *saveptr;
char *token = strtok_r(input, ",", &saveptr);
```

风险：内部静态状态被并发覆盖，解析结果错乱。严重级别：high。

### CON34-C: 共享对象必须有足够的存储持续时间

检测：栈上对象的地址传给另一个 task 或 ISR 异步使用。

```c
// BAD
void start_worker(void) {
    WorkItem item = { .type = CMD_READ, .addr = 0x100 };
    xQueueSend(work_queue, &item, 0);
}  // item 在栈上，函数返回后可能被覆盖
   // 注意：xQueueSend 会拷贝 item，所以这里其实是安全的
   // 但如果 queue 存的是指针就不安全：

// 真正的 BAD
void start_worker(void) {
    WorkItem item = { .type = CMD_READ, .addr = 0x100 };
    xQueueSend(ptr_queue, &(WorkItem *){&item}, 0);  // 发送指针
}  // task 收到指针时 item 已失效

// GOOD
WorkItem *item = pvPortMalloc(sizeof(*item));
*item = (WorkItem){ .type = CMD_READ, .addr = 0x100 };
xQueueSend(ptr_queue, &item, 0);  // receiver 负责 free
```

风险：consumer 读到已失效的栈数据，行为不可预测。严重级别：critical。

### CON35-C: 按固定顺序加锁避免死锁

检测：两个或多个 mutex 在不同路径中以不同顺序获取。

```c
// BAD
// Task A: take(mutexX) → take(mutexY)
// Task B: take(mutexY) → take(mutexX) → 死锁

// GOOD — 始终按固定顺序
// 所有路径: take(mutexX) → take(mutexY)
```

风险：死锁导致系统停止响应。嵌入式中通常没有死锁检测机制。严重级别：critical。

### CON40-C: 不要在同一表达式中两次引用原子变量

检测：同一原子变量在表达式中出现两次，两次读取之间值可能已变。

```c
// BAD
atomic_uint counter;
if (counter > 0 && counter < 100) {  // 两次读取，中间可能变化
    process(counter);                  // 第三次读取
}

// GOOD
uint32_t val = atomic_load(&counter);
if (val > 0 && val < 100) {
    process(val);
}
```

风险：TOCTOU 竞态，条件检查和使用之间值已变。严重级别：high。

## ERR — 错误处理

### ERR33-C: 检测并处理标准库/系统调用错误

检测：HAL 函数、RTOS API、malloc 返回值未检查。

```c
// BAD
HAL_SPI_Transmit(&hspi, data, len, 1000);  // 忽略返回值
xSemaphoreTake(mutex, portMAX_DELAY);       // 忽略返回值

// GOOD
if (HAL_SPI_Transmit(&hspi, data, len, 1000) != HAL_OK) {
    handle_spi_error();
}
if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    handle_timeout();
}
```

风险：静默失败后继续执行，基于错误前提操作硬件。严重级别：high。

## STR — 字符串

### STR31-C: 字符串存储空间必须包含 null 终止符

检测：strncpy 后未确保 null 终止、固定长度 buffer 刚好等于最大字符串长度。

```c
// BAD
char name[8];
strncpy(name, input, 8);  // 如果 input >= 8 字节，name 无 null 终止
printf("Name: %s", name);  // 越界读取

// GOOD
char name[8];
strncpy(name, input, sizeof(name) - 1);
name[sizeof(name) - 1] = '\0';
```

风险：后续字符串操作越界读取。严重级别：high。

### STR32-C: 不要把非 null 终止的字符序列传给字符串库函数

检测：固定长度协议字段（可能不含 `\0`）直接传给 strlen/strcmp/printf %s。

```c
// BAD
typedef struct {
    char tag[4];  // 协议字段，可能不含 \0
    uint16_t len;
} Header;
printf("Tag: %s\n", hdr->tag);  // 越界读取

// GOOD
printf("Tag: %.4s\n", hdr->tag);  // 限制长度
```

风险：越界读取，信息泄露或崩溃。严重级别：high。

## DCL — 声明

### DCL30-C: 用适当的存储持续时间声明对象

检测：函数返回局部变量的地址、局部数组传给异步操作。

```c
// BAD
uint8_t *get_buffer(void) {
    uint8_t buf[128];
    return buf;  // 返回栈地址
}

// BAD — 嵌入式典型场景
void start_dma(void) {
    uint8_t tx_data[64];
    fill_data(tx_data);
    HAL_UART_Transmit_DMA(&huart, tx_data, 64);
}  // DMA 访问已失效的栈 buffer
```

风险：使用已失效栈帧，数据损坏。严重级别：critical。

### DCL39-C: 跨信任边界传递结构体时避免信息泄露

检测：结构体含 padding 字节，未初始化就发送到外部（UART、网络、日志）。

```c
// BAD
typedef struct {
    uint8_t type;    // 1 byte
    // 3 bytes padding
    uint32_t value;  // 4 bytes
} Message;
Message msg;
msg.type = CMD_RESP;
msg.value = result;
uart_send(&msg, sizeof(msg));  // padding 含栈残留

// GOOD
Message msg;
memset(&msg, 0, sizeof(msg));
msg.type = CMD_RESP;
msg.value = result;
```

风险：padding 中的栈残留数据被发送，可能泄露敏感信息。严重级别：medium。

## MSC — 杂项

### MSC37-C: 确保非 void 函数所有路径都有返回值

检测：函数声明返回值但某些分支没有 return。

```c
// BAD
HAL_StatusTypeDef init_sensor(void) {
    if (check_id() != SENSOR_ID) {
        return HAL_ERROR;
    }
    configure_sensor();
    // 忘记 return HAL_OK — 返回值未定义
}
```

风险：调用方读到未定义返回值，后续判断错误。严重级别：high。
