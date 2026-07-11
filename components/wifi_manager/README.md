# wifi_manager

App-owned Wi-Fi station manager for ESP32(-S3) projects. Wraps `esp_wifi` directly: scan, connect with bounded retries/timeout, and persist the last working SSID/password to NVS (namespace `wifi_cfg`) so it reconnects automatically on the next boot.

Callers decide what to do with a non-`ESP_OK` return (e.g. show a setup UI).

## Usage

```c
#include "wifi_manager.h"

ESP_ERROR_CHECK(wifi_manager_init()); // driver/netif setup - a real failure here IS fatal

if (wifi_manager_try_stored_credentials() != ESP_OK) {
    // Nothing stored yet, or it didn't work - scan and let the user pick.
    wifi_ap_info_t *aps;
    int count;
    if (wifi_manager_scan(&aps, &count) == ESP_OK) {
        // ...show `count` entries from `aps` (ssid/rssi/secured)...
        wifi_manager_connect(aps[chosen].ssid, typed_password);
        free(aps);
    }
}
```

See `include/wifi_manager.h` for the full API doc.
