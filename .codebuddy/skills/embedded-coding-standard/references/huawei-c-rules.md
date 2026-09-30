# 华为 C 语言编程规范 检测规则

基于华为技术有限公司《C语言编程规范》（2011版）公开内容，筛选嵌入式场景高频规则，转换为模型可直接匹配的检测模式。使用"华为C规范导向 finding"措辞。

参考来源：https://ilcc.gitbooks.io/wiki/content/StyleGuide/Huawei-C/index.html

## 头文件

### HW-HDR-01: 头文件中放置了实现而非声明

检测：头文件中包含变量定义、函数实现、内部函数声明、内部宏/枚举/结构定义。

```c
// BAD — header.h
int g_counter = 0;               // 变量定义
static void helper(void) { ... }  // 函数实现
#define _INTERNAL_MAGIC 0x55      // 内部宏

// GOOD — header.h
#ifndef HEADER_H
#define HEADER_H
int get_counter(void);            // 只声明接口
#endif
```

华为C规范原则1.1：头文件中适合放置接口的声明，不适合放置实现。严重级别：high。

### HW-HDR-02: 头文件循环依赖

检测：a.h 包含 b.h，b.h 包含 c.h，c.h 又包含 a.h，形成循环。

```c
// BAD
// a.h → includes b.h
// b.h → includes c.h
// c.h → includes a.h  ← 循环

// GOOD — 单向依赖，c.h 不包含任何头文件
// a.h → b.h → c.h（终点）
```

华为C规范规则1.2：禁止头文件循环依赖。严重级别：high。

### HW-HDR-03: 包含用不到的头文件

检测：.c/.h 文件中包含了未使用的头文件。

```c
// BAD
#include <stdio.h>    // 未使用任何 stdio 函数
#include <string.h>   // 未使用任何 string 函数
#include "module_a.h" // 未使用 module_a 的任何接口

// GOOD — 只包含实际需要的
#include "module_b.h" // 使用了 module_b 的接口
```

华为C规范规则1.3：.c/.h 文件禁止包含用不到的头文件。严重级别：medium。

### HW-HDR-04: 头文件不自包含

检测：包含某个头文件后还需要包含其他头文件才能编译通过。

```c
// BAD — a.h 使用了 uint32_t 但自己没有 include stdint.h
// a.h
#ifndef A_H
#define A_H
void process(uint32_t value);  // uint32_t 未定义
#endif

// GOOD — a.h 自包含
#ifndef A_H
#define A_H
#include <stdint.h>
void process(uint32_t value);
#endif
```

华为C规范规则1.4：头文件应当自包含。严重级别：medium。

### HW-HDR-05: 头文件缺少 include 保护符

检测：头文件没有 `#ifndef` 保护符或 `#pragma once`。

```c
// BAD — timer.h
void timer_init(void);
void timer_start(void);

// GOOD
#ifndef TIMER_H
#define TIMER_H
void timer_init(void);
void timer_start(void);
#endif /* TIMER_H */
```

华为C规范规则1.5：总是编写内部 #include 保护符。严重级别：medium。

### HW-HDR-06: 通过 extern 直接使用外部函数而非包含头文件

检测：在 .c 文件中通过 `extern` 声明使用其他 .c 文件定义的函数或变量，而不是 `#include` 对应头文件。

```c
// BAD — a.c
extern int foo(int input);  // 手写 extern，可能与定义不一致
int result = foo(42);

// GOOD — a.c
#include "b.h"              // 通过头文件获取声明
int result = foo(42);
```

华为C规范规则1.7：只能通过包含头文件的方式使用其他 .c 提供的接口。严重级别：medium。

## 函数

### HW-FUNC-01: 函数过长（超过50行非空非注释行）

检测：新增函数的有效代码行数（NBNC）超过 50 行。

```c
// BAD — 一个函数做了初始化、配置、校验三件事，超过 100 行
void sensor_setup(void) {
    // 30 行初始化
    // 40 行配置
    // 35 行校验
    // 共 105 行
}

// GOOD — 拆分为职责单一的函数
void sensor_setup(void) {
    sensor_init();        // ~30 行
    sensor_configure();   // ~40 行
    sensor_verify();      // ~35 行
}
```

华为C规范规则2.1：避免函数过长，新增函数不超过 50 行。严重级别：medium。

### HW-FUNC-02: 函数代码块嵌套过深（超过4层）

检测：函数中 if/for/while/switch 等控制块互相嵌套超过 4 层。

