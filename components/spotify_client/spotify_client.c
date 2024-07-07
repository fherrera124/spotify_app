/* Includes ------------------------------------------------------------------*/
#include "spotify_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "handler_callbacks.h"
#include "limits.h"
#include "parse_objects.h"
#include "spotify_client_priv.h"
#include "string_utils.h"
#include <string.h>

/* Private macro -------------------------------------------------------------*/
#define ACCESS_TOKEN_URL    "https://discord.com/api/v8/users/@me/connections/spotify/" CONFIG_SPOTIFY_UID "/access-token"
#define PLAYER              "/me/player"
#define TOKEN_URL           "https://accounts.spotify.com/api/token"
#define PLAYING             PLAYER "?market=AR&additional_types=episode"
#define PLAY_TRACK          PLAYER "/play"
#define PAUSE_TRACK         PLAYER "/pause"
#define PREV_TRACK          PLAYER "/previous"
#define NEXT_TRACK          PLAYER "/next"
#define VOLUME              PLAYER "/volume?volume_percent="
#define PLAYERURL(ENDPOINT) "https://api.spotify.com/v1" ENDPOINT
#define ACQUIRE_LOCK(mux)   xSemaphoreTake(mux, portMAX_DELAY)
#define RELEASE_LOCK(mux)   xSemaphoreGive(mux)
#define RETRIES_ERR_CONN    3
#define SPRINTF_BUF_SIZE    100

#define PREPARE_CLIENT(state, AUTH, TYPE)                                  \
    esp_http_client_set_url(http_client.handle, http_client.endpoint);     \
    esp_http_client_set_method(http_client.handle, http_client.method);    \
    esp_http_client_set_header(http_client.handle, "Authorization", AUTH); \
    esp_http_client_set_header(http_client.handle, "Content-Type", TYPE)

/* Private types -------------------------------------------------------------*/
typedef void (*handler_cb_t)(char*, esp_http_client_event_t*);

typedef struct {
    esp_http_client_handle_t handle; /*!<*/
    const char*              endpoint; /*!<*/
    esp_http_client_method_t method; /*!<*/
    handler_cb_t             handler_cb; /*!< Callback function to handle http events */
} HttpClient_data_t;

typedef enum {
    PAUSE = 1,
    PLAY,
    PREVIOUS,
    NEXT,
    CHANGE_VOLUME,
    GET_STATE
} PlayerCommand_t;

/* Locally scoped variables --------------------------------------------------*/
static const char*            TAG = "spotify_client";
EventGroupHandle_t            event_group = NULL;
static char                   http_buffer[MAX_HTTP_BUFFER];
static char                   ws_buffer[4096];
static char                   sprintf_buf[SPRINTF_BUF_SIZE];
static SemaphoreHandle_t      http_buf_lock = NULL; /* Mutex to manage access to the http client buffer */
static uint8_t                s_retries = 0; /* number of retries on error connections */
AccessToken                   access_token = { .value = "Bearer " };
static HttpClient_data_t      http_client = { 0 };
esp_websocket_client_handle_t ws_client_handle;
static QueueHandle_t          event_queue;
List                          playlists = { .type = PLAYLIST_LIST };
List                          devices = { .type = DEVICE_LIST };
TrackInfo*                    track_info = &(TrackInfo) { .artists.type = STRING_LIST }; /* pointer to an unnamed object, constructed in place
by the the COMPOUND LITERAL expression "(TrackInfo) { 0 }". NOTE: Although the syntax of a compound
literal is similar to a cast, the important distinction is that a cast is a non-lvalue expression
while a compound literal is an lvalue */

/* Globally scoped variables definitions -------------------------------------*/

/* External variables --------------------------------------------------------*/
extern const char certs_pem_start[] asm("_binary_certs_pem_start");
extern const char certs_pem_end[] asm("_binary_certs_pem_end");

