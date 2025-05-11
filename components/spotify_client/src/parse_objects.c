/* Includes ------------------------------------------------------------------*/
#include "parse_objects.h"
#include "esp_log.h"
#include "json_parser.h"
#include "spotify_client_priv.h"
#include <stdlib.h>
#include <string.h>

/* Private macro -------------------------------------------------------------*/
#define MAX_TOKENS 1000

// early check of unrecoverable error
#define ERR_CHECK(x) ESP_ERROR_CHECK(x)

/* Private types -------------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/

/* Locally scoped variables --------------------------------------------------*/
static const char* TAG = "PARSE_OBJECT";
static json_tok_t  tokens[MAX_TOKENS];

/* Globally scoped variables definitions -------------------------------------*/

/* Exported functions --------------------------------------------------------*/
void parse_access_token(const char* js, char* access_token, int size)
{
    jparse_ctx_t jctx;
    ERR_CHECK(json_parse_start_static(&jctx, js, strlen(js), tokens, MAX_TOKENS));
    ERR_CHECK(json_obj_get_string(&jctx, "access_token", access_token, size));
    json_parse_end_static(&jctx);
}

void parse_available_devices(const char* js, List* devices_list)
{
    jparse_ctx_t jctx;
    ERR_CHECK(json_parse_start_static(&jctx, js, strlen(js), tokens, MAX_TOKENS));
    int num_elem;
    ERR_CHECK(json_obj_get_array(&jctx, "devices", &num_elem));
    for (int i = 0; i < num_elem; i++) {
        json_arr_get_object(&jctx, i);
        DeviceItem_t* item = malloc(sizeof(*item));
        assert(item);
        ERR_CHECK(json_obj_dup_string(&jctx, "name", &item->name));
        ERR_CHECK(json_obj_dup_string(&jctx, "id", &item->id));
        assert(spotify_append_item_to_list(devices_list, (void*)item));
        ERR_CHECK(json_arr_leave_object(&jctx));
    }
    json_parse_end_static(&jctx);
}

void parse_playlist(const char* js, PlaylistItem_t* playlist_item)
{
    jparse_ctx_t jctx;
    ERR_CHECK(json_parse_start_static(&jctx, js, strlen(js), tokens, MAX_TOKENS));
    ERR_CHECK(json_obj_dup_string(&jctx, "name", &playlist_item->name));
    ERR_CHECK(json_obj_dup_string(&jctx, "uri", &playlist_item->uri));
    json_parse_end_static(&jctx);
}

void parse_connection_id(const char* js, char** data)
{
    jparse_ctx_t jctx;
    ERR_CHECK(json_parse_start_static(&jctx, js, strlen(js), tokens, MAX_TOKENS));
    ERR_CHECK(json_obj_get_object(&jctx, "headers"));
    ERR_CHECK(json_obj_dup_string(&jctx, "Spotify-Connection-Id", data));
    json_parse_end_static(&jctx);
}

