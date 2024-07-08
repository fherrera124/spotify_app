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

/* Locally scoped variables --------------------------------------------------*/
static const char* TAG = "SPOTIFY_CLIENT_EXAMPLE";

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

    // obtain the user playlists
    List* playlists = spotify_user_playlists();
    if (playlists->count > 0) {
        ESP_LOGI(TAG, "User playlists:");
        Node* playlist_n = playlists->first;
        while (playlist_n) {
            PlaylistItem_t* data = playlist_n->data;
            ESP_LOGI(TAG, "Playlist name: %s", data->name);
            ESP_LOGI(TAG, "Playlist uri: %s", data->uri);
            playlist_n = playlist_n->next;
        }
        spotify_free_nodes(playlists);
    } else {
        ESP_LOGW(TAG, "No playlists found");
    }
    assert(playlists->count == 0);

    // obtain the available devices
    List* available_devices = spotify_available_devices();
    if (available_devices->count > 0) {
        ESP_LOGI(TAG, "Available devices:");
        Node* device_n = available_devices->first;
        while (device_n) {
            DeviceItem_t* data = device_n->data;
            ESP_LOGI(TAG, "Device name: %s", data->name);
            ESP_LOGI(TAG, "Device id: %s", data->id);
            device_n = device_n->next;
        }
        spotify_free_nodes(available_devices);
    } else {
        ESP_LOGW(TAG, "No available devices found");
    }
    assert(available_devices->count == 0);

    // enable the player and wait for events
    spotify_dispatch_event(ENABLE_PLAYER_EVENT);
    SpotifyEvent_t event;
    TrackInfo      track = { .artists.type = STRING_LIST };
    assert(track.name = calloc(1, 1));
    while (1) {
        spotify_wait_event(&event, portMAX_DELAY);
        if (event.type == NEW_TRACK) {
            ESP_LOGI(TAG, "#");
            spotify_clear_track(&track);
            spotify_clone_track(&track, (TrackInfo*)event.payload);
            ESP_LOGI(TAG, "Track: \"%s\"", track.name);
            ESP_LOGI(TAG, "Album: \"%s\"", track.album);
            Node* artist_n = track.artists.first;
            if (track.artists.count == 1) {
                ESP_LOGI(TAG, "Artist: \"%s\"", (char*)artist_n->data);
            } else {
                ESP_LOGI(TAG, "Artists:");
                while (artist_n) {
                    ESP_LOGI(TAG, " \"%s\"", (char*)artist_n->data);
                    artist_n = artist_n->next;
                }
            }
        } else if (event.type == SAME_TRACK) {
            TrackInfo* track_updated = event.payload;
            if (track.isPlaying != track_updated->isPlaying) {
                track.isPlaying = track_updated->isPlaying;
                if (track.isPlaying) {
                    ESP_LOGW(TAG, "Unpaused");
                } else {
                    ESP_LOGW(TAG, "Paused");
                }
            }
            if (track_updated->progress_ms != track.progress_ms) {
                track.progress_ms = track_updated->progress_ms;
                ESP_LOGW(TAG, "progress: %lld", track.progress_ms);
            }
        }
        spotify_dispatch_event(DATA_PROCESSED_EVENT);
    }
}
