#!/bin/bash
# init-standard.sh — 在用户工程生成 .ai/coding-standard.md（从内置模板拷贝）
# 用法:
#   init-standard.sh <project_path>
#
# 把 skill 内置的编码规范模板复制到 <project_path>/.ai/coding-standard.md。
# 该文件是用户工程的本地规范权威——审查（rules 含 Local coding standard）只认这一份。
# 若工程目录到 Git 仓库根目录之间已有 CLAUDE.md / AGENTS.md，则写入短提示，方便 AI agent 发现工程规范。
# 已存在时不覆盖，保护用户编辑。

set -euo pipefail

PROJECT="${1:-}"

if [ -z "$PROJECT" ]; then
    echo "ERROR: 缺少工程路径参数。用法: init-standard.sh <project_path>" >&2
    exit 1
fi

if [ ! -d "$PROJECT" ]; then
    echo "ERROR: 工程目录不存在: $PROJECT" >&2
    exit 1
fi

PROJECT_ABS="$(cd "$PROJECT" && pwd)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEMPLATE="$SCRIPT_DIR/../references/coding-standard.md"
TARGET_DIR="$PROJECT_ABS/.ai"
TARGET="$TARGET_DIR/coding-standard.md"
AGENT_NOTE_MARKER="<!-- embedded-coding-standard:init -->"
GIT_ROOT="$(git -C "$PROJECT_ABS" rev-parse --show-toplevel 2>/dev/null || true)"
AGENT_SEARCH_STOP="${GIT_ROOT:-$PROJECT_ABS}"

if [ ! -f "$TEMPLATE" ]; then
    echo "ERROR: 找不到内置模板: $TEMPLATE" >&2
    exit 1
fi

sync_agent_note() {
    local file="$1"

    if [ ! -f "$file" ]; then
        return
    fi

    if [ -f "$file" ] && grep -Fq "$AGENT_NOTE_MARKER" "$file"; then
        echo "已存在规范提示: $file"
        return
    fi

    if [ -s "$file" ]; then
        printf '\n' >> "$file"
    fi

    cat >> "$file" <<'EOF'
<!-- embedded-coding-standard:init -->
## Embedded Coding Standard

- 本工程编码规范位于 `.ai/coding-standard.md`，这是项目本地规范权威。
- 修改或审查嵌入式 C/C++ 代码前，先读取并遵守该文件；不要回退到 skill 内置模板。
<!-- /embedded-coding-standard:init -->
EOF

    echo "已同步规范提示: $file"
}

sync_existing_agent_notes() {
    local found=0
    local name
    local dir
    local file
    local parent

    for name in CLAUDE.md AGENTS.md; do
        dir="$PROJECT_ABS"
        while true; do
            file="$dir/$name"
            if [ -f "$file" ]; then
                sync_agent_note "$file"
                found=1
            fi

            if [ "$dir" = "$AGENT_SEARCH_STOP" ]; then
                break
            fi

            parent="$(dirname "$dir")"
            if [ "$parent" = "$dir" ]; then
                break
            fi
            dir="$parent"
        done
    done

    if [ "$found" -eq 0 ]; then
        echo "未在工程目录到仓库根目录范围内发现 CLAUDE.md / AGENTS.md —— 跳过规范提示同步。"
    fi
}

if [ -f "$TARGET" ]; then
    echo "已存在 $TARGET —— 不覆盖（保护你的编辑）。"
    echo "如需重新生成，先备份并删除该文件再运行本脚本。"
else
    mkdir -p "$TARGET_DIR"
    cp "$TEMPLATE" "$TARGET"
    echo "已生成 $TARGET"
fi

sync_existing_agent_notes

echo "这是本工程的本地规范权威——自由编辑它；审查时只认这一份。"
