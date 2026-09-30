# Checklists

系统性扫描时使用。每条都对应真实缺陷模式，不是泛泛提醒。

## 标准选择

- 用户是否明确指定了标准？指定 MISRA 时是否给了版本/偏差流程？
- 未指定时：FreeRTOS 项目 → CERT C + FreeRTOS；裸机驱动 → BARR-C + CERT C
- 当前请求是正式合规审查还是标准导向预审查？

## 内存安全

- buffer/pointer 是否活得比 owner 更久？（尤其栈 buffer 传给 ISR/DMA/后台 task）
- 长度单位是否正确？（element vs byte、word vs byte、编码长度 vs payload）
- copy/format 操作是否信任外部长度但没校验目标容量？
- 有符号或窄整数溢出是否影响 allocation size / loop bound / offset？
- `sizeof(ptr)` 是否被误当成 `sizeof(array)`？
- DMA buffer 是否在传输完成前一直有效？可缓存 buffer 交 DMA 前后是否 clean/invalidate？
- 发出去的 struct 是否含未初始化字段？出错路径是否 reset 复用状态对象？

## 中断

- ISR 中是否有阻塞、sleep、等待？
- 是否用了正确的 ISR-safe API（而不是 task-only API）？
- 清中断标志、读状态、取数据的顺序是否正确？（W1C/sticky bit 处理）
- ISR 与 task 共享数据是否有明确同步策略？（`volatile` 不保证 read-modify-write 安全）
- ISR 唤醒 task 后是否正确触发 yield/reschedule hint？
- ISR 内工作量是否可接受？延迟工作的 queue/context 生命周期是否安全？

## RTOS

- 锁顺序是否一致？持锁时是否调用可能阻塞的下层？
- timeout 单位是否正确？（ticks vs ms）tick wraparound 处理是否安全？
- notification/queue 是否会静默丢工作？（producer 速度 > consumer）
- cleanup/恢复路径是否释放所有资源？restart 前是否完整 reset 旧状态？
- 栈上数据指针是否塞进 queue？task 参数对象是否活得过 task 的使用期？

## 并发

- 共享状态跨越 ISR/task/DMA 但没有明确同步策略？
- 读改写序列是否有原子保护？多字段状态对象是否可能被读到半更新值？
- 临界区保护范围是否过小（看到一半状态）或过大（延迟放大/死锁压力）？
- lock-free 代码假设的内存序平台是否实际提供？
- MMIO 和普通内存之间是否缺少必要 barrier？DMA 完成后 cache 是否同步？

## 驱动与 HAL

- 寄存器编程顺序是否满足硬件约束？（分阶段 enable、flush/dummy read）
- 对 W1C/sticky/status bit 是否做了危险的 read-modify-write？
- 错误路径是否留下时钟/中断/DMA/片选处于半开启状态？
- polling loop 是否有超时？超时后是否恢复到安全状态？
- 软件 shadow state 在 reset/fault 后是否和硬件漂移？

## 代码质量

仅当风格问题掩盖正确性前提、诱发 API 误用、或削弱调试质量时才报告：

- 硬件访问/协议处理/业务逻辑是否混在同一函数？
- 命名是否表达 ownership、阻塞行为、副作用、上下文限制、单位？
- 寄存器位/超时/buffer 大小是否使用魔法数？
- 错误码是否保留足够故障区分度？
- 状态机是否靠零散 flag 隐式表达？

## 安全

- 源码中是否硬编码密钥、密码、token？（扫描连续十六进制数组、`key`/`secret`/`password` 关键词）
- 敏感数据使用后是否擦除？（`memset` 可能被优化掉，需要 `explicit_bzero`）
- 生产构建是否禁用了 SWD/JTAG？是否设置了 readout protection？
- OTA 是否有签名验证和防回滚？断电是否安全（A/B 分区）？
- 密码/密钥比较是否恒定时间？安全关键分支是否有冗余检查？
- 外部输入（UART/BLE/USB/网络）的长度字段是否校验后再用于内存操作？
- 看门狗是否在主循环/task 中喂（而不是 ISR 中）？
- HardFault handler 是否保存诊断信息（而不是空的 `while(1)`）？

## 危险函数速查

diff 中出现以下函数时自动标记审查：

| 函数 | 风险 | 替代 |
|------|------|------|
| `sprintf` | buffer 溢出 | `snprintf` |
| `strcpy` | buffer 溢出 | `strncpy` + null 终止 / `strlcpy` |
| `strcat` | buffer 溢出 | `strncat` + 剩余大小 |
| `gets` | 无边界 | `fgets` |
| `scanf("%s")` | 无宽度 | `scanf("%Ns")` |
| `atoi`/`atol` | 无错误检测 | `strtol` + errno |
| `strtok` | 非可重入 | `strtok_r` |
| `alloca`/VLA | 栈不可控 | 固定 buffer 或堆 |
| `memcpy(d,s,n)` | n 未校验 | 校验 n <= dst_size |

## ARM Cortex-M 原子性参考

判断共享变量是否需要临界区保护时参考：