SpotifyEvent_t parse_track(const char* js, TrackInfo** track, int initial_state)
{
    // ESP_LOGW(TAG, "%s", js);
    assert(track && *track);

    SpotifyEvent_t spotify_evt = { .type = UNKNOW };

    jparse_ctx_t jctx;
    ERR_CHECK(json_parse_start_static(&jctx, js, strlen(js), tokens, MAX_TOKENS));

    if (initial_state) {
        // this function was called for the purpose of initial state,
        // that is, a request via http was made, not really an event from ws
        goto initial_state;
    }

    int num_elem;
    if (json_obj_get_array(&jctx, "payloads", &num_elem) != OS_SUCCESS) {
        ESP_LOGE(TAG, "\"payloads\" array is missing:\n%s", js);
        spotify_evt.type = UNKNOW;
        return spotify_evt;
    }
    if (num_elem == 0) {
        ESP_LOGE(TAG, "\"payloads\" array is empty:\n%s", js);
        spotify_evt.type = UNKNOW;
        return spotify_evt;
    }
    if (num_elem > 1) {
        ESP_LOGW(TAG, "\"payloads\" array has more than a element:\n%s", js);
    }
    if (json_arr_get_object(&jctx, 0) != OS_SUCCESS) {
        int strlen;
        if (json_arr_get_strlen(&jctx, 0, &strlen) != OS_SUCCESS) {
            ESP_LOGE(TAG, "\"payloads\" array first element isn't an object nor a string:\n%s", js);
        } else {
            ESP_LOGE(TAG, "\"payloads\" array first element is a string:\n%s", js);
        }
        spotify_evt.type = UNKNOW;
        return spotify_evt;
    }
    if (json_obj_get_array(&jctx, "events", &num_elem) != OS_SUCCESS) {
        ESP_LOGE(TAG, "\"events\" array is missing:\n%s", js);
        spotify_evt.type = UNKNOW;
        return spotify_evt;
        // here we can debug and get useful info that we can treat as events
    }
    if (num_elem == 0) {
        ESP_LOGE(TAG, "\"events\" array is empty:\n%s", js);
        spotify_evt.type = UNKNOW;
        return spotify_evt;
    }
    if (num_elem > 1) {
        ESP_LOGW(TAG, "\"events\" array has more than a element:\n%s", js);
    }
    if (json_arr_get_object(&jctx, 0) != OS_SUCCESS) {
        ESP_LOGE(TAG, "\"events\" array first element isn't an object:\n%s", js);
        spotify_evt.type = UNKNOW;
        return spotify_evt;
    }
    // json_tok_t* t = jctx.cur;
    // printf("token content: %.*s\n", (int)t->end - t->start, js + t->start);

    bool match;
    if (json_obj_match_string(&jctx, "type", "DEVICE_STATE_CHANGED", &match)) {
        ESP_LOGE(TAG, "\"type\" key not found or its content is not a string:\n%s", js);
        spotify_evt.type = UNKNOW;
        return spotify_evt;
    }
    if (match) {
        // TODO: manage this event
        ESP_LOGW(TAG, "Device state changed:\n%s", js);
        spotify_evt.type = DEVICE_STATE_CHANGED;
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
            spotify_evt.type = SAME_TRACK;
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
            // volume...
        } else {
            spotify_evt.type = NEW_TRACK;
            spotify_evt.payload = *track;
            spotify_clear_track(*track);
            ERR_CHECK(json_obj_get_string(&jctx, "id", (*track)->id, 30));
            ERR_CHECK(json_obj_dup_string(&jctx, "name", &(*track)->name));
            ERR_CHECK(json_obj_get_int64(&jctx, "duration_ms", &(*track)->duration_ms));
            ERR_CHECK(json_obj_get_array(&jctx, "artists", &num_elem));
            for (int i = 0; i < num_elem; i++) {
                ERR_CHECK(json_arr_get_object(&jctx, i));
                char* artist_name;
                ERR_CHECK(json_obj_dup_string(&jctx, "name", &artist_name));
                assert(spotify_append_item_to_list(&(*track)->artists, artist_name));
                ERR_CHECK(json_arr_leave_object(&jctx));
            }
            ERR_CHECK(json_obj_leave_array(&jctx));
            ERR_CHECK(json_obj_get_object(&jctx, "album"));
            ERR_CHECK(json_obj_dup_string(&jctx, "name", &(*track)->album.name));
            ERR_CHECK(json_obj_get_array(&jctx, "images", &num_elem));
            int h;
            for (int i = 0; i < num_elem; i++) {
                ERR_CHECK(json_arr_get_object(&jctx, i));
                ERR_CHECK(json_obj_get_int(&jctx, "height", &h));
                if (h == 300) {
                    ERR_CHECK(json_obj_dup_string(&jctx, "url", &(*track)->album.url_cover));
                    ERR_CHECK(json_arr_leave_object(&jctx));
                    break;
                }
                ERR_CHECK(json_arr_leave_object(&jctx));
            }
            ERR_CHECK(json_obj_leave_array(&jctx));
            ERR_CHECK(json_obj_leave_object(&jctx));
            ERR_CHECK(json_obj_leave_object(&jctx));
            ERR_CHECK(json_obj_get_int64(&jctx, "progress_ms", &(*track)->progress_ms));
            ERR_CHECK(json_obj_get_bool(&jctx, "is_playing", &(*track)->isPlaying));
        }

        return spotify_evt;
    }
    // unknow event
    spotify_evt.type = UNKNOW;
    return spotify_evt;
}

/* Private functions ---------------------------------------------------------*/
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
