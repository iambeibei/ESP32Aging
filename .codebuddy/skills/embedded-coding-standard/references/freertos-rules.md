# FreeRTOS 检测规则

基于 FreeRTOS 官方文档和 API 约束，转换为模型可直接匹配的检测模式。按 API 类别和运行时约束组织。

## ISR API 边界

### FRTOS-ISR-01: ISR 中使用了非 FromISR API

检测：中断处理函数（`*_IRQHandler`、`*_ISR`、`*_isr`、`*Callback` 被 ISR 调用的回调）中调用了不带 `FromISR` 后缀的 FreeRTOS API。

```c
// BAD
void USART1_IRQHandler(void) {
    xSemaphoreGive(sem);         // 应该用 xSemaphoreGiveFromISR
    xQueueSend(queue, &data, 0); // 应该用 xQueueSendFromISR
    vTaskDelay(1);               // ISR 中绝对不能 delay
    taskENTER_CRITICAL();        // 应该用 taskENTER_CRITICAL_FROM_ISR
}

// GOOD
void USART1_IRQHandler(void) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(sem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
```

非 FromISR API 列表（ISR 中禁用）：
- `xSemaphoreTake` / `xSemaphoreGive`（非 FromISR 版本）
- `xQueueSend` / `xQueueReceive`（非 FromISR 版本）
- `xEventGroupSetBits` / `xEventGroupWaitBits`（非 FromISR 版本）
- `vTaskDelay` / `vTaskDelayUntil`
- `vTaskSuspend` / `vTaskResume`（需用 `xTaskResumeFromISR`）
- `taskENTER_CRITICAL` / `taskEXIT_CRITICAL`（需用 `FROM_ISR` 变体）
- `pvPortMalloc` / `vPortFree`（除非分配器是 ISR-safe 的）

风险：可能触发 assert 失败、调度器损坏、HardFault。严重级别：critical。

### FRTOS-ISR-02: 缺少 yield 处理

检测：FromISR API 调用后没有检查 `pxHigherPriorityTaskWoken` 并条件 yield。

```c
// BAD
void DMA1_Channel1_IRQHandler(void) {
    BaseType_t woken;
    xSemaphoreGiveFromISR(dma_sem, &woken);
    // 缺少 portYIELD_FROM_ISR(woken)
    // 被唤醒的高优先级 task 要等到下次 tick 才能运行
}

// GOOD
void DMA1_Channel1_IRQHandler(void) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(dma_sem, &woken);
    portYIELD_FROM_ISR(woken);
}
```

注意：如果 ISR 中多次调用 FromISR API，用 OR 合并 woken 标志，最后统一 yield：

```c
BaseType_t woken = pdFALSE;
xSemaphoreGiveFromISR(sem1, &woken);
xQueueSendFromISR(queue, &data, &woken);  // 内部 OR
portYIELD_FROM_ISR(woken);
```

风险：高优先级 task 响应延迟一个 tick 周期。严重级别：medium。

### FRTOS-ISR-03: 中断优先级超出 FreeRTOS 管理范围

检测：使用 FromISR API 的中断，其优先级数值低于（即优先级高于）`configMAX_SYSCALL_INTERRUPT_PRIORITY`。

```c
// BAD — STM32 上 NVIC 优先级数值越小优先级越高
NVIC_SetPriority(USART1_IRQn, 1);  // 优先级 1
// configMAX_SYSCALL_INTERRUPT_PRIORITY = 5
// 这个 ISR 优先级高于 FreeRTOS 管理范围，调用 FromISR API 不安全

// GOOD
NVIC_SetPriority(USART1_IRQn, 6);  // 优先级 6，在 FreeRTOS 管理范围内
```

如何检测：在代码中查找 `NVIC_SetPriority` 或 `HAL_NVIC_SetPriority`，比对设置的优先级与 `configMAX_SYSCALL_INTERRUPT_PRIORITY`。

风险：破坏 FreeRTOS 临界区保护，导致内核数据结构损坏。严重级别：critical。

## 任务管理

### FRTOS-TASK-01: 任务栈大小不足