| 访问宽度 | Cortex-M3/M4/M7 原子? | 条件 |
|---------|----------------------|------|
| 8-bit (LDRB/STRB) | 是 | — |
| 16-bit (LDRH/STRH) | 是 | 自然对齐 |
| 32-bit (LDR/STR) | 是 | 自然对齐 |
| 64-bit | 否 | 需临界区或 LDREXD/STREXD |
| struct / 多字段 | 否 | 始终需临界区 |
| 位域 | 否 | 编译器生成 RMW，需临界区 |
| read-modify-write (`|=` `&=` `++`) | 否 | 即使 32-bit 也是三条指令 |

注意：Cortex-M0/M0+ 不支持 LDREX/STREX，需依赖关中断保护。

## 合规边界

- 这次审查是否有足够证据支持"正式合规"？
- 是否需要降级成"标准导向 finding"？
- 是否存在项目内部 deviation 但当前看不到？

## 深度机理参考

深入分析上述检查项“为什么是陷阱”时参考。每条是机理补充，不重复上面的检查问句（原 `domain-knowledge.md` 并入）。

### DMA Buffer 生命周期

buffer 必须在“传输完成”前一直有效，不是只在“启动 DMA”那一刻有效。常见陷阱：

- 局部栈 buffer 直接交给 DMA，函数返回后 DMA 还在写
- ring buffer 槽位在完成 IRQ 前被复用
- 可缓存 buffer 交给 DMA 前没有 clean（设备读到旧数据），完成后没有 invalidate（CPU 读到 cache 中的旧值）

### 长度与类型不匹配

- `sizeof(ptr)` 误当 `sizeof(array)` → 只拷贝指针大小的字节
- 16 位长度变量截断更大的 payload → 环绕后只写入低位长度
- 寄存器字段实际只保留低 N 位，但软件按全宽理解 → 配置值被截断

### 部分初始化

- 发出去的 struct 含未初始化 padding → 信息泄露或校验失败
- 出错路径没有 reset 复用状态对象 → 下次使用时残留旧状态
- callback context 只在成功路径填充，失败路径仍被访问 → 野指针或脏数据

### 中断标志位处理

- 硬件要求“先清标志再读数据”，但代码先读数据再清标志 → 还没捕获完就 acknowledge，硬件覆盖原始数据
- 对 W1C（Write-1-to-Clear）bit 做 read-modify-write → 意外清掉其他 pending bit
- 对 sticky bit 做常规读改写 → 读到 1 后写回 1 把它清了

### ISR 延迟工作与下半部

ISR 把工作交给 task/worker 时的关键检查：

- queue element / context 的生命周期必须覆盖 consumer 消费时刻
- 如果 consumer 速度跟不上中断速率，backpressure 是显式处理（返回错误/丢弃最旧）还是默默丢弃最新
- 突发负载下唤醒路径：连续快速 post 可能导致 notification 被覆盖（只记住最后一次）

### 调度陷阱

- 持锁时调用可能阻塞的下层 → 优先级反转或死锁
- 用 delay 轮询代替事件驱动唤醒 → 延迟放大 + CPU 浪费
- timer callback 做了本应交给 task 的重工作 → timer daemon task 被阻塞，影响所有 timer
- producer 速度 > consumer + task notification 只有一个 pending bit → 丢事件

### 寄存器访问陷阱

- 多个控制位一次性写入，但硬件要求分阶段 enable（如先配置再使能）→ 硬件在中间状态收到使能
- 需要 flush 或 dummy read 的平台（如写完控制寄存器后读一次确保生效），代码没做 → 写操作停在总线 buffer 里
- reset/fault 后软件 shadow state 和真实硬件漂移 → retry 基于错误前提操作寄存器

### 硬件状态机

- abort/timeout/reset 路径没有像成功路径一样谨慎 → DMA descriptor / FIFO / pending IRQ 残留
- restart 前没有重新初始化外设完整状态 → 外设从上次故障的中间状态继续运行
- 没有区分瞬时故障和持续性故障就无条件 retry → 永久故障时进入无限重试

### 内存可见性

- 数据结构没完全写完就先发布 ready flag → reader 看到半更新的结构体
- `volatile` 只保证编译器不优化掉访问，不保证 CPU 内存序 → 多核或有 store buffer 的平台上仍可能 stale read
- MMIO 和普通内存混用但缺少 barrier → 寄存器写入顺序在 CPU 层面被重排

### 嵌入式代码质量深度陷阱

- bytes/words 混淆：DMA 配置用 word count 但软件传 byte count → 传输量翻倍或减半
- ticks/milliseconds 混淆：`vTaskDelay(1000)` 是 1000 ticks 不是 1000ms → 实际超时取决于 tick rate
- 绝对超时 vs 相对超时混淆：用相对 delay 做轮询但没考虑自身执行时间 → 累积漂移
- ISR 里耦合高层策略决策 → 中断延迟不可控
- 应用层直接操作原始寄存器绕过 HAL → 和 HAL 的 shadow state 冲突
