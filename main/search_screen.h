#pragma once

#include "esp_err.h"

/**
 * @brief Creates the search-query/search-selection queues and the dedicated
 * search_task, and wires search_task_handle/search_query_queue/
 * search_selection_queue (app_globals.h). Call once from app_main(), after
 * spotify_client_init() has produced a valid `client`.
 *
 * @return ESP_OK on success, ESP_FAIL if a queue or the task couldn't be created.
 */
esp_err_t search_screen_init(void);
