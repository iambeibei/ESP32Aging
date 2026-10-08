# ESP32Aging 代码审查报告（华为 C 语言编程规范）

> 生成时间：2026-09-30
> 审查对象：`main/` 全目录（61 个文件，约 25,000 行）
> 代码基线：`0.0.0.10`（`4aa258b`） + 未提交的 2 行判据修改
> 规则组合：`Huawei C`（记录于 `.em_skill.json`）

---

## 一、审查元信息

| 项目 | 内容 |
|---|---|
| 规则 | 华为 C 语言编程规范（HW-HDR / HW-FUNC / HW-NAME / HW-VAR / HW-MACRO / HW-QA / HW-SEC / HW-EXPR / HW-FMT 共 33 条） |
| 方式 | 按 6 个子系统并行分批全量通读；单模型审查 |
| 平台上下文 | ESP-IDF v5.4.4 / ESP32-S3 / FreeRTOS 双核 / PSRAM / SPIFFS + FATFS + SQLite |
| 验证 | 静态审查；未编译运行（后续修复阶段已 `idf.py build` 通过） |
| 结论性质 | 华为 C 规范**导向 finding**，非认证结论 |

**问题统计**：critical 12 / high 26 / medium 11 类 / low 5 类

> 说明：审查后曾完成一轮修复（18 文件 +1339 行），但代码已回退到 `0.0.0.10`，因此**下列问题目前全部处于未修复状态**，本文件可作为整改 backlog 使用。

---

## 二、Critical（12）

