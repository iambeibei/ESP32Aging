#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern char sn_buffer[20];
extern uint8_t IsRoot;
extern  char *SSID;
extern  char *WIFI_PS;
extern  char *SSID1;
extern  char *WIFI_PS1;
extern uint8_t Chanel;
extern  char *Mesh_ID;
extern  char *Mesh_PS;
extern  char *DEVICE_ID;
extern int UDP_Port;
extern  char *SERVER_IP;
extern int SERVER_UDP_Port;
extern char *IsRoot_R;
extern char *Chanel_R;
extern char *UDP_Port_R;
extern char *SERVER_UDP_Port_R;
extern  char *AgingNumber;
extern uint8_t MESH_ID[6];

extern uint8_t Network_Flag;


#ifdef __cplusplus
}
#endif