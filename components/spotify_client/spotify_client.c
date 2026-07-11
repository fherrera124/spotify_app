/* Includes ------------------------------------------------------------------*/
#include "spotify_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "handler_callbacks.h"
#include "parse_objects.h"
#include "spotify_client_priv.h"
#include <string.h>

/* Private macro -------------------------------------------------------------*/
#define RETRIES_ERR_CONN 3

/* Private function prototypes -----------------------------------------------*/
static esp_err_t http_event_cb_wrapper(esp_http_client_event_t *evt);
static esp_err_t http_retries_available(esp_spotify_client_handle_t client, esp_err_t err);
static void debug_mem();
static void prepare_client(esp_http_client_handle_t http_client, const char *auth, const char *content_type, const char *url, esp_http_client_method_t method);

/* Locally scoped variables --------------------------------------------------*/
static const char *TAG = "spotify_client";

/* Globally scoped variables definitions -------------------------------------*/

/* External variables --------------------------------------------------------*/
extern const char certs_pem_start[] asm("_binary_certs_pem_start");
extern const char certs_pem_end[] asm("_binary_certs_pem_end");

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
    client->http_client.user_data.tokens = client->json_tokens;

    client->track_info = (TrackInfo *)calloc(1, sizeof(TrackInfo));
    if (!client->track_info)
    {
        ESP_LOGE(TAG, "Error allocating memory for track info");
        spotify_client_deinit(client);
        return NULL;
    }
    client->track_info->artists.type = STRING_LIST;
    client->track_info->device.volume_percent = -1; // calloc left it 0, which would be indistinguishable from a real 0% volume
    strcpy(client->access_token.value, BEARER_PREFIX);

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
        .ping_interval_sec = WS_PING_INTERVAL_SEC,
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

    int res = xTaskCreate(player_task, "player_task", PLAYER_TASK_STACK_SIZE, client, priority, &client->player_task_handle);
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
    if (client->player_task_handle)
    {
        /* player_task blocks on client->ws_client.event_group and touches
         * client's other members; it must be torn down before anything it
         * depends on is freed below, otherwise it would wake up (or resume
         * mid-access) into freed memory. */
        vTaskDelete(client->player_task_handle);
        client->player_task_handle = NULL;
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

/* Private functions ---------------------------------------------------------*/
static esp_err_t http_event_cb_wrapper(esp_http_client_event_t *evt)
{
    esp_spotify_client_handle_t client = evt->user_data;
    evt->user_data = &client->http_client.user_data;
    return client->http_client.http_event_cb(evt);
}

static inline esp_err_t http_retries_available(esp_spotify_client_handle_t client, esp_err_t err)
{
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    if (++(client->s_retries) <= RETRIES_ERR_CONN)
    {
        esp_http_client_close(client->http_client.handle);
        vTaskDelay(pdMS_TO_TICKS(HTTP_RETRY_DELAY_MS));
        ESP_LOGW(TAG, "Retrying %d/%d...", client->s_retries, RETRIES_ERR_CONN);
        debug_mem();
        return ESP_OK;
    }
    client->s_retries = 0;
    return ESP_FAIL;
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

static inline void prepare_client(esp_http_client_handle_t http_client, const char *auth, const char *content_type, const char *url, esp_http_client_method_t method)
{
    esp_http_client_set_url(http_client, url);
    esp_http_client_set_method(http_client, method);
    esp_http_client_set_header(http_client, "Authorization", auth);
    esp_http_client_set_header(http_client, "Content-Type", content_type);
}

/**
 * @brief Sets up and performs a single logical HTTP request on
 * client->http_client.handle, retrying on connection errors up to
 * RETRIES_ERR_CONN times, and closes the connection before returning.
 *
 * Caller must already hold client->http_buf_lock and must have set
 * client->http_client.http_event_cb (and user_data.ctx / post field
 * beforehand, if the request needs them) before calling this.
 */
esp_err_t perform_http_request(esp_spotify_client_handle_t client, const char *auth, const char *content_type, const char *url, esp_http_client_method_t method, HttpStatus_Code *status_code)
{
    esp_err_t err;
    HttpStatus_Code s_code = 0;
    prepare_client(client->http_client.handle, auth, content_type, url, method);
    do
    {
        ESP_LOGD(TAG, "Endpoint to send: %s", url);
        err = esp_http_client_perform(client->http_client.handle);
        if (err == ESP_OK)
        {
            client->s_retries = 0;
            s_code = esp_http_client_get_status_code(client->http_client.handle);
            int length = esp_http_client_get_content_length(client->http_client.handle);
            ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", s_code, length);
            break;
        }
        if (err == ESP_ERR_NOT_SUPPORTED)
        {
            /* esp_http_client_perform() only returns ESP_ERR_NOT_SUPPORTED
             * from one path: a 401 whose WWW-Authenticate scheme isn't
             * Basic/Digest (the only two it auto-negotiates) - exactly what
             * Spotify sends. The real status code is already set to 401
             * regardless; report it as the clean 401 it is, so callers'
             * "refresh token and retry on 401" logic (gated on
             * err == ESP_OK) isn't skipped (ANALYSIS.md 1.19). */
            HttpStatus_Code real_status = esp_http_client_get_status_code(client->http_client.handle);
            if (real_status == HttpStatus_Unauthorized)
            {
                client->s_retries = 0;
                s_code = real_status;
                err = ESP_OK;
                break;
            }
        }
    } while (http_retries_available(client, err) == ESP_OK);
    esp_http_client_close(client->http_client.handle);
    if (status_code)
    {
        *status_code = s_code;
    }
    return err;
}
