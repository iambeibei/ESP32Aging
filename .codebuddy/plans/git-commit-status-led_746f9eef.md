---
name: git-commit-status-led
overview: 按已确认范围提交本次状态指示灯功能（含 AGENTS.md 与设计文档），不包含 .codebuddy/plans 与之前的文档改动。
todos:
  - id: stage-files
    content: 按精确路径暂存 8 个状态灯相关文件并核对暂存区
    status: completed
  - id: create-commit
    content: 用既定中文提交信息创建本地提交
    status: completed
    dependencies:
      - stage-files
  - id: verify-commit
    content: 复核提交统计、确认排除文件仍未被提交
    status: completed
    dependencies:
      - create-commit
---

## 需求概述
将本次“状态指示灯”功能的全部改动提交到本地 Git 仓库，并附一条简短的中文提交说明；不推送到远端。

## 提交范围（用户已确认）
- 修改：`AGENTS.md`、`main/CMakeLists.txt`、`main/Task/appTask.c`、`main/Task/task_config.h`
- 新增：`main/APP/Status/status_led.c`、`main/APP/Status/status_led.h`、`STATUS_LED_DESIGN.md`、`tests/test_status_led.py`

## 明确排除
- `AGING_CMD_PN_GUARD_DESIGN.md`、`子节点日志.md`、`根节点日志.md`
- `.codebuddy/plans/*.md`（4 个，保持未跟踪）
- 不执行 push、不 amend、不修改 git config

## 提交说明要点
- 新增 IO18 PN 状态灯与 IO17 老化运行灯
- IO18 常亮/快闪/慢闪规则与 IO17 随老化启停的时序
- 补宿主机 mock 测试与设计文档
- 同一文件内已有的 agingStart 回执调整随本次一并提交


## 技术方案
### 提交策略
采用“精确路径暂存”而非 `git add -A`，确保工作区中用户先前的文档改动与 IDE 计划文件不被带入本次提交。`appTask.c` 同时包含状态灯钩子与已有的 agingStart 回执调整，两者无法按文件拆分，用户已确认一并提交。

### 暂存清单
```
AGENTS.md
main/CMakeLists.txt
main/Task/appTask.c
main/Task/task_config.h
STATUS_LED_DESIGN.md
main/APP/Status/status_led.c
main/APP/Status/status_led.h
tests/test_status_led.py
```

### 提交信息（沿用仓库中文描述式风格，无 conventional prefix）
```
新增 IO18 PN 状态灯与 IO17 老化运行灯

- 新增 status_led 模块：50 ms 轮询驱动常亮/快闪/慢闪，事件接口无阻塞
- IO18：set_pn 成功常亮，重复设置快闪 3 次，老化失败 1 Hz 慢闪，正常结束熄灭
- IO17：agingStart 置低，agingIdle/agingError/stop_aging 恢复高电平，掉电恢复上电即点亮
- 补宿主机 mock 测试与 STATUS_LED_DESIGN.md
- 同时提交 appTask.c 中已有的 agingStart 回执调整
```

### 执行与校验
1. `git add <精确路径>`，随后 `git status --short` 核对暂存区仅含上述 8 项；
2. 若发现范围外文件被暂存，先 `git reset` 该路径再重新暂存；
3. `git commit -F -` 传入上述提交信息（heredoc 方式，避免转义问题）；
4. `git log -1 --stat` 与 `git status --short` 复核：提交成功、被排除文件仍为未提交/未跟踪状态。

### 注意事项
- 提交者身份沿用仓库现有配置（wbb），不做任何 git config 变更；
- 若仓库配置了 pre-commit 钩子导致失败，只报告失败原因，不擅自加 `--no-verify`；
- 提交完成后仅报告 commit hash 与被排除文件清单，不执行 push。

