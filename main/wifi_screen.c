#include "wifi_screen.h"
#include "app_globals.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "bsp_jc3248w535.h"
#include "ui/ui.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WIFI_SCREEN";

/* How long to wait before re-scanning after a failed connection attempt -
 * avoids hammering esp_wifi_scan_start()/the AP in a tight loop if the
 * network is persistently unreachable. */
#define RETRY_DELAY_MS 2000

/* Runs on the lvgl_port task (touch dispatch) when an AP row is tapped.
 * user_data points directly into the `aps` array that
 * wifi_screen_run_until_connected() hasn't freed yet (same ownership timing
 * as playlist/device/search's row callbacks - the array outlives the wait
 * on wifi_ap_selected_queue below). */
static void wifi_ap_clicked_cb(lv_event_t *e)
{
    wifi_ap_info_t *ap = lv_event_get_user_data(e);
    xQueueSend(wifi_ap_selected_queue, &ap, 0);
}

esp_err_t wifi_screen_run_until_connected(void)
{
    if (!wifi_ap_selected_queue)
    {
        wifi_ap_selected_queue = xQueueCreate(1, sizeof(wifi_ap_info_t *));
    }
    if (!wifi_password_submit_queue)
    {
        wifi_password_submit_queue = xQueueCreate(1, sizeof(char *));
    }
    if (!wifi_ap_selected_queue || !wifi_password_submit_queue)
    {
        ESP_LOGE(TAG, "Error creating Wi-Fi screen queues");
        return ESP_FAIL;
    }

    bsp_display_lock(0);
    lv_disp_load_scr(ui_WifiScreen);
    bsp_display_unlock();

    for (;;)
    {
        bsp_display_lock(0);
        lv_obj_clean(ui_WifiList);
        lv_obj_clear_flag(ui_WifiList, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui_WifiStatusLabel, "Buscando redes...");
        lv_obj_clear_flag(ui_WifiStatusLabel, LV_OBJ_FLAG_HIDDEN);
        bsp_display_unlock();

        wifi_ap_info_t *aps = NULL;
        int count = 0;
        esp_err_t scan_err = wifi_manager_scan(&aps, &count);

        bsp_display_lock(0);
        lv_obj_clean(ui_WifiList);
        if (scan_err != ESP_OK)
        {
            lv_label_set_text(ui_WifiStatusLabel, "Error buscando redes");
        }
        else if (count == 0)
        {
            lv_label_set_text(ui_WifiStatusLabel, "No se encontraron redes");
        }
        else
        {
            lv_obj_add_flag(ui_WifiStatusLabel, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < count; i++)
            {
                char row_text[48];
                if (aps[i].secured)
                {
                    snprintf(row_text, sizeof(row_text), "%s", aps[i].ssid);
                }
                else
                {
                    snprintf(row_text, sizeof(row_text), "%s (abierta)", aps[i].ssid);
                }
                lv_obj_t *btn = lv_list_add_button(ui_WifiList, LV_SYMBOL_WIFI, row_text);
                lv_obj_add_event_cb(btn, wifi_ap_clicked_cb, LV_EVENT_CLICKED, &aps[i]);
            }
        }
        bsp_display_unlock();

        if (scan_err != ESP_OK || count == 0)
        {
            // Nothing to pick from - wait a bit and scan again instead of
            // blocking forever on a tap that can't happen.
            free(aps);
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }

        wifi_ap_info_t *selected = NULL;
        xQueueReceive(wifi_ap_selected_queue, &selected, portMAX_DELAY);

        char *password = NULL;
        if (selected->secured)
        {
            bsp_display_lock(0);
            lv_textarea_set_text(ui_WifiPasswordInput, "");
            lv_obj_add_flag(ui_WifiList, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_WifiStatusLabel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ui_WifiPasswordBackBtn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ui_WifiPasswordInput, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ui_WifiKeyboard, LV_OBJ_FLAG_HIDDEN);
            bsp_display_unlock();

            xQueueReceive(wifi_password_submit_queue, &password, portMAX_DELAY);

            bsp_display_lock(0);
            lv_obj_add_flag(ui_WifiPasswordBackBtn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_WifiPasswordInput, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_WifiKeyboard, LV_OBJ_FLAG_HIDDEN);
            bsp_display_unlock();

            if (!password)
            {
                // Cancelled (wifiPasswordBackFn) - back to the list, don't
                // attempt anything.
                free(aps);
                continue;
            }
        }
        else
        {
            password = strdup("");
        }

        bsp_display_lock(0);
        lv_label_set_text(ui_WifiStatusLabel, "Conectando...");
        lv_obj_clear_flag(ui_WifiStatusLabel, LV_OBJ_FLAG_HIDDEN);
        bsp_display_unlock();

        // password is always non-NULL here: the secured-network branch above
        // already `continue`d past a cancel, and the open-network branch set
        // it to strdup("").
        esp_err_t err = wifi_manager_connect(selected->ssid, password);
        free(password);
        free(aps);

        if (err == ESP_OK)
        {
            return ESP_OK;
        }

        bsp_display_lock(0);
        lv_label_set_text(ui_WifiStatusLabel, "No se pudo conectar, reintentando...");
        bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
}
