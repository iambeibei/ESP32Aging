# MISRA C 检测规则

MISRA C 是受许可约束的专有标准。本文件不复述标准原文，而是基于 MISRA C 的公开定位和工程原则，按嵌入式高频风险类别提供可检测的模式。输出时使用"MISRA 导向 finding"措辞，不声称正式合规。

## 类型与隐式转换

### MISRA-TYPE-01: 隐式窄化转换

检测：宽类型赋值给窄类型时无显式 cast，尤其在寄存器配置、DMA 长度、协议字段处。

```c
// BAD
uint32_t total = calculate_size();
uint16_t reg_val = total;  // 隐式截断，无警告（取决于编译器设置）

// GOOD
if (total > UINT16_MAX) { return ERROR; }
uint16_t reg_val = (uint16_t)total;
```

MISRA 原则：所有窄化转换必须显式，且在转换前验证值域。严重级别：high。

### MISRA-TYPE-02: 有符号/无符号隐式混用

检测：有符号和无符号整数在比较、算术、赋值中隐式混用。

```c
// BAD
int16_t offset = -1;
uint16_t index = 10;
if (offset < index) {  // offset 被隐式转换为 unsigned，-1 变成 65535
    // 这个分支不会执行
}

// BAD — 嵌入式常见
int len = get_packet_len();   // signed
memcpy(dst, src, len);        // memcpy 期望 size_t (unsigned)
                               // len 为负时变成极大值

// GOOD
int len = get_packet_len();
if (len <= 0) { return ERROR; }
memcpy(dst, src, (size_t)len);
```

MISRA 原则：避免有符号/无符号之间的隐式转换，所有混用必须显式。严重级别：high。

### MISRA-TYPE-03: 整数提升导致的意外结果

检测：窄整数（uint8_t、uint16_t）在运算中被隐式提升为 int，运算结果可能与预期不同。

```c
// BAD
uint8_t a = 200, b = 100;
uint8_t result = a + b;  // a+b 提升为 int，结果 300，截断为 44
                          // 开发者可能期望溢出行为，但这依赖隐式提升

// BAD — 位操作陷阱
uint8_t flags = 0xFF;
uint32_t mask = ~flags;  // flags 提升为 int (0x000000FF)
                          // ~int 结果是 0xFFFFFF00（signed int）
                          // 赋值给 uint32_t: 0xFFFFFF00 ✓
                          // 但如果 flags 是 uint16_t:
                          // ~(int)0x0000FFFF = (int)0xFFFF0000 (可能是负数)
```

MISRA 原则：理解整数提升规则，在位操作中使用足够宽的类型。严重级别：medium。

### MISRA-TYPE-04: 对位域的有符号假设

检测：位域声明为 `int` 而非 `unsigned int`，单 bit 位域是有符号的。

```c
// BAD
struct {
    int enabled : 1;  // 取值范围是 -1 和 0，不是 0 和 1
} ctrl;
ctrl.enabled = 1;     // 实现定义行为
if (ctrl.enabled == 1) {}  // 可能永远为假

// GOOD
struct {
    unsigned int enabled : 1;  // 取值范围 0 和 1
} ctrl;
```

MISRA 原则：位域必须声明为 unsigned int 或 _Bool。严重级别：high。

## 指针与数组

### MISRA-PTR-01: 指针算术超出对象边界

检测：指针加减后可能超出原始数组/buffer 范围，包括恰好指向末尾之后的"past-the-end"指针被解引用。

```c
// BAD
void process(uint8_t *buf, uint16_t len) {
    uint8_t *end = buf + len;    // 如果 len > 实际 buffer 大小?
    while (buf < end) {
        handle(*buf++);          // 可能越界
    }
}
```

MISRA 原则：指针算术的结果必须在对象边界内。严重级别：high。

### MISRA-PTR-02: 函数指针类型不匹配

检测：函数指针转换为不兼容的签名类型。嵌入式常见于回调注册和中断向量表。

```c
// BAD
typedef void (*Callback)(void);
void handler(uint32_t event) { /* ... */ }
Callback cb = (Callback)handler;  // 签名不匹配
cb();  // 调用时参数不正确，未定义行为

// GOOD
typedef void (*Callback)(uint32_t);
Callback cb = handler;
cb(EVENT_ID);
```

MISRA 原则：函数指针只能转换为兼容的函数类型。严重级别：high。

### MISRA-PTR-03: 指针与整数之间的转换缺少显式标注

检测：整数直接转换为指针，或指针转换为整数，无中间 `(uintptr_t)` 或明确标注。嵌入式中寄存器地址定义是例外。

```c
// BAD — 地址宽度可能不匹配
uint32_t addr = (uint32_t)buffer_ptr;  // 64 位平台上截断

// GOOD — 使用 uintptr_t
uintptr_t addr = (uintptr_t)buffer_ptr;

// 嵌入式合理用法 — 寄存器地址（需标注）
#define GPIOA_BASE  ((uintptr_t)0x40020000U)
volatile uint32_t *gpioa = (volatile uint32_t *)GPIOA_BASE;
```