```c
// BAD — 嵌套 5 层
void process(void) {
    if (cond1) {              // 1
        switch (state) {      // 2
            case S1:
                if (cond2) {  // 3
                    for (...) { // 4
                        if (cond3) { // 5
                            // ...
                        }
                    }
                }
        }
    }
}

// GOOD — 提前返回、分解函数降低嵌套
void handle_state_s1(void) {
    if (!cond2) return;
    for (...) {
        if (cond3) { ... }
    }
}
```

华为C规范规则2.2：新增函数的代码块嵌套不超过 4 层。严重级别：medium。

### HW-FUNC-03: 可重入函数使用共享变量未加保护

检测：可重入函数（可能被多个任务/ISR 调用）使用了全局变量或 static 变量，但没有通过互斥手段保护。

```c
// BAD
int g_exam;
unsigned int example(int para) {
    g_exam = para;              // 多线程竞态
    unsigned int temp = square_exam();
    return temp;
}

// GOOD
unsigned int example(int para) {
    unsigned int temp;
    mutex_lock(&g_lock);
    g_exam = para;
    temp = square_exam();
    mutex_unlock(&g_lock);
    return temp;
}
```

华为C规范规则2.3：可重入函数应避免使用共享变量；若需要使用，则应通过互斥手段加以保护。严重级别：critical。

### HW-FUNC-04: 函数错误返回码未全面处理

检测：函数（标准库/第三方/用户定义）的返回值指示错误时，调用方未检查。

```c
// BAD
FILE *fp = fopen("config.txt", "r");
char buff[128] = "";
fscanf(fp, "%s", buff);  // 未检查 fscanf 返回值
fclose(fp);

// GOOD
FILE *fp = fopen("config.txt", "r");
if (fp == NULL) { return; }
char buff[128] = "";
if (fscanf(fp, "%s", buff) == EOF) {
    fclose(fp);
    return;
}
fclose(fp);
```

华为C规范规则2.5：对函数的错误返回码要全面处理。严重级别：high。

### HW-FUNC-05: 废弃代码未清除

检测：没有被调用的函数和变量仍然存在于代码中。

```c
// BAD
static void old_handler(void) { ... }  // 没有被任何地方调用
static int legacy_counter;             // 没有被使用

// GOOD — 删除废弃代码，需要时从版本控制中找回
```

华为C规范规则2.7：废弃代码（没有被调用的函数和变量）要及时清除。严重级别：low。

## 标识符命名

### HW-NAME-01: 全局变量缺少 g_ 前缀

检测：全局变量没有 `g_` 前缀。

```c
// BAD
int task_count;
uint32_t error_code;

// GOOD
int g_task_count;
uint32_t g_error_code;
```

华为C规范规则3.2：全局变量应增加 "g_" 前缀。严重级别：low。

### HW-NAME-02: 静态变量缺少 s_ 前缀

检测：static 变量没有 `s_` 前缀。

```c
// BAD
static int instance_count;

// GOOD
static int s_instance_count;
```

华为C规范规则3.3：静态变量应增加 "s_" 前缀。严重级别：low。

### HW-NAME-03: 使用单字节命名变量（i/j/k除外）

检测：变量使用单个字符命名，但不是循环变量 i、j、k。

```c
// BAD
int a = get_value();
char c = read_byte();
int x, y, z;  // 含义不明

// GOOD
int sensor_value = get_value();
char rx_byte = read_byte();
```

华为C规范规则3.4：禁止使用单字节命名变量，但允许定义 i、j、k 作为局部循环变量。严重级别：low。

### HW-NAME-04: 宏命名不符合全大写加下划线规范

检测：数值或字符串常量的宏定义未采用全大写字母加下划线的命名方式。

```c
// BAD
#define maxSize 100
#define PiValue 3.14

// GOOD
#define MAX_SIZE 100
#define PI_VALUE 3.14
```

华为C规范规则3.5：数值或字符串常量定义应采用全大写字母，单词之间加下划线。严重级别：low。

### HW-NAME-05: 使用了汉语拼音命名

检测：标识符使用了汉语拼音而非英文单词。

```c
// BAD
int chengji;        // "成绩"
void chuli_data(void);  // "处理data"

// GOOD
int score;
void process_data(void);
```

华为C规范原则3.2：不得使用汉语拼音。严重级别：medium。

## 变量

### HW-VAR-01: 变量被用作多种用途

