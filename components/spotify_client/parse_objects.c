/* Includes ------------------------------------------------------------------*/
#include "parse_objects.h"
#include "esp_log.h"
#include "json_parser.h"
#include "spotify_client_priv.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Private macro -------------------------------------------------------------*/
// early check of unrecoverable error
#define ERR_CHECK(x) ESP_ERROR_CHECK(x)

/* Private types -------------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/
static void parse_device_volume(jparse_ctx_t *jctx, TrackInfo *track);

/* Locally scoped variables --------------------------------------------------*/
static const char* TAG = "PARSE_OBJECT";

/* Globally scoped variables definitions -------------------------------------*/

/* Exported functions --------------------------------------------------------*/
esp_err_t parse_access_token(const char* js, char* access_token, int size, json_tok_t *tokens, int *expires_in)
{
    jparse_ctx_t jctx;
    // Not ERR_CHECK: Discord's response is external data (rate-limit/error
    // bodies etc. are plausible) - must not crash the device on a
    // missing/malformed field (ANALYSIS.md 3.2).
    if (json_parse_start_static(&jctx, js, strlen(js), tokens, MAX_TOKENS) != OS_SUCCESS)
    {
        ESP_LOGE(TAG, "Failed to parse access token response JSON:\n%s", js);
        return ESP_FAIL;
    }
    if (json_obj_get_string(&jctx, "access_token", access_token, size) != OS_SUCCESS)
    {
        ESP_LOGE(TAG, "\"access_token\" missing from access token response:\n%s", js);
        json_parse_end_static(&jctx);
        return ESP_FAIL;
    }
    if (expires_in)
    {
        if (json_obj_get_int(&jctx, "expires_in", expires_in) != OS_SUCCESS)
        {
            ESP_LOGW(TAG, "\"expires_in\" missing from access token response, only reactive refresh will work");
            *expires_in = 0;
        }
    }
    json_parse_end_static(&jctx);
    return ESP_OK;
}

esp_err_t parse_available_devices(const char* js, List* devices_list, json_tok_t *tokens)
{
    jparse_ctx_t jctx;
    // Not ERR_CHECK anywhere: malformed data means a shorter/empty list,
    // not a crashed device (ANALYSIS.md 1.21, same reasoning as
    // parse_access_token).
    if (json_parse_start_static(&jctx, js, strlen(js), tokens, MAX_TOKENS) != OS_SUCCESS)
    {
        ESP_LOGE(TAG, "Failed to parse available devices response JSON:\n%s", js);
        return ESP_FAIL;
    }
    int num_elem;
    if (json_obj_get_array(&jctx, "devices", &num_elem) != OS_SUCCESS)
    {
        ESP_LOGE(TAG, "\"devices\" array missing from available devices response:\n%s", js);
        json_parse_end_static(&jctx);
        return ESP_FAIL;
    }
    for (int i = 0; i < num_elem; i++) {
        if (json_arr_get_object(&jctx, i) != OS_SUCCESS) {
            ESP_LOGE(TAG, "Device %d in \"devices\" array isn't an object, skipping it", i);
            continue;
        }
        // calloc, not malloc: name/id/is_active must start NULL/false so
        // that if name or id fails to parse below, freeing whichever of
        // the two DID get allocated is safe (free(NULL) is a no-op)
        // instead of freeing an uninitialized garbage pointer.
        DeviceItem_t* item = calloc(1, sizeof(*item));
        if (!item) {
            ESP_LOGE(TAG, "Out of memory allocating device item, truncating device list");
            json_arr_leave_object(&jctx);
            break;
        }
        if (json_obj_dup_string(&jctx, "name", &item->name) != OS_SUCCESS ||
            json_obj_dup_string(&jctx, "id", &item->id) != OS_SUCCESS) {
            ESP_LOGE(TAG, "\"name\"/\"id\" missing from a device entry, skipping it");
            free(item->name);
            free(item->id);
            free(item);
            json_arr_leave_object(&jctx);
            continue;
        }
        // Defensive, not ERR_CHECK: "is_active" is only used to highlight
        // the current device in a picker UI, not worth discarding the
        // whole entry over if a future response shape ever omits it.
        if (json_obj_get_bool(&jctx, "is_active", &item->is_active) != OS_SUCCESS) {
            item->is_active = false;
        }
        if (!spotify_append_item_to_list(devices_list, (void*)item)) {
            ESP_LOGE(TAG, "Out of memory appending device item, truncating device list");
            free(item->name);
            free(item->id);
            free(item);
            json_arr_leave_object(&jctx);
            break;
        }
        json_arr_leave_object(&jctx);
    }
    json_parse_end_static(&jctx);
    return ESP_OK;
}