| ID | 规则 | 位置 | 风险 / 触发 | 修复方向 |
|---|---|---|---|---|
| R1 | HW-QA-01/03、HW-FUNC-03 | `SqLite.c:2074`（分配未判空即 memset）、`1747/1753`；使用点 `appTask.c:3242 / 4977 / 5006` | `g_db1_result` 在 FATFS 挂载失败或 PSRAM 不足时为 NULL，三处直接 `->json_data[0]` → 空指针崩溃；且该全局缓冲被 MQTT 任务与老化任务并发 memset+填充，无锁 → 读到半填充数据会把错误记录标记为已推送 | 分配后判空并返回失败；改用调用方自带缓冲或为全局量加锁 |
| R2 | HW-SEC-02/03、HW-QA-01 | `app_enc.c:252-289`（`uint16_t len = in_data_len - 6`）；调用点 `appTask.c:2089 / 2205` | `in_data_len < 6` 无符号下溢成 65534，保护式两侧类型不同恒失效 → `iot_aes_cbc_decrypt_data(..., 65534, ...)` 大规模越界读+写；调用方既未保证长度也忽略返回值 | 入口 `if (in_data_len < 6) return ESP_ERR_INVALID_SIZE;`，同类型比较；调用方检查返回值 |
| R3 | HW-QA-01 | `mesh.c:163-179`（`CMD_ROUTE_TABLE`）连带 `:396-400`、`:431` | `s_route_table` 仅 300 字节，只校验 `size%6==0` 未校验上界 → 越界写 `.bss` 相邻对象；`s_route_table_size` 被写成 >50 后继续越界。mesh 未加密，空中报文即可触发 | 校验 `(size-1)/6 <= CONFIG_MESH_ROUTE_TABLE_SIZE` 后再 memcpy 并钳制 `s_route_table_size` |
| R4 | HW-QA-01 | `mesh.c:145-148`、`206-218`、`240-268`、`272-284` | ① 入口未校验 `data->size` 直接读 `data->data[0]`；② `name_len`（最大 255）直接 memcpy 进 `char name[64]` → 栈溢出（含返回地址）；③ 扫描结果循环 `pos+=name_len/+6` 越界读 | 入口判空判长度；每 case 校验最小长度；`name_len` 裁剪到 `sizeof(buf)-1` |
| R5 | HW-QA-01/SEC-02 | `mesh.c:305-354`（`CMD_USER_MSG`） | `int msg_len = data->size - 7` 可为负 → VLA 负长度（UB）；`memcpy` 负值转 size_t 接近 4GB；`printf("%.*s", 负精度)` UB | 先 `if (data->size < 8) return;`；`msg_len` 用 size_t；回执缓冲改固定长度 |
| R6 | HW-QA-01 | `appTask.c:1154-1187`（`readconfig`） | ① `file_names[6][64]` 但 `max_files` 传 10 → 写穿栈；② `read_devices` 只有 6 个元素，第 7 个起堆越界写；③ 分配失败仍 `memset` → 写 0 地址崩溃 | 容量与 `max_files` 用同一常量；槽位满时 `continue`；分配失败立即返回 |
| R7 | HW-QA-01 | `appTask.c:2077-2101`（`app_ble_recv_data_Ack`）+ 调用点 `:3830`（`RecBuf[256]`） | 解密输出 `outLen`（最大 260，不受 `max_len` 约束）未校验即 `memcpy` → 写穿 256 字节栈缓冲 | 回拷前 `if (outLen > max_len) return -1;` 或给解密接口加输出容量参数 |
| R8 | HW-QA-01 | `appTask.c:4664-4673`（`AgingDeviceCheck` 蓝牙分支） | 未找到含采样数据的 step 时 `itest == step_count`，随后 `steps[itest].sample_data[0].Value` 越界读/NULL 解引用（CAN/RS485 分支都有保护） | 循环后补 `itest >= step_count \|\| sample_data == NULL` 兜底 |
| R9 | HW-FUNC-03 | `appTask.c:3088/4230/4361/4508/5051`（`vTaskDelete` + `AgingDevice_RuntimeFree`/`aging_config_free`）vs 读侧 `:4674/4706/4732/5089/5241/5314` | MQTT 任务先删采集任务再 `free(Aging_device)`，被删任务可能仍持有互斥量或即将访问已释放内存 → UAF / 堆损坏 / 互斥量永久丢失 | 改为"置停止事件 → 任务自行退出并释放 → 确认句柄归零"，禁止跨任务 `vTaskDelete`+`free` |
| R10 | HW-QA-03 | `Json_data.c:1150`（`update_device_id_in_json`） | `cJSON_SetValuestring(item, "DEVICE_ID")` 把 valuestring 指向 `.rodata` 字面量，随后 `ReplaceItemInObject` 删除旧节点时对字面量 `free()` → 堆破坏/abort。每次设置设备编号必触发 | 删除该调用，改用 `cJSON_CreateString` + `ReplaceItemInObject` 并判空 |
| R11 | HW-QA-03 | `app_enc.c:689-705`（`server_calculate_secret`，置 NULL 三行被注释） | `mbedtls_*_free` + `free` 后指针仍非空；再次进入（BLE 重连/握手重放）→ 双重释放/堆破坏 | 取消置 NULL 的注释；入口 `if (srv_grp == NULL) return;` |
| R12 | HW-QA-01/SEC-02 | `udp_client.c:767`（`xQueueCreate(10,1024)`）vs `:715`（`recv_buf[400]`）；`:205` `buffer_size-1`；`:457/:751` `strcpy(server_ip, SERVER_IP)` | 队列按 1024 拷贝 400 字节栈缓冲 → 越界读 624 字节；`buffer_size==0` 下溢；外部配置长度不可控且未判空。**注：该模块未列入 `CMakeLists.txt`，属潜伏缺陷** | 队列项尺寸与缓冲统一；入口判 `buffer_size==0`；改 `snprintf` + 判空 |

---

## 三、High（26）

### Task（appTask.c）