检测：`xTaskCreate` 的 `usStackDepth` 参数过小（注意单位是 word 不是 byte），尤其当 task 函数中有大局部变量、深调用链、printf 类函数。

```c
// BAD
xTaskCreate(sensor_task, "SENSOR", 64, NULL, 3, NULL);
// 64 words = 256 bytes，如果 task 里有 uint8_t buf[128] 就溢出了

// GOOD
xTaskCreate(sensor_task, "SENSOR", 256, NULL, 3, NULL);
// 256 words = 1024 bytes，给足余量
```

检查点：
- task 内最大局部变量总大小
- 调用链深度（尤其递归或 printf/sprintf）
- 是否启用了 `configCHECK_FOR_STACK_OVERFLOW`

风险：栈溢出覆盖相邻内存，行为不可预测。严重级别：critical。

### FRTOS-TASK-02: 任务参数生命周期问题

检测：传给 `xTaskCreate` 的 `pvParameters` 指向栈变量或临时对象。

```c
// BAD
void create_worker(uint8_t channel) {
    TaskConfig cfg = { .channel = channel, .rate = 1000 };
    xTaskCreate(worker_task, "WORK", 256, &cfg, 2, NULL);
}  // cfg 在栈上，worker_task 启动时可能已失效

// GOOD — 静态分配或堆分配
static TaskConfig cfg;  // 或 pvPortMalloc
cfg.channel = channel;
cfg.rate = 1000;
xTaskCreate(worker_task, "WORK", 256, &cfg, 2, NULL);
```

风险：新 task 读到已失效的参数。严重级别：critical。

### FRTOS-TASK-03: 使用 vTaskDelete 后未清理资源

检测：task 被删除但持有的 mutex、分配的内存、注册的回调未清理。

```c
// BAD
void worker_task(void *param) {
    uint8_t *buf = pvPortMalloc(512);
    xSemaphoreTake(mutex, portMAX_DELAY);
    // ... 工作 ...
    vTaskDelete(NULL);  // mutex 未释放，buf 未 free
}

// GOOD
void worker_task(void *param) {
    uint8_t *buf = pvPortMalloc(512);
    xSemaphoreTake(mutex, portMAX_DELAY);
    // ... 工作 ...
    xSemaphoreGive(mutex);
    vPortFree(buf);
    vTaskDelete(NULL);
}
```

风险：mutex 永远被持有（死锁）、内存泄漏。严重级别：high。

## 同步原语

### FRTOS-SYNC-01: 使用 binary semaphore 当 mutex 用

检测：binary semaphore 被用于互斥访问（不同 task 中 take 和 give），而不是用 mutex。

```c
// BAD — binary semaphore 没有优先级继承
SemaphoreHandle_t lock = xSemaphoreCreateBinary();
xSemaphoreGive(lock);
// Task A: xSemaphoreTake(lock, ...); access_resource(); xSemaphoreGive(lock);
// Task B: xSemaphoreTake(lock, ...); access_resource(); xSemaphoreGive(lock);

// GOOD — mutex 有优先级继承
SemaphoreHandle_t lock = xSemaphoreCreateMutex();
```

风险：无优先级继承导致优先级反转，高优先级 task 被无限期阻塞。严重级别：high。

### FRTOS-SYNC-02: Mutex 在 ISR 中使用

检测：在 ISR 中调用 `xSemaphoreTakeFromISR` 或 `xSemaphoreGiveFromISR` 且该 semaphore 是 mutex 类型。

```c
// BAD — mutex 不能在 ISR 中使用
void TIM2_IRQHandler(void) {
    BaseType_t woken;
    xSemaphoreTakeFromISR(data_mutex, &woken);  // mutex 不支持 FromISR
    shared_data++;
    xSemaphoreGiveFromISR(data_mutex, &woken);
}

// GOOD — 用 taskENTER_CRITICAL_FROM_ISR 或重新设计
void TIM2_IRQHandler(void) {
    UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
    shared_data++;
    taskEXIT_CRITICAL_FROM_ISR(saved);
}
```

风险：mutex 的优先级继承机制在 ISR 上下文中无意义，行为未定义。严重级别：critical。

