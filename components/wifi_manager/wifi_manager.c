#include "wifi_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

/* Private macro -------------------------------------------------------------*/
#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
/* How many disconnect->reconnect attempts before giving up on a given
 * connect_and_wait() call. */
#define WIFI_MAX_RETRY 5
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define SCAN_MAX_RESULTS 20
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

/* Locally scoped variables --------------------------------------------------*/
static const char *TAG = "WIFI_MANAGER";
static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_num = 0;

/* Private function prototypes -----------------------------------------------*/
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static esp_err_t connect_and_wait(const char *ssid, const char *password, bool persist_on_success);
static esp_err_t load_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size);
static void save_credentials(const char *ssid, const char *password);
static int compare_rssi_desc(const void *a, const void *b);

/* Exported functions --------------------------------------------------------*/
esp_err_t wifi_manager_init(void)
{
    if (!esp_netif_create_default_wifi_sta())
    {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    // Explicit, app-owned NVS persistence (see save_credentials/
    // load_credentials) instead of leaning on the Wi-Fi driver's own
    // WIFI_STORAGE_FLASH - keeps exactly one place responsible for what's
    // remembered across reboots, easy to reason about/clear later.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        return err;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group)
    {
        return ESP_ERR_NO_MEM;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL, NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    return esp_wifi_start();
}

esp_err_t wifi_manager_try_stored_credentials(void)
{
    char ssid[33] = {0};
    char password[65] = {0};
    esp_err_t err = load_credentials(ssid, sizeof(ssid), password, sizeof(password));
    if (err != ESP_OK)
    {
        // Nothing usable stored (first boot, or NVS read failed) - caller
        // should show the setup screen, not treat this as fatal.
        return err;
    }
    ESP_LOGI(TAG, "Trying stored network \"%s\"", ssid);
    return connect_and_wait(ssid, password, false);
}

esp_err_t wifi_manager_scan(wifi_ap_info_t **out_aps, int *out_count)
{
    *out_aps = NULL;
    *out_count = 0;

    wifi_scan_config_t scan_config = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true /* block until done */);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0)
    {
        return ESP_OK;
    }
    if (num > SCAN_MAX_RESULTS)
    {
        num = SCAN_MAX_RESULTS;
    }

    // wifi_ap_record_t is ~60+ bytes (country/HE-AP sub-structs most callers
    // don't need) - only fetched into a short-lived local buffer, never
    // handed to the caller as-is (see wifi_ap_info_t).
    wifi_ap_record_t *records = malloc(num * sizeof(wifi_ap_record_t));
    if (!records)
    {
        ESP_LOGE(TAG, "Out of memory allocating scan records");
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&num, records);
    if (err != ESP_OK)
    {
        free(records);
        return err;
    }

    wifi_ap_info_t *aps = calloc(num, sizeof(wifi_ap_info_t));
    if (!aps)
    {
        free(records);
        ESP_LOGE(TAG, "Out of memory allocating AP info list");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < num; i++)
    {
        strlcpy(aps[i].ssid, (char *)records[i].ssid, sizeof(aps[i].ssid));
        aps[i].rssi = records[i].rssi;
        aps[i].secured = (records[i].authmode != WIFI_AUTH_OPEN);
    }
    free(records);

    qsort(aps, num, sizeof(wifi_ap_info_t), compare_rssi_desc);

    *out_aps = aps;
    *out_count = num;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    return connect_and_wait(ssid, password ? password : "", true);
}

/* Private functions ---------------------------------------------------------*/
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id != WIFI_EVENT_STA_DISCONNECTED)
    {
        return;
    }
    if (s_retry_num < WIFI_MAX_RETRY)
    {
        s_retry_num++;
        ESP_LOGI(TAG, "Wi-Fi disconnected, retrying (%d/%d)...", s_retry_num, WIFI_MAX_RETRY);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED)
        {
            ESP_LOGW(TAG, "esp_wifi_connect retry failed: %s", esp_err_to_name(err));
        }
    }
    else
    {
        ESP_LOGW(TAG, "Wi-Fi connect failed after %d retries, giving up", s_retry_num);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
}

/* Shared by wifi_manager_connect() and wifi_manager_try_stored_credentials():
 * applies ssid/password, connects, and blocks (bounded) for the outcome.
 * Never ESP_ERROR_CHECK's anything network-related - a bad password or an
 * unreachable AP is an ordinary, expected failure here, not a device abort. */
static esp_err_t connect_and_wait(const char *ssid, const char *password, bool persist_on_success)
{
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_CONNECTED_BIT)
    {
        if (persist_on_success)
        {
            save_credentials(ssid, password);
        }
        return ESP_OK;
    }
    // Timed out or explicitly failed (WIFI_FAIL_BIT) - stop the driver from
    // continuing to hammer a bad AP in the background while the caller
    // decides what to do next (e.g. show the AP list again).
    esp_wifi_disconnect();
    return ESP_FAIL;
}

static esp_err_t load_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        return err;
    }
    size_t len = ssid_size;
    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &len);
    if (err != ESP_OK)
    {
        nvs_close(handle);
        return err;
    }
    len = password_size;
    err = nvs_get_str(handle, NVS_KEY_PASS, password, &len);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        // A saved open network has no password key at all - not a failure.
        password[0] = '\0';
        return ESP_OK;
    }
    return err;
}

static void save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to open NVS to save Wi-Fi credentials");
        return;
    }
    nvs_set_str(handle, NVS_KEY_SSID, ssid);
    nvs_set_str(handle, NVS_KEY_PASS, password);
    nvs_commit(handle);
    nvs_close(handle);
}

static int compare_rssi_desc(const void *a, const void *b)
{
    const wifi_ap_info_t *ap_a = a;
    const wifi_ap_info_t *ap_b = b;
    return (int)ap_b->rssi - (int)ap_a->rssi;
}
