#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "rotary_encoder.h"
#include "spotify_client.h"
#include <stdio.h>

/* Locally scoped variables --------------------------------------------------*/
static const char*    TAG = "SPOTIFY_APP";
rotary_encoder_info_t info = { 0 };

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("spotify_client", ESP_LOG_DEBUG);
    esp_log_level_set("HANDLER_CALLBACKS", ESP_LOG_DEBUG);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());
    ESP_ERROR_CHECK(spotify_client_init(5));
    ESP_ERROR_CHECK(rotary_encoder_default_init(&info));
    spotify_dispatch_event(ENABLE_PLAYER_EVENT);
    SpotifyClientEvent_t data;

    rotary_encoder_event_t queue_event = { 0 };

    while (1) {
        spotify_wait_event(&data);
        xQueueReset(info.queue);
        ESP_LOGW(TAG, "New data awaiting to be processed");
        BaseType_t res = xQueueReceive(info.queue, &queue_event, pdMS_TO_TICKS(2000));
        if (res) {
            if (queue_event.event_type == BUTTON_EVENT) {
                ESP_LOGW(TAG, "Send event of data processed");
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
            }
        } else {
            ESP_LOGE(TAG, "Timeout awaiting for the data to be processed!");
            spotify_dispatch_event(DATA_PROCESSED_EVENT);
        }
    }
}
