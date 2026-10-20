---
name: public-stop-aging-subscription
overview: 子节点新增订阅广播停止主题 server/<AgingNumber>/public/cmd/stop_aging，复用现有 stop_aging 分支处理（strstr 子串命中 + 幂等），实现一键停止整架全部点位；仅 mqtt_app.c 一处改动，处理代码零修改。
todos:
  - id: add-public-stop-aging-sub
    content: 在 mqtt_app.c 子节点订阅区新增 server/<AgingNumber>/public/cmd/stop_aging 订阅（复用 AgingNumber 范式，注释说明一键广播停止用途）
    status: completed
  - id: build-verify
    content: 编译验证 mqtt_app.c 改动零告警、镜像尺寸与分区余量正常
    status: completed
    dependencies:
      - add-public-stop-aging-sub
  - id: coding-standard-review
    content: 用 [skill:embedded-coding-standard] 对照本地规范自审本次订阅改动
    status: completed
    dependencies:
      - build-verify
---


## 用户需求

子节点新增订阅广播停止主题 `server/<AgingNumber>/public/cmd/stop_aging`（如 `server/2/public/cmd/stop_aging`，其中 `2` 为 AgingNumber），功能与现有定点老化停止指令完全一致，便于服务器一键停止整架全部点位。仅子节点订阅，根节点不订阅。

## 核心发现（调研结论）

**处理代码零改动**：`parse_jsonCommand_MQTT()` 的主题分发使用子串匹配 `strstr(TopicBuf, "stop_aging")`（appTask.c:2640），新主题含子串 `"stop_aging"`，天然命中现有 stop_aging 分支（appTask.c:2640-2687，含删老化任务、断 BLE、清 NVS/状态/配置、删 Aging.json、上报 `agingStop`），该分支为幂等操作，定点+广播双主题重复执行无害。

唯一改动点：`mqtt_app.c` MQTT_EVENT_CONNECTED 的子节点订阅区（`IsRoot == 0` 分支，定点 stop_aging 订阅块之后）新增一条订阅，复用 CanNextstep/CanRestart 的 `AgingNumber` 范式。

## 约束与边界

- payload 必须含数值 `Seq` 字段（appTask.c:2554 入口门），需服务器侧保证广播主题 payload 与现有 stop_aging 同格式；
- 不改 appTask.c 任何代码（分发与处理全复用）；
- 订阅失败打 ERROR 日志，与既有订阅块风格一致；
- 无测试套件，验证 = 编译 + 串口/实机观察（订阅成功日志 + 广播主题下发后 `agingStop` 上报）。



## 技术方案

### 技术栈

- 语言：C（gnu17）、ESP-IDF v5.4.4、ESP32-S3、esp-mqtt 组件
- 复用既有能力：`AgingNumber` 全局变量（ConfigData.h:26，readconfig 加载，默认 `"-1"` 非 NULL）、`snprintf` 主题构造范式、`esp_mqtt_client_subscribe`（QoS 0）
- 无新依赖、无新文件、无新任务；改动限于 `mqtt_app.c` 单文件一处

### 实现方式

**插入点**：mqtt_app.c:448（定点 `server/<DEVICE_ID>/cmd/stop_aging` 订阅块结束）之后，`set_pn` 订阅之前，与相邻订阅块完全同构：

```c
// 订阅广播停止老化主题：一键停止本架（AgingNumber）全部点位，
// 命中 parse_jsonCommand_MQTT 的 stop_aging 分支（strstr 子串匹配，幂等）
memset(topic, 0, sizeof(topic));
snprintf(topic, sizeof(topic), "server/%s/public/cmd/stop_aging", AgingNumber);
if (esp_mqtt_client_subscribe(event->client, topic, 0) < 0)
{
    ESP_LOGE(TAG, "Failed to subscribe to public stop_aging");
}
```

### 关键决策与权衡

- **订阅即用，零分发改动**：现有 `strstr(TopicBuf, "stop_aging")` 天然覆盖新主题，避免新增分支引入回归；stop_aging 处理为幂等操作（删任务/清状态/断 BLE 均安全重复），定点+广播双通道并存无副作用；
- **主题用 AgingNumber 而非 DEVICE_ID**：广播语义要求同一老化架号全部点位订阅同一主题，与 CanNextstep/CanRestart 的 `server/rack/<AgingNumber>/...` 范式一致；
- **仅子节点订阅**：与现有老化命令（start/stop/set_pn）分发模型一致——根为总控不跑老化；放入 `IsRoot == 0` 分支；
- **不做主题去重/防抖**：MQTT QoS 0 + 幂等处理已足够；重复下发第二次执行无害，不增加复杂度（YAGNI）。

### 性能与可靠性

- 时间/空间复杂度：O(1)，仅 MQTT 连接时执行一次；无内存分配（复用局部 `char topic[64]`，主题约 30 字符容量充足）；
- 订阅失败仅打 ERROR 不中断其余订阅（与既有 9 个订阅块一致）；
- 广播风暴风险：服务器一键下发仅一条 publish，broker  fanout 至本架 N 个子节点，各节点处理为本地幂等操作，无网络回压。

### 架构影响

- 订阅清单扩展（子节点 9 → 10 条），无架构模式变化；未来如需其他广播指令（如 public/cmd/start_aging），沿用同一范式追加即可。


# Agent Extensions
- **embedded-coding-standard**
  - 用途：改动完成后对照 `.ai/coding-standard.md` 本地规范自审订阅代码块（注释用途说明、错误路径非空、命名贴合相邻代码）
  - 预期结果：确认改动合规，无规范回归
