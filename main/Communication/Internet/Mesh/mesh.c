#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_bridge.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh_lite.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "ConfigData.h"
#include "mesh.h"
#include "dual_net.h"
#include "task_config.h"


#define MESH_MAX_LEVEL 6
#define MESH_MAX_CHILDREN 5
#define FAILOVER_LOST_IP_MS 30000
#define FAILOVER_SCAN_INTERVAL_MS 60000
#define FAILOVER_RETRY_COOLDOWN_MS 300000
#define FAILOVER_RTC_MAGIC 0x4D4C4657U

static const char *TAG = "mesh_lite_app";
static char s_softap_prefix[16];
static char s_softap_ssid[33];
/* Returned by lg_mesh_get_ssid_by_mac(); has to outlive the callback. */
static uint8_t s_parent_ssid[33];
static uint8_t s_uplink_index;
static volatile bool s_scan_requested;
static volatile bool s_scan_active;
static volatile bool s_restart_for_failover;
static volatile bool s_trial_success;
static bool s_trial_active;
static bool s_sntp_started;
static RTC_NOINIT_ATTR uint32_t s_failover_magic;
static RTC_NOINIT_ATTR volatile uint32_t s_failover_attempts;
static RTC_NOINIT_ATTR uint32_t s_trial_uplink_index;

static bool parse_mesh_id(const char *value, uint8_t *id)
{
    if (!value || strlen(value) != 2 || !isxdigit((unsigned char)value[0]) ||
        !isxdigit((unsigned char)value[1])) {
        return false;
    }
    unsigned int parsed;
    if (sscanf(value, "%2x", &parsed) != 1 || parsed == 0 || parsed > 0xff) {
        return false;
    }
    *id = (uint8_t)parsed;
    return true;
}

static bool valid_router(const char *ssid, const char *password)
{
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    size_t pass_len = password ? strlen(password) : 0;
    /* IoT-Bridge logs this buffer as a C string; keep room for a terminator. */
    return ssid_len > 0 && ssid_len <= 31 && strcmp(ssid, "default") != 0 &&
           pass_len >= 8 && pass_len <= 63 && strcmp(password, "default") != 0;
}

static uint8_t load_uplink_index(void)
{
    nvs_handle_t handle;
    uint8_t index = 0;
    if (nvs_open("mesh_net", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, "uplink", &index) != ESP_OK || index > 1) index = 0;
        nvs_close(handle);
    }
    return index;
}

static esp_err_t save_uplink_index(uint8_t index)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("mesh_net", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "uplink", index);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void sntp_sync_task(void *arg)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        setenv("TZ", "CST-8", 1);
        tzset();
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "SNTP sync: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "SNTP init: %s", esp_err_to_name(err));
        s_sntp_started = false;
    }
    vTaskDelete(NULL);
}

static void network_event_handler(void *arg, esp_event_base_t base,
                                  int32_t event_id, void *event_data)
{
    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        if (IsRoot && s_trial_active) {
            ESP_LOGI(TAG, "Alternate uplink obtained IP " IPSTR, IP2STR(&event->ip_info.ip));
            s_trial_success = true;
            return;
        }
        /*
         * Network_Flag 的写入权按角色划分，避免两个写者竞争同一全局量：
         * 根节点由 dual_net 综合「以太网 + WiFi」裁决（以太网可用时不会被判为断网）；
         * 子节点上行由 mesh 父节点提供，dual_net 不运行，沿用原判据。
         */
        if (IsRoot == 1)
        {
            dual_net_arbitrate_network_flag();
        }
        else
        {
            Network_Flag = 1;
        }
        s_failover_attempts = 0;
        ESP_LOGI(TAG, "STA IP " IPSTR ", level=%u, role=%s",
                 IP2STR(&event->ip_info.ip), esp_mesh_lite_get_level(),
                 IsRoot ? "root" : "node");
        if (!s_sntp_started) {
            if (xTaskCreatePinnedToCore(sntp_sync_task, "sntp_sync", LG_STACK_SNTP,
                                        NULL, LG_PRIO_SNTP, NULL, LG_APP_CPU_CORE) == pdPASS) {
                s_sntp_started = true;
            }
        }
    } else if ((base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) ||
               (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)) {
        /* 与上面 STA_GOT_IP 对称：根节点交给 dual_net 裁决，子节点沿用原判据 */
        if (IsRoot == 1)
        {
            dual_net_arbitrate_network_flag();
        }
        else
        {
            Network_Flag = 0;
        }
        if (s_trial_active) s_trial_success = false;
        ESP_LOGW(TAG, "STA uplink lost; local Mesh-Lite remains active");
    }
}

