#pragma once

/**
 * @brief Enables the Spotify player and runs the "now playing" event loop
 * forever (never returns). Call once from app_main(), after
 * spotify_client_init() has produced a valid `client` and ui_init() has
 * built ui_PlayerScreen.
 */
void player_screen_start(void);
