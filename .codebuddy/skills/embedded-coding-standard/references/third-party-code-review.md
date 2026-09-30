---
name: embedded-coding-standard-review
description: 当用户希望按第三方标准审查嵌入式代码时使用。适用于基于 MISRA C、CERT C、Barr Group Embedded C Coding Standard、华为 C 语言编程规范、FreeRTOS 官方约束等外部标准或规范，对 C/C++ firmware、driver、HAL、BSP、ISR、RTOS task、DMA、register access、low-level concurrency 进行代码审查。
---

# Embedded Code Review

按第三方标准审查嵌入式 C/C++ 代码。审查前必须确认并记录规则组合，再按标准约束组织审查输出。

## 何时使用

用户希望按第三方标准审查以下内容时：

- 嵌入式 C/C++ 代码、驱动、HAL、BSP、启动代码
- ISR、中断回调、DMA、寄存器访问
- RTOS 任务、队列、信号量、互斥锁、定时器、通知
- 底层并发、轮询、超时、恢复路径

## 规则选择与记录

代码审查前必须确认 review 规则组合，不直接自动审查。

1. 确认目标项目根目录。
2. 读取目标根目录 `.em_skill.json`。
3. 如果不存在规则配置，或用户要求修改规则，先询问用户选择规则组合。
4. 用户确认后写回 `.em_skill.json`。
5. 如果已有规则配置，默认复用，并在输出开头声明本次使用的规则组合。

可选规则：

- `MISRA C`
- `CERT C`
- `BARR-C`
- `Huawei C`
- `FreeRTOS`
- `Security`
- `C-Pitfall`
- `Local coding standard`（读工程 `.ai/coding-standard.md`；需先 init，见“加载检测规则”）

`.em_skill.json` 示例：

```json
{
  "embedded_coding_standard": {
    "review": {
      "rules": ["CERT C", "FreeRTOS", "Local coding standard"],
      "updated_at": "YYYY-MM-DD"
    }
  }
}
```

询问时可以给出推荐组合，但必须等用户确认：

- `cert` / `安全` → CERT C
- `misra` / `车规` → MISRA C
- `barr` / `驱动` → BARR-C
- `华为` / `规范` → 华为C规范
- `freertos` / `rtos` → FreeRTOS

推荐逻辑：

1. 代码中出现 FreeRTOS API → CERT C + FreeRTOS
2. 代码是裸机驱动/HAL/BSP → BARR-C + CERT C
3. 无法判断 → CERT C + 华为C规范（最通用的安全+工程规范组合）
4. boot/OTA/crypto/debug/auth → 增加 Security
5. 启动文件、链接器、编译器优化、未定义行为高风险代码 → 增加 C-Pitfall
6. 用户要求本地规范 → 增加 Local coding standard

典型调用（完整示例）：

- `按 CERT C + FreeRTOS 审查这个补丁，重点看 ISR/task API、竞态和未定义行为。`
- `按 BARR-C 审查这个驱动模块，重点看可维护性、命名、寄存器封装和错误处理。`
- `按 MISRA C 导向审查这个 HAL 文件，列出高风险偏差。`
- `按华为C规范审查这个模块，重点看头文件组织、函数设计、命名和安全编码。`
- `按 CERT C + FreeRTOS 审查 ~/path/to/repo 的 HEAD~5..HEAD 改动。`

更多调用模板见 `references/prompt-examples.md`。

## 标准 Profile

### MISRA C

适用场景：安全关键、车规、工业控制等高约束场景，或用户明确要求。

核心约束：指针/数组/整数转换的未定义行为、宏与表达式副作用、隐式转换、控制流可证明性、全局状态与可重入性、编译器扩展隔离。

输出措辞：使用 `MISRA 导向审查`、`可能与 MISRA 原则冲突`。没有用户提供的版本、偏差流程或标准摘录时，不要声称正式合规，不要复述专有规则正文。

### CERT C

适用场景：强调安全编码、未定义行为、资源/API 使用安全。

核心约束：数组越界、未初始化读取、整数溢出/截断/符号扩展、格式化 API 边界、错误处理一致性、并发数据竞争。

输出措辞：可以引用公开 rule family（`ARR`/`EXP`/`INT`/`MEM`/`CON`/`ERR`/`API`），确定时可给 rule ID。

### BARR-C

适用场景：裸机驱动、BSP、外设层，强调缺陷预防、可维护性、可移植性。

核心约束：命名/类型/接口清晰度、宏与寄存器封装易误用性、模块分层、错误码与状态机可调试性、编译器扩展隔离。

