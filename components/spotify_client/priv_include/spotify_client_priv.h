/**
 * @file spotifyclient.h
 * @author Francisco Herrera (fherrera@lifia.info.unlp.edu.ar)
 * @brief
 * @version 0.1
 * @date 2022-11-06
 *
 * @copyright Copyright (c) 2022
 *
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdbool.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "spotify_client.h"
#include "parse_objects.h"

/* Exported macro ------------------------------------------------------------*/
// eventgroup macros
#define ENABLE_PLAYER       (1 << 0)
#define DISABLE_PLAYER      (1 << 1)
#define WS_READY_FOR_DATA   (1 << 2)
#define WS_CONNECT_EVENT    (1 << 3)
#define WS_DISCONNECT_EVENT (1 << 4)
#define WS_DATA_EVENT       (1 << 5)
#define WS_DATA_CONSUMED    (1 << 6)
#define DO_PLAY             (1 << 7)
#define DO_PAUSE            (1 << 8)
#define DO_NEXT             (1 << 9)
#define DO_PREVIOUS         (1 << 10)
#define DO_PAUSE_UNPAUSE    (1 << 11)

#define ACQUIRE_LOCK(mux) xSemaphoreTake(mux, portMAX_DELAY)
#define RELEASE_LOCK(mux) xSemaphoreGive(mux)
#define MAX_HTTP_BUFFER 8192
#define MAX_WS_BUFFER   4096
#define SPRINTF_BUF_SIZE 100
#define ACCESS_TOKEN_BUF_SIZE 400
#define WS_PING_INTERVAL_SEC 30
/* Stack size for player_task (spotify_client_init's xTaskCreate). Distinct
 * from MAX_HTTP_BUFFER on purpose even though they happen to share a value:
 * one sizes a task's execution stack, the other an HTTP response buffer -
 * changing one shouldn't silently change the other. */
#define PLAYER_TASK_STACK_SIZE 8192
#define HTTP_RETRY_DELAY_MS 1000
/* The literal prefix stored at the start of access_token.value (see
 * spotify_client_init) so callers can send it straight as an Authorization
 * header. BEARER_PREFIX_LEN is derived from the string itself (not a second
 * hardcoded number) so the two can never drift out of sync; used everywhere
 * the raw token needs to be read out from or written into that buffer past
 * the prefix. */
#define BEARER_PREFIX "Bearer "
#define BEARER_PREFIX_LEN (sizeof(BEARER_PREFIX) - 1)
/* Exported types ------------------------------------------------------------*/
/* Player commands as understood by player_cmd() (player_commands.c).
 * Deliberately not aliased to the DO_* EventGroup bits above (see
 * bits_to_player_cmd()): keeping this enum independent means the EventGroup
 * bit layout can change without having to keep these values in sync by hand. */
typedef enum
{
    PAUSE = 1,
    PLAY,
    PAUSE_UNPAUSE,
    PREVIOUS,
    NEXT,
    GET_STATE
} PlayerCommand_t;
/* Scratch state for playlist_http_event_cb's byte-by-byte JSON scan
 * (handler_callbacks.c): tracks brace depth (in_items/brace_count),
 * whether the current item overflowed the buffer (item_overflow), and
 * whether we're currently inside a JSON string value - and if so, whether
 * the next byte is backslash-escaped (in_string/escaped) - so structural
 * characters ('{'/'}') and "insignificant" whitespace that happen to
 * appear inside free-text fields (playlist names/descriptions) aren't
 * mistaken for JSON structure.
 *
 * Grouped into its own struct (instead of loose fields on evt_user_data_t)
 * specifically so it can be reset in one line wherever a fresh playlist
 * fetch starts: `user_data->playlist_scan = (playlist_scan_state_t){0};` -
 * see spotify_user_playlists() and playlist_http_event_cb's
 * ON_FINISH/DISCONNECTED cases. */
typedef struct {
    int in_items;
    int brace_count;
    int item_overflow;
    bool in_string;
    bool escaped;
} playlist_scan_state_t;

typedef struct {
    uint8_t *buffer;
    size_t buffer_size;
    size_t current_size;
    void * ctx;
    /* json_tok_t* scratch buffer (owned by the esp_spotify_client this
     * user_data belongs to) for parse_* calls that only have access to this
     * struct, not the client handle (e.g. playlist_http_event_cb). Kept as
     * void* here to avoid pulling json_parser.h into this header; cast back
     * to json_tok_t* at the call site. */
    void * tokens;
    /* Per-instance scratch state for the HTTP event callbacks in
     * handler_callbacks.c. These used to be function-local statics shared by
     * every esp_spotify_client instance; keeping them here instead means two
     * clients (or a client's HTTP and WS pipelines) can't corrupt each
     * other's in-progress parsing state. */
    int output_len;
    playlist_scan_state_t playlist_scan;
} evt_user_data_t;

/* Shared client struct, used across spotify_client.c/spotify_auth.c/
 * player_commands.c/player_task.c (ANALYSIS.md 2.6) - lives here since all
 * four need the full definition. */
struct esp_spotify_client
{
    TrackInfo *track_info;
    char sprintf_buf[SPRINTF_BUF_SIZE];
    SemaphoreHandle_t http_buf_lock; /* Mutex to manage access to the http client buffer */
    uint8_t s_retries;               /* number of retries on error connections */
    struct
    {
        char value[ACCESS_TOKEN_BUF_SIZE];
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
    TaskHandle_t player_task_handle;
    json_tok_t json_tokens[MAX_TOKENS]; /* scratch buffer for parse_objects.c, per-instance not global (ANALYSIS.md 2.4) */
};

/* Exported variables declarations -------------------------------------------*/

/* Exported macro ------------------------------------------------------------*/

/* Exported functions prototypes ---------------------------------------------*/
/* spotify_auth.c */
esp_err_t get_access_token(esp_spotify_client_handle_t client);
esp_err_t get_access_token_locked(esp_spotify_client_handle_t client);
bool access_token_needs_refresh(esp_spotify_client_handle_t client);

/* spotify_client.c */
esp_err_t perform_http_request(esp_spotify_client_handle_t client, const char *auth, const char *content_type, const char *url, esp_http_client_method_t method, HttpStatus_Code *status_code);

/* player_commands.c */
esp_err_t player_cmd(esp_spotify_client_handle_t client, PlayerCommand_t cmd, void *payload, HttpStatus_Code *status_code);
bool bits_to_player_cmd(uint32_t bit, PlayerCommand_t *out_cmd);

/* player_task.c */
void player_task(void *pvParameters);

#ifdef __cplusplus
}
#endif