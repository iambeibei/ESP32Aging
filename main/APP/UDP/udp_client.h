#ifndef UDP_CLIENT_H
#define UDP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t udp_start_state;

/**
 * UDP客户端配置结构体
 */
typedef struct {
    char server_ip[64];        // 服务器IP地址
    uint16_t server_port;      // 服务器端口
    uint16_t local_port;       // 本地端口(0表示自动分配)
    int timeout_sec;           // 接收超时时间(秒)
    bool use_ipv6;            // 是否使用IPv6
} udp_client_config_t;

typedef enum 
{
    HandShake,//握手
    HandShakeSuccess,//握手成功
    WaitStart,//开始命令
    SendDataFinish,//发送完成
    ReissueMode,//补发模式
    TestEnd,//测试结束
    LogUpload,//日志上传
    StartLogUpload,//开始日志上传
    Idle,//空闲
}UDP_Send_Mode;

/**
 * UDP客户端句柄
 */
typedef struct udp_client* udp_client_handle_t;

/**
 * 默认配置
 */
#define UDP_CLIENT_CONFIG_DEFAULT() { \
    .server_ip = "", \
    .server_port = 0, \
    .local_port = 0, \
    .timeout_sec = 10, \
    .use_ipv6 = false \
}


/**
 * 初始化UDP客户端
 * 
 * @param config 客户端配置
 * @return 客户端句柄,失败返回NULL
 */
udp_client_handle_t udp_client_init(const udp_client_config_t *config);



/**
 * 只发送数据(不等待接收)
 * 
 * @param handle 客户端句柄
 * @param data 要发送的数据
 * @param len 数据长度
 * @return 成功返回0,失败返回-1
 */
int udp_client_send(const uint8_t *data, size_t len);

/**
 * 只接收数据(需要先发送过)
 * 
 * @param handle 客户端句柄
 * @param buffer 接收缓冲区
 * @param buffer_size 缓冲区大小
 * @param timeout_ms 超时时间(毫秒)
 * @return 接收到的数据长度,失败返回-1
 */
int udp_client_receive(uint8_t *buffer, size_t buffer_size, int timeout_ms);

/**
 * 获取本地端口
 * 
 * @param handle 客户端句柄
 * @return 本地端口,失败返回0
 */
uint16_t udp_client_get_local_port(void);

/**
 * 获取服务器IP地址字符串
 * 
 * @param handle 客户端句柄
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return 成功返回true,失败返回false
 */
bool udp_client_get_server_ip(char *buffer, size_t buffer_size);

/**
 * 重新连接(关闭并重新创建socket)
 * 
 * @param handle 客户端句柄
 * @return 成功返回0,失败返回-1
 */
int udp_client_reconnect(void);

/**
 * 关闭UDP客户端
 * 
 * @param handle 客户端句柄
 */
void udp_client_deinit(void);

int get_total_records(void);

esp_err_t storage_UDP_Send_record(int index);

char *build_data_report_json(int idnum, int timestamp, float voltage, float current);

int send_data_report(int idnum, int timestamp, float voltage, float current, bool store_on_fail);

void handle_retransmission(const char *recv_buf);

int send_control_string(const char *str);


int app_read_UDP_data(char *buffer, size_t buffer_size, int timeout_ms);

int UDP_Init(void);


#ifdef __cplusplus
}
#endif

#endif /* UDP_CLIENT_H */
