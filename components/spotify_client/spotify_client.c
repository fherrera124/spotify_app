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
#define ACCESS_TOKEN_URL "https://discord.com/api/v8/users/@me/connections/spotify/" CONFIG_SPOTIFY_UID "/access-token"
#define PLAYER "/me/player"
#define TOKEN_URL "https://accounts.spotify.com/api/token"
#define PLAYER_STATE PLAYER "?market=from_token&additional_types=episode"
#define PLAY_TRACK PLAYER "/play"
#define PAUSE_TRACK PLAYER "/pause"
#define PREV_TRACK PLAYER "/previous"
#define NEXT_TRACK PLAYER "/next"
#define VOLUME PLAYER "/volume?volume_percent="
#define PLAYERURL(ENDPOINT) "https://api.spotify.com/v1" ENDPOINT
#define ACQUIRE_LOCK(mux) xSemaphoreTake(mux, portMAX_DELAY)
#define RELEASE_LOCK(mux) xSemaphoreGive(mux)
#define RETRIES_ERR_CONN 3
#define MAX_HTTP_BUFFER 8192
#define MAX_WS_BUFFER 4096
#define SPRINTF_BUF_SIZE 100

/* Private types -------------------------------------------------------------*/
typedef enum
{
    PAUSE = DO_PAUSE,
    PLAY = DO_PLAY,
    PAUSE_UNPAUSE = DO_PAUSE_UNPAUSE,
    PREVIOUS = DO_PREVIOUS,
    NEXT = DO_NEXT,
    CHANGE_VOLUME,
    GET_STATE
} PlayerCommand_t;

struct esp_spotify_client
{
    TrackInfo *track_info;
    char sprintf_buf[SPRINTF_BUF_SIZE];
    SemaphoreHandle_t http_buf_lock; /* Mutex to manage access to the http client buffer */
    uint8_t s_retries;               /* number of retries on error connections */
    struct
    {
        char value[400];
        time_t expiresIn;
    } access_token;
    struct
    {
        esp_http_client_handle_t handle;
        http_event_handle_cb http_event_cb;
        evt_user_data_t user_data;
    } http_client;
    struct
    {
        esp_websocket_client_handle_t handle;
        evt_user_data_t user_data;
        EventGroupHandle_t event_group;
    } ws_client;
    QueueHandle_t event_queue;
};

/* Locally scoped variables --------------------------------------------------*/
static const char *TAG = "spotify_client";

/* Globally scoped variables definitions -------------------------------------*/

/* External variables --------------------------------------------------------*/
extern const char certs_pem_start[] asm("_binary_certs_pem_start");
extern const char certs_pem_end[] asm("_binary_certs_pem_end");

/* Private function prototypes -----------------------------------------------*/
static esp_err_t get_access_token(esp_spotify_client_handle_t client);
static esp_err_t http_event_cb_wrapper(esp_http_client_event_t *evt);
static void player_task(void *pvParameters);
static esp_err_t confirm_ws_session(esp_spotify_client_handle_t client, char *conn_id);
static void free_track(TrackInfo *track_info);
static esp_err_t http_retries_available(esp_spotify_client_handle_t client, esp_err_t err);
static void debug_mem();
static bool access_token_empty(esp_spotify_client_handle_t client);
static void prepare_client(esp_http_client_handle_t http_client, const char *auth, const char *content_type, const char *url, esp_http_client_method_t method);
static esp_err_t player_cmd(esp_spotify_client_handle_t client, PlayerCommand_t cmd, void *payload, HttpStatus_Code *status_code);