| ID | 位置 | 问题 | 修复方向 |
|---|---|---|---|
| R13 | `:4249` | `agingcfg.devices[0].ProtoID = protoid;` 在 `device_count<=0 \|\| devices==NULL` 判空**之前**，无设备节点时先崩在解引用 | 校验前移 |
| R14 | `:1795` | `memcpy(cmd_buf, cmd->valuestring, sizeof-1)`：源串短于 31 字节时越界读（"ota" 等短命令必现） | 改 `snprintf` |
| R15 | `:4549/4993` | `Lost_Data_List` 分配未判空（`memset(NULL,0,1024)`），且任务被删除/完成时从不 free | 改 static + 全退出路径释放 |
| R16 | `:1647-1658` | `Onely_Set_DeviceId` 泄漏 `updated_json`（接口文档已写明调用方负责 free） | 补 `cJSON_free(updated_json)` |
| R17 | `:3536` vs `:3091/4506/5036` | `memset(&agingResumeState,0,...)` 把 `AgingJson` 指针清零而不 free → 三条主路径各泄漏整份工艺 JSON | 新增 `aging_resume_state_clear()` |
| R18 | `:5575/5366/5468` | `Upload_data_queue` 仅在 `IsRoot==0` 分支创建，根节点仍创建 mqtt_rx 任务 → `xQueueSend(NULL,...)` 崩溃 | 队列创建与判空 |
| R19 | `:3830/3998` | `RecBuf` 接收长度等于容量，接收侧不补 `'\0'`，随后按 C 串打印/解析 → 越界读 | 传 `sizeof-1` 并在接收后补 NUL |
| R20 | `:89/2224/2225/107/139/141/102` | `read_devices`/`CurentDevicenum`（数组下标）/`IsAgingDevice`/`CurrentAgingStep`/`agingDataAMode`/`Cannextstep` 跨任务读写无保护 | 统一互斥或命令队列串行化，标志加 `volatile` |
| R21 | `:3073/3076/4216/4221/4495/4639` | 跨任务 `vTaskDelete` 他人任务：被删任务可能持有 UART/Sqlite/BLE 互斥量 → 永久阻塞；私有堆全部泄漏 | 见 R9 |
| R22 | `:1736/1655/1812/2130/2892/3100/3564/4368/4514/4813/4928/5005/5057` | 返回值未处理：`Onely_Set_DeviceId`、`process_packet_AllConfig`、`Aging_Execute_ActionList`（动作失败仍推进工步）、`SelfRecovery_Write_*`、`query_db1_to_global` 等 | 逐点补检查；动作失败应中止工步 |
| R23 | `:4959-4963`、`:4975-4991` | 群控 `while(Cannextstep==0)` 与补发 `while(QueryLatest...==0)` 无超时无 volatile；空 `value_data` 记录导致 100ms 间隔无限循环 | 事件组 + 超时 + 空数据分支 + 轮数上限 |

### Tool / Protocol

| ID | 位置 | 问题 | 修复方向 |
|---|---|---|---|
| R24 | `app_enc.c:177-187 + 513-562` | `sign_data[64]` 未初始化，签名失败时不写数据 → 64 字节未初始化栈内容被加密外发；`rlen>32` 越界写 | 清零 + 返回值传播 + 长度校验 |
| R25 | `app_enc.c:438-473 / 507-509` | 重复握手泄漏 `srv_grp/srv_pri/srv_ctr_drbg`；错误分支对未 init 的 `entropy`/`pub` 调 free（UB） | 进入时释放旧对象；分段 cleanup |
| R26 | `app_enc.c:369-374` | 熵源尾块 `memcpy(output+offset,&seed,4)` 固定写 4 字节而 offset 只推进 `len%4` → 最多 3 字节堆溢出 | 尾块按字节拷贝 |
| R27 | `app_enc.c:241-245 + 655-706` | `server_calculate_secret` 返回 void，ECDH 失败仍 `return ESP_OK` 并置 `IsEnc=true` → 用全 0 密钥继续会话 | 改返回 `esp_err_t` |
| R28 | `app_enc.c:255-256` | 对二进制密文用 `strlen`；`sizeof(*buf2)/sizeof(buf2[0])` 恒为 1（逻辑错误） | 用显式长度；删除该表达式 |
| R29 | `app_enc.c:32/34/37/40` | `iv`/`SendPublicKey`/`publicKeyA1`/`shartKey` 全局被握手与加解密并发访问 | 互斥或封装为会话句柄 |
| R30 | `app_enc.c:353/397/487/542/577/631/670…` | mbedtls 关键返回值普遍未检查，失败仍按成功继续 | 逐点判断并提前返回 |
| R31 | `scpi_dynamic.c:468-475` | 解析失败时 `items[item_count]`（已 strdup 5 个字段）未被 `scpi_dynamic_free` 覆盖（它只遍历 `i < item_count`） | 错误出口先 `free_cmd_item(cmd)` |
| R32 | `modbus_cmd.c:271-281` | `byte_count=response[2]` 外部可控，未校验 `3+byte_count+2 <= response_len`（同文件 203/346 两处都有该校验）→ 越界读 | 补长度校验 |
| R33 | `modbus_json_parser.c:158-164`、`can_json_parser.c:199-205` | `json_get_int()` 强转 `uint16_t/uint8_t` 无范围校验：`70000→4464`、`-1→65535` → 读写错误寄存器 | 钳位到合法范围 |
| R34 | `aging_config.c:494-582 / 735-808` | `step->StepId` 分配后两个 free 函数都漏掉 → 每次解析 N 个步骤泄漏 N 块 | 补 `free(step->StepId)` |
| R35 | `aging_config.c:412` | `int64_t ProtoID` 用 32 位 `json_get_int` 读取被截断（已有 `json_get_ll` 未用）；`LastProtoID` 为 `int` 同样截断 | 改用 `json_get_ll`；`LastProtoID` 改 `int64_t` |

