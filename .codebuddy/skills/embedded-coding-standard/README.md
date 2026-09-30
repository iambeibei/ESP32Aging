# embedded-coding-standard 使用说明

## 用途

提供嵌入式 C/C++ 编码规范查阅与第三方标准代码审查：

- 写代码前查阅 / 对齐规范。
- 按第三方标准审查嵌入式代码：MISRA C、CERT C、BARR-C、华为 C 语言编程规范、
  FreeRTOS、安全规则和 C 语言陷阱。
- 用工程 `.ai/coding-standard.md`（由内置模板 init 生成）作为本地规范约束。

审查统一按第三方标准 review 流程执行；内置 `coding-standard.md` 是**模板**，运行时本地规范
以工程 `.ai/coding-standard.md` 为唯一权威。
首次审查某个项目时，会先询问 review 规则组合，并写入项目根目录 `.em_skill.json`；
后续默认复用该配置，除非用户要求修改。

## 初始化你的编码规范

内置 `references/coding-standard.md` 是只读模板。要用本地规范参与审查，先在工程里生成一份：

```bash
scripts/init-standard.sh <工程路径>
```

- 在 `<工程>/.ai/coding-standard.md` 生成一份面向 AI 写代码的精简规范（已存在则不覆盖）。
- 从 `<工程>` 向上检索到 Git 仓库根目录；若路径上已有 `CLAUDE.md` 或 `AGENTS.md`，则追加规范入口提示（带标记，重复运行不重复追加）；不存在则不创建。
- 该文件是工程的**本地规范权威**——自由增删改条目。
- 审查（rules 含 `Local coding standard`）只读这一份；不存在时提示先 init，不回退模板。

## 安装到 Codex

个人安装路径：

```text
%USERPROFILE%\.codex\skills\embedded-coding-standard
~/.codex/skills/embedded-coding-standard
```

PowerShell 安装示例：

```powershell
$src = 'N:\AI\embedded_skill\embedded-coding-standard'
$dst = "$env:USERPROFILE\.codex\skills\embedded-coding-standard"
New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force
```

Codex 调用：

```text
$embedded-coding-standard
```

第三方审查示例：

```text
按 CERT C + FreeRTOS 审查这个补丁，重点看 ISR/task API、竞态和未定义行为。
按 BARR-C 审查这个驱动模块，重点看可维护性、寄存器封装和错误处理。
按 CERT C + FreeRTOS 审查 N:\path\to\repo 的 HEAD~5..HEAD 改动。
```

首次审查时会在项目根目录写入 `.em_skill.json` 记录规则组合；完整格式定义见
`references/third-party-code-review.md`（不在 README 重复）。

## 安装到 Claude Code

个人安装路径：

```text
%USERPROFILE%\.claude\skills\embedded-coding-standard
~/.claude/skills/embedded-coding-standard
```

项目本地安装路径：

```text
.claude/skills/embedded-coding-standard
```

PowerShell 安装示例：

```powershell
$src = 'N:\AI\embedded_skill\embedded-coding-standard'
$dst = "$env:USERPROFILE\.claude\skills\embedded-coding-standard"
New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force
```

Claude Code 调用：

```text
/embedded-coding-standard
```

## 典型输入

- 审查目标：文件 / diff / PR / 目录路径。
- （可选）项目已有的命名 / 分层约定，作为 N / F 类规则的基线。
- 第一次审查时选择第三方标准 profile：MISRA C / CERT C / BARR-C / 华为 C 规范 /
  FreeRTOS / Security / C-Pitfall / Local coding standard。

## 典型输出

- 针对某条本地规范的解释与示例（查阅模式）。
- 第三方审查 findings：Standard / Severity / Location / Risk / Trigger /
  Fix direction / Evidence strength。
- 若纳入本地规范，违反项以 `Local coding standard` 标注，并引用本地分类或规则原文。
- 输出开头声明本次使用的 `.em_skill.json` 规则组合。

## 资源

- 编码规范模板：`references/coding-standard.md`（init 生成工程 `.ai/coding-standard.md`）
- 初始化脚本：`scripts/init-standard.sh`
- 第三方标准审查入口：`references/third-party-code-review.md`
- 第三方规则库：`references/*-rules.md`
- Diff 预检脚本：`scripts/prepare-diff.sh`

## 故障排查

- 如果 Codex 没有发现 skill，确认
  `%USERPROFILE%\.codex\skills\embedded-coding-standard\SKILL.md` 存在，然后重启会话。
- 如果 Claude Code 没有发现 skill，确认
  `%USERPROFILE%\.claude\skills\embedded-coding-standard\SKILL.md` 或
  `.claude\skills\embedded-coding-standard\SKILL.md` 存在，然后重启会话。