esp_err_t parse_playlist(const char* js, PlaylistItem_t* playlist_item, json_tok_t *tokens)
{
    playlist_item->name = NULL;
    playlist_item->uri = NULL;

    jparse_ctx_t jctx;
    if (json_parse_start_static(&jctx, js, strlen(js), tokens, MAX_TOKENS) != OS_SUCCESS)
    {
        ESP_LOGE(TAG, "Failed to parse playlist item JSON, skipping it:\n%s", js);
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    if (json_obj_dup_string(&jctx, "name", &playlist_item->name) != OS_SUCCESS)
    {
        ESP_LOGE(TAG, "\"name\" missing from playlist item, skipping it:\n%s", js);
        err = ESP_FAIL;
    }
    else if (json_obj_dup_string(&jctx, "uri", &playlist_item->uri) != OS_SUCCESS)
    {
        ESP_LOGE(TAG, "\"uri\" missing from playlist item, skipping it:\n%s", js);
        err = ESP_FAIL;
    }
    json_parse_end_static(&jctx);
    return err;
}

esp_err_t parse_search_results(const char* js, List* tracks_list, json_tok_t *tokens, int max_tokens)
{
    jparse_ctx_t jctx;
    // Same "never abort on external data" reasoning as parse_available_devices.
    if (json_parse_start_static(&jctx, js, strlen(js), tokens, max_tokens) != OS_SUCCESS)
    {
        ESP_LOGE(TAG, "Failed to parse search results response JSON");
        return ESP_FAIL;
    }
    if (json_obj_get_object(&jctx, "tracks") != OS_SUCCESS)
    {
        ESP_LOGE(TAG, "\"tracks\" object missing from search response");
        json_parse_end_static(&jctx);
        return ESP_FAIL;
    }
    int num_elem;
    if (json_obj_get_array(&jctx, "items", &num_elem) != OS_SUCCESS)
    {
        ESP_LOGE(TAG, "\"items\" array missing from search response");
        json_parse_end_static(&jctx);
        return ESP_FAIL;
    }
    for (int i = 0; i < num_elem; i++) {
        if (json_arr_get_object(&jctx, i) != OS_SUCCESS) {
            ESP_LOGE(TAG, "Track %d in \"items\" isn't an object, skipping it", i);
            continue;
        }
        // calloc, not malloc: name/uri/artists must start NULL so a partial
        // failure below can free whichever of the three DID get allocated
        // without touching uninitialized garbage.
        TrackSearchItem_t* item = calloc(1, sizeof(*item));
        if (!item) {
            ESP_LOGE(TAG, "Out of memory allocating search result item, truncating results");
            json_arr_leave_object(&jctx);
            break;
        }
        if (json_obj_dup_string(&jctx, "name", &item->name) != OS_SUCCESS ||
            json_obj_dup_string(&jctx, "uri", &item->uri) != OS_SUCCESS) {
            ESP_LOGW(TAG, "\"name\"/\"uri\" missing from a search result, skipping it");
            free(item->name);
            free(item->uri);
            free(item);
            json_arr_leave_object(&jctx);
            continue;
        }
        // Join every artist's "name" into item->artists (", "-separated) -
        // defensive, not fatal if "artists" is missing/malformed: a track
        // without a visible artist list is still worth showing/playing.
        int num_artists;
        if (json_obj_get_array(&jctx, "artists", &num_artists) == OS_SUCCESS) {
            for (int a = 0; a < num_artists; a++) {
                if (json_arr_get_object(&jctx, a) != OS_SUCCESS) {
                    continue;
                }
                char* artist_name;
                if (json_obj_dup_string(&jctx, "name", &artist_name) == OS_SUCCESS) {
                    if (!item->artists) {
                        item->artists = artist_name;
                    } else {
                        size_t old_len = strlen(item->artists);
                        size_t add_len = strlen(artist_name);
                        char* joined = realloc(item->artists, old_len + 2 + add_len + 1);
                        if (joined) {
                            memcpy(joined + old_len, ", ", 2);
                            memcpy(joined + old_len + 2, artist_name, add_len + 1);
                            item->artists = joined;
                        }
                        free(artist_name);
                    }
                }
                json_arr_leave_object(&jctx);
            }
            json_obj_leave_array(&jctx);
        }
        if (!spotify_append_item_to_list(tracks_list, (void*)item)) {
            ESP_LOGE(TAG, "Out of memory appending search result item, truncating results");
            free(item->name);
            free(item->uri);
            free(item->artists);
            free(item);
            json_arr_leave_object(&jctx);
            break;
        }
        json_arr_leave_object(&jctx);
    }
    json_parse_end_static(&jctx);
    return ESP_OK;
}

void parse_connection_id(const char* js, char** data, json_tok_t *tokens)
{
    jparse_ctx_t jctx;
    ERR_CHECK(json_parse_start_static(&jctx, js, strlen(js), tokens, MAX_TOKENS));
    ERR_CHECK(json_obj_get_object(&jctx, "headers"));
    ERR_CHECK(json_obj_dup_string(&jctx, "Spotify-Connection-Id", data));
    json_parse_end_static(&jctx);
}

SpotifyEvent_t parse_track(const char* js, TrackInfo** track, int initial_state, json_tok_t *tokens)
{
    // ESP_LOGW(TAG, "%s", js);
    assert(track && *track);

    SpotifyEvent_t spotify_evt = { .player_event = UNKNOW };

    jparse_ctx_t jctx;
    ERR_CHECK(json_parse_start_static(&jctx, js, strlen(js), tokens, MAX_TOKENS));

    if (initial_state) {
        // this function was called for the purpose of initial state,
        // that is, a request via http was made, not really an event from ws
        goto initial_state;
    }

    bool is_event;
    if (json_obj_match_string(&jctx, "uri", "wss://event", &is_event)) {
        ESP_LOGD(TAG, "\"uri\" key not found or its content is not a string:\n%s", js);
        spotify_evt.player_event = UNKNOW;
        return spotify_evt;
    }
    if (!is_event) {
        ESP_LOGD(TAG, "Ignoring non-event WS push (\"uri\" != \"wss://event\"):\n%s", js);
        spotify_evt.player_event = UNKNOW;
        return spotify_evt;
    }

    int num_elem;
    if (json_obj_get_array(&jctx, "payloads", &num_elem) != OS_SUCCESS ||
        json_arr_get_object(&jctx, 0) != OS_SUCCESS) {
        ESP_LOGW(TAG, "\"wss://event\" message without a usable \"payloads[0]\" object:\n%s", js);
        spotify_evt.player_event = UNKNOW;
        return spotify_evt;
    }
    if (json_obj_get_array(&jctx, "events", &num_elem) != OS_SUCCESS) {
        ESP_LOGE(TAG, "\"events\" array is missing:\n%s", js);
        spotify_evt.player_event = UNKNOW;
        return spotify_evt;
        // here we can debug and get useful info that we can treat as events
    }
    if (num_elem == 0) {
        ESP_LOGE(TAG, "\"events\" array is empty:\n%s", js);
        spotify_evt.player_event = UNKNOW;
        return spotify_evt;
    }
    if (num_elem > 1) {
        ESP_LOGW(TAG, "\"events\" array has more than a element:\n%s", js);
    }
    if (json_arr_get_object(&jctx, 0) != OS_SUCCESS) {
        ESP_LOGE(TAG, "\"events\" array first element isn't an object:\n%s", js);
        spotify_evt.player_event = UNKNOW;
        return spotify_evt;
    }
    // json_tok_t* t = jctx.cur;
    // printf("token content: %.*s\n", (int)t->end - t->start, js + t->start);

    bool match;
    if (json_obj_match_string(&jctx, "type", "DEVICE_STATE_CHANGED", &match)) {
        ESP_LOGE(TAG, "\"type\" key not found or its content is not a string:\n%s", js);
        spotify_evt.player_event = UNKNOW;
        return spotify_evt;
    }
    if (match) {
        // Spotify's own "device state changed" WS message. Not a distinct
        // PlayerEvent_t case (removed, see spotify_client.h): nothing anywhere
        // does anything with it, so it fell through to the same `default:`
        // as UNKNOW anyway - this log line already gives the only
        // observable trace of it happening.
        ESP_LOGW(TAG, "Device state changed:\n%s", js);
        spotify_evt.player_event = UNKNOW;
        return spotify_evt;
    }
    ERR_CHECK(json_obj_match_string(&jctx, "type", "PLAYER_STATE_CHANGED", &match));
    // TODO: continue relaxing the code by printing useful info of the error before returning
    if (match) {
        ERR_CHECK(json_obj_get_object(&jctx, "event"));
        ERR_CHECK(json_obj_get_object(&jctx, "state"));
    initial_state:
        ERR_CHECK(json_obj_get_object(&jctx, "item"));
        ERR_CHECK(json_obj_match_string(&jctx, "id", (*track)->id, &match));
        if (match) {
            ERR_CHECK(json_obj_leave_object(&jctx));
            spotify_evt.player_event = SAME_TRACK;
            spotify_evt.payload = *track;
            int64_t progress;
            ERR_CHECK(json_obj_get_int64(&jctx, "progress_ms", &progress));
            if (progress != (*track)->progress_ms) {
                (*track)->progress_ms = progress;
            }
            bool is_playing;
            ERR_CHECK(json_obj_get_bool(&jctx, "is_playing", &is_playing));
            if (is_playing != (*track)->isPlaying) {
                (*track)->isPlaying = is_playing;
            }
            parse_device_volume(&jctx, *track);
        } else {
            spotify_evt.player_event = NEW_TRACK;
            spotify_evt.payload = *track;
            spotify_clear_track(*track);
            ERR_CHECK(json_obj_get_string(&jctx, "id", (*track)->id, SPOTIFY_ID_BUF_SIZE));
            ERR_CHECK(json_obj_dup_string(&jctx, "name", &(*track)->name));
            ERR_CHECK(json_obj_get_int64(&jctx, "duration_ms", &(*track)->duration_ms));
            ERR_CHECK(json_obj_get_array(&jctx, "artists", &num_elem));
            for (int i = 0; i < num_elem; i++) {
                ERR_CHECK(json_arr_get_object(&jctx, i));
                char* artist_name;
                ERR_CHECK(json_obj_dup_string(&jctx, "name", &artist_name));
                if (!spotify_append_item_to_list(&(*track)->artists, artist_name)) {
                    ESP_LOGE(TAG, "Out of memory appending artist, track will have a partial artist list");
                    free(artist_name);
                    ERR_CHECK(json_arr_leave_object(&jctx));
                    break;
                }
                ERR_CHECK(json_arr_leave_object(&jctx));
            }
            ERR_CHECK(json_obj_leave_array(&jctx));
            ERR_CHECK(json_obj_get_object(&jctx, "album"));
            ERR_CHECK(json_obj_dup_string(&jctx, "name", &(*track)->album.name));
            ERR_CHECK(json_obj_get_array(&jctx, "images", &num_elem));
            // Pick the image closest to ALBUM_COVER_PREFERRED_SIZE instead of
            // requiring an exact match (episodes and some releases don't
            // offer every size) - first pass just finds the best index/height,
            // second pass re-enters that one index to dup its "url" (jsmn's
            // parent-linked tokens support random access, so this is safe).
            int best_idx = -1, best_height = 0, best_diff = INT_MAX;
            for (int i = 0; i < num_elem; i++) {
                ERR_CHECK(json_arr_get_object(&jctx, i));
                int h;
                ERR_CHECK(json_obj_get_int(&jctx, "height", &h));
                int diff = abs(h - ALBUM_COVER_PREFERRED_SIZE);
                if (diff < best_diff) {
                    best_diff = diff;
                    best_idx = i;
                    best_height = h;
                }
                ERR_CHECK(json_arr_leave_object(&jctx));
                if (diff == 0) {
                    break;
                }
            }
            if (best_idx >= 0) {
                ERR_CHECK(json_arr_get_object(&jctx, best_idx));
                ERR_CHECK(json_obj_dup_string(&jctx, "url", &(*track)->album.url_cover));
                ERR_CHECK(json_arr_leave_object(&jctx));
                (*track)->album.cover_size = best_height;
                if (best_height != ALBUM_COVER_PREFERRED_SIZE) {
                    ESP_LOGW(TAG, "No %dpx cover among %d image(s) for track \"%s\"; using closest available (%dpx)",
                             ALBUM_COVER_PREFERRED_SIZE, num_elem, (*track)->name, best_height);
                }
            } else {
                ESP_LOGW(TAG, "No usable cover image among %d image(s) for track \"%s\"",
                         num_elem, (*track)->name);
            }
            ERR_CHECK(json_obj_leave_array(&jctx));
            ERR_CHECK(json_obj_leave_object(&jctx));
            ERR_CHECK(json_obj_leave_object(&jctx));
            ERR_CHECK(json_obj_get_int64(&jctx, "progress_ms", &(*track)->progress_ms));
            ERR_CHECK(json_obj_get_bool(&jctx, "is_playing", &(*track)->isPlaying));
            parse_device_volume(&jctx, *track);
        }

        return spotify_evt;
    }
    // unknow event
    spotify_evt.player_event = UNKNOW;
    return spotify_evt;
}

/* Private functions ---------------------------------------------------------*/
/* "device" is a sibling of "item" at the state level (both branches of
 * parse_track() are back there after leaving "item"). Some payloads omit
 * "device"/"volume_percent" (e.g. a device without volume control) - skip
 * silently rather than crash (ANALYSIS.md 3.2). volume_percent is left
 * untouched (-1 if never seen, else the last known value) when either key
 * is missing. */
static void parse_device_volume(jparse_ctx_t *jctx, TrackInfo *track)
{
    if (json_obj_get_object(jctx, "device") != OS_SUCCESS) {
        ESP_LOGI(TAG, "parse_device_volume: no \"device\" object in this payload");
        return;
    }
    int volume_percent;
    if (json_obj_get_int(jctx, "volume_percent", &volume_percent) == OS_SUCCESS) {
        // ESP_LOGI (not D): "PARSE_OBJECT" isn't bumped to DEBUG in main.c,
        // only "spotify_client" is - temporary diagnostic logging for the
        // "does a WS push from another client's volume change carry an
        // updated volume_percent too" question, remove once answered.
        ESP_LOGI(TAG, "parse_device_volume: volume_percent=%d (was %d)", volume_percent, track->device.volume_percent);
        track->device.volume_percent = volume_percent;
    }
    else
    {
        ESP_LOGI(TAG, "parse_device_volume: \"device\" present but no \"volume_percent\"");
    }
    json_obj_leave_object(jctx);
}

/* static void onDevicePlaying(const char* js)
{
    TrackInfo* track = (TrackInfo*)obj;

    jsmntok_t* device = object_get_member(js, root, "device");
    assert(device && "key \"device\" missing");

    jsmntok_t* value = object_get_member(js, device, "id");
    assert(value && "key \"id\" missing");

    track->device.id = jsmn_obj_dup(js, value);
    assert(track->device.id && "Error allocating memory");

    value = object_get_member(js, device, "name");
    assert(value && "key \"name\" missing");

    track->device.name = jsmn_obj_dup(js, value);
    assert(track->device.name && "Error allocating memory");

    value = object_get_member(js, device, "volume_percent");
    assert(value && "key \"volume_percent\" missing");

    snprintf(track->device.volume_percent, 4, "%s\n", js + value->start);

    ESP_LOGD(TAG, "Device id: %s, name: %s", track->device.id, track->device.name);
} */

/**
 * @brief u8g2 selection list menu uses a string with '\\n' as
 * item separator. For example: 'item1\\nitem2\\ngo to Menu\\nEtc...'.
 * This function build that string with each playlist name.
 *
 */
/* esp_err_t static str_append(jsmntok_t* obj, const char* buf, char** str)
{
    if (*str == NULL) {
        *str = jsmn_obj_dup(buf, obj);
        return (*str == NULL) ? ESP_ERR_NO_MEM : ESP_OK;
    }

    uint16_t obj_len = obj->end - obj->start;
    uint16_t str_len = strlen(*str);

    char* r = realloc(*str, str_len + obj_len + 2);
    if (r == NULL)
        return ESP_ERR_NO_MEM;

    *str = r;

    (*str)[str_len++] = '\n';

    for (uint16_t i = 0; i < obj_len; i++) {
        (*str)[i + str_len] = *(buf + obj->start + i);
    }
    (*str)[str_len + obj_len] = '\0';

    ESP_LOGI(TAG, "str len: %d", strlen(*str));

    return ESP_OK;
} */