static void scan_start_cb(void)
{
    if (s_scan_requested) s_scan_active = true;
}

static void scan_end_cb(void)
{
    s_scan_requested = false;
    s_scan_active = false;
}

static void scan_done_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    if (!s_scan_active) return;
    if (Network_Flag || !IsRoot) {
        scan_end_cb();
        return;
    }
    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || count == 0) {
        scan_end_cb();
        return;
    }
    wifi_ap_record_t *records = calloc(count, sizeof(*records));
    if (!records) {
        scan_end_cb();
        return;
    }
    const char *other_ssid = s_uplink_index == 0 ? SSID1 : SSID;
    if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
        for (uint16_t i = 0; i < count; ++i) {
            if (strcmp((const char *)records[i].ssid, other_ssid) == 0) {
                ESP_LOGW(TAG, "Alternate SSID found; testing it after restart");
                s_restart_for_failover = true;
                break;
            }
        }
    }
    free(records);
    scan_end_cb();
}

static void root_failover_task(void *arg)
{
    TickType_t lost_since = 0;
    TickType_t last_scan = 0;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        /*
         * 以太网守卫：上行已由有线承担时，WiFi STA 没有 IP 属于正常情况。
         * 此时不应按「上行故障」处理，否则会扫描备用路由器并 esp_restart()，
         * 导致插着网线的根节点被无谓重启（本任务只在根节点创建）。
         */
        if (dual_net_is_eth_uplink_ready()) {
            if (lost_since != 0 || s_scan_requested) {
                ESP_LOGI(TAG, "以太网上行可用，暂停主备路由器切换（WiFi 侧无 IP 属正常）");
            }
            lost_since = 0;
            s_scan_requested = false;
            s_restart_for_failover = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (s_trial_active) {
            if (s_trial_success) {
                esp_err_t err = save_uplink_index(s_uplink_index);
                if (err == ESP_OK) {
                    s_trial_uplink_index = UINT32_MAX;
                    ESP_LOGI(TAG, "Alternate uplink confirmed; restarting with saved credentials");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
                ESP_LOGE(TAG, "Failed to save confirmed uplink: %s", esp_err_to_name(err));
            } else {
                if (lost_since == 0) lost_since = now;
                if ((now - lost_since) >= pdMS_TO_TICKS(FAILOVER_LOST_IP_MS)) {
                    s_trial_uplink_index = UINT32_MAX;
                    ESP_LOGW(TAG, "Alternate uplink did not obtain IP; returning to saved uplink");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (Network_Flag) {
            lost_since = 0;
        } else {
            if (lost_since == 0) lost_since = now;
            if (s_failover_attempts >= 2 &&
                (now - lost_since) >= pdMS_TO_TICKS(FAILOVER_RETRY_COOLDOWN_MS)) {
                s_failover_attempts = 0;
            }
            const char *other_ssid = s_uplink_index == 0 ? SSID1 : SSID;
            const char *other_password = s_uplink_index == 0 ? WIFI_PS1 : WIFI_PS;
            if (s_failover_attempts < 2 && valid_router(other_ssid, other_password) &&
                (now - lost_since) >= pdMS_TO_TICKS(FAILOVER_LOST_IP_MS) &&
                (last_scan == 0 || (now - last_scan) >= pdMS_TO_TICKS(FAILOVER_SCAN_INTERVAL_MS)) &&
                !s_scan_requested) {
                s_scan_requested = true;
                last_scan = now;
                esp_err_t err = esp_mesh_lite_wifi_scan_start(NULL, pdMS_TO_TICKS(3000));
                if (err != ESP_OK) {
                    s_scan_requested = false;
                    ESP_LOGW(TAG, "Failover scan: %s", esp_err_to_name(err));
                }
            }
        }
        if (s_restart_for_failover && Network_Flag) {
            s_restart_for_failover = false;
        }
        if (s_restart_for_failover) {
            uint8_t next = s_uplink_index == 0 ? 1 : 0;
            s_trial_uplink_index = next;
            ++s_failover_attempts;
            ESP_LOGW(TAG, "Restarting to verify uplink %u", next);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*
 * Fallback used when Mesh-Lite cannot derive the parent SSID from the scan record.
 * Registered through esp_mesh_lite_get_ssid_by_mac_cb_register().
 *
 * Every node publishes the SoftAP name built by mesh_init_Custom(), so the parent
 * name can always be reconstructed from the candidate BSSID. This must mirror that
 * construction exactly, including the MAC suffix mode, otherwise the node would
 * hand an unknown SSID to esp_wifi_connect().
 *
 * whitelist=false: the callback answers for any BSSID that already passed the
 * mesh_id/vendor filter, and is only consulted when the SSID cannot be read from
 * the scan result.
 */
static const uint8_t *lg_mesh_get_ssid_by_mac(const uint8_t *bssid)
{
    if (!bssid) {
        return NULL;
    }

    memset(s_parent_ssid, 0, sizeof(s_parent_ssid));
#ifdef CONFIG_BRIDGE_SOFTAP_SSID_END_WITH_THE_MAC
    snprintf((char *)s_parent_ssid, sizeof(s_parent_ssid), "%s_%02x%02x%02x",
             s_softap_prefix, bssid[3], bssid[4], bssid[5]);
#else
    snprintf((char *)s_parent_ssid, sizeof(s_parent_ssid), "%s", s_softap_prefix);
#endif
    ESP_LOGD(TAG, "ssid by mac " MACSTR ": %s", MAC2STR(bssid), (char *)s_parent_ssid);
    return s_parent_ssid;
}

/*
 * Diagnostic anchor for the join failure: shows what the STA interface actually
 * holds. Once a parent has been selected this must no longer be empty.
 */
static void lg_mesh_dump_sta_config(const char *when)
{
    wifi_config_t cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "[%s] STA config read failed", when);
        return;
    }
    ESP_LOGD(TAG, "[%s] sta ssid=\"%s\" len=%u bssid_set=%d password_set=%d level=%u",
             when, (const char *)cfg.sta.ssid, (unsigned)strlen((const char *)cfg.sta.ssid),
             cfg.sta.bssid_set, cfg.sta.password[0] != '\0',
             (unsigned)esp_mesh_lite_get_level());
}

static void lg_mesh_node_scan_done_handler(void *arg, esp_event_base_t base,
                                           int32_t event_id, void *event_data)
{
    lg_mesh_dump_sta_config("node scan done");
}

/*
 * Diagnostic helper for the mesh join failure.
 *
 * The parent-selection and esp_wifi_connect decision tree (the "Test Log ..."
 * lines) lives in the closed-source esp-mesh-lite library. Those logs do not go
 * through ESP_LOGD directly: they use ESP_MESH_LITE_LOGx(), which is gated by the
 * fixed ESP_MESH_LITE_LOG_LEVEL=ESP_LOG_DEBUG in esp_mesh_lite_log.h and finally
 * reaches esp_log_writev(). Since esp_log_writev() filters by the runtime level of
 * the tag, esp_log_level_set() alone is enough to reveal them - DEBUG is the
 * effective maximum for that library, so VERBOSE buys nothing.
 *
 * Set LG_MESH_DIAG_LOG to 0 to disable, or to 2 to also promote the per-packet
 * communication and espnow tags (very noisy, only for targeted debugging).
 */
#ifndef LG_MESH_DIAG_LOG
#define LG_MESH_DIAG_LOG 1
#endif

#if LG_MESH_DIAG_LOG
static const char *const s_mesh_diag_tags[] = {
    "mesh_lite_app", /* TAG itself; promoting it exposes this file's own ESP_LOGD lines */
    "vendor_ie",
    "Mesh-Lite",
#if LG_MESH_DIAG_LOG > 1
    "ESP_Mesh_Lite_Comm",
    "mesh-lite-espnow",
#endif
};

static void mesh_lite_diag_log_enable(void)
{
    for (size_t i = 0; i < sizeof(s_mesh_diag_tags) / sizeof(s_mesh_diag_tags[0]); ++i) {
        esp_log_level_set(s_mesh_diag_tags[i], ESP_LOG_DEBUG);
    }
    ESP_LOGI(TAG, "Mesh-Lite diagnostic logging enabled (level=%d)", LG_MESH_DIAG_LOG);
}
#else
static void mesh_lite_diag_log_enable(void)
{
}
#endif

esp_err_t mesh_init_Custom(void)
{
    uint8_t mesh_id;
    if (IsRoot > 1) {
        ESP_LOGE(TAG, "IsRoot must be 0 or 1");
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_mesh_id(Mesh_ID, &mesh_id)) {
        ESP_LOGE(TAG, "Mesh_ID must be exactly two hex digits from 01 to FF");
        return ESP_ERR_INVALID_ARG;
    }
    if (!Mesh_PS || strlen(Mesh_PS) < 8 || strlen(Mesh_PS) > 63) {
        ESP_LOGE(TAG, "Mesh_PS must contain 8-63 characters");
        return ESP_ERR_INVALID_ARG;
    }
    if (IsRoot && !valid_router(SSID, WIFI_PS)) {
        ESP_LOGE(TAG, "Root SSID/WIFI_PS is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (IsRoot && valid_router(SSID1, WIFI_PS1) && strcmp(SSID, SSID1) == 0) {
        ESP_LOGE(TAG, "SSID and SSID1 must differ for failover");
        return ESP_ERR_INVALID_ARG;
    }

    if (esp_reset_reason() != ESP_RST_SW || s_failover_magic != FAILOVER_RTC_MAGIC) {
        s_failover_magic = FAILOVER_RTC_MAGIC;
        s_failover_attempts = 0;
        s_trial_uplink_index = UINT32_MAX;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop init failed");
    esp_bridge_create_all_netif();

    s_uplink_index = IsRoot ? load_uplink_index() : 0;
    if (IsRoot && s_trial_uplink_index <= 1) {
        s_uplink_index = (uint8_t)s_trial_uplink_index;
        s_trial_active = true;
    }
    if (s_uplink_index == 1 && !valid_router(SSID1, WIFI_PS1)) {
        s_uplink_index = 0;
        s_trial_uplink_index = UINT32_MAX;
        s_trial_active = false;
    }
    wifi_config_t sta_config = {0};
    if (IsRoot) {
        const char *ssid = s_uplink_index == 0 ? SSID : SSID1;
        const char *password = s_uplink_index == 0 ? WIFI_PS : WIFI_PS1;
        memcpy(sta_config.sta.ssid, ssid, strlen(ssid));
        memcpy(sta_config.sta.password, password, strlen(password));
    }
    ESP_RETURN_ON_ERROR(esp_bridge_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "STA config failed");

    uint8_t ap_mac[6];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_AP, ap_mac), TAG, "AP MAC read failed");
    snprintf(s_softap_prefix, sizeof(s_softap_prefix), "LGMesh_%02X", mesh_id);
#ifdef CONFIG_BRIDGE_SOFTAP_SSID_END_WITH_THE_MAC
    snprintf(s_softap_ssid, sizeof(s_softap_ssid), "LGMesh_%02X_%02x%02x%02x",
             mesh_id, ap_mac[3], ap_mac[4], ap_mac[5]);
#else
    snprintf(s_softap_ssid, sizeof(s_softap_ssid), "%s", s_softap_prefix);
#endif
    wifi_config_t ap_config = {0};
    /* Match the SSID format used by IoT-Bridge and Mesh-Lite for this build. */
    memcpy(ap_config.ap.ssid, s_softap_prefix, strlen(s_softap_prefix));
    memcpy(ap_config.ap.password, Mesh_PS, strlen(Mesh_PS));
    ap_config.ap.max_connection = MESH_MAX_CHILDREN;
    ESP_RETURN_ON_ERROR(esp_bridge_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "AP config failed");

    /* Run before esp_mesh_lite_init() so the vendor_ie startup logs are captured. */
    mesh_lite_diag_log_enable();

    esp_mesh_lite_config_t config = ESP_MESH_LITE_DEFAULT_INIT();
    config.mesh_id = mesh_id;
    config.max_level = MESH_MAX_LEVEL;
    config.max_connect_number = MESH_MAX_CHILDREN;
    config.join_mesh_ignore_router_status = true;
    config.join_mesh_without_configured_wifi = !IsRoot;
    config.softap_ssid = s_softap_prefix;
    config.softap_password = Mesh_PS;
    esp_mesh_lite_init(&config);
    if (esp_mesh_lite_get_mesh_id() != mesh_id) {
        /* A previous Mesh-Lite run may have persisted another ID in NVS. */
        esp_mesh_lite_set_mesh_id(mesh_id, true);
    }
    ESP_RETURN_ON_ERROR(esp_mesh_lite_set_softap_info(s_softap_ssid, Mesh_PS), TAG, "SoftAP info failed");
    wifi_config_t applied_ap = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_AP, &applied_ap), TAG, "AP config read failed");
    if (strncmp((const char *)applied_ap.ap.ssid, s_softap_ssid,
                sizeof(applied_ap.ap.ssid)) != 0) {
        ESP_LOGE(TAG, "SoftAP SSID mismatch: expected=%s, actual=%.*s",
                 s_softap_ssid, (int)sizeof(applied_ap.ap.ssid), applied_ap.ap.ssid);
        return ESP_ERR_INVALID_STATE;
    }
    if (IsRoot) {
        ESP_RETURN_ON_ERROR(esp_mesh_lite_set_allowed_level(1), TAG, "root level failed");
    } else {
        ESP_RETURN_ON_ERROR(esp_mesh_lite_set_disallowed_level(1), TAG, "node level failed");
    }

    /*
     * Give Mesh-Lite a way to resolve the parent SSID from the BSSID even when the
     * scan record cannot supply it. Without this, the STA keeps an empty SSID and
     * esp_wifi_connect() only answers "Haven't to connect to a suitable AP now!".
     */
    ESP_RETURN_ON_ERROR(esp_mesh_lite_get_ssid_by_mac_cb_register(lg_mesh_get_ssid_by_mac, false),
                        TAG, "ssid-by-mac callback failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    network_event_handler, NULL), TAG, "IP handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                    network_event_handler, NULL), TAG, "IP handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                    network_event_handler, NULL), TAG, "WiFi handler failed");
    if (IsRoot && valid_router(SSID1, WIFI_PS1)) {
        static esp_mesh_lite_scan_cb_t scan_callbacks = {
            .scan_start_cb = scan_start_cb,
            .scan_end_cb = scan_end_cb,
        };
        ESP_RETURN_ON_ERROR(esp_mesh_lite_scan_cb_register(&scan_callbacks), TAG, "scan callbacks failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                                        scan_done_handler, NULL), TAG, "scan handler failed");
    }
    if (!IsRoot) {
        /* Anchor: shows whether Mesh-Lite ever writes the parent into the STA config. */
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                                        lg_mesh_node_scan_done_handler, NULL),
                            TAG, "node scan handler failed");
    }

    esp_mesh_lite_start();
    lg_mesh_dump_sta_config("after start");
    if (IsRoot && valid_router(SSID1, WIFI_PS1)) {
        if (xTaskCreatePinnedToCore(root_failover_task, "mesh_failover", LG_STACK_MESH_FAILOVER,
                                    NULL, LG_PRIO_MESH_FAILOVER,
                                    NULL, LG_APP_CPU_CORE) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create failover task");
        }
    }
    ESP_LOGI(TAG, "Mesh-Lite started: ID=%02X, role=%s, AP=%s, uplink=%u",
             mesh_id, IsRoot ? "root" : "node", s_softap_ssid, s_uplink_index);
    return ESP_OK;
}