检测：同一个变量在不同上下文中代表不同含义。

```c
// BAD
WORD Locate;
Locate = 3;
Locate = DeleteFromQue(Locate);  // Locate 同时表示位置和返回值

// GOOD
WORD ret;
WORD locate;
locate = 3;
ret = DeleteFromQue(locate);
```

华为C规范原则4.1：一个变量只有一个功能，不能把一个变量用作多种用途。严重级别：medium。

### HW-VAR-02: 使用未经初始化的变量

检测：变量声明后未赋值就作为右值使用。

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

华为C规范规则4.3：严禁使用未经初始化的变量作为右值。严重级别：high。

### HW-VAR-03: 局部变量与全局变量同名

检测：函数内的局部变量与全局变量同名。

```c
// BAD
int g_counter;

void process(void) {
    int counter = 0;  // 与 g_counter 同名，容易混淆
    counter++;
}

// GOOD
void process(void) {
    int local_counter = 0;
    local_counter++;
}
```

华为C规范规则4.1：防止局部变量与全局变量同名。严重级别：medium。

### HW-VAR-04: 通讯结构体未注意字节序

检测：通讯报文中的结构体直接按主机序使用，没有进行网络序转换。

```c
// BAD
typedef struct {
    uint16_t msg_type;
    uint32_t msg_id;
} Header;

void send_msg(Header *hdr) {
    uart_send((uint8_t *)hdr, sizeof(Header));  // 直接发，字节序依赖平台
}

// GOOD
void send_msg(Header *hdr) {
    uint8_t buf[sizeof(Header)];
    uint16_t net_type = htons(hdr->msg_type);
    uint32_t net_id = htonl(hdr->msg_id);
    memcpy(buf, &net_type, 2);
    memcpy(buf + 2, &net_id, 4);
    uart_send(buf, sizeof(buf));
}
```

华为C规范规则4.2：通讯过程中使用的结构，必须注意字节序。严重级别：high。

## 宏与常量

### HW-MACRO-01: 宏定义表达式括号不完备

检测：宏定义表达式中的参数或整体缺少括号保护。

```c
// BAD
#define RECTANGLE_AREA(a, b) a * b
#define RECTANGLE_AREA(a, b) (a * b)
#define RECTANGLE_AREA(a, b) (a) * (b)

// GOOD
#define RECTANGLE_AREA(a, b) ((a) * (b))
```

华为C规范规则5.1：用宏定义表达式时，要使用完备的括号。严重级别：high。

### HW-MACRO-02: 宏的多条表达式未用 do-while(0) 包装

检测：宏定义包含多条语句但没有用大括号或 `do { } while(0)` 包装。

```c
// BAD
#define FOO(x) \
    printf("arg is %d\n", x); \
    do_something(x);

// GOOD
#define FOO(x) do { \
    printf("arg is %d\n", x); \
    do_something(x); \
} while(0)
```

华为C规范规则5.2：将宏所定义的多条表达式放在大括号中（推荐 do-while(0)）。严重级别：high。

### HW-MACRO-03: 宏参数在展开时被修改

检测：宏调用时参数包含 `++`、`--` 等副作用操作。

```c
// BAD
#define SQUARE(a) ((a) * (a))
int a = 5;
int b = SQUARE(a++);  // a 被加了两次

// GOOD
int b = SQUARE(a);
a++;
```

华为C规范规则5.3：使用宏时，不允许参数发生变化。严重级别：high。

### HW-MACRO-04: 直接使用魔鬼数字

检测：代码中直接使用无明确含义的数字面量。

```c
// BAD
if (status == 3) { ... }
buf[5] = 0x55;
delay(1000);

// GOOD
#define STATUS_TIMEOUT  3
#define SYNC_BYTE       0x55
#define DELAY_1S_MS     1000
if (status == STATUS_TIMEOUT) { ... }
buf[SYNC_POS] = SYNC_BYTE;
delay(DELAY_1S_MS);
```

华为C规范规则5.4：不允许直接使用魔鬼数字。严重级别：medium。

### HW-MACRO-05: 宏中使用 return/goto/break 导致资源泄漏

检测：宏定义中使用了 `return`、`goto`、`continue`、`break` 等改变程序流程的语句。

