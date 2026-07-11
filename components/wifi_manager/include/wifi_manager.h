#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* App-owned Wi-Fi station manager. No call here aborts the device on a
 * network failure: every function returns esp_err_t for the caller to
 * react to (show a setup screen, retry, etc.) instead of ESP_ERROR_CHECK. */

/* One scanned access point, trimmed down from wifi_ap_record_t (~60+ bytes,
 * mostly fields a picker UI doesn't need) to just ssid/rssi/secured. */
typedef struct {
    char    ssid[33]; /* 32 bytes max SSID + NUL */
    int8_t  rssi;
    bool    secured;  /* false: open network, no password needed */
} wifi_ap_info_t;

/* Sets up the STA netif/driver and event handlers. Call once, before any
 * other wifi_manager_* function. Does not attempt to connect. */
esp_err_t wifi_manager_init(void);

/* If a previous successful connection saved credentials (NVS namespace
 * "wifi_cfg"), attempts to reconnect with them (bounded retries/timeout).
 * Returns ESP_OK if connected; any other value (ESP_ERR_NVS_NOT_FOUND if
 * nothing was stored yet, ESP_FAIL if the stored credentials didn't work -
 * wrong password, AP gone, etc.) means the caller should show the setup
 * screen instead. */
esp_err_t wifi_manager_try_stored_credentials(void);

/* Blocking scan (typically 1-3s). *out_aps is a heap-allocated array the
 * caller must free(); *out_count is its length (0 if nothing found). */
esp_err_t wifi_manager_scan(wifi_ap_info_t **out_aps, int *out_count);

/* Connects to ssid/password (bounded retries/timeout) and, on success,
 * persists both to NVS so wifi_manager_try_stored_credentials() can use
 * them on a future boot. password may be "" for an open network. */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