### Communication

| ID | 位置 | 问题 | 修复方向 |
|---|---|---|---|
| R36 | `Ble_control.c:673-704`（`ble_send_data`） | `uint8_t sendBuf[1024]`，`if (len > 500)` 校验在 **memcpy/加密之后**；加密输出比输入多补齐字节，`len≈1008` 即溢出；`len<0` 转 size_t 变巨大值；`pdata` 未判空 | 校验前移到任何拷贝/加密之前 |
| R37 | `mesh.c:379-388` | `char *print;` 未初始化，`asprintf` 失败仍 `ESP_LOGI(...,print)` + `free(print)` → 野指针（每 2 秒一次） | 初始化 NULL 并判断返回值 |
| R38 | `dual_net.c:646/715` | `esp_err_t ret = ESP_FAIL;` 之后再未赋值 → `dual_net_init()` 恒返回失败，每次上电写一条并不存在的故障日志（兼 HW-FMT-01） | 删除占位变量与误导日志，成功返回 `ESP_OK` |
| R39 | `mesh.c:980-987` | `int now = xTaskGetTickCount() * portTICK_PERIOD_MS` 约 24.8 天有符号溢出，父断连防抖失效 | 改 tick 域比较 + `TickType_t` |

### APP

| ID | 位置 | 问题 | 修复方向 |
|---|---|---|---|
| R40 | `mqtt_app.c:411-424` | `esp_mqtt_client_init` 未判空；`register_event`/`start` 失败直接 return，`s_client` 非 NULL 使幂等判断失效 → MQTT 永久不可用 + 句柄泄漏 | 判空 + 失败分支 `destroy` 并置 NULL |
| R41 | `mqtt_app.c:376/393` | 遗嘱 JSON `create_up_line_json()` 结果从不 free，每次启动/重连泄漏 | 由模块持有，destroy 后释放 |
| R42 | `GetProto.c:290-307` | `set_header` 死存储两次，最终 `cleanup` 返回值覆盖 `perform` 错误码 → HTTP 失败仍返回 `ESP_OK` | 保留 `perform` 的错误码 |
| R43 | `GetProto.c:558/564` | `read_response(client, buf, out_buffer_len - 1)`：`out_buffer_len` 无校验，0/负数转 size_t 变极大值 → 越界写 | 入口校验 `> 1` 且判空 |
| R44 | `GetProto.c:109-110` | `static char *output_buffer` / `static int output_len` 被 4 个 HTTP 入口共用 → 并发请求响应体交错拼接、可能重复释放 | 改挂到 `user_data`（`ota.c` 已是正确模式） |
| R45 | `GetProto.c:495-500` | `cJSON_CreateObject/CreateString` 未判空即 `AddItemToObject` | 逐项判空 |
| R46 | `mqtt_app.c:469/473` | `snprintf(topic, sizeof(topic), topic_name, DEVICE_ID)` 格式串来自函数参数（HW-SEC-04） | 保证只传字面量；检查写入长度 |
| R47 | `GetProto.c:298/387/566` | HTTP 状态码只打印不校验，4xx/5xx 被当成功 → 上层把错误页当协议解析 | 校验 2xx（对齐 `ota.c:283/433`） |