### FRTOS-SYNC-03: 递归获取非递归 mutex

检测：同一 task 对 `xSemaphoreCreateMutex()` 创建的 mutex 连续 take 两次。

```c
// BAD
void outer(void) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    inner();  // inner 也 take 同一个 mutex → 死锁
    xSemaphoreGive(mutex);
}
void inner(void) {
    xSemaphoreTake(mutex, portMAX_DELAY);  // 永远等不到
    // ...
    xSemaphoreGive(mutex);
}

// GOOD — 使用递归 mutex
SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutex();
// 用 xSemaphoreTakeRecursive / xSemaphoreGiveRecursive
```

风险：自死锁，task 永远阻塞。严重级别：critical。

### FRTOS-SYNC-04: Task notification 丢失事件

检测：使用 `xTaskNotifyGive` / `ulTaskNotifyTake` 做事件通知，但 producer 可能在 consumer 未 take 时连续多次 give。

```c
// BAD — 如果 ISR 在 task 处理期间触发多次，只记住一次
void ISR_Handler(void) {
    BaseType_t woken;
    vTaskNotifyGiveFromISR(worker_handle, &woken);
    portYIELD_FROM_ISR(woken);
}
void worker_task(void *param) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // 清零计数
        process_one_item();  // 如果期间来了 3 次中断，丢 2 次
    }
}

// GOOD — 用 counting semaphore 或循环处理到队列空
void worker_task(void *param) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (items_available()) {  // 处理所有待处理项
            process_one_item();
        }
    }
}
```

风险：事件丢失，数据积压或丢弃。严重级别：high。

### FRTOS-SYNC-05: Queue 满时的静默丢弃

检测：`xQueueSend` / `xQueueSendFromISR` 返回 `errQUEUE_FULL` 但未处理。

```c
// BAD
void ADC_IRQHandler(void) {
    BaseType_t woken;
    uint16_t sample = ADC1->DR;
    xQueueSendFromISR(adc_queue, &sample, &woken);  // 满了就丢
    portYIELD_FROM_ISR(woken);
}

// GOOD
void ADC_IRQHandler(void) {
    BaseType_t woken;
    uint16_t sample = ADC1->DR;
    if (xQueueSendFromISR(adc_queue, &sample, &woken) == errQUEUE_FULL) {
        overrun_count++;  // 至少记录丢失
    }
    portYIELD_FROM_ISR(woken);
}
```

风险：数据静默丢失，难以诊断。严重级别：medium（取决于数据重要性）。

## 临界区与调度

### FRTOS-CRIT-01: 临界区内调用阻塞 API

检测：在 `taskENTER_CRITICAL` / `taskEXIT_CRITICAL` 之间调用可能阻塞的 API。

```c
// BAD
taskENTER_CRITICAL();
xSemaphoreTake(mutex, portMAX_DELAY);  // 可能阻塞，但调度器被禁用
taskEXIT_CRITICAL();

// BAD
taskENTER_CRITICAL();
vTaskDelay(10);  // 调度器被禁用，delay 不会生效
taskEXIT_CRITICAL();
```

临界区内禁止调用的 API：
- 任何带 timeout 且可能阻塞的 API（`xSemaphoreTake`、`xQueueReceive`、`xEventGroupWaitBits`）
- `vTaskDelay` / `vTaskDelayUntil`
- `vTaskSuspend`

风险：调度器被禁用时阻塞 = 系统挂死。严重级别：critical。

### FRTOS-CRIT-02: 临界区过长

检测：`taskENTER_CRITICAL` 和 `taskEXIT_CRITICAL` 之间包含耗时操作（循环、外设访问、memcpy 大块数据）。

```c
// BAD
taskENTER_CRITICAL();
for (int i = 0; i < 1024; i++) {
    dst[i] = src[i];  // 长时间关中断
}
taskEXIT_CRITICAL();

// GOOD — 最小化临界区
taskENTER_CRITICAL();
uint32_t snapshot_idx = shared_index;
taskEXIT_CRITICAL();
// 在临界区外做耗时操作
memcpy(dst, &src[snapshot_idx], len);
```

