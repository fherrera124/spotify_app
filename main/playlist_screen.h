#pragma once

#include "esp_err.h"

/**
 * @brief Creates the playlist-selection queue and the dedicated
 * playlist_task, and wires playlist_task_handle/playlist_selection_queue
 * (app_globals.h). Call once from app_main(), after spotify_client_init()
 * has produced a valid `client`.
 *
 * @return ESP_OK on success, ESP_FAIL if the queue or task couldn't be created.
 */
esp_err_t playlist_screen_init(void);
