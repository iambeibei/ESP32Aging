#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "appTask.h"
#include "app_mem.h"


void app_main(void)
{
    /* PSRAM 已由 SDK 启动初始化；早于数据库、配置 JSON 及所有业务任务。 */
    app_memory_init();
    app_task_init();

}