/* Private function prototypes -----------------------------------------------*/
static esp_err_t get_access_token();
static esp_err_t _http_event_handler(esp_http_client_event_t* evt);
static void      player_task(void* pvParameters);
static esp_err_t confirm_ws_session(char* conn_id);
static void      free_track(TrackInfo* track_info);
static esp_err_t http_retries_available(esp_err_t err);
static void      debug_mem();
static void      playlists_handler_cb(char* dest, esp_http_client_event_t* evt);
static bool      access_token_empty();
static esp_err_t player_cmd(PlayerCommand_t cmd, void* payload, HttpStatus_Code* status_code);

/* Exported functions --------------------------------------------------------*/
esp_err_t spotify_client_init(UBaseType_t priority)
{
    static esp_http_client_config_t http_cfg = {
        .url = "https://api.spotify.com/v1",
        .event_handler = _http_event_handler,
        .cert_pem = certs_pem_start,
        .buffer_size_tx = DEFAULT_HTTP_BUF_SIZE + 256,
    };

    static esp_websocket_client_config_t websocket_cfg = {
        .uri = "wss://dealer.spotify.com",
        .cert_pem = certs_pem_start,
        .ping_interval_sec = 30,
    };

    assert(track_info->name = calloc(1, 1));

    http_client.handle = esp_http_client_init(&http_cfg);
    if (!http_client.handle) {
        ESP_LOGE(TAG, "Error on esp_http_client_init()");
        return ESP_FAIL;
    }

    ws_client_handle = esp_websocket_client_init(&websocket_cfg);
    if (!ws_client_handle) {
        ESP_LOGE(TAG, "Error on esp_websocket_client_init()");
        return ESP_FAIL;
    }

    http_buf_lock = xSemaphoreCreateMutex();
    if (!http_buf_lock) {
        ESP_LOGE(TAG, "Error on xSemaphoreCreateMutex()");
        return ESP_FAIL;
    }

    event_queue = xQueueCreate(1, sizeof(SpotifyClientEvent_t));
    if (!event_queue) {
        ESP_LOGE(TAG, "Failed to create queue for events");
        return ESP_FAIL;
    }

    http_client.handler_cb = default_http_handler_cb;

    event_group = xEventGroupCreate();

    if (!event_group) {
        ESP_LOGE("EventGroup", "Failed to create event group");
        return ESP_FAIL;
    }

    int res = xTaskCreate(player_task, "player_task", 4096, NULL, priority, NULL);
    if (!res) {
        ESP_LOGE(TAG, "Error creating task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t spotify_dispatch_event(SendEvent_t event)
{
    if (!event_group) {
        ESP_LOGE(TAG, "Run spotify_client_init() first");
        return ESP_FAIL;
    }
    switch (event) {
    case ENABLE_PLAYER_EVENT:
        xEventGroupSetBits(event_group, ENABLE_PLAYER);
        break;
    case DISABLE_PLAYER_EVENT:
        xEventGroupSetBits(event_group, DISABLE_PLAYER);
        break;
    case DATA_PROCESSED_EVENT:
        xEventGroupSetBits(event_group, DATA_PROCESSED);
        break;
    case DO_PLAY:
        player_cmd(PLAY, NULL, NULL);
        break;
    case DO_PAUSE:
        player_cmd(PAUSE, NULL, NULL);
        break;
    case DO_NEXT:
        player_cmd(NEXT, NULL, NULL);
        break;
    case DO_PREVIOUS:
        player_cmd(PREVIOUS, NULL, NULL);
        break;
    default:
        ESP_LOGE(TAG, "Unknown event: %d", event);
        return ESP_FAIL;
    }
    return ESP_OK;
}

BaseType_t spotify_wait_event(SpotifyClientEvent_t* event, TickType_t xTicksToWait)
{
    // TODO: check first if the player is enabled,
    // if not, send an event of the error
    return xQueueReceive(event_queue, event, xTicksToWait);

    // maybe we can send the DATA_PROCESSED_EVENT here
}

// ok
esp_err_t spotify_play_context_uri(const char* uri, HttpStatus_Code* status_code)
{
    esp_err_t       err;
    HttpStatus_Code s_code = 0;
    if (access_token_empty()) {
        if ((err = get_access_token()) != ESP_OK) {
            if (status_code) {
                *status_code = s_code;
            }
            return err;
        }
    }
    ACQUIRE_LOCK(http_buf_lock);
    int str_len = sprintf(sprintf_buf, "{\"context_uri\":\"%s\"}", uri);
    assert(str_len <= SPRINTF_BUF_SIZE);

    http_client.handler_cb = default_http_handler_cb;
    http_client.method = HTTP_METHOD_PUT;
    http_client.endpoint = PLAYERURL(PLAY_TRACK);

    esp_http_client_set_post_field(http_client.handle, sprintf_buf, str_len);
    PREPARE_CLIENT(http_client, access_token.value, "application/json");
retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", http_client.endpoint);
    if ((err = esp_http_client_perform(http_client.handle)) == ESP_OK) {
        s_retries = 0;
        HttpStatus_Code s_code = esp_http_client_get_status_code(http_client.handle);
        int             length = esp_http_client_get_content_length(http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", s_code, length);
        ESP_LOGD(TAG, "%s", http_buffer);
        esp_http_client_set_post_field(http_client.handle, NULL, 0);
    } else if (http_retries_available(err) == ESP_OK) {
        goto retry;
    }
    if (status_code) {
        *status_code = s_code;
    }
    RELEASE_LOCK(http_buf_lock);
    return err;
}

List* spotify_user_playlists()
{
    esp_err_t err;
    if (access_token_empty()) {
        ESP_ERROR_CHECK(get_access_token());
    }
    ACQUIRE_LOCK(http_buf_lock);
    http_client.handler_cb = playlists_handler_cb;
    http_client.method = HTTP_METHOD_GET;
    http_client.endpoint = PLAYERURL("/me/playlists?offset=0&limit=50");
    PREPARE_CLIENT(http_client, access_token.value, "application/json");
retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", http_client.endpoint);
    if ((err = esp_http_client_perform(http_client.handle)) == ESP_OK) {
        s_retries = 0;
        HttpStatus_Code status_code = esp_http_client_get_status_code(http_client.handle);
        int             length = esp_http_client_get_content_length(http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", status_code, length);
        if (status_code != HttpStatus_Ok) {
            ESP_LOGE(TAG, "Error. HTTP Status Code = %d", status_code);
        }
    } else if (http_retries_available(err) == ESP_OK) {
        goto retry;
    }
    RELEASE_LOCK(http_buf_lock);
    return &playlists;
}

List* spotify_available_devices()
{
    esp_err_t err;
    if (access_token_empty()) {
        ESP_ERROR_CHECK(get_access_token());
    }
    ACQUIRE_LOCK(http_buf_lock);
    http_client.handler_cb = default_http_handler_cb;
    http_client.method = HTTP_METHOD_GET;
    http_client.endpoint = PLAYERURL(PLAYER "/devices");
    PREPARE_CLIENT(http_client, access_token.value, "application/json");
retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", http_client.endpoint);
    if ((err = esp_http_client_perform(http_client.handle)) == ESP_OK) {
        s_retries = 0;
        HttpStatus_Code status_code = esp_http_client_get_status_code(http_client.handle);
        int             length = esp_http_client_get_content_length(http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", status_code, length);
        if (status_code == HttpStatus_Ok) {
            ESP_LOGD(TAG, "Active devices:\n%s", http_buffer);
            parse_available_devices(http_buffer, &devices);
        } else {
            ESP_LOGE(TAG, "Error. HTTP Status Code = %d", status_code);
        }
    } else if (http_retries_available(err) == ESP_OK) {
        goto retry;
    }
    RELEASE_LOCK(http_buf_lock);
    return &devices;
}

void spotify_clear_track(TrackInfo* track)
{
    track->id[0] = 0;
    free(track->name);
    free(track->album);
    track->isPlaying = false;
    track->progress_ms = 0;
    track->duration_ms = 0;
    spotify_free_nodes(&track->artists);
    free(track->device.id);
    free(track->device.name);
    free(track->device.type);
    strcpy(track->device.volume_percent, "-1");
}

esp_err_t spotify_clone_track(TrackInfo* dest, const TrackInfo* src)
{
    strcpy(dest->id, src->id);
    dest->name = strdup(src->name);
    dest->album = strdup(src->album);
    dest->isPlaying = src->isPlaying;
    dest->progress_ms = src->progress_ms;
    dest->duration_ms = src->duration_ms;
    /* dest->device.id = strdup(src->device.id);
    dest->device.type = strdup(src->device.type);
    strcpy(dest->device.volume_percent, src->device.volume_percent); */
    Node* node = src->artists.first;
    while (node) {
        char* artist = strdup((char*)node->data);
        assert(spotify_append_item_to_list(&dest->artists, (void*)artist));
        node = node->next;
    }
    return ESP_OK;
}

/* Private functions ---------------------------------------------------------*/
static void player_task(void* pvParameters)
{
    handler_args_t handler_args = {
        .buffer = ws_buffer,
        .event_group = event_group
    };
    int                  first_msg = 1;
    int                  enabled = 0;
    SpotifyClientEvent_t spotify_evt;
    EventBits_t          uxBits;
    while (1) {
        uxBits = xEventGroupWaitBits(
            event_group,
            ENABLE_PLAYER | DISABLE_PLAYER | WS_DATA_EVENT | WS_DISCONNECT_EVENT | DATA_PROCESSED,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY);

        if (uxBits & (ENABLE_PLAYER | WS_DISCONNECT_EVENT)) {
            if (uxBits & ENABLE_PLAYER) {
                if (enabled) {
                    ESP_LOGW(TAG, "Already enabled!!");
                    continue;
                }
                enabled = 1;
            }
            first_msg = 1;
            ESP_ERROR_CHECK(get_access_token());
            // if there is a device atached to playback,
            // instead of wait for an event from ws, we
            // send a "fake" PLAYER_STATE_CHANGED event
            HttpStatus_Code status_code;
            ESP_ERROR_CHECK(player_cmd(GET_STATE, NULL, &status_code));
            if (status_code == HttpStatus_Ok) {
                // maybe free track??
                ACQUIRE_LOCK(http_buf_lock);
                spotify_evt = parse_track(http_buffer, &track_info, 1);
                RELEASE_LOCK(http_buf_lock);
                xQueueSend(event_queue, &spotify_evt, portMAX_DELAY);
            } else if (status_code == 204) {
                // no device is atached to playback,
                // fire an event of no device playing
                spotify_evt.type = NO_PLAYER_ACTIVE;
                xQueueSend(event_queue, &spotify_evt, portMAX_DELAY);
            } else {
                ESP_LOGE(TAG, "Error trying to get player state. Status code: %d", status_code);
                // TODO: send error to queue
                break;
            }

            // start the ws session
            char* uri = http_utils_join_string("wss://dealer.spotify.com/?access_token=", 0, access_token.value + 7, strlen(access_token.value) - 7);
            esp_websocket_client_set_uri(ws_client_handle, uri);
            free(uri);
            esp_websocket_register_events(ws_client_handle, WEBSOCKET_EVENT_ANY, default_ws_handler_cb, &handler_args);
            esp_err_t err = esp_websocket_client_start(ws_client_handle);
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group, READY_FOR_DATA);
            } else {
                // TODO: send error to queue
            }
        } else if (uxBits & DISABLE_PLAYER) {
            enabled = 0;
            esp_websocket_client_close(ws_client_handle, portMAX_DELAY);
        } else if (uxBits & WS_DATA_EVENT) {

            // now the ws buff is our
            // analize data of ws event

            if (first_msg) {
                first_msg = 0;
                char* conn_id = NULL;
                parse_connection_id(ws_buffer, &conn_id);
                assert(conn_id);
                ESP_LOGD(TAG, "Connection id: '%s'", conn_id);
                ESP_ERROR_CHECK(confirm_ws_session(conn_id));
                xEventGroupSetBits(event_group, READY_FOR_DATA);
            } else {
                spotify_evt = parse_track(ws_buffer, &track_info, 0);
                xQueueSend(event_queue, &spotify_evt, portMAX_DELAY);
            }
        } else if (uxBits & DATA_PROCESSED) {
            xEventGroupSetBits(event_group, READY_FOR_DATA);
            // now the ws buff isn't our anymore
        }
    }
}

static esp_err_t confirm_ws_session(char* conn_id)
{
    esp_err_t err;
    ACQUIRE_LOCK(http_buf_lock);
    http_client.handler_cb = default_http_handler_cb;
    http_client.method = HTTP_METHOD_PUT;
    char* url = http_utils_join_string("https://api.spotify.com/v1/me/notifications/player?connection_id=", 0, conn_id, 0);
    http_client.endpoint = url; // esp_http_client_set_url(http_client.handle, url);
    PREPARE_CLIENT(http_client, access_token.value, "application/json");
retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", http_client.endpoint);
    if ((err = esp_http_client_perform(http_client.handle)) == ESP_OK) {
        s_retries = 0;
        HttpStatus_Code status_code = esp_http_client_get_status_code(http_client.handle);
        int             length = esp_http_client_get_content_length(http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", status_code, length);
        free(conn_id);
        free(url);
        err = (status_code == HttpStatus_Ok) ? ESP_OK : ESP_FAIL;
    } else if (http_retries_available(err) == ESP_OK) {
        goto retry;
    }
    RELEASE_LOCK(http_buf_lock);
    return err;
}

static inline esp_err_t http_retries_available(esp_err_t err)
{
    static const char* HTTP_METHOD_LOOKUP[] = { "GET", "POST", "PUT" };
    ESP_LOGE(TAG, "HTTP %s request failed: %s",
        HTTP_METHOD_LOOKUP[http_client.method],
        esp_err_to_name(err));
    if (++s_retries <= RETRIES_ERR_CONN) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGW(TAG, "Retrying %d/%d...", s_retries, RETRIES_ERR_CONN);
        debug_mem();
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t _http_event_handler(esp_http_client_event_t* evt)
{
    http_client.handler_cb(http_buffer, evt);
    return ESP_OK;
}

static inline void free_track(TrackInfo* track)
{
    free(track->name);
    free(track->album);
    spotify_free_nodes(&track->artists);
    free(track->device.id);
    free(track->device.name);
    free(track->device.type);
    strcpy(track->device.volume_percent, "-1");
}

static inline void debug_mem()
{
    /* uxTaskGetStackHighWaterMark() returns the minimum amount of remaining
     * stack space that was available to the task since the task started
     * executing - that is the amount of stack that remained unused when the
     * task stack was at its greatest (deepest) value. This is what is referred
     * to as the stack 'high water mark'.
     * */
    ESP_LOGI(TAG, "stack high water mark: %d", uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "minimum free heap size: %lu", esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "free heap size: %lu", esp_get_free_heap_size());
}

static esp_err_t get_access_token()
{
    esp_err_t err;
    ACQUIRE_LOCK(http_buf_lock);
    http_client.handler_cb = default_http_handler_cb;
    http_client.method = HTTP_METHOD_GET;
    http_client.endpoint = ACCESS_TOKEN_URL;
    PREPARE_CLIENT(http_client, CONFIG_DISCORD_TOKEN, "application/json");

retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", http_client.endpoint);
    if ((err = esp_http_client_perform(http_client.handle)) == ESP_OK) {
        s_retries = 0;
        HttpStatus_Code status_code = esp_http_client_get_status_code(http_client.handle);
        int             length = esp_http_client_get_content_length(http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", status_code, length);
        if (status_code == HttpStatus_Ok) {
            parse_access_token(http_buffer, access_token.value + 7, 400 - 7);
            ESP_LOGD(TAG, "Access Token obtained:\n%s", &access_token.value[7]);
        } else {
            ESP_LOGE(TAG, "Error trying to obtain an access token. Status code: %d", status_code);
            err = ESP_FAIL;
        }
    } else if (http_retries_available(err) == ESP_OK) {
        goto retry;
    }
    RELEASE_LOCK(http_buf_lock);
    return err;
}

// ok
static esp_err_t player_cmd(PlayerCommand_t cmd, void* payload, HttpStatus_Code* status_code)
{
    esp_err_t       err;
    HttpStatus_Code s_code = 0;
    if (access_token_empty()) {
        if ((err = get_access_token()) != ESP_OK) {
            goto exit;
        }
    }
    switch (cmd) {
    case PAUSE:
        http_client.method = HTTP_METHOD_PUT;
        http_client.endpoint = PLAYERURL(PAUSE_TRACK);
        break;
    case PLAY:
        http_client.method = HTTP_METHOD_PUT;
        http_client.endpoint = PLAYERURL(PLAY_TRACK);
        break;
    case PREVIOUS:
        http_client.method = HTTP_METHOD_POST;
        http_client.endpoint = PLAYERURL(PREV_TRACK);
        break;
    case NEXT:
        http_client.method = HTTP_METHOD_POST;
        http_client.endpoint = PLAYERURL(NEXT_TRACK);
        break;
    case CHANGE_VOLUME:
        break;
    case GET_STATE:
        http_client.method = HTTP_METHOD_GET;
        http_client.endpoint = PLAYERURL(PLAYER);
        break;
    default:
        ESP_LOGE(TAG, "Unknow command: %d", cmd);
        err = ESP_FAIL;
        goto exit;
    }
    ACQUIRE_LOCK(http_buf_lock);
    http_client.handler_cb = default_http_handler_cb;
    PREPARE_CLIENT(http_client, access_token.value, "application/json");
retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", http_client.endpoint);
    if ((err = esp_http_client_perform(http_client.handle)) == ESP_OK) {
        s_retries = 0;
        s_code = esp_http_client_get_status_code(http_client.handle);
        int length = esp_http_client_get_content_length(http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", s_code, length);
        ESP_LOGD(TAG, "%s", http_buffer);
    } else if (http_retries_available(err) == ESP_OK) {
        goto retry;
    }
    if (status_code) {
        *status_code = s_code;
    }
    RELEASE_LOCK(http_buf_lock);
    return err;
}

/**
 * @brief We don't have enough memory to store the whole JSON. So the
 * approach is to process the "items" array one playlist at a time.
 *
 */
static void playlists_handler_cb(char* dest, esp_http_client_event_t* evt)
{
    static const char* items_key = "\"items\"";
    static int         chars_stored = 0; // Number of chars stored in buffer
    static int         in_items = 0; // Bandera para indicar si estamos dentro del arreglo "items"
    static int         brace_count = 0; // Contador de llaves para detectar el final de un elemento

    char* src = (char*)evt->data;
    int   src_len = evt->data_len;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!in_items) {
            char* match_found = memmem(src, src_len, items_key, strlen(items_key));
            if (!match_found)
                break;
            in_items = 1;
            match_found += strlen(items_key);
            src_len -= match_found - src;
            src = match_found;
        }
        for (int i = 0; i < src_len; i++) {
            // Skip unnecessary spaces
            if (isspace((unsigned char)src[i])) {
                char prev = i ? src[i - 1] : 0;
                char next = (i < src_len - 1) ? src[i + 1] : 0;
                if (prev == ',' && next == '\"')
                    continue;
                if (prev == ':' && chars_stored > 1) {
                    if (dest[chars_stored - 2] == '\"')
                        continue;
                }
                if (strchr(" \"[]{}", prev) || strchr(" \"[]{}", next))
                    continue;
            }
            if (src[i] == '{') {
                if (brace_count == 0) {
                    // Start of new playlist
                    chars_stored = 0;
                }
                brace_count++;
            }
            if (brace_count > 0) {
                assert(chars_stored < MAX_HTTP_BUFFER - 1);
                dest[chars_stored++] = src[i];
            }
            if (src[i] == '}') {
                brace_count--;
                if (brace_count == 0) {
                    // End of playlist
                    dest[chars_stored] = '\0';
                    ESP_LOGD(TAG, "Playlist (len: %d):\n%s", strlen(dest), dest);
                    PlaylistItem_t* item = malloc(sizeof(item));
                    assert(item);
                    parse_playlist(http_buffer, item);
                    assert(spotify_append_item_to_list(&playlists, (void*)item));
                    chars_stored = 0;
                }
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        chars_stored = in_items = brace_count = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        chars_stored = in_items = brace_count = 0;
        break;
    default:
        break;
    }
}

static inline bool access_token_empty()
{
    return strlen(access_token.value) == 7;
}