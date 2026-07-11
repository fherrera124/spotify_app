#include "search_screen.h"
#include "app_globals.h"
#include "esp_log.h"
#include "bsp_jc3248w535.h"
#include "ui/ui.h"
#include <stdio.h>

static const char *TAG = "SEARCH_SCREEN";

/* Runs on the lvgl_port task (touch dispatch) when a search result row is
 * tapped; the row's uri (TrackSearchItem_t.uri, still owned by the List that
 * search_task hasn't freed yet - see search_task) was stored as the event's
 * user_data when the row button was created. Just hands it off; search_task
 * does the actual (blocking) spotify_play_track_uri call. */
static void search_row_clicked_cb(lv_event_t *e)
{
    char *uri = lv_event_get_user_data(e);
    xQueueSend(search_selection_queue, &uri, 0);
}

/* Dedicated task so the (blocking) spotify_search_tracks()/
 * spotify_play_track_uri() HTTP calls never freeze lvgl_port's own task -
 * same reasoning as playlist_task/device_task. Unlike those, has two wait
 * points instead of one: first for a submitted query (or "back" tapped
 * before ever submitting one), then - only if a query came in - for a row
 * tap (or "back" tapped once results/status are showing). See
 * searchBackFn (ui_events.c) for how it decides which of the two queues to
 * signal. */
static void search_task(void *arg)
{
    for (;;)
    {
        char *query = NULL;
        xQueueReceive(search_query_queue, &query, portMAX_DELAY);

        List *tracks = NULL;
        if (query)
        {
            tracks = spotify_search_tracks(client, query);
            free(query);

            bsp_display_lock(0);
            lv_obj_clean(ui_SearchResultList);
            if (!tracks)
            {
                lv_label_set_text(ui_SearchStatusLabel, "Error al buscar");
                lv_obj_clear_flag(ui_SearchStatusLabel, LV_OBJ_FLAG_HIDDEN);
            }
            else if (tracks->count == 0)
            {
                lv_label_set_text(ui_SearchStatusLabel, "No se encontraron canciones");
                lv_obj_clear_flag(ui_SearchStatusLabel, LV_OBJ_FLAG_HIDDEN);
            }
            else
            {
                lv_obj_add_flag(ui_SearchStatusLabel, LV_OBJ_FLAG_HIDDEN);
                Node *node = tracks->first;
                while (node)
                {
                    TrackSearchItem_t *item = node->data;
                    char row_text[128];
                    snprintf(row_text, sizeof(row_text), "%s - %s", item->name, item->artists ? item->artists : "");
                    lv_obj_t *btn = lv_list_add_button(ui_SearchResultList, LV_SYMBOL_AUDIO, row_text);
                    lv_obj_add_event_cb(btn, search_row_clicked_cb, LV_EVENT_CLICKED, item->uri);
                    node = node->next;
                }
            }
            bsp_display_unlock();

            // Woken up by either a row tap (uri) or the back button (NULL
            // sentinel, see searchBackFn) - covers the no-results/error case
            // too, since only "back" can be tapped then.
            char *selected_uri = NULL;
            xQueueReceive(search_selection_queue, &selected_uri, portMAX_DELAY);

            if (selected_uri)
            {
                HttpStatus_Code status_code;
                spotify_play_track_uri(client, selected_uri, &status_code);
            }
        }
        // query == NULL means "back" was tapped before ever submitting one -
        // nothing was fetched, nothing to clean up beyond leaving the screen,
        // which is exactly what happens next either way.

        bsp_display_lock(0);
        lv_disp_load_scr(ui_PlayerScreen);
        bsp_display_unlock();

        if (tracks)
        {
            // spotify_free_nodes() frees the Nodes/TrackSearchItem_t's but not
            // the List struct itself (it's calloc'd by spotify_search_tracks()).
            spotify_free_nodes(tracks);
            free(tracks);
        }
    }
}

esp_err_t search_screen_init(void)
{
    search_query_queue = xQueueCreate(1, sizeof(char *));
    search_selection_queue = xQueueCreate(1, sizeof(char *));
    if (!search_query_queue || !search_selection_queue)
    {
        ESP_LOGE(TAG, "Error creating search queues");
        return ESP_FAIL;
    }
    if (xTaskCreate(search_task, "search_task", 8192, NULL, 5, &search_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Error creating search task");
        return ESP_FAIL;
    }
    return ESP_OK;
}
