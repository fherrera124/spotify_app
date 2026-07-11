/* Includes ------------------------------------------------------------------*/
#include "spotify_client.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "handler_callbacks.h"
#include "parse_objects.h"
#include "spotify_client_priv.h"
#include "string_utils.h"
#include <string.h>

/* Private function prototypes -----------------------------------------------*/
static esp_err_t confirm_ws_session(esp_spotify_client_handle_t client, char *conn_id);

/* Locally scoped variables --------------------------------------------------*/
static const char *TAG = "spotify_client";

/* Exported functions --------------------------------------------------------*/
void player_task(void *pvParameters)
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

        // Temporary diagnostic logging for ANALYSIS.md 1.15 (unconfirmed
        // event freeze) - remove once resolved.
        ESP_LOGI(TAG, "player_task woke up, uxBits=0x%04lx", (unsigned long)uxBits);

        if (uxBits & player_bits)
        {
            if (!enabled)
            {
                ESP_LOGW(TAG, "Task disabled");
                continue;
            }
            uint32_t n = uxBits & player_bits;
            PlayerCommand_t cmd;
            if ((n & (n - 1)) != 0 || !bits_to_player_cmd(n, &cmd))
            { // check that only a bit was set, and that it maps to a command
                ESP_LOGW(TAG, "Invalid command");
                continue;
            }
            HttpStatus_Code s_code;
            esp_err_t err = player_cmd(client, cmd, NULL, &s_code);
            if (err == ESP_OK && s_code == HttpStatus_Unauthorized)
            {
                if ((err = get_access_token(client)) == ESP_OK)
                {
                    err = player_cmd(client, cmd, NULL, &s_code);
                }
            }
            // s_code >= HttpStatus_BadRequest also catches e.g. 403 Forbidden
            // (non-Premium accounts) - report it instead of silently doing
            // nothing (ANALYSIS.md 3.4).
            if (err != ESP_OK || s_code >= HttpStatus_BadRequest)
            {
                ESP_LOGE(TAG, "Player command failed (cmd=%d, http_status=%d)", cmd, s_code);
                spotify_evt.player_event = PLAYER_ERROR;
                spotify_evt.payload = NULL;
                spotify_evt.error_code = (err == ESP_OK) ? s_code : 0;
                xQueueSend(client->event_queue, &spotify_evt, portMAX_DELAY);
            }
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
            if (get_access_token(client) != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to obtain access token, player left disabled");
                spotify_evt.player_event = PLAYER_ERROR;
                spotify_evt.payload = NULL;
                spotify_evt.error_code = 0;
                xQueueSend(client->event_queue, &spotify_evt, portMAX_DELAY);
                enabled = 0;
                continue;
            }
            // if there is a device atached to playback,
            // instead of wait for an event from ws, we
            // send a "fake" NEW_TRACK event
            HttpStatus_Code status_code;
            if (player_cmd(client, GET_STATE, NULL, &status_code) != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to fetch player state, player left disabled");
                spotify_evt.player_event = PLAYER_ERROR;
                spotify_evt.payload = NULL;
                spotify_evt.error_code = 0;
                xQueueSend(client->event_queue, &spotify_evt, portMAX_DELAY);
                enabled = 0;
                continue;
            }
            if (status_code == HttpStatus_Ok)
            {
                // maybe free track??
                ACQUIRE_LOCK(client->http_buf_lock);
                spotify_evt = parse_track((char *)(client->http_client.user_data.buffer), &client->track_info, 1, client->json_tokens);
                RELEASE_LOCK(client->http_buf_lock);
                ESP_LOGI(TAG, "GET_STATE -> parse_track type=%d, volume_percent=%d", spotify_evt.player_event, client->track_info->device.volume_percent);
                xQueueSend(client->event_queue, &spotify_evt, portMAX_DELAY);
            }
            else if (status_code == HTTP_STATUS_NO_CONTENT)
            {
                // no device is atached to playback,
                // fire an event of no device playing
                spotify_evt.player_event = NO_PLAYER_ACTIVE;
                xQueueSend(client->event_queue, &spotify_evt, portMAX_DELAY);
            }
            else
            {
                /* Report and give up on this enable attempt instead of
                 * breaking out of the loop: player_task is a FreeRTOS task
                 * function and must never return, which "break" here would
                 * have caused (the enclosing switch-less while(1) has no
                 * other loop for it to break out of). */
                ESP_LOGE(TAG, "Error trying to get player state. Status code: %d", status_code);
                spotify_evt.player_event = PLAYER_ERROR;
                spotify_evt.payload = NULL;
                spotify_evt.error_code = status_code;
                xQueueSend(client->event_queue, &spotify_evt, portMAX_DELAY);
                enabled = 0;
                continue;
            }

            // start the ws session
            char *uri = http_utils_join_string("wss://dealer.spotify.com/?access_token=", 0, client->access_token.value + BEARER_PREFIX_LEN, strlen(client->access_token.value) - BEARER_PREFIX_LEN);

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
                ESP_LOGE(TAG, "Failed to start websocket client, player left disabled");
                spotify_evt.player_event = PLAYER_ERROR;
                spotify_evt.payload = NULL;
                spotify_evt.error_code = 0;
                xQueueSend(client->event_queue, &spotify_evt, portMAX_DELAY);
                enabled = 0;
                continue;
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
                /* client->json_tokens is also used by the HTTP-side parse_*
                 * calls under http_buf_lock; take the same lock here so
                 * this client's own HTTP and WS pipelines don't race on it. */
                ACQUIRE_LOCK(client->http_buf_lock);
                parse_connection_id((char *)client->ws_client.user_data.buffer, &conn_id, client->json_tokens);
                RELEASE_LOCK(client->http_buf_lock);
                if (!conn_id)
                {
                    ESP_LOGE(TAG, "Failed to parse websocket connection id, disabling player");
                    spotify_evt.player_event = PLAYER_ERROR;
                    spotify_evt.payload = NULL;
                    spotify_evt.error_code = 0;
                    xQueueSend(client->event_queue, &spotify_evt, portMAX_DELAY);
                    enabled = 0;
                    esp_websocket_client_close(client->ws_client.handle, portMAX_DELAY);
                    continue;
                }
                ESP_LOGD(TAG, "Connection id: '%s'", conn_id);
                if (confirm_ws_session(client, conn_id) != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to confirm websocket session, disabling player");
                    spotify_evt.player_event = PLAYER_ERROR;
                    spotify_evt.payload = NULL;
                    spotify_evt.error_code = 0;
                    xQueueSend(client->event_queue, &spotify_evt, portMAX_DELAY);
                    enabled = 0;
                    esp_websocket_client_close(client->ws_client.handle, portMAX_DELAY);
                    continue;
                }
                xEventGroupSetBits(client->ws_client.event_group, WS_READY_FOR_DATA);
            }
            else
            {
                ACQUIRE_LOCK(client->http_buf_lock);
                spotify_evt = parse_track((char *)client->ws_client.user_data.buffer, &client->track_info, 0, client->json_tokens);
                RELEASE_LOCK(client->http_buf_lock);
                ESP_LOGI(TAG, "WS_DATA_EVENT -> parse_track type=%d, volume_percent=%d", spotify_evt.player_event, client->track_info->device.volume_percent);
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

/* Private functions ---------------------------------------------------------*/
/* Confirms the WebSocket connection just opened (identified by conn_id, from
 * parse_connection_id) with the REST API, so Spotify starts pushing
 * PLAYER_STATE_CHANGED events over it. Only ever called from player_task
 * above, right after the dealer WebSocket hands out a connection id. */
static esp_err_t confirm_ws_session(esp_spotify_client_handle_t client, char *conn_id)
{
    ACQUIRE_LOCK(client->http_buf_lock);
    client->http_client.http_event_cb = json_http_event_cb;
    char *url = http_utils_join_string("https://api.spotify.com/v1/me/notifications/player?connection_id=", 0, conn_id, 0);
    HttpStatus_Code status_code;
    esp_err_t err = perform_http_request(client, client->access_token.value, "application/json", url, HTTP_METHOD_PUT, &status_code);
    free(conn_id);
    free(url);
    if (err == ESP_OK)
    {
        err = (status_code == HttpStatus_Ok) ? ESP_OK : ESP_FAIL;
    }
    RELEASE_LOCK(client->http_buf_lock);
    return err;
}
