/* Includes ------------------------------------------------------------------*/
#include "spotify_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "handler_callbacks.h"
#include "parse_objects.h"
#include "spotify_client_priv.h"
#include "spotify_utils.h"
#include "string_utils.h"
#include <ctype.h>
#include <string.h>

/* Private macro -------------------------------------------------------------*/
#define PLAYER "/me/player"
/* additional_types=episode dropped: parse_track() only understands the
 * track schema, not episode - would ERR_CHECK-crash on the first podcast.
 * Re-add once parse_track() handles episodes (ANALYSIS.md 1.4/3.3). */
#define PLAYER_STATE PLAYER "?market=from_token"
#define PLAY_TRACK PLAYER "/play"
#define PAUSE_TRACK PLAYER "/pause"
#define PREV_TRACK PLAYER "/previous"
#define NEXT_TRACK PLAYER "/next"
#define VOLUME PLAYER "/volume?volume_percent="
#define SEEK PLAYER "/seek?position_ms="
#define PLAYERURL(ENDPOINT) "https://api.spotify.com/v1" ENDPOINT
/* How many results to request from /v1/search - fits a touch-list, keeps
 * SEARCH_HTTP_BUF_SIZE/SEARCH_MAX_TOKENS below from needing to be huge
 * (ANALYSIS.md 3.7). */
#define SEARCH_RESULT_LIMIT 6
/* "market=from_token" makes Spotify omit each item's bulky
 * "available_markets" array and filters to what's actually playable
 * (ANALYSIS.md 3.7). Built at runtime via snprintf, not a macro - the
 * query is user-typed, percent-encoded text. */
#define SEARCH_URL_FMT PLAYERURL("/search?q=%s&type=track&limit=%d&market=from_token")
/* Max length of the percent-encoded query (encoding only ever grows the
 * raw typed text) - generous for an on-screen keyboard. */
#define SEARCH_QUERY_ENCODED_MAX 256
/* Dedicated, on-demand buffer/tokens for spotify_search_tracks() only -
 * full track objects are far bigger than anything else this component
 * parses, so reusing the shared 8KB/1000-token budget would silently
 * truncate results. Freed right after each call (ANALYSIS.md 1.23/3.7). */
#define SEARCH_HTTP_BUF_SIZE (40 * 1024)
#define SEARCH_MAX_TOKENS 4000

/* Private function prototypes -----------------------------------------------*/
static void free_track(TrackInfo *track_info);
static inline char *dup_or_null(const char *s);
static int url_encode(const char *str, char *out, size_t out_size);

/* Locally scoped variables --------------------------------------------------*/
static const char *TAG = "spotify_client";

