---
name: embedded-coding-standard
description: >-
  嵌入式 C/C++ 编码规范查阅与第三方标准代码审查。当用户要初始化、查阅或对齐工程
  `.ai/coding-standard.md` 本地规范，或要求按 MISRA C、CERT C、BARR-C、华为 C
  语言编程规范、FreeRTOS 官方约束等外部标准审查 firmware、driver、HAL、BSP、ISR、
  RTOS task、DMA、寄存器访问或底层并发代码时使用。审查统一按第三方标准 review 流程
  执行，并在项目 `.em_skill.json` 中记录用户选择的 review 规则组合；内置
  `references/coding-standard.md` 只作为 init 模板，运行时本地规范以工程文件为准。
  中文优先，不编造规范条目或合规结论。
---

# Embedded Coding Standard

为嵌入式 C/C++ 项目提供两类能力：

1. **本地编码规范查阅**：基于内置模板，在工程 `.ai/coding-standard.md` 维护项目规范。
2. **第三方标准代码审查**：按 MISRA C、CERT C、BARR-C、华为 C 规范、FreeRTOS、
   安全规则、C 语言陷阱审查固件 / 驱动 / HAL / ISR / RTOS 代码。

## 如何路由

- **初始化本地规范**（首次为工程建立编码规范）：运行
  `scripts/init-standard.sh <工程路径>`，在工程下生成 `.ai/coding-standard.md`（从内置模板拷贝，
  已存在不覆盖）；从工程目录向上检索到 Git 仓库根目录，若路径上已有 `CLAUDE.md` / `AGENTS.md`，
  则写入幂等规范入口提示，不存在则不创建。用户随后在该文件里自由增删改条目。
- **查阅规范**（写代码前对齐命名 / 分层 / 内存 / ISR / 错误处理等条目）：
  读工程的 `.ai/coding-standard.md`（不存在则引导用户先 init），按分类或规则原文引用要点，
  不展开整篇。[references/coding-standard.md](references/coding-standard.md)
  只是 AI 写代码约束模板，**不参与运行时**。
- **代码审查**（文件 / diff / PR / 目录）：读
  [references/third-party-code-review.md](references/third-party-code-review.md)，**所有审查流程、
  规则选择、配置格式、输出规范均以该文件为准**。审查中 `Local coding standard` 只读
  `<工程>/.ai/coding-standard.md`，不存在则提示 init。

## 边界

- 审查统一走第三方标准流程；内置 `coding-standard.md` 是只读模板，运行时本地规范以工程
  `.ai/coding-standard.md` 为唯一权威，不另开 review 流程。
- 审查基于已加载规则库或明确项目上下文，不编造 rule ID、偏差编号、认证或合规结论。
- 只违反本地规范而不属第三方标准的问题，Standard 写 `Local coding standard` 并引用本地分类或规则原文；
  `coding-standard.md` 没有的规则不反向新增。
- 不替代构建 / 测试 / 烧录 / 硬件验证。
- 输出 findings 后，除非用户明确要求修复，不自动改代码。
- 输出语言跟随用户；中文请求默认中文。

## 资源

- 本地编码规范模板（只读，init 源）：[references/coding-standard.md](references/coding-standard.md)
- 第三方审查总入口（流程权威）：[references/third-party-code-review.md](references/third-party-code-review.md)
- 第三方规则库：`references/cert-c-rules.md`、`misra-c-rules.md`、`barr-c-rules.md`、
  `huawei-c-rules.md`、`freertos-rules.md`、`security-rules.md`、`c-pitfalls-rules.md`
- 扫描清单（含深度机理参考、ARM 原子性表、危险函数表）：[references/checklists.md](references/checklists.md)
- 调用示例：[references/prompt-examples.md](references/prompt-examples.md)
- 脚本：`scripts/prepare-diff.sh`（diff 预检）、`scripts/init-standard.sh`（生成工程规范）
- 安装与调用：[README.md](README.md)
