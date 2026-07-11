#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdbool.h>
#include <time.h>

#include "json_parser.h"
#include "spotify_client.h"

/* Exported macro --------------------------------------------------------------*/
/* Size of the caller-owned json_tok_t buffer every parse_* function below
 * expects; see esp_spotify_client's json_tokens field. */
#define MAX_TOKENS 1000

/* Exported types ------------------------------------------------------------*/

/* Globally scoped variables declarations ------------------------------------*/

/* Exported functions prototypes ---------------------------------------------*/
/* All parse_* functions below take a caller-owned json_tok_t[MAX_TOKENS]
 * scratch buffer instead of relying on internal shared state, so concurrent
 * callers (e.g. different esp_spotify_client instances) don't step on each
 * other. The caller must serialize its own uses of a given buffer. */
/* expires_in (seconds) is optional: pass NULL to ignore it, or a non-NULL
 * out-param that's set to the token's lifetime, or to 0 if the response
 * didn't include that field (caller should then only rely on reactive
 * (401-triggered) refresh, not proactive expiry tracking).
 * Returns ESP_OK only if "access_token" was found and fit in `size` (it's
 * left untouched otherwise, safe to keep using the previous value);
 * ESP_FAIL if the response was unparseable or didn't have that field
 * (e.g. a Discord rate-limit/error body instead of a token) - caller
 * should treat this the same as a failed HTTP request, not crash. */
esp_err_t      parse_access_token(const char* js, char* access_token, int size, json_tok_t *tokens, int *expires_in);
/* Returns ESP_OK if both "name" and "uri" were found; otherwise
 * playlist_item->name/uri are left NULL (caller should discard the item)
 * instead of crashing on a malformed/unexpected fragment. */
esp_err_t      parse_playlist(const char* js, PlaylistItem_t* playlist_item, json_tok_t *tokens);
/* Returns ESP_OK once the response itself parsed and had a "devices"
 * array, even if individual malformed entries inside it were skipped
 * (logged, not fatal); ESP_FAIL only if the whole response was
 * unparseable or missing "devices" entirely - caller should treat that
 * the same as a failed HTTP request. */
esp_err_t      parse_available_devices(const char* js, List*, json_tok_t *tokens);
/* Unlike the other parse_* functions, takes an explicit max_tokens instead
 * of assuming MAX_TOKENS: search responses are heavier than anything else
 * this component parses (full track objects with nested artists[]/album{}),
 * so callers own a bigger, on-demand scratch buffer just for this call (see
 * spotify_search_tracks(), player_commands.c) instead of the shared
 * per-client json_tokens[MAX_TOKENS]. Same "never abort on external data"
 * behavior as parse_available_devices: skips malformed entries instead of
 * failing the whole call; ESP_FAIL only if the response itself didn't parse
 * or was missing "tracks"/"items" entirely. */
esp_err_t      parse_search_results(const char* js, List* tracks_list, json_tok_t *tokens, int max_tokens);
void           parse_connection_id(const char* js, char** str, json_tok_t *tokens);
SpotifyEvent_t parse_track(const char* js, TrackInfo** track_info, int initial_state, json_tok_t *tokens);

#ifdef __cplusplus
}
#endif