/* Exported functions --------------------------------------------------------*/
esp_spotify_client_handle_t spotify_client_init(UBaseType_t priority)
{
    esp_spotify_client_handle_t client = calloc(1, sizeof(struct esp_spotify_client));
    if (!client)
    {
        ESP_LOGE(TAG, "Error allocating memory for client");
        return NULL;
    }

    client->http_client.user_data.buffer = (uint8_t *)calloc(1, MAX_HTTP_BUFFER);
    if (!client->http_client.user_data.buffer)
    {
        spotify_client_deinit(client);
        return NULL;
    }
    client->http_client.user_data.buffer_size = MAX_HTTP_BUFFER;

    client->track_info = (TrackInfo *)calloc(1, sizeof(TrackInfo));
    if (!client->track_info)
    {
        ESP_LOGE(TAG, "Error allocating memory for track info");
        spotify_client_deinit(client);
        return NULL;
    }
    client->track_info->artists.type = STRING_LIST;
    strcpy(client->access_token.value, "Bearer ");

    esp_http_client_config_t http_cfg = {
        .url = "https://api.spotify.com/v1",
        .user_data = client,
        .event_handler = http_event_cb_wrapper,
        .cert_pem = certs_pem_start,
        .buffer_size_tx = DEFAULT_HTTP_BUF_SIZE + 256,
    };

    esp_websocket_client_config_t websocket_cfg = {
        .uri = "wss://dealer.spotify.com",
        .user_context = &client->ws_client.user_data,
        .cert_pem = certs_pem_start,
        .ping_interval_sec = 30,
        .disable_auto_reconnect = true,
    };

    client->track_info->name = calloc(1, 1);
    if (!client->track_info->name)
    {
        ESP_LOGE(TAG, "Error allocating memory for track name");
        spotify_client_deinit(client);
        return NULL;
    }

    client->http_client.handle = esp_http_client_init(&http_cfg);
    if (!client->http_client.handle)
    {
        ESP_LOGE(TAG, "Error on esp_http_client_init()");
        spotify_client_deinit(client);
        return NULL;
    }
    client->http_client.http_event_cb = json_http_event_cb;
    client->ws_client.handle = esp_websocket_client_init(&websocket_cfg);
    if (!client->ws_client.handle)
    {
        ESP_LOGE(TAG, "Error on esp_websocket_client_init()");
        spotify_client_deinit(client);
        return NULL;
    }
    esp_websocket_client_destroy_on_exit(client->ws_client.handle);
    client->ws_client.user_data.buffer = (uint8_t *)calloc(1, MAX_WS_BUFFER);
    if (!client->ws_client.user_data.buffer)
    {
        spotify_client_deinit(client);
        return NULL;
    }
    client->ws_client.user_data.buffer_size = MAX_WS_BUFFER;

    client->http_buf_lock = xSemaphoreCreateMutex();
    if (!client->http_buf_lock)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        spotify_client_deinit(client);
        return NULL;
    }

    client->event_queue = xQueueCreate(1, sizeof(SpotifyEvent_t));
    if (!client->event_queue)
    {
        ESP_LOGE(TAG, "Failed to create queue for events");
        spotify_client_deinit(client);
        return NULL;
    }

    if (!(client->ws_client.event_group = xEventGroupCreate()))
    {
        ESP_LOGE("EventGroup", "Failed to create event group");
        spotify_client_deinit(client);
        return NULL;
    }
    client->ws_client.user_data.ctx = client->ws_client.event_group;

    int res = xTaskCreate(player_task, "player_task", 4096, client, priority, NULL);
    if (!res)
    {
        ESP_LOGE(TAG, "Failed to create player task");
        spotify_client_deinit(client);
        return NULL;
    }

    return client;
}

