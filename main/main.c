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
#include "decode_jpg.h"
#include "jpeg_decoder.h"

esp_spotify_client_handle_t client = NULL;

/* Private macro -------------------------------------------------------------*/
#define COVER_W 300
#define COVER_H 300
// we will scale the image to half size on the display
#define COVER_W_HALF (COVER_W / 2)
#define COVER_H_HALF (COVER_H / 2)

/* Locally scoped variables --------------------------------------------------*/
static const char *TAG = "SPOTIFY_APP";

static lv_img_dsc_t pic_img_dsc = {
    .header = {
        .cf = LV_IMG_CF_TRUE_COLOR,
        .always_zero = 0,
        .reserved = 0,
        .w = COVER_W_HALF,
        .h = COVER_H_HALF,
    },
    .data_size = COVER_W_HALF * COVER_H_HALF * 2,
    .data = NULL,
};

/* Private function prototypes -----------------------------------------------*/
static void now_playing_screen();
static char *join_artist_names(List *artists);

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
    client = spotify_client_init(5);
    if (!client)
    {
        ESP_LOGE(TAG, "Error initializing Spotify client");
        return;
    }

    now_playing_screen();
}

static uint16_t *pixels = NULL;

/* Private functions ---------------------------------------------------------*/
static void now_playing_screen()
{
    if (!pixels)
    {
        // Alocate pixel memory. Each line is an array of IMAGE_W 16-bit pixels; the `*pixels` array itself contains pointers to these lines.
        pixels = heap_caps_calloc((COVER_W_HALF * COVER_H_HALF), sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!pixels)
        {
            ESP_LOGE(TAG, "Failed to alloc buffer");
            return;
        }
    }

    pic_img_dsc.data = (uint8_t *)pixels;
    bsp_display_lock(0);
    lv_img_set_src(ui_CoverImage, &pic_img_dsc);
    bsp_display_unlock();

    static TrackInfo track = {.artists.type = STRING_LIST};
    assert(track.name = strdup("No device playing..."));

    SpotifyEvent_t event;
    TickType_t event_stamp = 0;
    char t_time[6] = {"00:00"};

    // enable the player and wait for events
    player_dispatch_event(client, ENABLE_PLAYER_EVENT);

    // in the first iteration we wait forever
    TickType_t ticks_to_wait = portMAX_DELAY;
    uint32_t percent = 0;
    char *artist_str = NULL;
    while (1)
    {
        /* Wait for track event ------------------------------------------------------*/
        if (pdPASS == spotify_wait_event(client, &event, ticks_to_wait))
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
                player_dispatch_event(client, DATA_PROCESSED_EVENT);
                continue;
            }
            ticks_to_wait = 0;

            switch (event.type)
            {
            case NEW_TRACK:
                spotify_clear_track(&track);
                spotify_clone_track(&track, (TrackInfo *)event.payload);
                player_dispatch_event(client, DATA_PROCESSED_EVENT);
                percent = (track.progress_ms * 100) / track.duration_ms;
                if (percent > 100)
                    percent = 100;
                if (artist_str)
                {
                    free(artist_str);
                }
                artist_str = join_artist_names(&track.artists);
                bsp_display_lock(0);
                lv_label_set_text(ui_Track, track.name);
                lv_label_set_text(ui_Artists, artist_str);
                bsp_display_unlock();
                size_t buf_size = COVER_W * COVER_H;
                uint8_t *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                uint32_t jpg_size = 0;
                if (!buf)
                {
                    ESP_LOGE(TAG, "Failed to alloc buffer");
                }
                else
                {
                    jpg_size = fetch_album_art(client, &track, buf, buf_size);
                }
                if ((int)jpg_size <= 0)
                {
                    ESP_LOGE(TAG, "Failed to fetch album cover");
                    // black image
                    memset(pixels, 0, COVER_W_HALF * COVER_H_HALF * sizeof(uint16_t));
                }
                else
                {
                    decode_image(pixels, buf, jpg_size, COVER_W_HALF, COVER_H_HALF, JPEG_IMAGE_SCALE_1_2);
                }
                if (buf)
                {
                    free(buf);
                }
                bsp_display_lock(0);
                lv_obj_invalidate(ui_CoverImage);
                bsp_display_unlock();
                break;
            case SAME_TRACK:
                TrackInfo *t_updated = event.payload;
                track.isPlaying = t_updated->isPlaying;
                track.progress_ms = t_updated->progress_ms;
                player_dispatch_event(client, DATA_PROCESSED_EVENT);
                percent = (track.progress_ms * 100) / track.duration_ms;
                if (percent > 100)
                    percent = 100;
                break;
            case NO_PLAYER_ACTIVE:
                // TODO: get all devices available
                break;
            default:
                player_dispatch_event(client, DATA_PROCESSED_EVENT);
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

        /* Time progress */
        // t_time

        bsp_display_lock(0);
        lv_bar_set_value(ui_ProgressBar, percent, LV_ANIM_OFF);
        bsp_display_unlock();
    }
}

char *join_artist_names(List *artists)
{
    if (!artists || artists->count == 0) return strdup("");

    size_t total_len = 0;
    Node *node = artists->first;
    while (node) {
        total_len += strlen((char *)node->data);
        if (node->next) total_len += 2; // ", "
        node = node->next;
    }
    
    // Reservamos la cadena final (+1 para el '\0')
    char *result = malloc(total_len + 1);
    if (!result) return NULL;

    result[0] = '\0';

    node = artists->first;
    while (node) {
        strcat(result, (char *)node->data);
        if (node->next)
            strcat(result, ", ");
        node = node->next;
    }

    return result;
}