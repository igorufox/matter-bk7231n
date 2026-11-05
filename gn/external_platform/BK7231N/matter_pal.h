/*
 * BK7231N (BDK 3.0.33 / LibreTiny) platform abstraction for the Matter platform layer.
 *
 * The upstream src/platform/Beken code is written against ARMINO (BK7235, RISC-V): <modules/wifi.h>,
 * "matter_pal.h", flash_namespace_value.h ... none of that exists in BDK 3.0.33.
 * Part 1 pulls the BDK "v1" API that exists on both (wlan_ui_pub.h, rtos_pub.h, net.h, BkDriverFlash.h).
 * Part 2 declares the ARMINO-only API the copied sources use. That part is THE PORT: each function has to be
 * implemented on top of BDK / LibreTiny / ESPurna (see bk7231n_port.c, currently stubs for sizing).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Part 1: BDK 3.0.33 (exists, used as is) ---------- */
#include "error.h"          /* kNoErr */
#include "lwip/opt.h"       /* LWIP_IPV6_NUM_ADDRESSES for net.h (lwIP must be built with CONFIG_IPV6) */
#include "rtos_pub.h"       /* beken_semaphore_t, rtos_*_semaphore, rtos_delay_milliseconds */
/* rtos_pub.h macros clash with chip::kMillisecondsPerSecond & co. */
#undef kNanosecondsPerSecond
#undef kMicrosecondsPerSecond
#undef kMillisecondsPerSecond
#include "wlan_ui_pub.h"    /* LinkStatusTypeDef, bk_wlan_get_link_status, bk_sta_cipher_type, BK_SECURITY_TYPE_*, bk_reboot */
#include "net.h"            /* net_get_sta_handle */
#include "BkDriverFlash.h"  /* bk_flash_get_info, BK_PARTITION_OTA, bk_logic_partition_t */

#define CONFIG_FLASH_ORIGIN_API 1

extern void bk_printf(const char * fmt, ...);
extern void * os_zalloc(size_t size);
extern void * os_realloc(void * ptr, size_t size);
extern void os_free(void * ptr);
#ifndef os_memcpy
#define os_memcpy memcpy
#endif

/* ---------- Part 2: ARMINO API used by the Beken platform layer (to implement on BDK) ---------- */
typedef int bk_err_t;
#define BK_OK 0
#define BK_FAIL -1
#define BK_LOG_ON_ERR(x)                                                                                                           \
    do                                                                                                                             \
    {                                                                                                                              \
        bk_err_t _e = (x);                                                                                                         \
        if (_e != BK_OK)                                                                                                           \
            bk_printf("%s:%d err %d\r\n", __func__, __LINE__, _e);                                                                \
    } while (0)
#define BK_CHECK_POINTER_NULL_TO_VOID(p)                                                                                           \
    do                                                                                                                             \
    {                                                                                                                              \
        if (!(p))                                                                                                                  \
            return;                                                                                                                \
    } while (0)

/* events: Wi-Fi STA connect/disconnect, scan done, got IPv4 (BDK: bk_wlan_status_register_cb + netif callbacks) */
typedef enum
{
    EVENT_MOD_WIFI,
    EVENT_MOD_NETIF,
} event_module_t;
#define EVENT_ID_ALL -1
enum
{
    EVENT_WIFI_SCAN_DONE = 0,
    EVENT_WIFI_STA_CONNECTED,
    EVENT_WIFI_STA_DISCONNECTED,
};
enum
{
    EVENT_NETIF_GOT_IP4 = 0,
};
typedef int (*event_cb_t)(void * arg, event_module_t event_module, int event_id, void * event_data);
bk_err_t bk_event_register_cb(event_module_t module, int event_id, event_cb_t cb, void * arg);
bk_err_t bk_event_unregister_cb(event_module_t module, int event_id, event_cb_t cb);

typedef struct
{
    int disconnect_reason;
    bool local_generated;
} wifi_event_sta_disconnected_t;

/* Wi-Fi STA (BDK: bk_wlan_start_sta / network_InitTypeDef_st; in ESPurna this goes through its wifi module) */
typedef enum
{
    WIFI_SECURITY_NONE = 0,
    WIFI_SECURITY_AUTO = 10,
} wifi_security_t;
typedef struct
{
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    wifi_security_t security;
    char password[65];
} wifi_sta_config_t;
typedef struct
{
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    int security;
} wifi_link_status_t;
bk_err_t bk_wifi_sta_set_config(const wifi_sta_config_t * config);
bk_err_t bk_wifi_sta_start(void);
bk_err_t bk_wifi_sta_connect(void);
bk_err_t bk_wifi_sta_get_link_status(wifi_link_status_t * status);
bk_err_t bk_wifi_sta_get_mac(uint8_t * mac);

/* scan (BDK: bk_wlan_start_scan + wlan_sta_scan_result / mhdr_scanu_reg_cb) */
typedef struct
{
    char ssid[33];
} wifi_scan_config_t;
typedef struct
{
    char ssid[33];
    uint8_t bssid[6];
    int rssi;
    uint8_t channel;
    int security;
} wifi_scan_ap_info_t;
typedef struct
{
    int ap_num;
    wifi_scan_ap_info_t * aps;
} wifi_scan_result_t;
bk_err_t bk_wifi_scan_start(const wifi_scan_config_t * config);
bk_err_t bk_wifi_scan_get_result(wifi_scan_result_t * result);
void bk_wifi_scan_free_result(wifi_scan_result_t * result);

/* key-value store, namespace + name (ARMINO flash_namespace_value; BDK: FlashDB of LibreTiny or ESPurna's KV) */
int bk_read_data(const char * ns, const char * name, char * buf, uint32_t len, uint32_t * out_len);
int bk_write_data(const char * ns, const char * name, const char * buf, uint32_t len);
int bk_clean_data(const char * ns, const char * name);
int bk_ensure_namespace(const char * ns);
int bk_ensure_name_data(const char * ns, const char * name);
#define BK_PARTITION_MATTER_FLASH BK_PARTITION_USR_CONFIG
int bk_erase_all(int partition);
int bK_clear_namespace(const char * ns); /* sic, upstream name */

/* ARMINO FreeRTOS heap extension (BDK: configTOTAL_HEAP_SIZE) */
size_t prvHeapGetTotalSize(void);

/* Wi-Fi credentials record used by the upstream code (unused on BDK) */
typedef struct
{
    char ssid[33];
    char key[65];
} ssid_key_save_t;

/* OTA download partition (BDK: flash_ctrl writes into 'download', or ESPurna/LibreTiny OTA) */
/* RBL header of a Beken OTA image (same format ESPurna's OTA writes to 'download') */
typedef struct
{
    char magic[4];
    uint32_t algo;
    uint32_t timestamp;
    char name[16];
    char version[24];
    char sn[24];
    uint32_t crc32;
    uint32_t hash;
    uint32_t size_raw;
    uint32_t size_package;
    uint32_t info_crc32;
} ota_data_struct_t;
int bk_write_ota_data_to_flash(char * buf, uint32_t offset, uint32_t len);
int bk_read_ota_data_in_flash(char * buf, uint32_t offset, uint32_t len);
void bk_erase_ota_data_in_flash(void);

/* entropy for the CHIP DRBG (BDK: bk_rand / TRNG) */
int mbedtls_hardware_poll(void * data, unsigned char * output, size_t len, size_t * olen);

#ifdef __cplusplus
}
#endif