输出措辞：使用 `BARR-C 导向 finding`。BARR-C 有版权约束，不要大段复述标准正文。

### 华为C规范

适用场景：通用嵌入式 C 代码审查，强调清晰性、简洁性、安全编码和工程规范。

核心约束：头文件组织（自包含/循环依赖/include保护）、函数设计（长度/嵌套/可重入性/错误处理）、标识符命名（g_/s_前缀/禁止拼音）、变量安全（初始化/单一功能/字节序）、宏与常量安全（括号完备/do-while(0)/魔鬼数字）、内存安全（越界/泄漏/野指针）、表达式求值顺序、格式化字符串安全。

输出措辞：使用 `华为C规范导向 finding`。基于华为公开编程规范。

### FreeRTOS

适用场景：项目使用 FreeRTOS，关心 ISR/task API 边界、优先级、临界区、同步。

核心约束：ISR 中只能用 `FromISR` 族 API 且需满足优先级约束；任务态和 ISR-safe API 不混用；临界区、调度点、唤醒语义、通知覆盖行为需单独检查。

输出措辞：可以直接判断，如 `违反 FreeRTOS ISR API 约束`。约束来自公开 API 和官方文档。

## 审查流程

### Phase 0: Preflight — 范围与上下文

如果用户提供了仓库路径或 diff range，先运行 preflight 脚本：

```
scripts/prepare-diff.sh <repo_path> [diff_range]
```

脚本自动输出：
- 仓库信息（分支、最近提交）
- 目标识别（MCU、RTOS、编译器、SDK/HAL）
- Diff 统计、行数、大小评估（SMALL/MEDIUM/LARGE）
- 关键路径检测（ISR/DMA、crypto、NFC、boot/OTA、debug/security、concurrency）
- 完整 diff 内容

根据脚本输出决定审查策略：
- **SMALL（≤100 行）**：单模型审查
- **MEDIUM（101-500 行）**：单模型审查，按优先级组织
- **LARGE（>500 行）**：按子系统分批审查
- **检测到关键路径**：建议双模型交叉审查

如果用户直接给了代码片段而非仓库路径，跳过此步，直接进入 Phase 1。

### Phase 1: 审查执行

#### 1. 建立工程上下文

识别：MCU/SoC/平台、编译器/SDK/RTOS/HAL、代码运行上下文（task/ISR/裸机/混合）、是否涉及 DMA/cache/MMIO/shared state/ownership transfer。

#### 2. 加载检测规则

审查时必须读取 `.em_skill.json` 中记录的规则库。规则库包含具体的违规模式、BAD/GOOD 代码对比和严重级别，是执行检测的核心依据：

**按标准 profile 加载**（必须）：
- `references/cert-c-rules.md` — CERT C（ARR/INT/MEM/EXP/CON/ERR/STR/DCL/MSC）
- `references/misra-c-rules.md` — MISRA C 导向（类型转换/指针/控制流/宏/副作用/可重入性）
- `references/barr-c-rules.md` — BARR-C 导向（寄存器访问/命名/分层/错误处理/可移植性）
- `references/huawei-c-rules.md` — 华为C规范（头文件/函数/命名/变量/宏/质量保证/安全/表达式/注释排版）
- `references/freertos-rules.md` — FreeRTOS（ISR API/任务管理/同步原语/临界区/定时器/内存）

**按配置加载**：
- `references/security-rules.md` — 安全审查（密钥管理/调试接口/OTA/侧信道/输入验证/运行时保护）
- `references/c-pitfalls-rules.md` — C 语言陷阱（链接器/启动代码/编译器优化/C++ 互操作/可移植性/危险函数）

按已确认的规则组合读取，例如 `CERT C + FreeRTOS + Security` 审查时读取 `cert-c-rules.md` + `freertos-rules.md` + `security-rules.md`。

**按需加载**：
- `references/checklists.md` — 系统性扫描清单 + 深度机理参考（含 ARM 原子性参考表、危险函数速查表）

**本地规范（`Local coding standard`）**：与其他规则不同，本地规范**只读工程文件**，不读 skill 内置模板：

- 读取 `<目标工程>/.ai/coding-standard.md`（唯一权威）。
- 若该文件不存在：**提示用户先运行 `scripts/init-standard.sh <工程路径>` 初始化**；不回退到内置模板，不擅自生成。
- 该文件由用户 init 后全权拥有、自由编辑；没有“基线 + 合并”逻辑。
- finding 的 Standard 写 `Local coding standard`，Location 引用文件中的分类标题或规则文本。

#### 3. 审查优先级

