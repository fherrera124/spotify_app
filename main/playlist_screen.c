#include "playlist_screen.h"
#include "app_globals.h"
#include "esp_log.h"
#include "bsp_jc3248w535.h"
#include "ui/ui.h"

static const char *TAG = "PLAYLIST_SCREEN";

/* Runs on the lvgl_port task (touch dispatch) when a playlist row is
 * tapped; the row's uri (PlaylistItem_t.uri, still owned by the List that
 * playlist_task hasn't freed yet - see playlist_task) was stored as the
 * event's user_data when the row button was created. Just hands it off;
 * playlist_task does the actual (blocking) spotify_play_context_uri call. */
static void playlist_row_clicked_cb(lv_event_t *e)
{
    char *uri = lv_event_get_user_data(e);
    xQueueSend(playlist_selection_queue, &uri, 0);
}

/* Dedicated task so the (blocking, ~1-2s) spotify_user_playlists() HTTP
 * call never freezes lvgl_port's own task (which pumps lv_timer_handler()
 * and would otherwise stall rendering/input for that whole time). Sits
 * idle until openPlaylistsFn() (ui_events.c) wakes it via
 * xTaskNotifyGive(). */
static void playlist_task(void *arg)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        List *playlists = spotify_user_playlists(client);

        bsp_display_lock(0);
        lv_obj_clean(ui_PlaylistList);
        if (!playlists)
        {
            lv_label_set_text(ui_PlaylistStatusLabel, "Error al obtener playlists");
            lv_obj_clear_flag(ui_PlaylistStatusLabel, LV_OBJ_FLAG_HIDDEN);
        }
        else if (playlists->count == 0)
        {
            lv_label_set_text(ui_PlaylistStatusLabel, "No se encontraron playlists");
            lv_obj_clear_flag(ui_PlaylistStatusLabel, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(ui_PlaylistStatusLabel, LV_OBJ_FLAG_HIDDEN);
            Node *node = playlists->first;
            while (node)
            {
                PlaylistItem_t *item = node->data;
                lv_obj_t *btn = lv_list_add_button(ui_PlaylistList, LV_SYMBOL_AUDIO, item->name);
                lv_obj_add_event_cb(btn, playlist_row_clicked_cb, LV_EVENT_CLICKED, item->uri);
                node = node->next;
            }
        }
        bsp_display_unlock();

        // Woken up by either a row tap (uri) or the back button (NULL
        // sentinel, see playlistBackFn in ui_events.c) - covers the
        // no-playlists/error case too, since only "back" can be tapped then.
        char *selected_uri = NULL;
        xQueueReceive(playlist_selection_queue, &selected_uri, portMAX_DELAY);

        if (selected_uri)
        {
            HttpStatus_Code status_code;
            spotify_play_context_uri(client, selected_uri, &status_code);
        }

        bsp_display_lock(0);
        lv_disp_load_scr(ui_PlayerScreen);
        bsp_display_unlock();

        if (playlists)
        {
            // spotify_free_nodes() frees the Nodes/PlaylistItem_t's but not
            // the List struct itself (it's calloc'd by spotify_user_playlists()).
            spotify_free_nodes(playlists);
            free(playlists);
        }
    }
}

esp_err_t playlist_screen_init(void)
{
    playlist_selection_queue = xQueueCreate(1, sizeof(char *));
    if (!playlist_selection_queue)
    {
        ESP_LOGE(TAG, "Error creating playlist selection queue");
        return ESP_FAIL;
    }
    if (xTaskCreate(playlist_task, "playlist_task", 8192, NULL, 5, &playlist_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Error creating playlist task");
        return ESP_FAIL;
    }
    return ESP_OK;
}
