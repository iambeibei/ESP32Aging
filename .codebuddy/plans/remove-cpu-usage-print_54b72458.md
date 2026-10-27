---
name: remove-cpu-usage-print
overview: 删除按键诊断中的 CPU 占用率采样与打印模块，并关闭仅为其服务的 FreeRTOS 运行时间统计配置，保留堆/PSRAM/任务栈诊断。
todos:
  - id: remove-cpu-module
    content: 删除 app_cpu.c/h 与 test_cpu_usage.py，清理 CMakeLists 与 appTask.c 中 include 及三处调用
    status: completed
  - id: disable-runtime-stats
    content: 删除 sdkconfig.defaults 三项统计配置并就地关闭 build/mesh-lite/sdkconfig 对应项
    status: completed
    dependencies:
      - remove-cpu-module
  - id: update-docs
    content: 改写 CLAUDE.md 按键诊断段落，并从 AGENTS.md 测试清单移除 test_cpu_usage.py
    status: completed
    dependencies:
      - remove-cpu-module
  - id: verify-build
    content: 用 [skill:embedded-coding-standard] 复核改动，执行残留 grep、完整重编与宿主回归测试
    status: completed
    dependencies:
      - disable-runtime-stats
      - update-docs
---

## 功能概述
移除按键诊断中的 CPU 占用率采样与打印能力，并同步关闭仅为其服务的 FreeRTOS 运行时间统计配置；按键的堆内存、PSRAM、任务栈水位诊断保持不变。

## 核心功能
- 删除 CPU 占用率模块及其全部调用点，按键诊断不再输出 CPU0/CPU1/双核平均占用率
- 按键诊断保留：内部 SRAM free/min/largest、PSRAM free/min/largest、free heap、最小堆、各任务栈水位
- 同步关闭 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` 等三项配置，消除无消费者的运行时统计开销
- 删除该功能的宿主测试脚本，并更新工程文档中的相关描述与测试清单
- 构建产物与配置保持一致：`sdkconfig.defaults` 与当前 `build/mesh-lite/sdkconfig` 均不再启用运行时间统计


## 技术栈
沿用现有工程：ESP32-S3、ESP-IDF v5.4.4、C（工程风格）、FreeRTOS 双核、CMake；宿主测试为 python3 + cc 的 mock 编译方式。

## 实现方案
采用「删模块 + 清引用 + 关配置 + 文档对齐」四步，全部改动可逆且局限在已确认的 8 个文件与 3 个删除项。

### 1. 删除模块与引用
- 删除 `main/Tool/app_cpu.c`、`main/Tool/app_cpu.h`（整模块，仅被 `appTask.c` 使用）。
- `main/CMakeLists.txt` 删除 `"Tool/app_cpu.c"`（第 21 行），`INCLUDE_DIRS` 中的 `"Tool"` 保留（其他 Tool 模块仍在使用）。
- `main/Task/appTask.c` 删除三处：
  - 第 42 行 `#include "app_cpu.h"`；
  - `show_free_heap()` 第一条语句 `app_cpu_usage_log(tag);`，函数其余堆/PSRAM 日志与 `app_memory_log("button")` 保持原样，`tag` 参数继续被 `ESP_LOGI(tag, ...)` 使用，签名不变；
  - `button_task()` 主循环顶部与等待按键释放的 `while` 内的两处 `app_cpu_usage_poll();`。
- 删除 `tests/test_cpu_usage.py`。

### 2. 关闭 FreeRTOS 运行时间统计
- `sdkconfig.defaults`：删除第 47-51 行的注释块与三项 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` / `_USING_ESP_TIMER` / `_COUNTER_TYPE_U64`，替换为一行说明「运行时间统计仅为已移除的 CPU 占用率诊断服务，保持 Kconfig 默认关闭」，避免后续被误恢复。
- `build/mesh-lite/sdkconfig` 就地修改（禁止删除重建，否则丢失 `CONFIG_BRIDGE_ETH_SPI_PHY_RST0_GPIO=5` 等未入 defaults 的手工项）：
  - `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` 改为 `# CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is not set`；
  - 第 1817/1818 行的计数器位宽选择恢复默认：`CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U32=y`，并注释掉 `_U64`；
  - `CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y` 改为 `# ... is not set`；
  - 构建时由 confgen 规范化不可见符号，其余配置（`FREERTOS_USE_STATS_FORMATTING_FUNCTIONS`、`FREERTOS_HZ`、SMP/UNICORE、以太网 SPI 引脚）不得改动。

