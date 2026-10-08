/* Mesh IP Internal Networking Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_mesh.h"

/*******************************************************
 *                Macros
 *******************************************************/
/*
 * 注意：该宏在 mesh.h 中也有同名定义，两处必须保持一致。
 * 取值必须 >= "子节点数 + 1(根节点)"，50个子节点至少需要51，
 * 这里留余量取64。路由表不足会导致广播下行与单播目标解析静默失效。
 */
#define CONFIG_MESH_ROUTE_TABLE_SIZE 64
#define MAC_ADDR_LEN (6u)
#define MAC_ADDR_EQUAL(a, b) (0 == memcmp(a, b, MAC_ADDR_LEN))

/*******************************************************
 *                Type Definitions
 *******************************************************/
typedef void (mesh_raw_recv_cb_t)(mesh_addr_t *from, mesh_data_t *data);

/*******************************************************
 *                Function Declarations
 *******************************************************/

/**
 * @brief Initializes netifs in a default way before knowing if we are going to be a root
 *
 * @param cb callback receive function for mesh raw packets
 *
 * @return ESP_OK on success
 */
esp_err_t mesh_netifs_init(mesh_raw_recv_cb_t *cb);

/**
 * @brief Destroy the netifs and related structures
 *
 * @return ESP_OK on success
 */
esp_err_t mesh_netifs_destroy(void);

/**
 * @brief Start the mesh netifs based on the configuration (root/node)
 *
 * @return ESP_OK on success
 */
esp_err_t mesh_netifs_start(bool is_root);

/**
 * @brief Stop the netifs and reset to the default mode
 *
 * @return ESP_OK on success
 */
esp_err_t mesh_netifs_stop(void);

/**
 * @brief Start the netif for root AP
 *
 * Note: The AP netif needs to be started separately after root received
 * an IP address from the router so the DNS address could be used for dhcps
 *
 * @param is_root must be true, ignored otherwise
 * @param dns_addr DNS address to use in DHCP server running on roots AP
 * @param rebuild true时强制重建AP netif(网关真的变了才需要)；
 *                false时若AP netif已存在则原样保留，避免清掉DHCP租约与NAPT会话
 *
 * @return ESP_OK on success
 */
esp_err_t mesh_netif_start_root_ap(bool is_root, uint32_t dns_addr, bool rebuild);

/**
 * @brief 查询Root的mesh AP netif是否已经建立
 *
 * 用于在Root重新拿到外网IP时判断是否真的需要重建AP口，
 * 避免频繁重建把DHCP租约表和NAPT会话清空。
 *
 * @return true if root AP netif is up
 */
bool mesh_netif_root_ap_ready(void);

/**
 * @brief Returns MAC address of the station interface
 *
 * Used mainly for checking node addresses of the peers in routing table
 * to avoid sending data to oneself
 *
 * @return Pointer to MAC address
 */
uint8_t* mesh_netif_get_station_mac(void);
