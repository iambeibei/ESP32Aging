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

/// @brief Mesh网络ID，必须是6字节的十六进制字符串，如 "11:22:33:44:55:66"
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

uint8_t MESH_ID[6]; // 6字节的Mesh ID，所有节点必须相同才能加入同一个网络






#pragma endregion




// 网络标志位
uint8_t Network_Flag = 0; // 0=未连接，1=已连接