MISRA 原则：指针/整数转换必须通过 uintptr_t，且仅在系统级代码中允许。严重级别：medium。

## 控制流

### MISRA-CTRL-01: Switch 缺少 default 分支

检测：switch 语句没有 default 分支，或 default 分支为空。

```c
// BAD
switch (state) {
    case STATE_IDLE:  do_idle();  break;
    case STATE_RUN:   do_run();   break;
    case STATE_STOP:  do_stop();  break;
    // 如果 state 被错误赋值为 3，静默跳过
}

// GOOD
switch (state) {
    case STATE_IDLE:  do_idle();  break;
    case STATE_RUN:   do_run();   break;
    case STATE_STOP:  do_stop();  break;
    default:
        handle_invalid_state(state);
        break;
}
```

MISRA 原则：每个 switch 必须有 default 分支。嵌入式中硬件错误或位翻转可能导致意外值。严重级别：medium。

### MISRA-CTRL-02: Switch case 落入 (fallthrough)

检测：switch case 没有 break/return/goto，执行落入下一个 case。

```c
// BAD
switch (cmd) {
    case CMD_INIT:
        init_peripheral();
        // 忘记 break，落入 CMD_START
    case CMD_START:
        start_peripheral();
        break;
}

// GOOD（有意 fallthrough 需标注）
switch (cmd) {
    case CMD_INIT:
        init_peripheral();
        /* FALLTHROUGH */  // 显式标注
    case CMD_START:
        start_peripheral();
        break;
}
```

MISRA 原则：每个非空 case 必须以 break/return/goto 终止，或显式标注 fallthrough。严重级别：medium。

### MISRA-CTRL-03: 循环变量在循环体内被修改

检测：for 循环的计数变量在循环体内被额外修改。

```c
// BAD
for (int i = 0; i < len; i++) {
    if (buf[i] == MARKER) {
        i += skip_len;  // 循环体内修改 i，难以证明终止性
    }
}

// GOOD — 使用 while 更清晰
int i = 0;
while (i < len) {
    if (buf[i] == MARKER) {
        i += skip_len + 1;
    } else {
        i++;
    }
}
```

MISRA 原则：for 循环计数变量只能由 for 语句自身的增量表达式修改。严重级别：low。

### MISRA-CTRL-04: 函数有多个出口点

检测：函数有多个 return 语句。（注：MISRA 严格要求单出口，但嵌入式实践中适度的 early return 可以接受）

说明：这条规则在审查时降低优先级。如果 early return 用于参数校验或错误处理，且逻辑清晰，标记为 `project-context dependent` 而非违规。只有当多出口导致资源泄漏或清理路径遗漏时才标记为问题。

MISRA 原则：单入口/单出口原则，有助于证明程序正确性。严重级别：low（除非导致资源泄漏）。

## 宏与预处理器

### MISRA-MACRO-01: 类函数宏的参数未加括号

检测：宏参数在展开体中未用括号保护，或整个宏体未用括号包裹。

```c
// BAD
#define SQUARE(x)  x * x
int r = SQUARE(a + 1);  // 展开为 a + 1 * a + 1 = a + a + 1

// BAD
#define MAX(a, b)  a > b ? a : b
int r = MAX(x, y) + 1;  // 展开为 x > y ? x : y + 1

// GOOD
#define SQUARE(x)  ((x) * (x))
#define MAX(a, b)  ((a) > (b) ? (a) : (b))
// 但仍有多次求值副作用问题：SQUARE(i++) 展开为 ((i++) * (i++))
```

MISRA 原则：宏参数必须用括号保护，宏体必须用括号包裹。更好的做法是用 static inline 函数替代。严重级别：medium。

### MISRA-MACRO-02: 宏参数有副作用

检测：类函数宏在展开中多次引用某个参数，而调用处传入了有副作用的表达式。

```c
// BAD
#define MIN(a, b) ((a) < (b) ? (a) : (b))
int val = MIN(read_adc(), threshold);  // read_adc() 可能被调用两次

// GOOD — 使用 inline 函数
static inline int min_int(int a, int b) { return a < b ? a : b; }
int val = min_int(read_adc(), threshold);

// 或 GCC/Clang 扩展（非标准但嵌入式常用）
#define MIN(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
```

MISRA 原则：避免在宏参数中传入有副作用的表达式。严重级别：high（尤其涉及硬件读取或自增）。

### MISRA-MACRO-03: #define 和 #undef 在块作用域内

检测：在函数体内 `#define` 或 `#undef` 宏。

```c
// BAD
void configure(void) {
    #define TIMEOUT 100
    wait(TIMEOUT);
    #undef TIMEOUT
}
// TIMEOUT 的可见性与直觉不符，影响后续代码

// GOOD — 使用局部常量
void configure(void) {
    static const uint32_t timeout = 100;
    wait(timeout);
}
```

MISRA 原则：宏定义和取消定义必须在文件作用域。严重级别：low。

## 副作用与序列点

### MISRA-SIDE-01: volatile 访问与非 volatile 访问在同一表达式中

