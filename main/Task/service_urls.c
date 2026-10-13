#include "service_urls.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ConfigData.h"

/*
 * URL 模板填充实现。
 *
 * SERVER_IP 由 readconfig()（appTask.c）从 baseconfig.json 的 SERVER_IP 键
 * 写入，指向 appTask.c 内的静态缓冲，进程生命周期内有效；本模块只读取，
 * 不保存引用，调用方提供的 out 缓冲区即结果载体，无内部共享状态。
 */

/* readconfig() 对缺失 SERVER_IP 键使用的默认哨兵值（appTask.c 同名逻辑）。 */
#define LG_URL_SERVER_IP_UNSET "default"

/*
 * 防呆：模板必须且只含一个 %s（IP 占位）。
 * 含第二个转换说明时 snprintf 会因缺少实参读取垃圾 vararg（未定义行为）；
 * 不含 %s 时 IP 不会被替换，产出静默错误的 URL。
 */
static bool url_fmt_has_single_ip_placeholder(const char *url_fmt)
{
    bool found = false;
    for (const char *p = url_fmt; *p != '\0'; p++) {
        if (*p != '%') {
            continue;
        }
        if (p[1] != 's') {
            return false;
        }
        if (found) {
            return false;
        }
        found = true;
        p++; /* 跳过已确认的 's'，由 for 的 p++ 继续 */
    }
    return found;
}

esp_err_t lg_url_build(const char *url_fmt, char *out, size_t out_size)
{
    if (url_fmt == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!url_fmt_has_single_ip_placeholder(url_fmt)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (SERVER_IP == NULL || SERVER_IP[0] == '\0' ||
        strcmp(SERVER_IP, LG_URL_SERVER_IP_UNSET) == 0) {
        /* 未配置或配置缺失：与 MQTT 侧同样表现为连不上，此处明确报错不静默降级。 */
        return ESP_ERR_INVALID_STATE;
    }

    int written = snprintf(out, out_size, url_fmt, SERVER_IP);
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}