---

## 四、Medium（按类归集）

1. **HW-FUNC-01 函数过长（>50 行）**
   `Aging_Test_Task`（约 400+ 行）、`mesh_UartCommand`、`parse_jsonCommand_MQTT`、`parse_jsonCommand_HTTP`、`Init_ByNetwork_Flag`、`aging_command_json_parse`、`Data_Set/Get_Method`、`ProcessAgingStepData`、`app_task_init`；`SqLite.c:1308 InsertStructuredRecord`、`migrate_legacy_schema_to_v6`、`serialize_value_array_for_storage`；`mqtt_app.c:98 mqtt_event_handler_cb`（约 260 行）、`udp_test_task`（245）、`http_event_handler`（116）、`perform_http_ota`（141）；`Json_data.c:258/346/534`、`aging_config.c:494/649/735/850`、`scpi_dynamic.c:398`、`CanJson_Parse`、`ModbusJson_Parse`。
2. **HW-FUNC-02 嵌套 >4 层**：`Aging_Test_Task:4553-4965` 最深约 9 层；`parse_jsonCommand_MQTT` 约 6 层；`Json_data.c:590-618`、`udp_client.c:494-549`、`mqtt_app.c:255-332` 均 5 层。
3. **HW-MACRO-04 魔鬼数字**：`appTask.c:2227/2704`（`1024*8`）、`:1697-1728` 帧长 25/17/13/8；`SqLite.c:138/218/300/334`（4096/8 与 FATFS 分配单元强耦合）；`can_json_parser.c:342-360` 用字面量 8/6 而 `can_protocol_ext.h:33-34` 已定义宏；`modbus_cmd.c` 0x03/0x06/0x10、125/123 上限；`app_enc.c` 134/146/65/64；`ota.c:255/413/555`。
4. **HW-MACRO-01/NAME-04 宏**：`app_crc.c:3 reversal32(a,b,c,d)` 参数无括号、`(a<<24)` 对 int 移位 UB、宏名全小写（位于 `#if 0` 死代码内）。
5. **HW-HDR-04 头文件不自包含**：`appTask.h`（用 `bool`）、`log.h`（用 `esp_err_t`/`size_t`）、`mesh_netif.h`（零 include）、`mesh.h:48`（`SemaphoreHandle_t`）、`RS485.h`、`udp_client.h:120`、`mqtt_app.h`、`Json_data.h`、`SelfRecovery.h`。
6. **HW-HDR-03 无用 include**：`appTask.c:9/28/29/33/34/36/37`、`SqLite.c:10/16`、`mesh.c:16/18/22/24`、`Uart.c:10`、`RS485.c:11`、`Json_data.c:7`、`app_crc.c:1`、`aging_config.c:13`。
7. **HW-QA-04 switch 缺 default / if-else if 缺 else**：`switch(agingState):4563`、`readconfig:1174`、`parse_jsonCommand_MQTT:2961-3308`、`dual_net.c:417-481`、`udp_client.c:496/611/664`（`Network_Flag` 非 0/1 时 `while(1)` 无 delay 空转触发 TWDT）。
8. **HW-FUNC-05 / HW-QA-02 死代码与条件泄漏**：`mesh_UartCommand`（含栈溢出缺陷）、`app_RS485_data_handle`、`str_ends_with_local`、`Modbus_ParseResponseBasic`（含 R32）、`can_extended.c:838 can_ext_send_and_receive`、`udp_client.c` 整文件未编译且与 `SqLite.h` 类型冲突；条件泄漏：`mesh_netif.c:332/494/559`、`Ble_control.c:220-319`、`dual_net.c:531-621`、`mqtt_app.c:413/420`。
9. **HW-SEC-02/03 整数截断与符号转换**：`appTask.c:4811`（`uint16_t i → uint8_t CurrentAgingStep`）、`:5204`（`time_t → int`）、`SqLite.c:44`（`(int)now`，2038）、`:115`（`(short)`）、`sqllib1.c:38`（`INT_MIN` 取负 UB）、`can_extended.c` timeout 单位 ms/tick 混用、`app_crc.c:560/595`（`int len → uint16_t`）。
10. **HW-VAR-04 通讯字节序**：`modbus_cmd.c:410-415` 按主机序取字节（同文件 `BuildFrame_SafeCode`/`ExtractU64` 已用显式移位）；`can_protocol_ext.h:77-95 can_ext_transfer_t` 未 `packed` 且无显式序列化。
11. **HW-FUNC-04 返回值未检查（介质类）**：`log.c:26/31/259/274`（丢弃 `xSemaphoreTake`，mutex 为 NULL 时完全不加锁）、`log.c:141/178`（`fseek`）、`log.c:116/154`（`fflush/fclose`）、`SqLite.c:1414-1441`（全部 `sqlite3_bind_*`）、`SqLite.c:466`（`(void)sqlite3_close` 后仍 unlink）、`udp_client.c:132/143`（`inet_addr` 失败发往广播地址）、`esp_http_client_set_header` 系列。