```c
// BAD
#define CHECK_AND_RETURN(cond, ret) { if (cond == NULL) { return ret; } }
// 使用处：
pMem1 = malloc(256);
CHECK_AND_RETURN(pMem1, ERR);  // 如果失败直接 return，pMem1 泄漏
pMem2 = malloc(256);
CHECK_AND_RETURN(pMem2, ERR);  // 如果失败直接 return，pMem1 和 pMem2 泄漏

// GOOD — 用错误码 + goto 统一清理
pMem1 = malloc(256);
if (!pMem1) goto cleanup;
pMem2 = malloc(256);
if (!pMem2) goto cleanup;
// ...
cleanup:
    free(pMem1);
    free(pMem2);
```

华为C规范建议5.3：宏定义中尽量不使用 return、goto、continue、break 等改变程序流程的语句。严重级别：high。

## 质量保证

### HW-QA-01: 内存操作越界

检测：数组/指针/内存操作时可能越界——缓冲区大小不足、使用危险字符串函数、memcpy 长度未校验。

```c
// BAD
char temp[10];
itoa(value, temp, 10);  // value 可达 10 位数，temp 溢出

// GOOD
char temp[12];  // int 最大 11 位字符 + '\0'
itoa(value, temp, 10);
```

华为C规范规则6.1：禁止内存操作越界。严重级别：critical。

### HW-QA-02: 内存泄漏

检测：异常出口处未释放已分配的内存或资源（定时器/文件句柄/信号量等）。

```c
// BAD
void process(void) {
    uint8_t *buf1 = malloc(256);
    if (buf1 == NULL) return;
    uint8_t *buf2 = malloc(256);
    if (buf2 == NULL) return;  // buf1 泄漏
    // ...
}

// GOOD
void process(void) {
    uint8_t *buf1 = NULL, *buf2 = NULL;
    buf1 = malloc(256);
    if (!buf1) goto cleanup;
    buf2 = malloc(256);
    if (!buf2) goto cleanup;
    // ...
cleanup:
    free(buf1);
    free(buf2);
}
```

华为C规范规则6.2：禁止内存泄漏。严重级别：critical。

### HW-QA-03: 引用已释放的内存空间

检测：内存释放后继续使用该指针（野指针）。

```c
// BAD
int* get_value(void) {
    int local = 100;
    return &local;  // 返回局部变量地址
}

// BAD
free(buffer);
process(buffer);  // 使用已释放的内存

// GOOD — 释放后置 NULL
free(buffer);
buffer = NULL;
if (buffer != NULL) { process(buffer); }
```

华为C规范规则6.3：禁止引用已经释放的内存空间。严重级别：critical。

### HW-QA-04: if-else if 缺少 else 或 switch 缺少 default

检测：`if ... else if` 结构没有 `else` 子句，或 `switch` 语句没有 `default` 分支。

```c
// BAD
if (cmd == CMD_READ) { ... }
else if (cmd == CMD_WRITE) { ... }
// 没有 else 处理未知命令

// BAD
switch (status) {
    case OK: ...
    case ERROR: ...
    // 没有 default
}

// GOOD
if (cmd == CMD_READ) { ... }
else if (cmd == CMD_WRITE) { ... }
else { handle_unknown_cmd(cmd); }

switch (status) {
    case OK: ...
    case ERROR: ...
    default: handle_unexpected(status); break;
}
```

华为C规范规则6.5：所有的 if ... else if 结构应该由 else 子句结束；switch 语句必须有 default 分支。严重级别：medium。

## 安全性

### HW-SEC-01: 字符串未确保 NULL 终止

检测：使用 strncpy/snprintf 等截断函数后未确保目标字符串以 '\0' 结尾。

```c
// BAD
char a[16];
strncpy(a, "0123456789abcdef", sizeof(a));  // 刚好 16 字节，无空间放 '\0'

// GOOD
char a[16];
strncpy(a, "0123456789abcdef", sizeof(a) - 1);
a[sizeof(a) - 1] = '\0';
```

华为C规范规则13.1：确保所有字符串是以 NULL 结束。严重级别：high。

### HW-SEC-02: 整数溢出

检测：整数运算可能溢出——有符号溢出是未定义行为，无符号溢出会回绕。

```c
// BAD — 无符号下溢
unsigned int length;
length -= FSM_HDRLEN;  // length < FSM_HDRLEN 时回绕到极大值

// GOOD
if (length < FSM_HDRLEN) { return ERROR; }
length -= FSM_HDRLEN;
```

华为C规范规则13.3：避免整数溢出。严重级别：critical。

### HW-SEC-03: 有符号与无符号转换导致符号错误