// TODO: make sure to set client to NULL
esp_err_t spotify_client_deinit(esp_spotify_client_handle_t client)
{
    if (!client)
    {
        return ESP_FAIL;
    }
    if (client->http_client.user_data.buffer)
    {
        free(client->http_client.user_data.buffer);
        client->http_client.user_data.buffer = NULL;
    }
    if (client->track_info)
    {
        spotify_clear_track(client->track_info);
        free(client->track_info);
        client->track_info = NULL;
    }
    if (client->http_client.handle)
    {
        esp_http_client_cleanup(client->http_client.handle);
        client->http_client.handle = NULL;
    }
    if (client->ws_client.handle)
    {
        esp_websocket_client_destroy(client->ws_client.handle);
        client->ws_client.handle = NULL;
    }
    if (client->ws_client.user_data.buffer)
    {
        free(client->ws_client.user_data.buffer);
        client->ws_client.user_data.buffer = NULL;
    }
    if (client->http_buf_lock)
    {
        vSemaphoreDelete(client->http_buf_lock);
        client->http_buf_lock = NULL;
    }
    if (client->event_queue)
    {
        vQueueDelete(client->event_queue);
        client->event_queue = NULL;
    }
    if (client->ws_client.event_group)
    {
        vEventGroupDelete(client->ws_client.event_group);
        client->ws_client.event_group = NULL;
    }
    free(client);
    return ESP_OK;
}

esp_err_t player_dispatch_event(esp_spotify_client_handle_t client, SendEvent_t event)
{
    if (!client->ws_client.event_group)
    {
        ESP_LOGE(TAG, "Run spotify_client_init() first");
        return ESP_FAIL;
    }
    switch (event)
    {
    case ENABLE_PLAYER_EVENT:
        xEventGroupSetBits(client->ws_client.event_group, ENABLE_PLAYER);
        break;
    case DISABLE_PLAYER_EVENT:
        xEventGroupSetBits(client->ws_client.event_group, DISABLE_PLAYER);
        break;
    case DATA_PROCESSED_EVENT:
        xEventGroupSetBits(client->ws_client.event_group, WS_DATA_CONSUMED);
        break;
    case DO_PLAY_EVENT:
        xEventGroupSetBits(client->ws_client.event_group, DO_PLAY);
        break;
    case DO_PAUSE_EVENT:
        xEventGroupSetBits(client->ws_client.event_group, DO_PAUSE);
        break;
    case PAUSE_UNPAUSE_EVENT:
        xEventGroupSetBits(client->ws_client.event_group, DO_PAUSE_UNPAUSE);
        break;
    case DO_NEXT_EVENT:
        xEventGroupSetBits(client->ws_client.event_group, DO_NEXT);
        break;
    case DO_PREVIOUS_EVENT:
        xEventGroupSetBits(client->ws_client.event_group, DO_PREVIOUS);
        break;
    default:
        ESP_LOGE(TAG, "Unknown event: %d", event);
        return ESP_FAIL;
    }
    return ESP_OK;
}

BaseType_t spotify_wait_event(esp_spotify_client_handle_t client, SpotifyEvent_t *event, TickType_t xTicksToWait)
{
    // TODO: check first if the player is enabled,
    // if not, send an event of the error
    return xQueueReceive(client->event_queue, event, xTicksToWait);

    // maybe we can send the DATA_PROCESSED_EVENT here
}