其余共享变量无保护（medium）：`mesh.c:94 s_root_mac`、`mesh.c:392` 裸写路由表、`can_extended.c:19 Use_Send_Flag` / `RS485.c:517 Use_Write_Flag`、`Ble_control.c:502/531`（`find_device_by_address` 无锁、解锁后仍用 `target`）、`Json_data.c:870` 与 `appTask.c:3011` 对 `RecordId` 撕裂读写、`dual_net.c:109/783` 回调指针竞争。

---

## 五、Low（命名与注释，项目内部风格不统一，集中汇总）

- **HW-NAME-01 全局变量缺 `g_`**：`appTask.c` 的 `Aging_device/read_devices/read_device_count/CurentDevicenum/IsAgingDevice/agingcfg/agingState/CurrentAgingStep/DeviceNumber/Programld/Cannextstep/Mqtt_Log_Mode/agingDataAMode` 等 20+ 个；`SqLite.c:32 db1_name`、`spiffs_config.c:14 current_conf`、`mesh.c:45/58/59/60/64`（全局却用 `s_` 前缀，误导性最强）、`ConfigData.c` 全文件、`app_enc.c:19-40`、`udp_client.h:11`。
- **HW-NAME-02 static 变量缺 `s_`**：`Aging_device/read_devices/ScanComplete/Upload_data_queue`、`mesh.c:42-53`、`mesh_netif.c:58-62`、`can_extended.c:13-21`、`Ble_control.c:200`、`SelfRecovery.c:6`、`Json_data.c:11`。
- **HW-NAME-04 宏命名**：`HTTP_DATA_Cache`、`MQTT_DATA_Cache`、`WiFiNums`、`SPI_MOSIPIN`、`can_protocol_ext.h:48-52`、`app_crc.c:3 reversal32`。
- **拼写/可读性**：`Onely_Set_DeviceId`(Only)、`Programld`(ProgramId)、`CurentDevicenum`(Current)、`cfg_Chanel`(Channel)、`shartKey`(sharedKey)、`calcute_sum`、`noparentfountnum`、`app_DataUpload_Functiong`；`appTask.c:4776 int c`（非 i/j/k 单字符）。
- **HW-FMT-02 文件头缺失**：`appTask.c/.h`、`main.c`、`task_config.h`、`SqLite.c/.h`、`sqllib1.c/.h`、`spiffs_config.c/.h`、`log.c/.h`、`Json_data.c`、`app_enc.c`、`app_crc.c`、`app_mem.c`、`aging_config.c`、`ConfigData.c`、`Externaldevice.c`、`SelfRecovery.c`、`scpi_dynamic.c/.h`、`can_json_parser.c/.h`、`modbus_cmd.c/.h`、`modbus_json_parser.c/.h`、`mesh.h`、`dual_net.h`、`can_extended.h`、`Ble_control.h`。
- **HW-FMT-03 全局变量注释不足**：`CurentDevicenum`（做下标无取值约束）、`g_db1_result`（未标注"非线程安全"）、`current_conf`、`udp_start_state`。
- **HW-FMT-01 注释与代码矛盾**：`appTask.c:2697`（注释"每秒"实际 4000ms）、`:1198`（"现在不使用"但代码仍在）、`:2067`（临时方案未跟踪）；`mqtt_app.c:466`（注释"要用 %d"实际 `%s`）；`Uart.c:254`（"队列长度 10"实际 3）；`log.h:57`（`#endif //DATA_RECORD_H` 与保护宏不一致）。
- **附带（非规则条目）**：`app_task_init()`/`Mesh_cmd_info()`/`RandomSendPKey()` 空参数列表应写 `(void)`；`ota.c` 明文 HTTP 下载固件无签名校验；`GetProto.c:518` 注释掉 `crt_bundle_attach`；`mqtt_app.c:385`、`GetProto.c:53/227` 硬编码账号密码。