### 3. 文档对齐
- `CLAUDE.md:82-85` 整段改写：删除 CPU 占用率估算、`Tool/app_cpu.c`、ESP Timer/64 位统计的描述，改为「按键诊断打印内部 SRAM/PSRAM 用量与各任务栈水位」。
- `AGENTS.md:115` 测试清单删除 `test_cpu_usage.py` 一行，其余脚本说明保持不变。

## 实现要点
- **顺序**：先删调用再删模块，避免中间态出现 `app_cpu.c` 仍被 `#error` 门控引用；最后统一改配置触发重编。
- **配置一致性**：`app_cpu.c` 内有对 `CONFIG_FREERTOS_RUN_TIME_STATS_*` 的编译期 `#error`，模块删除后该门控随之消失，因此关闭统计不会造成构建失败；反之若先关统计后删模块会直接编译失败。
- **性能影响**：关闭运行统计后每个任务不再维护运行时间计数、上下文切换少一次计数更新，RAM 与 tick 开销略降；镜像可能小幅缩小。
- **爆炸半径**：`print_application_task_stacks()` 不依赖运行统计，按键行为、GPIO40、`button_task` 优先级与栈均不变；UART/RS485/CAN/以太网/OTA 均不涉及。

## 目录结构
```
main/
├── CMakeLists.txt                      # [MODIFY] 删除 "Tool/app_cpu.c" 编译项
├── Task/
│   └── appTask.c                       # [MODIFY] 删 include 与 3 处 CPU 采样/打印调用
└── Tool/
    ├── app_cpu.c                       # [DELETE] CPU 占用率采样实现
    └── app_cpu.h                       # [DELETE] 其对外接口
tests/
└── test_cpu_usage.py                   # [DELETE] 该模块宿主测试
sdkconfig.defaults                      # [MODIFY] 删除 3 项运行时间统计并留说明注释
build/mesh-lite/sdkconfig                # [MODIFY] 就地关闭统计、位宽回落 U32（禁止重建）
CLAUDE.md                                # [MODIFY] 改写按键诊断段落
AGENTS.md                                # [MODIFY] 测试清单删除 test_cpu_usage.py
```

## 验证方案
1. 残留检查：`grep -rn "app_cpu\|app_cpu_usage" main/ tests/` 应为空结果。
2. 配置检查：`build/mesh-lite/sdkconfig` 中 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` 为 not set；`build/mesh-lite/config/sdkconfig.h` 中该宏未定义。
3. 完整重编（配置变更会触发大范围重编）：
   `cd /home/wbb/espidf/esp-idf-mesh-lite && . ./export.sh`，再执行
   `IDF_SKIP_CHECK_SUBMODULES=1 idf.py -B build/mesh-lite -D SDKCONFIG=build/mesh-lite/sdkconfig build`，要求 exit=0。
4. 宿主回归：`test_status_led`、`test_root_uplink`、`test_mqtt_uplink`、`test_system_restart`、`test_http_protocol`、`test_psram_memory` 全部 exit=0（`test_mesh_ota_binary.py` 需 OpenSSL，不在本次范围）。
5. 可选：`idf.py size` 对比关闭统计前后的 RAM 与镜像占用。


## Agent Extensions
### Skill
- **embedded-coding-standard**
  - Purpose: 在删除完成、编译通过后按 CERT C + FreeRTOS + Local coding standard 复核改动，重点确认无残留引用、配置门控未破坏、按键诊断其余路径无副作用
  - Expected outcome: 输出逐条结论（无残留符号、无悬空声明、配置项与其他 FreeRTOS 选项无冲突），确认可交付