1. 内存破坏、未定义行为、生命周期错误
2. 中断误用、竞态、死锁、漏唤醒、同步错误
3. 安全漏洞（密钥泄露、未保护的调试接口、不安全的 OTA）
4. 超时、恢复、状态机、外设时序缺陷
5. C 语言陷阱（链接器问题、编译器优化、C++ 互操作）
6. 可维护性、可移植性、接口契约、可调试性

### Phase 2: 双模型交叉审查（可选）

当 diff 较大（>500 行）、涉及关键路径、或用户明确要求时，使用双模型交叉审查：

1. **派发两个独立审查**：使用 Agent 工具派两个 subagent 并行审查同一段代码，各自独立给出 findings
2. **交叉比对结果**：
   - **共识 finding**（两个都发现）→ 高置信度，标记 `[consensus]`
   - **单方 finding** → 二次验证后决定是否保留
   - **矛盾 finding** → 标记 `[disputed]`，列出双方理由，留给用户判断
3. **合并输出**：按统一格式输出，标注每条 finding 的来源

用户可以通过 "用双模型审查" 或 "quick review 就行" 覆盖默认策略。

### Phase 3: 输出格式

整体结构：

1. **Findings**（主体，按严重级别排序）
2. **Open questions**（和 confirmed findings 分开）
3. **Assumptions**
4. 如有必要，很短的 change summary

单条 finding 包含：

- **Standard**：`CERT C` / `FreeRTOS` / `MISRA-style` / `BARR-C 导向` / `华为C规范导向` / `Security` / `C-Pitfall`
- **Severity**：`critical` / `high` / `medium` / `low`
- **Location**：文件、函数、行号
- **Risk**：运行时会出什么问题
- **Trigger**：什么条件或时序下暴露
- **Fix direction**：最直接的修复方向
- **Evidence strength**：`formal claim not possible` / `strong public-basis` / `project-context dependent`

严重级别：

- `critical`：高概率内存破坏、危险硬件状态、系统级死锁、安全漏洞
- `high`：严重竞态、中断误用、数据丢失、恢复失败
- `medium`：较窄条件触发的正确性问题，或易埋雷的可维护性问题
- `low`：削弱可读性、可调试性或审查质量

### Phase 4: 审查后交互

输出 findings 后，提供明确的下一步选项：

```
发现 X 个问题（critical: _, high: _, medium: _, low: _）。

下一步：
1. 修复所有问题
2. 只修复 critical/high
3. 修复指定问题（告诉我编号）
4. 用双模型重新审查（如果当前是单模型）
5. 审查完成，不做修改
```

**重要**：在用户明确确认前，不要自动修改代码。

## 合规边界

- 代码片段审查不能推出项目级正式合规。正式合规需要全量代码、工具配置、偏差流程、静态分析、编译器约束和团队规则映射。
- 没有完整证据时使用 `formal claim not possible`；能明确指出与标准公开要求冲突时使用 `standard-guided finding`；用户给了内部规则后使用 `project-specific deviation`。
- 不要编造 rule ID、等级、偏差编号或认证结论。
- 如果没有明确缺陷，要明确说没有，并写清残余风险或测试空白。
- 用可证伪、可落地的陈述，避免空泛提醒。不要罗列泛泛风格建议。

## 资源索引

| 文件 | 用途 |
|------|------|
| `scripts/prepare-diff.sh` | Preflight：提取 diff、识别目标、检测关键路径 |
| `scripts/init-standard.sh` | 在工程 `.ai/` 生成 `coding-standard.md`；向上检索到 Git 根，若路径上已有 `CLAUDE.md` / `AGENTS.md` 则追加规范入口提示 |
| `references/coding-standard.md` | 本地编码规范**模板**（init 源，面向 AI 写代码约束，不参与运行时） |
| `references/cert-c-rules.md` | CERT C 检测规则（29 条） |
| `references/misra-c-rules.md` | MISRA C 导向检测规则（20 条） |
| `references/barr-c-rules.md` | BARR-C 导向检测规则（22 条） |
| `references/huawei-c-rules.md` | 华为C规范检测规则（25 条） |
| `references/freertos-rules.md` | FreeRTOS 检测规则（19 条） |
| `references/security-rules.md` | 嵌入式安全检测规则（14 条） |
| `references/c-pitfalls-rules.md` | C 语言/工具链陷阱检测规则（11 条） |
| `references/checklists.md` | 系统性扫描清单 + 深度机理参考（含 ARM 原子性表、危险函数表） |
| `references/prompt-examples.md` | 用户调用模板 |
