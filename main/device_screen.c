#include "device_screen.h"
#include "app_globals.h"
#include "esp_log.h"
#include "bsp_jc3248w535.h"
#include "ui/ui.h"

static const char *TAG = "DEVICE_SCREEN";

/* Runs on the lvgl_port task (touch dispatch) when a device row is
 * tapped; the row's id (DeviceItem_t.id, still owned by the List that
 * device_task hasn't freed yet - see device_task) was stored as the
 * event's user_data when the row button was created. Just hands it off;
 * device_task does the actual (blocking) spotify_transfer_playback call. */
static void device_row_clicked_cb(lv_event_t *e)
{
    char *id = lv_event_get_user_data(e);
    xQueueSend(device_selection_queue, &id, 0);
}

/* Dedicated task so the (blocking) spotify_available_devices()/
 * spotify_transfer_playback() HTTP calls never freeze lvgl_port's own task
 * (which pumps lv_timer_handler() and would otherwise stall rendering/
 * input for that whole time) - same reasoning as playlist_task
 * (playlist_screen.c). Sits idle until openDevicesFn() (ui_events.c) wakes
 * it via xTaskNotifyGive(). */
static void device_task(void *arg)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        List *devices = spotify_available_devices(client);

        bsp_display_lock(0);
        lv_obj_clean(ui_DeviceList);
        if (!devices)
        {
            lv_label_set_text(ui_DeviceStatusLabel, "Error al obtener dispositivos");
            lv_obj_clear_flag(ui_DeviceStatusLabel, LV_OBJ_FLAG_HIDDEN);
        }
        else if (devices->count == 0)
        {
            lv_label_set_text(ui_DeviceStatusLabel, "No se encontraron dispositivos");
            lv_obj_clear_flag(ui_DeviceStatusLabel, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(ui_DeviceStatusLabel, LV_OBJ_FLAG_HIDDEN);
            Node *node = devices->first;
            while (node)
            {
                DeviceItem_t *item = node->data;
                lv_obj_t *btn = lv_list_add_button(ui_DeviceList, item->is_active ? LV_SYMBOL_OK : LV_SYMBOL_BLUETOOTH, item->name);
                lv_obj_add_event_cb(btn, device_row_clicked_cb, LV_EVENT_CLICKED, item->id);
                node = node->next;
            }
        }
        bsp_display_unlock();

        // Woken up by either a row tap (id) or the close button (NULL
        // sentinel, see closeDevicesFn in ui_events.c) - covers the
        // no-devices/error case too, since only "close" can be tapped then.
        char *selected_id = NULL;
        xQueueReceive(device_selection_queue, &selected_id, portMAX_DELAY);

        if (selected_id)
        {
            HttpStatus_Code status_code;
            esp_err_t err = spotify_transfer_playback(client, selected_id, &status_code);
            if (err != ESP_OK || (status_code != HttpStatus_Ok && status_code != HTTP_STATUS_NO_CONTENT))
            {
                ESP_LOGW(TAG, "spotify_transfer_playback(%s) failed (err=%s, http_status=%d)", selected_id, esp_err_to_name(err), status_code);
            }
        }

        // Unlike playlist_task (a separate screen, navigated via
        // lv_disp_load_scr), the device picker is a modal on top of
        // ui_PlayerScreen - closing it is just re-hiding it.
        bsp_display_lock(0);
        lv_obj_add_flag(ui_DeviceModal, LV_OBJ_FLAG_HIDDEN);
        bsp_display_unlock();

        if (devices)
        {
            // spotify_free_nodes() frees the Nodes/DeviceItem_t's but not
            // the List struct itself (it's calloc'd by spotify_available_devices()).
            spotify_free_nodes(devices);
            free(devices);
        }
    }
}

esp_err_t device_screen_init(void)
{
    device_selection_queue = xQueueCreate(1, sizeof(char *));
    if (!device_selection_queue)
    {
        ESP_LOGE(TAG, "Error creating device selection queue");
        return ESP_FAIL;
    }
    if (xTaskCreate(device_task, "device_task", 8192, NULL, 5, &device_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Error creating device task");
        return ESP_FAIL;
    }
    return ESP_OK;
}