检测：同一表达式中既读 volatile 变量又做其他有副作用的操作。

```c
// BAD
uint32_t val = *status_reg | (*data_reg << 8);
// 两个 volatile 读取的顺序未定义

// GOOD
uint32_t status = *status_reg;
uint32_t data = *data_reg;
uint32_t val = status | (data << 8);
```

MISRA 原则：volatile 访问不应与其他副作用在同一表达式中组合。严重级别：high。

### MISRA-SIDE-02: 逻辑运算符右侧有副作用

检测：`&&` 或 `||` 的右操作数包含副作用（函数调用、自增、volatile 读取）。

```c
// BAD
if (is_ready() && clear_interrupt()) {
    // clear_interrupt 在 is_ready() 为假时不会被调用
    // 如果调用者期望 clear_interrupt 总是执行，这是 bug
}

// GOOD
bool ready = is_ready();
bool cleared = clear_interrupt();
if (ready && cleared) { /* ... */ }
```

MISRA 原则：`&&` `||` 右侧不应有持久副作用。严重级别：medium（取决于副作用是否预期被短路）。

## 实现定义行为

### MISRA-IMPL-01: 依赖编译器扩展而不隔离

检测：代码直接使用 `__attribute__`、`__packed`、`#pragma`、`__asm` 等编译器扩展，且没有通过宏或抽象层隔离。

```c
// BAD — 散落在各处
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint32_t value;
} __attribute__((aligned(4))) Message;

// GOOD — 通过宏隔离
#if defined(__GNUC__)
  #define PACKED   __attribute__((packed))
  #define ALIGNED(n) __attribute__((aligned(n)))
#elif defined(__ICCARM__)
  #define PACKED
  #define ALIGNED(n) _Pragma("data_alignment=" #n)
#endif

typedef struct PACKED {
    uint8_t type;
    uint32_t value;
} ALIGNED(4) Message;
```

MISRA 原则：实现定义行为必须文档化并局部隔离。严重级别：low（但影响可移植性）。

### MISRA-IMPL-02: 位域排列假设

检测：代码假设位域在内存中的特定排列方式（MSB-first 或 LSB-first），用位域直接映射硬件寄存器或协议帧。

```c
// BAD — 位域排列是实现定义的
typedef struct {
    uint32_t opcode  : 4;
    uint32_t channel : 4;
    uint32_t length  : 8;
    uint32_t data    : 16;
} Command;
volatile Command *cmd_reg = (volatile Command *)CMD_REG_ADDR;
// 不同编译器/平台可能排列不同

// GOOD — 用显式位操作
#define CMD_OPCODE_SHIFT   0
#define CMD_OPCODE_MASK    0x0FU
#define CMD_CHANNEL_SHIFT  4
#define CMD_CHANNEL_MASK   0x0FU
uint32_t cmd = (opcode & CMD_OPCODE_MASK) << CMD_OPCODE_SHIFT
             | (channel & CMD_CHANNEL_MASK) << CMD_CHANNEL_SHIFT;
*(volatile uint32_t *)CMD_REG_ADDR = cmd;
```

MISRA 原则：不要依赖位域的内存布局，使用显式位操作代替。严重级别：high（直接影响硬件正确性）。

## 可重入性与全局状态

### MISRA-REENT-01: 非可重入函数在多上下文中调用

检测：使用静态局部变量或全局状态的函数被 ISR 和 task 同时调用，或被多个 task 调用。

```c
// BAD
static uint8_t temp_buf[64];
HAL_StatusTypeDef sensor_read(uint8_t *out, uint16_t len) {
    // 使用 temp_buf 做中间缓存
    spi_transfer(temp_buf, len);
    memcpy(out, temp_buf, len);
    return HAL_OK;
}
// 如果两个 task 同时调用 sensor_read，temp_buf 会互相覆盖

// GOOD — 调用者提供 buffer
HAL_StatusTypeDef sensor_read(uint8_t *buf, uint8_t *out, uint16_t len) {
    spi_transfer(buf, len);
    memcpy(out, buf, len);
    return HAL_OK;
}
// 或加互斥保护
```

MISRA 原则：函数应避免共享可变状态，或明确记录可重入性限制。严重级别：high。

### MISRA-REENT-02: 全局变量缺少访问保护声明

检测：模块文件作用域变量被多个函数修改，且没有注释或类型标注说明访问约束。

```c
// BAD
static uint32_t sample_count;  // 被 ISR 和 task 都修改，无标注
void ADC_IRQHandler(void) { sample_count++; }
void get_count(uint32_t *out) { *out = sample_count; }  // 无同步

// GOOD
static volatile uint32_t sample_count;  // volatile 标注 + 明确同步策略
void ADC_IRQHandler(void) {
    sample_count++;  // ISR 独占写入
}
uint32_t get_count(void) {
    taskENTER_CRITICAL();
    uint32_t val = sample_count;
    taskEXIT_CRITICAL();
    return val;
}
```

MISRA 原则：共享变量必须有清晰的同步策略，volatile 只解决编译器优化问题。严重级别：high。
