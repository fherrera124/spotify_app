#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "app_globals.h"
#include <stdio.h>
#include "bsp_jc3248w535.h"
#include "ui/ui.h"
#include "player_screen.h"
#include "playlist_screen.h"
#include "device_screen.h"
#include "search_screen.h"
#include "wifi_manager.h"
#include "wifi_screen.h"

esp_spotify_client_handle_t client = NULL;
TaskHandle_t playlist_task_handle = NULL;
QueueHandle_t playlist_selection_queue = NULL;
QueueHandle_t volume_target_queue = NULL;
QueueHandle_t seek_target_queue = NULL;
TaskHandle_t device_task_handle = NULL;
QueueHandle_t device_selection_queue = NULL;
TaskHandle_t search_task_handle = NULL;
QueueHandle_t search_query_queue = NULL;
QueueHandle_t search_selection_queue = NULL;
QueueHandle_t wifi_ap_selected_queue = NULL;
QueueHandle_t wifi_password_submit_queue = NULL;

/* Locally scoped variables --------------------------------------------------*/
static const char *TAG = "SPOTIFY_APP";

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("spotify_client", ESP_LOG_DEBUG);

    bool landscape = true;
    lv_display_t *disp = bsp_display_start(landscape);
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() falló");
        return;
    }
    bsp_display_backlight_on();
    bsp_display_lock(0);
    ui_init();
    bsp_display_unlock();

    vTaskDelay(200 / portTICK_PERIOD_MS);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // wifi_manager_init() failures are fatal (driver/netif setup); connect
    // failures are not - they fall through to ui_WifiScreen instead of
    // aborting the device (ANALYSIS.md 1.24).
    ESP_ERROR_CHECK(wifi_manager_init());
    if (wifi_manager_try_stored_credentials() != ESP_OK)
    {
        if (wifi_screen_run_until_connected() != ESP_OK)
        {
            ESP_LOGE(TAG, "Error setting up the Wi-Fi setup screen");
            return;
        }
        bsp_display_lock(0);
        lv_disp_load_scr(ui_PlayerScreen);
        bsp_display_unlock();
    }

    // Initialize the Spotify client
    client = spotify_client_init(5);
    if (!client)
    {
        ESP_LOGE(TAG, "Error initializing Spotify client");
        return;
    }

    if (playlist_screen_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Error initializing playlist screen");
        return;
    }

    if (device_screen_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Error initializing device screen");
        return;
    }

    if (search_screen_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Error initializing search screen");
        return;
    }

    player_screen_start();
}
