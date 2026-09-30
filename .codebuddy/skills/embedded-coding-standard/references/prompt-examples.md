# Prompt Examples

当用户想方便调用这个 Skill，或需要按第三方标准模式发起审查时，使用这份参考。

## 快捷调用

最简触发——粘贴代码或说一句话即可。若项目根目录没有 `.em_skill.json` 规则配置，
先询问用户选择 review 规则组合，确认后写入 `.em_skill.json`，再开始审查：

```text
审查这段代码
```

```text
review this
```

```text
审查代码，用华为规范
```

```text
review with cert
```

```text
用 misra 审查这个 HAL 文件
```

```text
这个驱动模块按 barr 标准审查一下
```

期望输出：

- 若无 `.em_skill.json`，先给推荐规则组合并等待用户确认
- 用户确认后记录到 `.em_skill.json`
- 若已有 `.em_skill.json`，输出开头声明本次复用的规则组合
- 输出格式与完整调用一致：Findings → Open questions → Assumptions
- 如推荐依据不确定，会在询问中说明依据

## CERT C + FreeRTOS 全量审查

```text
按 CERT C + FreeRTOS 审查这个固件补丁。重点检查内存安全、ISR/RTOS 误用、竞态、超时处理和硬件恢复路径。先给 findings，按严重级别排序。
```

期望输出：

- 若 `.em_skill.json` 还没有规则配置，先询问是否记录 `CERT C + FreeRTOS`
- 优先给具体缺陷，不要先讲风格
- 能带文件和行号就带
- open questions 和 confirmed findings 分开

## BARR-C 驱动与 DMA 审查

```text
按 BARR-C 审查这个 UART/SPI/I2C 驱动。重点看 DMA buffer 生命周期、中断确认顺序、错误恢复、命名/分层和部分传输处理。
```

期望输出：

- 明确指出栈 buffer 与异步路径的生命周期问题
- 检查 IRQ / DMA completion 的时序关系
- 审查 abort、timeout、cleanup 路径

## FreeRTOS 聚焦审查

```text
按 FreeRTOS 官方约束审查这个补丁。只检查 task/ISR API 使用、死锁、优先级反转、漏唤醒和超时语义。忽略纯风格问题。
```

期望输出：

- 分析阻塞行为和锁顺序
- 指出 `FromISR` API 使用错误
- 检查 notification、queue、semaphore 语义是否会丢工作

## CERT C 并发与 ISR 审查

```text
按 CERT C + FreeRTOS 审查这个中断处理函数和它的下半部 worker。重点看共享状态同步、事件丢失、可重入性，以及 ISR 是否做了过重工作。
```

期望输出：

- 区分 ISR 上下文问题和 task 上下文问题
- 解释事件丢失或陈旧数据的触发时序
- 指出 ISR 中的阻塞调用或不可重入调用

## 仅 CERT C 内存安全审查

```text
按 CERT C 只审查这些 C 文件里的内存安全问题。重点看 buffer 大小、整数截断、未初始化数据、DMA 可见内存和 ownership transfer。
```

期望输出：

- 不输出无关风格建议
- 给出具体 overflow 或生命周期路径
- 需要时写明 allocator、cache policy 等假设

## 仅 CERT C 并发审查

```text
按 CERT C 只审查这个模块里的并发问题。重点看 ISR/task 共享状态、原子操作、临界区范围、锁顺序和内存可见性。
```

期望输出：

- 给出 stale read、lost update、状态不一致的具体序列
- 指出同步过弱或过宽的位置
- 把 `volatile` 误用视为正确性问题

## MISRA 导向 HAL 与寄存器审查

```text
按 MISRA C 导向审查这个 HAL 补丁。重点看 MMIO 顺序、read-modify-write 风险、状态位处理、外设状态迁移和 retry/recovery 路径；没有足够依据时不要声称正式合规。
```

期望输出：

- 分析寄存器写序和副作用
- 审 success path，也审 reset/abort path
- 指出软硬件状态漂移风险

## 指定标准并限制输出格式

```text
按 BARR-C + CERT C 审查这个 diff。输出只保留：
1. Findings
2. Open questions
3. Assumptions
Findings 保持简短，并按严重级别排序。
```

期望输出：

- 严格按请求结构输出
- 不写冗长教程式解释
- 摘要最少化，必要时可省略

## BARR-C 代码规范作为次级目标

```text
按 BARR-C 审查这个 BSP 模块。先给正确性问题，然后只补充和嵌入式相关的代码质量问题，例如单位歧义、API 易误用、分层不清、可调试性差。
```

期望输出：

- 风格问题放在后面
- 重点看命名、单位、ownership、分层、调试性
- 不给纯格式化建议

## 没有 findings 时的回退要求

```text
按第三方标准模式审查这个补丁。如果没有明确缺陷，请直接说明没有，并列出残余风险和缺失测试；如果证据不足以做正式合规判断，也请明确写出。
```

期望输出：

- 明确说明 no-finding
- 残余风险必须绑定到真实未覆盖路径
- 不输出 filler issues