---

## 六、本次未提交改动专项（2 行 diff）

```c
double value = -1.0;
bool read_ok = Data_Get_Method(&Aging_device[0], ..., &value, 1);
if(read_ok)
if (value <= current_step->judging_conditions[i].value_num)
```

1. **修复方向正确但不完整**：`Data_Get_Method` 返回 true 时 `value` 未必被写入，`-1.0` 初值会被当作真实数据 —— 放电判 `value <= 阈值` 时 `-1.0` 恒真 → 工步立即误判结束。
2. **缺大括号**：`if(read_ok)` 无 `{}`，只控制紧随的一条语句；语义当前恰好正确，但后续插入一行即改变语义。
3. **失败路径无日志/退避/超时**：设备永久掉线时 `while(conditionflag)` 永久挂起。
4. **覆盖面不足**：仅 Discharge/Recharge 两处加固，`AgingDeviceCheck`（R8）与 Standing 分支未同步。

---

## 七、Open questions（需业务/硬件确认）

1. `main/APP/UDP/udp_client.c` 未列入 `main/CMakeLists.txt`，且 `:419` 用 `g_db1_result.IDNUM` 与 `SqLite.h` 的指针声明冲突 —— 废弃还是待启用？
2. `mesh_UartCommand`（`appTask.c:1199-1623`，含 `rble_connect` 栈溢出）无调用点：预留调试入口还是死代码？
3. 根节点（`IsRoot==1`）是否会收到 `start_aging`？（决定 R18 可达性）
4. esp-mqtt 是否深拷贝 `last_will.msg`？`esp_mqtt_event_t::error_handle` 是否保证非 NULL？（决定 R41 可直接 free 与否）
5. `RS485` 与 `CAN` 同时使用 GPIO1/GPIO2（`RS485.c:26-27` vs `can_extended.h:15-16`）：分时复用还是配置错误？
6. `mesh.h:48 SemaphoreHandle_t`、`mesh_free()` 对静态缓冲 free 的判定依赖 IDF 内部实现，需按实际 IDF 版本复核。
7. cJSON 源码不在工作区，R10"对字符串字面量执行 free"建议实测确认一次。

## 八、Assumptions

- 未编译、未烧录、未运行，全部结论为静态审查；严重度已按可达性下调，触发条件多为推断。
- `sdkconfig`/分区表未读，PSRAM 使能、64 位 `time_t`、栈大小等影响部分结论触发概率。
- 被审查文件调用的其它模块内部实现，仅按头文件签名与返回契约推断调用侧责任。
- 命名类问题因项目内部风格不统一，统一按 low 汇总而非逐条计分。

---

## 九、建议整改顺序

1. **第一批（内存安全/崩溃）**：R1、R2、R6、R7、R8、R10、R11、R12
2. **第二批（外部输入越界）**：R3、R4、R5、R32、R33、R36
3. **第三批（并发与生命周期）**：R9（生命周期串行化）、R20、R21、R44
4. **第四批（返回值与资源）**：R16、R17、R22、R40、R41、R42、R47
5. **第五批（可维护性）**：函数过长/嵌套、魔鬼数字、头文件自包含与冗余 include、死代码
6. **最后**：命名前缀与文件头注释（量大面广，建议单独排期一次性整改）