esp_err_t spotify_play_context_uri(esp_spotify_client_handle_t client, const char *uri, HttpStatus_Code *status_code)
{
    esp_err_t err;
    HttpStatus_Code s_code = 0;
    if (access_token_empty(client))
    {
        if ((err = get_access_token(client)) != ESP_OK)
        {
            if (status_code)
            {
                *status_code = s_code;
            }
            return err;
        }
    }
    ACQUIRE_LOCK(client->http_buf_lock);
    int str_len = sprintf(client->sprintf_buf, "{\"context_uri\":\"%s\"}", uri);
    assert(str_len <= SPRINTF_BUF_SIZE);
    esp_http_client_set_post_field(client->http_client.handle, client->sprintf_buf, str_len);
    client->http_client.http_event_cb = json_http_event_cb;
    prepare_client(client->http_client.handle, client->access_token.value, "application/json", PLAYERURL(PLAY_TRACK), HTTP_METHOD_PUT);
retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", PLAYERURL(PLAY_TRACK));
    if ((err = esp_http_client_perform(client->http_client.handle)) == ESP_OK)
    {
        client->s_retries = 0;
        HttpStatus_Code s_code = esp_http_client_get_status_code(client->http_client.handle);
        int length = esp_http_client_get_content_length(client->http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", s_code, length);
        ESP_LOGD(TAG, "%s", client->http_client.user_data.buffer);
        esp_http_client_set_post_field(client->http_client.handle, NULL, 0);
    }
    else if (http_retries_available(client, err) == ESP_OK)
    {
        goto retry;
    }
    if (status_code)
    {
        *status_code = s_code;
    }
    esp_http_client_close(client->http_client.handle);
    RELEASE_LOCK(client->http_buf_lock);
    return err;
}

List *spotify_user_playlists(esp_spotify_client_handle_t client)
{
    esp_err_t err;
    List *playlists = calloc(1, sizeof(List));
    if (!playlists)
    {
        ESP_LOGE(TAG, "Cannot allocate memory for playlists");
        return NULL;
    }
    playlists->type = PLAYLIST_LIST;
    if (access_token_empty(client))
    {
        ESP_ERROR_CHECK(get_access_token(client));
    }
    ACQUIRE_LOCK(client->http_buf_lock);
    client->http_client.user_data.ctx = playlists; // pass the playlists as context to event handler
    client->http_client.http_event_cb = playlist_http_event_cb;
    prepare_client(client->http_client.handle, client->access_token.value, "application/json", PLAYERURL("/me/playlists?offset=0&limit=50"), HTTP_METHOD_GET);
retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", PLAYERURL("/me/playlists?offset=0&limit=50"));
    if ((err = esp_http_client_perform(client->http_client.handle)) == ESP_OK)
    {
        client->s_retries = 0;
        HttpStatus_Code status_code = esp_http_client_get_status_code(client->http_client.handle);
        int length = esp_http_client_get_content_length(client->http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", status_code, length);
        if (status_code != HttpStatus_Ok)
        {
            ESP_LOGE(TAG, "Error. HTTP Status Code = %d", status_code);
            free(playlists);
            playlists = NULL;
        }
    }
    else if (http_retries_available(client, err) == ESP_OK)
    {
        goto retry;
    }
    else
    {
        free(playlists);
        playlists = NULL;
    }
    client->http_client.user_data.ctx = NULL;
    esp_http_client_close(client->http_client.handle);
    RELEASE_LOCK(client->http_buf_lock);
    return playlists;
}

List *spotify_available_devices(esp_spotify_client_handle_t client)
{
    esp_err_t err;
    List *devices = calloc(1, sizeof(List));
    if (!devices)
    {
        ESP_LOGE(TAG, "Cannot allocate memory for devices");
        return NULL;
    }
    devices->type = DEVICE_LIST;
    if (access_token_empty(client))
    {
        ESP_ERROR_CHECK(get_access_token(client));
    }
    ACQUIRE_LOCK(client->http_buf_lock);
    client->http_client.http_event_cb = json_http_event_cb;
    prepare_client(client->http_client.handle, client->access_token.value, "application/json", PLAYERURL(PLAYER "/devices"), HTTP_METHOD_GET);
retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", PLAYERURL(PLAYER "/devices"));
    if ((err = esp_http_client_perform(client->http_client.handle)) == ESP_OK)
    {
        client->s_retries = 0;
        HttpStatus_Code status_code = esp_http_client_get_status_code(client->http_client.handle);
        int length = esp_http_client_get_content_length(client->http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", status_code, length);
        if (status_code == HttpStatus_Ok)
        {
            ESP_LOGD(TAG, "Active devices:\n%s", client->http_client.user_data.buffer);
            parse_available_devices((char *)(client->http_client.user_data.buffer), devices);
        }
        else
        {
            ESP_LOGE(TAG, "Error. HTTP Status Code = %d", status_code);
            free(devices);
            devices = NULL;
        }
    }
    else if (http_retries_available(client, err) == ESP_OK)
    {
        goto retry;
    }
    else
    {
        free(devices);
        devices = NULL;
    }
    esp_http_client_close(client->http_client.handle);
    RELEASE_LOCK(client->http_buf_lock);
    return devices;
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
    dest->name = strdup(src->name);
    dest->album.name = strdup(src->album.name);
    dest->album.url_cover = strdup(src->album.url_cover);
    dest->isPlaying = src->isPlaying;
    dest->progress_ms = src->progress_ms;
    dest->duration_ms = src->duration_ms;
    /* dest->device.id = strdup(src->device.id);
    dest->device.type = strdup(src->device.type);
    strcpy(dest->device.volume_percent, src->device.volume_percent); */
    Node *node = src->artists.first;
    while (node)
    {
        char *artist = strdup((char *)node->data);
        assert(spotify_append_item_to_list(&dest->artists, (void *)artist));
        node = node->next;
    }
    return ESP_OK;
}

/* Private functions ---------------------------------------------------------*/
static void player_task(void *pvParameters)
{
    esp_spotify_client_handle_t client = pvParameters;
    int first_msg = 1;
    int enabled = 0;
    SpotifyEvent_t spotify_evt;
    EventBits_t uxBits;
    int player_bits = DO_PLAY | DO_PAUSE | DO_PREVIOUS | DO_NEXT | DO_PAUSE_UNPAUSE;
    while (1)
    {
        uxBits = xEventGroupWaitBits(
            client->ws_client.event_group,
            ENABLE_PLAYER | DISABLE_PLAYER | WS_DATA_EVENT | WS_DISCONNECT_EVENT | WS_DATA_CONSUMED | player_bits,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY);

        if (uxBits & player_bits)
        {
            if (!enabled)
            {
                ESP_LOGW(TAG, "Task disabled");
                continue;
            }
            uint32_t n = uxBits & player_bits;
            if ((n & (n - 1)) != 0)
            { // check that only a bit was set
                ESP_LOGW(TAG, "Invalid command");
                continue;
            }
            HttpStatus_Code s_code;
            esp_err_t err = player_cmd(client, n, NULL, &s_code);
            if (err == ESP_OK && s_code == HttpStatus_Unauthorized)
            {
                if ((err = get_access_token(client)) == ESP_OK)
                {
                    err = player_cmd(client, n, NULL, &s_code);
                }
            }
            // TODO: send error to queue if err == ESP_FAIL
            continue;
        }
        else if (uxBits & (ENABLE_PLAYER | WS_DISCONNECT_EVENT))
        {
            if (uxBits & ENABLE_PLAYER)
            {
                if (enabled)
                {
                    ESP_LOGW(TAG, "Already enabled!!");
                    continue;
                }
                enabled = 1;
            }
            first_msg = 1;
            ESP_ERROR_CHECK(get_access_token(client));
            // if there is a device atached to playback,
            // instead of wait for an event from ws, we
            // send a "fake" NEW_TRACK event
            HttpStatus_Code status_code;
            ESP_ERROR_CHECK(player_cmd(client, GET_STATE, NULL, &status_code));
            if (status_code == HttpStatus_Ok)
            {
                // maybe free track??
                ACQUIRE_LOCK(client->http_buf_lock);
                spotify_evt = parse_track((char *)(client->http_client.user_data.buffer), &client->track_info, 1);
                RELEASE_LOCK(client->http_buf_lock);
                xQueueSend(client->event_queue, &spotify_evt, portMAX_DELAY);
            }
            else if (status_code == 204)
            {
                // no device is atached to playback,
                // fire an event of no device playing
                spotify_evt.type = NO_PLAYER_ACTIVE;
                xQueueSend(client->event_queue, &spotify_evt, portMAX_DELAY);
            }
            else
            {
                ESP_LOGE(TAG, "Error trying to get player state. Status code: %d", status_code);
                // TODO: send error to queue
                break;
            }

            // start the ws session
            char *uri = http_utils_join_string("wss://dealer.spotify.com/?access_token=", 0, client->access_token.value + 7, strlen(client->access_token.value) - 7);

            esp_websocket_client_set_uri(client->ws_client.handle, uri); // TODO: fix, on WebSocket Error
            free(uri);
            esp_websocket_register_events(client->ws_client.handle, WEBSOCKET_EVENT_ANY, default_ws_event_cb, NULL);
            esp_err_t err = esp_websocket_client_start(client->ws_client.handle);
            if (err == ESP_OK)
            {
                xEventGroupSetBits(client->ws_client.event_group, WS_READY_FOR_DATA);
            }
            else
            {
                // TODO: send error to queue
            }
        }
        else if (uxBits & DISABLE_PLAYER)
        {
            enabled = 0;
            esp_websocket_client_close(client->ws_client.handle, portMAX_DELAY);
        }
        else if (uxBits & WS_DATA_EVENT)
        {

            // now the ws buff is our
            // analize data of ws event

            if (first_msg)
            {
                first_msg = 0;
                char *conn_id = NULL;
                parse_connection_id((char *)client->ws_client.user_data.buffer, &conn_id);
                assert(conn_id);
                ESP_LOGD(TAG, "Connection id: '%s'", conn_id);
                ESP_ERROR_CHECK(confirm_ws_session(client, conn_id));
                xEventGroupSetBits(client->ws_client.event_group, WS_READY_FOR_DATA);
            }
            else
            {
                spotify_evt = parse_track((char *)client->ws_client.user_data.buffer, &client->track_info, 0);
                xQueueSend(client->event_queue, &spotify_evt, portMAX_DELAY);
            }
        }
        else if (uxBits & WS_DATA_CONSUMED)
        {
            xEventGroupSetBits(client->ws_client.event_group, WS_READY_FOR_DATA);
            // now the ws buff isn't our anymore
        }
    }
}

static esp_err_t confirm_ws_session(esp_spotify_client_handle_t client, char *conn_id)
{
    esp_err_t err;
    ACQUIRE_LOCK(client->http_buf_lock);
    client->http_client.http_event_cb = json_http_event_cb;
    char *url = http_utils_join_string("https://api.spotify.com/v1/me/notifications/player?connection_id=", 0, conn_id, 0);
    prepare_client(client->http_client.handle, client->access_token.value, "application/json", url, HTTP_METHOD_PUT);
retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", url);
    if ((err = esp_http_client_perform(client->http_client.handle)) == ESP_OK)
    {
        client->s_retries = 0;
        HttpStatus_Code status_code = esp_http_client_get_status_code(client->http_client.handle);
        int length = esp_http_client_get_content_length(client->http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", status_code, length);
        free(conn_id);
        free(url);
        err = (status_code == HttpStatus_Ok) ? ESP_OK : ESP_FAIL;
    }
    else if (http_retries_available(client, err) == ESP_OK)
    {
        goto retry;
    }
    esp_http_client_close(client->http_client.handle);
    RELEASE_LOCK(client->http_buf_lock);
    return err;
}

static inline esp_err_t http_retries_available(esp_spotify_client_handle_t client, esp_err_t err)
{
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    if (++(client->s_retries) <= RETRIES_ERR_CONN)
    {
        esp_http_client_close(client->http_client.handle);
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGW(TAG, "Retrying %d/%d...", client->s_retries, RETRIES_ERR_CONN);
        debug_mem();
        return ESP_OK;
    }
    client->s_retries = 0;
    return ESP_FAIL;
}

static esp_err_t http_event_cb_wrapper(esp_http_client_event_t *evt)
{
    esp_spotify_client_handle_t client = evt->user_data;
    evt->user_data = &client->http_client.user_data;
    return client->http_client.http_event_cb(evt);
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
    prepare_client(client->http_client.handle, NULL, NULL, track->album.url_cover, HTTP_METHOD_GET);
    client->http_client.user_data.buffer = out_buf;
    client->http_client.user_data.buffer_size = buf_size;
    esp_err_t err;
    ssize_t data_read = ESP_FAIL;

retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", track->album.url_cover);
    if ((err = esp_http_client_perform(client->http_client.handle)) == ESP_OK)
    {
        client->s_retries = 0;
        HttpStatus_Code status_code = esp_http_client_get_status_code(client->http_client.handle);
        int64_t length = esp_http_client_get_content_length(client->http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %" PRId64, status_code, length);
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
    else if (http_retries_available(client, err) == ESP_OK)
    {
        goto retry;
    }
    esp_http_client_close(client->http_client.handle);
    // restore the buffer
    client->http_client.user_data.buffer = buff_backup;
    client->http_client.user_data.buffer_size = buff_size_backup;
    RELEASE_LOCK(client->http_buf_lock);
    return data_read;
}

static esp_err_t get_access_token(esp_spotify_client_handle_t client)
{
    esp_err_t err;
    ACQUIRE_LOCK(client->http_buf_lock);
    client->http_client.http_event_cb = json_http_event_cb;
    prepare_client(client->http_client.handle, CONFIG_DISCORD_TOKEN, "application/json", ACCESS_TOKEN_URL, HTTP_METHOD_GET);
retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", ACCESS_TOKEN_URL);
    if ((err = esp_http_client_perform(client->http_client.handle)) == ESP_OK)
    {
        client->s_retries = 0;
        HttpStatus_Code status_code = esp_http_client_get_status_code(client->http_client.handle);
        int length = esp_http_client_get_content_length(client->http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", status_code, length);
        if (status_code == HttpStatus_Ok)
        {
            parse_access_token((char *)(client->http_client.user_data.buffer), client->access_token.value + 7, 400 - 7);
            ESP_LOGD(TAG, "Access Token obtained:\n%s", &(client->access_token.value[7]));
        }
        else
        {
            ESP_LOGE(TAG, "Error trying to obtain an access token. Status code: %d", status_code);
            err = ESP_FAIL;
        }
    }
    else if (http_retries_available(client, err) == ESP_OK)
    {
        goto retry;
    }
    esp_http_client_close(client->http_client.handle);
    RELEASE_LOCK(client->http_buf_lock);
    return err;
}

// ok
static esp_err_t player_cmd(esp_spotify_client_handle_t client, PlayerCommand_t cmd, void *payload, HttpStatus_Code *status_code)
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
    case CHANGE_VOLUME:
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
    prepare_client(client->http_client.handle, client->access_token.value, "application/json", url, method);

retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", url);
    if ((err = esp_http_client_perform(client->http_client.handle)) == ESP_OK)
    {
        client->s_retries = 0;
        s_code = esp_http_client_get_status_code(client->http_client.handle);
        int length = esp_http_client_get_content_length(client->http_client.handle);
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", s_code, length);
        ESP_LOGD(TAG, "%s", client->http_client.user_data.buffer);
        ESP_LOGD(TAG, "curr size %d", client->http_client.user_data.current_size);
    }
    else if (http_retries_available(client, err) == ESP_OK)
    {
        goto retry;
    }
    if (status_code)
    {
        *status_code = s_code;
    }
    if (s_code == HttpStatus_Forbidden)
    {
        if (cmd == PAUSE_UNPAUSE)
        {
            if (strcmp(url, PLAYERURL(PAUSE_TRACK)) == 0)
            {
                url = PLAYERURL(PLAY_TRACK);
            }
            else
            {
                url = PLAYERURL(PAUSE_TRACK);
            }
            esp_http_client_set_url(client->http_client.handle, url);
            goto retry;
        }
    }
    esp_http_client_close(client->http_client.handle);
    RELEASE_LOCK(client->http_buf_lock);
    return err;
}

static inline bool access_token_empty(esp_spotify_client_handle_t client)
{
    return strlen(client->access_token.value) == 7;
}

static inline void prepare_client(esp_http_client_handle_t http_client, const char *auth, const char *content_type, const char *url, esp_http_client_method_t method)
{
    esp_http_client_set_url(http_client, url);
    esp_http_client_set_method(http_client, method);
    esp_http_client_set_header(http_client, "Authorization", auth);
    esp_http_client_set_header(http_client, "Content-Type", content_type);
}