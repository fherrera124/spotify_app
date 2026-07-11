#pragma once

#include "esp_err.h"

/**
 * @brief Drives ui_WifiScreen (scan -> list -> optional password entry ->
 * connect) until the device is actually online, then returns. Unlike
 * playlist/device/search's task+queue screens, this runs directly on the
 * calling task (blocking) instead of spawning a dedicated one - it's a
 * one-shot boot-time flow, not a screen the user repeatedly reopens, and
 * the caller (app_main(), main.c) has nothing else to do while it waits.
 *
 * wifi_manager_init() must have already been called. Creates
 * wifi_ap_selected_queue/wifi_password_submit_queue (app_globals.h) if not
 * already created.
 *
 * @return ESP_OK once connected. ESP_FAIL only if the queues themselves
 * couldn't be created (out of memory) - otherwise loops internally on
 * connection failures rather than giving up, since there is no fallback
 * screen to hand control to (this app has nothing useful to do offline).
 */
esp_err_t wifi_screen_run_until_connected(void);