/* Exported functions --------------------------------------------------------*/
esp_err_t spotify_play_context_uri(esp_spotify_client_handle_t client, const char *uri, HttpStatus_Code *status_code)
{
    esp_err_t err;
    HttpStatus_Code s_code = 0;
    if (access_token_needs_refresh(client) && (err = get_access_token(client)) != ESP_OK)
    {
        if (status_code)
        {
            *status_code = s_code;
        }
        return err;
    }
    ACQUIRE_LOCK(client->http_buf_lock);
    int str_len = snprintf(client->sprintf_buf, SPRINTF_BUF_SIZE, "{\"context_uri\":\"%s\"}", uri);
    if (str_len < 0 || str_len >= SPRINTF_BUF_SIZE)
    {
        // snprintf already stopped safely at the buffer boundary; bail out
        // instead of sending a truncated/malformed JSON body
        ESP_LOGE(TAG, "Context URI too long for buffer (uri len=%d)", (int)strlen(uri));
        RELEASE_LOCK(client->http_buf_lock);
        if (status_code)
        {
            *status_code = 0;
        }
        return ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_set_post_field(client->http_client.handle, client->sprintf_buf, str_len);
    client->http_client.http_event_cb = json_http_event_cb;
    err = perform_http_request(client, client->access_token.value, "application/json", PLAYERURL(PLAY_TRACK), HTTP_METHOD_PUT, &s_code);
    if (err == ESP_OK && s_code == HttpStatus_Unauthorized && get_access_token_locked(client) == ESP_OK)
    {
        // token expired/invalid mid-flight: refresh and retry once, same
        // post field (still set on the handle, unchanged since the first attempt)
        err = perform_http_request(client, client->access_token.value, "application/json", PLAYERURL(PLAY_TRACK), HTTP_METHOD_PUT, &s_code);
    }
    esp_http_client_set_post_field(client->http_client.handle, NULL, 0);
    if (status_code)
    {
        *status_code = s_code;
    }
    RELEASE_LOCK(client->http_buf_lock);
    return err;
}

/* Unlike spotify_play_context_uri (plays a context - playlist/album - as a
 * whole), this plays a single loose track: {"uris":[...]} instead of
 * {"context_uri":...} - needed for a track picked from search results
 * (ANALYSIS.md 3.7), which has no containing context. */
esp_err_t spotify_play_track_uri(esp_spotify_client_handle_t client, const char *uri, HttpStatus_Code *status_code)
{
    esp_err_t err;
    HttpStatus_Code s_code = 0;
    if (access_token_needs_refresh(client) && (err = get_access_token(client)) != ESP_OK)
    {
        if (status_code)
        {
            *status_code = s_code;
        }
        return err;
    }
    ACQUIRE_LOCK(client->http_buf_lock);
    int str_len = snprintf(client->sprintf_buf, SPRINTF_BUF_SIZE, "{\"uris\":[\"%s\"]}", uri);
    if (str_len < 0 || str_len >= SPRINTF_BUF_SIZE)
    {
        ESP_LOGE(TAG, "Track URI too long for buffer (uri len=%d)", (int)strlen(uri));
        RELEASE_LOCK(client->http_buf_lock);
        if (status_code)
        {
            *status_code = 0;
        }
        return ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_set_post_field(client->http_client.handle, client->sprintf_buf, str_len);
    client->http_client.http_event_cb = json_http_event_cb;
    err = perform_http_request(client, client->access_token.value, "application/json", PLAYERURL(PLAY_TRACK), HTTP_METHOD_PUT, &s_code);
    if (err == ESP_OK && s_code == HttpStatus_Unauthorized && get_access_token_locked(client) == ESP_OK)
    {
        err = perform_http_request(client, client->access_token.value, "application/json", PLAYERURL(PLAY_TRACK), HTTP_METHOD_PUT, &s_code);
    }
    esp_http_client_set_post_field(client->http_client.handle, NULL, 0);
    if (status_code)
    {
        *status_code = s_code;
    }
    RELEASE_LOCK(client->http_buf_lock);
    return err;
}

esp_err_t spotify_set_volume(esp_spotify_client_handle_t client, int volume_percent, HttpStatus_Code *status_code)
{
    esp_err_t err;
    HttpStatus_Code s_code = 0;
    if (volume_percent < 0)
    {
        volume_percent = 0;
    }
    else if (volume_percent > 100)
    {
        volume_percent = 100;
    }
    if (access_token_needs_refresh(client) && (err = get_access_token(client)) != ESP_OK)
    {
        if (status_code)
        {
            *status_code = s_code;
        }
        return err;
    }
    ACQUIRE_LOCK(client->http_buf_lock);
    snprintf(client->sprintf_buf, SPRINTF_BUF_SIZE, "%s%d", PLAYERURL(VOLUME), volume_percent);
    client->http_client.http_event_cb = json_http_event_cb;
    err = perform_http_request(client, client->access_token.value, "application/json", client->sprintf_buf, HTTP_METHOD_PUT, &s_code);
    if (err == ESP_OK && s_code == HttpStatus_Unauthorized && get_access_token_locked(client) == ESP_OK)
    {
        err = perform_http_request(client, client->access_token.value, "application/json", client->sprintf_buf, HTTP_METHOD_PUT, &s_code);
    }
    /* Optimistic update: Spotify's PUT /volume returns 204 with no body, so
     * there's nothing to parse the new value back out of. Only commit it on
     * success, so a failed call doesn't make Device.volume_percent lie about
     * what the device is actually set to. */
    if (err == ESP_OK && (s_code == HttpStatus_Ok || s_code == HTTP_STATUS_NO_CONTENT))
    {
        client->track_info->device.volume_percent = volume_percent;
    }
    if (status_code)
    {
        *status_code = s_code;
    }
    RELEASE_LOCK(client->http_buf_lock);
    return err;
}

esp_err_t spotify_seek_to_position(esp_spotify_client_handle_t client, int position_ms, HttpStatus_Code *status_code)
{
    esp_err_t err;
    HttpStatus_Code s_code = 0;
    if (position_ms < 0)
    {
        position_ms = 0;
    }
    if (access_token_needs_refresh(client) && (err = get_access_token(client)) != ESP_OK)
    {
        if (status_code)
        {
            *status_code = s_code;
        }
        return err;
    }
    ACQUIRE_LOCK(client->http_buf_lock);
    snprintf(client->sprintf_buf, SPRINTF_BUF_SIZE, "%s%d", PLAYERURL(SEEK), position_ms);
    client->http_client.http_event_cb = json_http_event_cb;
    err = perform_http_request(client, client->access_token.value, "application/json", client->sprintf_buf, HTTP_METHOD_PUT, &s_code);
    if (err == ESP_OK && s_code == HttpStatus_Unauthorized && get_access_token_locked(client) == ESP_OK)
    {
        err = perform_http_request(client, client->access_token.value, "application/json", client->sprintf_buf, HTTP_METHOD_PUT, &s_code);
    }
    // Same optimistic-update reasoning as spotify_set_volume: PUT /seek
    // returns 204 with no body.
    if (err == ESP_OK && (s_code == HttpStatus_Ok || s_code == HTTP_STATUS_NO_CONTENT))
    {
        client->track_info->progress_ms = position_ms;
    }
    if (status_code)
    {
        *status_code = s_code;
    }
    RELEASE_LOCK(client->http_buf_lock);
    return err;
}

esp_err_t spotify_transfer_playback(esp_spotify_client_handle_t client, const char *device_id, HttpStatus_Code *status_code)
{
    esp_err_t err;
    HttpStatus_Code s_code = 0;
    if (access_token_needs_refresh(client) && (err = get_access_token(client)) != ESP_OK)
    {
        if (status_code)
        {
            *status_code = s_code;
        }
        return err;
    }
    ACQUIRE_LOCK(client->http_buf_lock);
    // Deliberately no "play" field: per Spotify's API, omitting it keeps
    // whatever play/pause state playback was already in, just moves it to
    // the new device - confirmed with the user as the wanted behavior
    // (not forcing playback to start).
    int str_len = snprintf(client->sprintf_buf, SPRINTF_BUF_SIZE, "{\"device_ids\":[\"%s\"]}", device_id);
    if (str_len < 0 || str_len >= SPRINTF_BUF_SIZE)
    {
        ESP_LOGE(TAG, "Device id too long for buffer (len=%d)", (int)strlen(device_id));
        RELEASE_LOCK(client->http_buf_lock);
        if (status_code)
        {
            *status_code = 0;
        }
        return ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_set_post_field(client->http_client.handle, client->sprintf_buf, str_len);
    client->http_client.http_event_cb = json_http_event_cb;
    err = perform_http_request(client, client->access_token.value, "application/json", PLAYERURL(PLAYER), HTTP_METHOD_PUT, &s_code);
    if (err == ESP_OK && s_code == HttpStatus_Unauthorized && get_access_token_locked(client) == ESP_OK)
    {
        err = perform_http_request(client, client->access_token.value, "application/json", PLAYERURL(PLAYER), HTTP_METHOD_PUT, &s_code);
    }
    esp_http_client_set_post_field(client->http_client.handle, NULL, 0);
    if (status_code)
    {
        *status_code = s_code;
    }
    RELEASE_LOCK(client->http_buf_lock);
    return err;
}

List *spotify_user_playlists(esp_spotify_client_handle_t client)
{
    List *playlists = calloc(1, sizeof(List));
    if (!playlists)
    {
        ESP_LOGE(TAG, "Cannot allocate memory for playlists");
        return NULL;
    }
    playlists->type = PLAYLIST_LIST;
    if (access_token_needs_refresh(client) && get_access_token(client) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to obtain access token");
        free(playlists);
        return NULL;
    }
    ACQUIRE_LOCK(client->http_buf_lock);
    client->http_client.user_data.ctx = playlists; // pass the playlists as context to event handler
    client->http_client.http_event_cb = playlist_http_event_cb;
    // Defensive: playlist_http_event_cb's scratch state should already be
    // reset by the previous request's ON_FINISH/DISCONNECTED, but force a
    // clean slate here too so a second playlist fetch can never start from
    // state left over by an earlier one.
    client->http_client.user_data.current_size = 0;
    client->http_client.user_data.playlist_scan = (playlist_scan_state_t){0};
    HttpStatus_Code status_code;
    esp_err_t err = perform_http_request(client, client->access_token.value, "application/json", PLAYERURL("/me/playlists?offset=0&limit=50"), HTTP_METHOD_GET, &status_code);
    if (err == ESP_OK && status_code == HttpStatus_Unauthorized && get_access_token_locked(client) == ESP_OK)
    {
        err = perform_http_request(client, client->access_token.value, "application/json", PLAYERURL("/me/playlists?offset=0&limit=50"), HTTP_METHOD_GET, &status_code);
    }
    if (err != ESP_OK || status_code != HttpStatus_Ok)
    {
        if (err == ESP_OK)
        {
            ESP_LOGE(TAG, "Error. HTTP Status Code = %d", status_code);
        }
        free(playlists);
        playlists = NULL;
    }
    client->http_client.user_data.ctx = NULL;
    RELEASE_LOCK(client->http_buf_lock);
    return playlists;
}

List *spotify_available_devices(esp_spotify_client_handle_t client)
{
    List *devices = calloc(1, sizeof(List));
    if (!devices)
    {
        ESP_LOGE(TAG, "Cannot allocate memory for devices");
        return NULL;
    }
    devices->type = DEVICE_LIST;
    if (access_token_needs_refresh(client) && get_access_token(client) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to obtain access token");
        free(devices);
        return NULL;
    }
    ACQUIRE_LOCK(client->http_buf_lock);
    client->http_client.http_event_cb = json_http_event_cb;
    HttpStatus_Code status_code;
    esp_err_t err = perform_http_request(client, client->access_token.value, "application/json", PLAYERURL(PLAYER "/devices"), HTTP_METHOD_GET, &status_code);
    if (err == ESP_OK && status_code == HttpStatus_Unauthorized && get_access_token_locked(client) == ESP_OK)
    {
        err = perform_http_request(client, client->access_token.value, "application/json", PLAYERURL(PLAYER "/devices"), HTTP_METHOD_GET, &status_code);
    }
    if (err == ESP_OK && status_code == HttpStatus_Ok)
    {
        ESP_LOGD(TAG, "Active devices:\n%s", client->http_client.user_data.buffer);
        if (parse_available_devices((char *)(client->http_client.user_data.buffer), devices, client->json_tokens) != ESP_OK)
        {
            spotify_free_nodes(devices);
            free(devices);
            devices = NULL;
        }
    }
    else
    {
        if (err == ESP_OK)
        {
            ESP_LOGE(TAG, "Error. HTTP Status Code = %d", status_code);
        }
        free(devices);
        devices = NULL;
    }
    RELEASE_LOCK(client->http_buf_lock);
    return devices;
}

List *spotify_search_tracks(esp_spotify_client_handle_t client, const char *query)
{
    List *tracks = calloc(1, sizeof(List));
    if (!tracks)
    {
        ESP_LOGE(TAG, "Cannot allocate memory for search results");
        return NULL;
    }
    tracks->type = TRACK_LIST;
    if (access_token_needs_refresh(client) && get_access_token(client) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to obtain access token");
        free(tracks);
        return NULL;
    }

    char encoded_query[SEARCH_QUERY_ENCODED_MAX];
    if (url_encode(query, encoded_query, sizeof(encoded_query)) < 0)
    {
        ESP_LOGE(TAG, "Search query too long to encode (len=%d)", (int)strlen(query));
        free(tracks);
        return NULL;
    }
    // PLAYERURL(...) is a fixed prefix (~35 chars) + "&type=track&limit=" +
    // digits + "&market=from_token" (~45 chars) on top of the encoded query.
    char url[SEARCH_QUERY_ENCODED_MAX + 96];
    int url_len = snprintf(url, sizeof(url), SEARCH_URL_FMT, encoded_query, SEARCH_RESULT_LIMIT);
    if (url_len < 0 || url_len >= (int)sizeof(url))
    {
        ESP_LOGE(TAG, "Search URL too long");
        free(tracks);
        return NULL;
    }

    // Dedicated, on-demand buffer/tokens (see SEARCH_HTTP_BUF_SIZE/
    // SEARCH_MAX_TOKENS above) - freed at the end of this call, never held
    // onto like the shared per-client buffer/json_tokens are.
    uint8_t *search_buf = heap_caps_malloc(SEARCH_HTTP_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    json_tok_t *search_tokens = heap_caps_malloc(SEARCH_MAX_TOKENS * sizeof(json_tok_t), MALLOC_CAP_SPIRAM);
    if (!search_buf || !search_tokens)
    {
        ESP_LOGE(TAG, "Out of memory allocating search buffers");
        free(search_buf);
        free(search_tokens);
        free(tracks);
        return NULL;
    }

    ACQUIRE_LOCK(client->http_buf_lock);
    // Same buffer-swap technique as fetch_album_art: temporarily point the
    // client's HTTP buffer at our bigger, search-specific one instead of
    // growing the shared MAX_HTTP_BUFFER every other endpoint uses.
    uint8_t *buff_backup = client->http_client.user_data.buffer;
    size_t buff_size_backup = client->http_client.user_data.buffer_size;
    client->http_client.http_event_cb = json_http_event_cb;
    client->http_client.user_data.buffer = search_buf;
    client->http_client.user_data.buffer_size = SEARCH_HTTP_BUF_SIZE;

    HttpStatus_Code status_code;
    esp_err_t err = perform_http_request(client, client->access_token.value, "application/json", url, HTTP_METHOD_GET, &status_code);
    if (err == ESP_OK && status_code == HttpStatus_Unauthorized && get_access_token_locked(client) == ESP_OK)
    {
        err = perform_http_request(client, client->access_token.value, "application/json", url, HTTP_METHOD_GET, &status_code);
    }
    if (err == ESP_OK && status_code == HttpStatus_Ok)
    {
        if (parse_search_results((char *)search_buf, tracks, search_tokens, SEARCH_MAX_TOKENS) != ESP_OK)
        {
            spotify_free_nodes(tracks);
            free(tracks);
            tracks = NULL;
        }
    }
    else
    {
        if (err == ESP_OK)
        {
            ESP_LOGE(TAG, "Error. HTTP Status Code = %d", status_code);
        }
        free(tracks);
        tracks = NULL;
    }

    client->http_client.user_data.buffer = buff_backup;
    client->http_client.user_data.buffer_size = buff_size_backup;
    RELEASE_LOCK(client->http_buf_lock);

    free(search_buf);
    free(search_tokens);
    return tracks;
}

ssize_t fetch_album_art(esp_spotify_client_handle_t client, TrackInfo *track, uint8_t *out_buf, size_t buf_size)
{
    if (!out_buf)
    {
        ESP_LOGE(TAG, "Invalid buffer");
        return ESP_FAIL;
    }

    ACQUIRE_LOCK(client->http_buf_lock);
    if (!track->album.url_cover)
    {
        ESP_LOGE(TAG, "No cover url");
        RELEASE_LOCK(client->http_buf_lock);
        return ESP_FAIL;
    }
    uint8_t *buff_backup = client->http_client.user_data.buffer;
    size_t buff_size_backup = client->http_client.user_data.buffer_size;
    client->http_client.http_event_cb = default_http_event_cb;
    client->http_client.user_data.buffer = out_buf;
    client->http_client.user_data.buffer_size = buf_size;
    ssize_t data_read = ESP_FAIL;

    HttpStatus_Code status_code;
    esp_err_t err = perform_http_request(client, NULL, NULL, track->album.url_cover, HTTP_METHOD_GET, &status_code);
    if (err == ESP_OK)
    {
        int64_t length = esp_http_client_get_content_length(client->http_client.handle);
        ESP_LOGD(TAG, "content_length = %" PRId64, length);
        if (length > buf_size)
        {
            ESP_LOGE(TAG, "Image too big");
        }
        else if (status_code != HttpStatus_Ok)
        {
            ESP_LOGE(TAG, "Error trying to obtain cover. Status code: %d", status_code);
        }
        else
        {
            data_read = client->http_client.user_data.current_size;
        }
    }
    // restore the buffer
    client->http_client.user_data.buffer = buff_backup;
    client->http_client.user_data.buffer_size = buff_size_backup;
    RELEASE_LOCK(client->http_buf_lock);
    return data_read;
}

void spotify_clear_track(TrackInfo *track)
{
    if (!track)
    {
        return;
    }
    free_track(track);
    track->id[0] = 0;
    track->isPlaying = false;
    track->progress_ms = 0;
    track->duration_ms = 0;
}

esp_err_t spotify_clone_track(TrackInfo *dest, const TrackInfo *src)
{
    strcpy(dest->id, src->id);
    dest->name = dup_or_null(src->name);
    dest->album.name = dup_or_null(src->album.name);
    dest->album.url_cover = dup_or_null(src->album.url_cover);
    dest->album.cover_size = src->album.cover_size;
    dest->isPlaying = src->isPlaying;
    dest->progress_ms = src->progress_ms;
    dest->duration_ms = src->duration_ms;
    dest->device.volume_percent = src->device.volume_percent;
    /* dest->device.id = strdup(src->device.id);
    dest->device.type = strdup(src->device.type); */
    Node *node = src->artists.first;
    while (node)
    {
        char *artist = strdup((char *)node->data);
        if (!artist || !spotify_append_item_to_list(&dest->artists, (void *)artist))
        {
            ESP_LOGE(TAG, "Out of memory cloning artist list, clone will be partial");
            free(artist);
            break;
        }
        node = node->next;
    }
    return ESP_OK;
}

/* player_task.c */
bool bits_to_player_cmd(uint32_t bit, PlayerCommand_t *out_cmd)
{
    switch (bit)
    {
    case DO_PLAY:
        *out_cmd = PLAY;
        return true;
    case DO_PAUSE:
        *out_cmd = PAUSE;
        return true;
    case DO_PAUSE_UNPAUSE:
        *out_cmd = PAUSE_UNPAUSE;
        return true;
    case DO_PREVIOUS:
        *out_cmd = PREVIOUS;
        return true;
    case DO_NEXT:
        *out_cmd = NEXT;
        return true;
    default:
        return false;
    }
}

esp_err_t player_cmd(esp_spotify_client_handle_t client, PlayerCommand_t cmd, void *payload, HttpStatus_Code *status_code)
{
    esp_err_t err;
    HttpStatus_Code s_code = 0;
    ACQUIRE_LOCK(client->http_buf_lock);
    esp_http_client_method_t method = HTTP_METHOD_GET;
    const char *url = NULL;
    switch (cmd)
    {
    case PAUSE:
        method = HTTP_METHOD_PUT;
        url = PLAYERURL(PAUSE_TRACK);
        break;
    case PLAY:
        method = HTTP_METHOD_PUT;
        url = PLAYERURL(PLAY_TRACK);
        break;
    case PAUSE_UNPAUSE:
        method = HTTP_METHOD_PUT;
        if (client->track_info->isPlaying)
        {
            url = PLAYERURL(PAUSE_TRACK);
        }
        else
        {
            url = PLAYERURL(PLAY_TRACK);
        }
        break;
    case PREVIOUS:
        method = HTTP_METHOD_POST;
        url = PLAYERURL(PREV_TRACK);
        break;
    case NEXT:
        method = HTTP_METHOD_POST;
        url = PLAYERURL(NEXT_TRACK);
        break;
    case GET_STATE:
        method = HTTP_METHOD_GET;
        url = PLAYERURL(PLAYER_STATE);
        break;
    default:
        ESP_LOGE(TAG, "Unknow command: %d", cmd);
        err = ESP_FAIL;
        if (status_code)
        {
            *status_code = s_code;
        }
        RELEASE_LOCK(client->http_buf_lock);
        return err;
    }
    client->http_client.http_event_cb = json_http_event_cb;
    err = perform_http_request(client, client->access_token.value, "application/json", url, method, &s_code);
    if (err == ESP_OK)
    {
        ESP_LOGD(TAG, "%s", client->http_client.user_data.buffer);
        ESP_LOGD(TAG, "curr size %d", client->http_client.user_data.current_size);
        /* Swap play<->pause and retry exactly once: a non-Premium account (or
         * any persistent condition) may return 403 on both endpoints, so this
         * must not loop back on itself (unlike the goto-based version this
         * replaced, which could hang player_task forever holding
         * http_buf_lock). */
        if (s_code == HttpStatus_Forbidden && cmd == PAUSE_UNPAUSE)
        {
            url = (strcmp(url, PLAYERURL(PAUSE_TRACK)) == 0) ? PLAYERURL(PLAY_TRACK) : PLAYERURL(PAUSE_TRACK);
            err = perform_http_request(client, client->access_token.value, "application/json", url, method, &s_code);
        }
    }
    if (status_code)
    {
        *status_code = s_code;
    }
    RELEASE_LOCK(client->http_buf_lock);
    return err;
}

/* Private functions ---------------------------------------------------------*/
/* Percent-encodes str into out (RFC 3986 unreserved chars pass through
 * as-is, everything else becomes %XX) for embedding as a URL query value -
 * needed for spotify_search_tracks()'s free-text query (spaces, accents,
 * "&"/"?" etc. would otherwise corrupt the URL or get interpreted as extra
 * query params). Neither ESP-IDF's http_utils nor anything else in this
 * component already does this (checked before adding it).
 * Returns the encoded length on success, or -1 if it wouldn't fit in
 * out_size (out is left in a partial, unterminated state in that case -
 * callers must treat a -1 return as "don't use out"). */
static int url_encode(const char *str, char *out, size_t out_size)
{
    static const char *hex = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; str[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char)str[i];
        bool unreserved = isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        size_t needed = unreserved ? 1 : 3;
        if (j + needed >= out_size) // leave room for the NUL terminator
        {
            return -1;
        }
        if (unreserved)
        {
            out[j++] = (char)c;
        }
        else
        {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 0xF];
        }
    }
    out[j] = '\0';
    return (int)j;
}

/* strdup(NULL) is undefined behavior; album.url_cover in particular is
 * legitimately NULL whenever none of the album's images is exactly 300px
 * (see parse_track), so callers can't assume these fields are non-NULL. */
static inline char *dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

static inline void free_track(TrackInfo *track)
{
    if (!track)
    {
        return;
    }
    if (track->name)
    {
        free(track->name);
        track->name = NULL;
    }
    if (track->album.name)
    {
        free(track->album.name);
        track->album.name = NULL;
    }
    if (track->album.url_cover)
    {
        free(track->album.url_cover);
        track->album.url_cover = NULL;
        track->album.cover_size = 0;
    }
    if (track->artists.first)
    {
        spotify_free_nodes(&track->artists);
    }
    if (track->device.id)
    {
        free(track->device.id);
        track->device.id = NULL;
    }
    if (track->device.name)
    {
        free(track->device.name);
        track->device.name = NULL;
    }
    if (track->device.type)
    {
        free(track->device.type);
        track->device.type = NULL;
    }
    track->device.volume_percent = -1;
}