检测：有符号整型隐式转换为无符号整型，负值变成极大正数。

```c
// BAD
int length = atoi(argv[1]);  // 可能为负
if (length < BUF_SIZE) {
    memcpy(buf, argv[2], length);  // 负值转 size_t 变极大值
}

// GOOD
int length = atoi(argv[1]);
if (length > 0 && (unsigned int)length < BUF_SIZE) {
    memcpy(buf, argv[2], length);
}
```

华为C规范规则13.4：避免符号错误。严重级别：high。

### HW-SEC-04: 格式化字符串中使用用户输入

检测：printf/fprintf/sprintf 等函数的格式化字符串中直接或间接包含用户输入。

```c
// BAD
char input[1000];
fgets(input, sizeof(input) - 1, stdin);
printf(input);  // 用户输入作为格式化字符串，可读栈内容

// GOOD
printf("%s", input);  // 用户输入仅作为参数
```

华为C规范规则13.7：避免将用户输入作为格式化字符串的一部分或者全部。严重级别：critical。

### HW-SEC-05: 使用 system() 执行包含用户输入的命令

检测：`system()` 或 `popen()` 的参数包含未经过滤的用户输入。

```c
// BAD
system(sprintf("any_exe %s", input));  // 命令注入

// GOOD — 使用 execve 代替 system
char *const args[] = {"", input, NULL};
execve("/usr/bin/any_exe", args, envs);
```

华为C规范规则13.10：防止命令注入。严重级别：critical。

## 表达式

### HW-EXPR-01: 表达式依赖未定义的求值顺序

检测：同一表达式中对同一变量有多个副作用（如 `x = b[i] + i++`），或函数参数中包含副作用。

```c
// BAD
x = b[i] + i++;        // i 的读写顺序未定义
x = func(i++, i);      // 参数求值顺序未定义
p->task_start_fn(p++); // 函数地址和参数计算顺序未定义

// GOOD
x = b[i] + i;
i++;
i++;
x = func(i, i);
```

华为C规范规则10.1：表达式的值在标准所允许的任何运算次序下都应该是相同的。严重级别：high。

### HW-EXPR-02: 赋值语句用在 if 条件或布尔表达式中

检测：`if (x = value)` 或布尔表达式中使用了赋值操作符。

```c
// BAD
if (status = HAL_OK) { ... }     // 赋值而非比较，始终为真
if (x = y) { ... }               // 容易与 == 混淆

// GOOD
if (status == HAL_OK) { ... }
x = y;
if (x != 0) { ... }
```

华为C规范建议10.4：赋值操作符不能使用在产生布尔值的表达式上。严重级别：high。

## 注释与排版

### HW-FMT-01: 修改代码后未维护相关注释

检测：代码修改后，相关注释已不准确或过时但仍保留。

```c
// BAD — 注释与代码矛盾
/* 端口号范围：0-3 */
int port = get_port();  // 实际返回 0-7
if (port > 5) { ... }  // 注释说 0-3，代码用 5

// GOOD
/* 端口号范围：0-7，>5 表示扩展端口 */
int port = get_port();
if (port > 5) { ... }
```

华为C规范规则8.1：修改代码时，维护代码周边的所有注释，不再有用的注释要删除。严重级别：low。

### HW-FMT-02: 文件头部缺少必要注释

检测：文件头部缺少版权说明、版本号、作者、功能说明等信息。

```c
// BAD — 文件开头无任何注释
#include "driver.h"
void init(void) { ... }

// GOOD
/*************************************************
Copyright (C) Company. 2024. All rights reserved.
File name: driver.c
Author: dev@example.com  Version: 1.0  Date: 2024-01-01
Description: UART driver initialization and data transfer
*************************************************/
#include "driver.h"
void init(void) { ... }
```

华为C规范规则8.2：文件头部应进行注释。严重级别：low。

### HW-FMT-03: 全局变量缺少详细注释

检测：全局变量没有注释说明其功能、取值范围、访问约束。

```c
// BAD
uint32_t g_error_code;

// GOOD
/* 全局错误码，记录最近一次操作的结果
 * 0 - SUCCESS, 1 - TIMEOUT, 2 - BUS_ERROR, 3 - PARAM_INVALID
 * 仅由 process_module() 写入，其他模块通过 get_last_error() 读取 */
uint32_t g_error_code;
```

华为C规范规则8.4：全局变量要有较详细的注释。严重级别：low。
