#!/bin/bash
# prepare-diff.sh — 提取 git diff 并构建审查上下文
# 用法:
#   prepare-diff.sh <repo_path>                        # 未提交改动
#   prepare-diff.sh <repo_path> HEAD~3..HEAD           # 最近 3 次提交
#   prepare-diff.sh <repo_path> main..feat/nfc         # 分支对比

set -euo pipefail

REPO="${1:-.}"
RANGE="${2:-}"

if [ ! -d "$REPO" ]; then
    echo "ERROR: 目录不存在: $REPO" >&2
    exit 1
fi

cd "$REPO"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "ERROR: 不是 git 仓库: $(pwd)" >&2
    exit 1
fi

GIT="git --no-pager"

echo "=== 仓库信息 ==="
echo "路径: $(pwd)"
echo "分支: $($GIT branch --show-current 2>/dev/null || echo 'detached')"
echo "最近提交: $($GIT log -1 --oneline 2>/dev/null || echo 'none')"

echo ""
echo "=== 目标识别 ==="

# MCU
BUILD_FILES=$(find . -maxdepth 3 \( -name 'CMakeLists.txt' -o -name 'Makefile' -o -name '*.cmake' -o -name '*.mk' -o -name '*.ld' -o -name '*.icf' -o -name '*.sct' \) 2>/dev/null | head -20)
if [ -n "$BUILD_FILES" ]; then
    MCU=$(echo "$BUILD_FILES" | xargs grep -lhiE 'STM32|nRF5|ESP32|ATSAMD|RP2040|GD32|CH32|MSP430|PIC|AVR|ATSAM|IMXRT|LPC|EFM32|CY8C' 2>/dev/null | head -3 || true)
    RTOS=$(echo "$BUILD_FILES" | xargs grep -lhiE 'FreeRTOS|Zephyr|ThreadX|CMSIS.RTOS|RT.Thread|embOS|uCOS' 2>/dev/null | head -3 || true)
    COMPILER=$(echo "$BUILD_FILES" | xargs grep -lhiE 'arm-none-eabi|armcc|iccarm|armclang|xtensa|riscv' 2>/dev/null | head -3 || true)
    [ -n "$MCU" ] && echo "MCU 相关: $MCU"
    [ -n "$RTOS" ] && echo "RTOS 相关: $RTOS"
    [ -n "$COMPILER" ] && echo "编译器相关: $COMPILER"
    [ -z "$MCU" ] && [ -z "$RTOS" ] && [ -z "$COMPILER" ] && echo "(构建文件中未识别到目标信息)"
else
    echo "(未找到构建文件)"
fi

# SDK/HAL 检测
SDK_FILES=$(find . -maxdepth 4 -name 'stm32*hal*.h' -o -name 'nrf_drv_*.h' -o -name 'esp_*.h' -o -name 'pico/stdlib.h' -o -name 'fsl_*.h' 2>/dev/null | head -5)
if [ -n "$SDK_FILES" ]; then
    echo "SDK/HAL: $SDK_FILES"
fi

# Diff 收集
echo ""
echo "=== DIFF 统计 ==="
if [ -n "$RANGE" ]; then
    $GIT diff --stat "$RANGE" 2>/dev/null
    DIFF=$($GIT diff "$RANGE" 2>/dev/null)
else
    STAGED=$($GIT diff --cached --stat 2>/dev/null)
    UNSTAGED=$($GIT diff --stat 2>/dev/null)

    if [ -n "$STAGED" ] || [ -n "$UNSTAGED" ]; then
        [ -n "$STAGED" ] && echo "--- 暂存 ---" && echo "$STAGED"
        [ -n "$UNSTAGED" ] && echo "--- 未暂存 ---" && echo "$UNSTAGED"
        DIFF="$($GIT diff --cached 2>/dev/null)
$($GIT diff 2>/dev/null)"
    else
        echo "(无未提交改动，显示最近一次提交)"
        $GIT diff HEAD~1 --stat 2>/dev/null || true
        DIFF=$($GIT diff HEAD~1 2>/dev/null || true)
    fi
fi

# 大小评估
LINE_COUNT=$(echo "$DIFF" | wc -l | tr -d ' ')
echo ""
echo "=== DIFF 大小 ==="
echo "行数: $LINE_COUNT"
if [ "$LINE_COUNT" -le 100 ]; then
    echo "评估: SMALL — 单模型审查即可"
elif [ "$LINE_COUNT" -le 500 ]; then
    echo "评估: MEDIUM"
else
    echo "评估: LARGE — 建议按子系统分批审查"
fi

# 关键路径检测
echo ""
echo "=== 关键路径 ==="
CRITICAL=""
if echo "$DIFF" | grep -qiE '(IRQ|ISR|_Handler|_IRQn|interrupt|DMA_|HAL_DMA|dma_ch)'; then
    CRITICAL="${CRITICAL} [ISR/DMA]"
fi
if echo "$DIFF" | grep -qiE '(crypt|aes|sha|hmac|trng|rng_|secure_|mbedtls)'; then
    CRITICAL="${CRITICAL} [CRYPTO]"
fi
if echo "$DIFF" | grep -qiE '(nfc|nci|ndef|rfid|contactless|iso14443|iso15693)'; then
    CRITICAL="${CRITICAL} [NFC]"
fi
if echo "$DIFF" | grep -qiE '(boot|bootloader|flash_write|flash_erase|OTA|fota|firmware_update)'; then
    CRITICAL="${CRITICAL} [BOOT/OTA]"
fi
if echo "$DIFF" | grep -qiE '(SWD|JTAG|debug_uart|debug_port|unlock|auth_bypass)'; then
    CRITICAL="${CRITICAL} [DEBUG/SECURITY]"
fi
if echo "$DIFF" | grep -qiE '(mutex|semaphore|critical|atomic|spinlock|taskENTER)'; then
    CRITICAL="${CRITICAL} [CONCURRENCY]"
fi

if [ -n "$CRITICAL" ]; then
    echo "检测到关键路径:${CRITICAL}"
    echo "  → 建议启用双模型交叉审查"
else
    echo "未检测到特殊关键路径"
fi

# 变更文件类型统计
echo ""
echo "=== 文件类型 ==="
if [ -n "$RANGE" ]; then
    $GIT diff --name-only "$RANGE" 2>/dev/null | sed 's/.*\.//' | sort | uniq -c | sort -rn | head -10
else
    ($GIT diff --cached --name-only 2>/dev/null; $GIT diff --name-only 2>/dev/null) | sed 's/.*\.//' | sort | uniq -c | sort -rn | head -10
fi

echo ""
echo "=== DIFF 内容 ==="
echo "$DIFF"