风险：中断延迟增大，影响实时性，可能导致数据丢失。严重级别：high。

### FRTOS-CRIT-03: vTaskSuspendAll 期间调用 FromISR API

检测：在 `vTaskSuspendAll` / `xTaskResumeAll` 之间，ISR 调用 FromISR API 试图唤醒 task。

说明：`vTaskSuspendAll` 只暂停调度器，不关中断。ISR 仍会执行，FromISR API 仍可调用，但唤醒的 task 切换会被推迟到 `xTaskResumeAll`。这本身不是错误，但如果代码依赖 ISR 唤醒后 task 立即运行，会出错。

风险：task 响应延迟。严重级别：medium。

## 定时器

### FRTOS-TMR-01: Timer callback 做重工作

检测：software timer callback 中包含阻塞操作、长循环、外设访问。

```c
// BAD
void heartbeat_timer_cb(TimerHandle_t timer) {
    uint8_t buf[128];
    format_heartbeat(buf);
    HAL_UART_Transmit(&huart, buf, 128, 1000);  // 阻塞最多 1 秒
}

// GOOD — 通知 task 去做
void heartbeat_timer_cb(TimerHandle_t timer) {
    xTaskNotifyGive(heartbeat_task_handle);
}
```

说明：所有 software timer callback 在同一个 timer daemon task 中执行。一个 callback 阻塞会影响所有 timer。

风险：所有 software timer 被阻塞。严重级别：high。

### FRTOS-TMR-02: Timer 周期单位错误

检测：`xTimerCreate` 的 `xTimerPeriodInTicks` 参数直接传毫秒值而没有用 `pdMS_TO_TICKS()`。

```c
// BAD — 如果 configTICK_RATE_HZ = 1000，碰巧正确
//        如果 configTICK_RATE_HZ = 100，实际周期是 10 秒而不是 1 秒
TimerHandle_t timer = xTimerCreate("HB", 1000, pdTRUE, NULL, heartbeat_cb);

// GOOD
TimerHandle_t timer = xTimerCreate("HB", pdMS_TO_TICKS(1000), pdTRUE, NULL, heartbeat_cb);
```

同样适用于 `vTaskDelay`、`xQueueReceive` 等所有接受 tick 参数的 API。

风险：超时时间错误，可能过长（系统响应慢）或过短（忙等浪费 CPU）。严重级别：high。

## 内存管理

### FRTOS-MEM-01: 在不支持 free 的 heap 实现中调用 vPortFree

检测：项目使用 `heap_1.c`（只分配不释放）但代码中调用了 `vPortFree`。

```c
// BAD（使用 heap_1.c 时）
void *buf = pvPortMalloc(128);
// ... 使用 ...
vPortFree(buf);  // heap_1 的 vPortFree 是空函数，内存永远不回收
```

如何检测：检查项目链接了哪个 heap 实现（heap_1 到 heap_5）。

风险：开发者以为内存被释放但实际没有，长期运行后耗尽。严重级别：medium。

### FRTOS-MEM-02: pvPortMalloc 返回值未检查

检测：`pvPortMalloc` 返回值直接使用，未检查 NULL。

```c
// BAD
uint8_t *buf = pvPortMalloc(size);
memset(buf, 0, size);  // buf 可能为 NULL

// GOOD
uint8_t *buf = pvPortMalloc(size);
if (buf == NULL) {
    handle_oom();
    return;
}
memset(buf, 0, size);
```

风险：NULL 解引用，HardFault。严重级别：high。

### FRTOS-MEM-03: ISR 中动态分配内存

检测：ISR 中调用 `pvPortMalloc` 或 `vPortFree`。

```c
// BAD
void UART_IRQHandler(void) {
    uint8_t *buf = pvPortMalloc(rx_len);  // 不确定是否 ISR-safe
    // heap_1/2/4 不是 ISR-safe，heap_3 取决于 malloc 实现
}
```

说明：大多数 FreeRTOS heap 实现使用 `vTaskSuspendAll` 保护，ISR 中调用行为未定义。

风险：heap 数据结构损坏。严重级别：critical。
