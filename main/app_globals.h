#pragma once

/* Shared app-level globals used across the per-screen modules
 * (player_screen.c, playlist_screen.c) and main/ui/ui_events.c. Single
 * canonical declaration site instead of ad-hoc `extern` repeated in each
 * consumer. Defined (once) in main.c. */

#include "spotify_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

extern esp_spotify_client_handle_t client;
extern TaskHandle_t playlist_task_handle;
extern QueueHandle_t playlist_selection_queue;
/* Latest debounced volume target (0-100) from the player screen's slider;
 * see volume_task (player_screen.c) / volumeSliderChangedFn (ui_events.c).
 * Size-1, xQueueOverwrite - only the most recent target matters. */
extern QueueHandle_t volume_target_queue;
/* Same idea, for the seek slider: target position in ms once released; see
 * seek_task (player_screen.c) / seekSliderChangedFn (ui_events.c). */
extern QueueHandle_t seek_target_queue;
/* Same pattern as playlist_task_handle/playlist_selection_queue, for the
 * device picker (device_screen.c): woken via xTaskNotifyGive()
 * (openDevicesFn); device_selection_queue carries the tapped id or a NULL
 * sentinel (closeDevicesFn). */
extern TaskHandle_t device_task_handle;
extern QueueHandle_t device_selection_queue;
/* Same pattern, for the search screen (search_screen.c) - needs two queues
 * instead of one: search_query_queue (submitted text or NULL, searchBackFn)
 * then search_selection_queue (tapped result's uri or NULL). */
extern TaskHandle_t search_task_handle;
extern QueueHandle_t search_query_queue;
extern QueueHandle_t search_selection_queue;
/* Used only by wifi_screen_run_until_connected() (wifi_screen.c,
 * ANALYSIS.md 1.24/3.8) - no task handle, it runs on whichever task calls
 * it (app_main() at boot), not a dedicated one. wifi_ap_selected_queue
 * carries a wifi_ap_info_t* into the not-yet-freed scan array;
 * wifi_password_submit_queue carries a strdup'd password or NULL
 * (cancelled). */
extern QueueHandle_t wifi_ap_selected_queue;
extern QueueHandle_t wifi_password_submit_queue;
