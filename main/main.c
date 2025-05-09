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
#include "spotify_client.h"
#include <stdio.h>
#include "esp_bsp.h"
#include "display.h"
#include "ui/ui.h"

/* Locally scoped variables --------------------------------------------------*/
static const char *TAG = "SPOTIFY_APP";

/* Private function prototypes -----------------------------------------------*/
static void now_playing_screen();

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("spotify_client", ESP_LOG_DEBUG);

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
        .rotate = LV_DISP_ROT_90,
    };

    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();
    bsp_display_lock(0);
    ui_init();
    bsp_display_unlock();

    vTaskDelay(200 / portTICK_PERIOD_MS);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    // Initialize the Spotify client
    ESP_ERROR_CHECK(spotify_client_init(5));

    now_playing_screen();

}

/* Private functions ---------------------------------------------------------*/
static void now_playing_screen()
{

    static TrackInfo track = {.artists.type = STRING_LIST};
    assert(track.name = strdup("No device playing..."));

    SpotifyEvent_t event;
    TickType_t event_stamp = 0;
    char t_time[6] = {"00:00"};

    // enable the player and wait for events
    spotify_dispatch_event(ENABLE_PLAYER_EVENT);

    // in the first iteration we wait forever
    TickType_t ticks_to_wait = portMAX_DELAY;
    uint32_t percent = 0;
    while (1)
    {
        /* Wait for track event ------------------------------------------------------*/
        if (pdPASS == spotify_wait_event(&event, ticks_to_wait))
        {
            event_stamp = xTaskGetTickCount();
            // just to be sure...
            if (ticks_to_wait != 0 && event.type != NEW_TRACK)
            {
                ESP_LOGW(TAG, "Still waiting for the first event of a track");
                ESP_LOGW(TAG, "Event: %d", event.type);
                if (event.type == NO_PLAYER_ACTIVE)
                {
                    // TODO: get all available devices
                }
                else
                {
                }
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
                continue;
            }
            ticks_to_wait = 0;

            switch (event.type)
            {
            case NEW_TRACK:
                spotify_clear_track(&track);
                spotify_clone_track(&track, (TrackInfo *)event.payload);
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
                percent = (track.progress_ms * 100) / track.duration_ms;
                if (percent > 100)
                    percent = 100;
                bsp_display_lock(0);
                lv_label_set_text(ui_Track, track.name);
                bsp_display_unlock();
                break;
            case SAME_TRACK:
                TrackInfo *t_updated = event.payload;
                track.isPlaying = t_updated->isPlaying;
                track.progress_ms = t_updated->progress_ms;
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
                percent = (track.progress_ms * 100) / track.duration_ms;
                if (percent > 100)
                    percent = 100;
                break;
            case NO_PLAYER_ACTIVE:
                // TODO: get all devices available
                break;
            default:
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
                continue;
            }
        }
        else
        { /* expired */
            if (track.isPlaying)
            {
                time_t t_progress_ms = track.progress_ms + pdTICKS_TO_MS(xTaskGetTickCount() - event_stamp);
                percent = (t_progress_ms * 100) / track.duration_ms;
                if (percent > 100)
                    percent = 100;
            }
        }

        /* TODO: Track artists */

        /* Time progress */
        // t_time

        bsp_display_lock(0); // o lvgl_acquire()
        lv_bar_set_value(ui_ProgressBar, percent, LV_ANIM_OFF);
        bsp_display_unlock(); // o lvgl_release()
    }
}