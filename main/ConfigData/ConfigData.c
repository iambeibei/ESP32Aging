#include "ConfigData.h"
#include "Externaldevice.h"

#pragma region 配置相关全局变量
char sn_buffer[20] = "AC702336910071810";

/// @brief 是否是根节点
uint8_t IsRoot = 1;

/// @brief WiFi通道
uint8_t Chanel = 1;

/// @brief 本地UDP端口
int UDP_Port = 0;

/// @brief 服务器UDP端口
int SERVER_UDP_Port = 1883;

/// @brief WIFI名称
char *SSID=NULL;
char *SSID1=NULL;

/// @brief WIFI密码
char *WIFI_PS=NULL;
char *WIFI_PS1=NULL;

/// @brief Mesh-Lite 网络ID，两位十六进制字符串，如 "66"
char *Mesh_ID=NULL;

/// @brief Mesh网络密码
char *Mesh_PS=NULL;

/// @brief DeviceID
char *DEVICE_ID=NULL;

/// @brief 服务器IP地址
char *SERVER_IP=NULL;

/// @brief 老化架号
char *AgingNumber=NULL;

// 读取配置时的字符串变量，string类型的配置需要用这个变量来存储

char *IsRoot_R=NULL;
char *Chanel_R= NULL;
char *UDP_Port_R=NULL;
char *SERVER_UDP_Port_R=NULL;






#pragma endregion




// 网络标志位
volatile uint8_t Network_Flag = 0; // 0=本机无 STA IP，1=本机已取得 STA IP
