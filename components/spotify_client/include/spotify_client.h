#pragma once

#include "esp_http_client.h"
#include "spotify_utils.h"

/* Exported macro ------------------------------------------------------------*/

/* Exported types ------------------------------------------------------------*/

typedef struct esp_spotify_client *esp_spotify_client_handle_t;

typedef enum {
    SAME_TRACK,
    NEW_TRACK,
    DEVICE_STATE_CHANGED, // <todo: delete later
    ACTIVE_DEVICES_FOUND,
    NO_ACTIVE_DEVICES,
    LAST_DEVICE_FAILED,
    VOLUME_CHANGED,
    TRANSFERRED_OK,
    TRANSFERRED_FAIL,
    NO_PLAYER_ACTIVE,
    UNKNOW
} Event_t;

typedef enum {
    ENABLE_PLAYER_EVENT = 1,
    DISABLE_PLAYER_EVENT,
    DATA_PROCESSED_EVENT,
    DO_PAUSE_EVENT,
    DO_PLAY_EVENT,
    PAUSE_UNPAUSE_EVENT,
    DO_PREVIOUS_EVENT,
    DO_NEXT_EVENT,
} SendEvent_t;

typedef struct
{
    char* id;
    bool  is_active;
    char* name;
    char* type;
    char  volume_percent[4];
} Device;

typedef struct
{
    char* name;
    char* url_cover;
} Album;

typedef struct
{
    char   id[30];
    char*  name;
    List   artists;
    Album  album;
    time_t duration_ms;
    time_t progress_ms;
    bool   isPlaying;
    Device device;
} TrackInfo;

typedef struct {
    Event_t type;
    void*   payload;
} SpotifyEvent_t;

/* Exported functions prototypes ---------------------------------------------*/
esp_spotify_client_handle_t  spotify_client_init(UBaseType_t priority);
esp_err_t  spotify_client_deinit(esp_spotify_client_handle_t client);
esp_err_t  player_dispatch_event(esp_spotify_client_handle_t client, SendEvent_t event);
BaseType_t spotify_wait_event(esp_spotify_client_handle_t client, SpotifyEvent_t* event, TickType_t xTicksToWait);
esp_err_t  spotify_play_context_uri(esp_spotify_client_handle_t client, const char* uri, HttpStatus_Code* status_code);
List*      spotify_user_playlists(esp_spotify_client_handle_t client);
List*      spotify_available_devices(esp_spotify_client_handle_t client);
void       spotify_clear_track(TrackInfo* track);
esp_err_t  spotify_clone_track(TrackInfo* dest, const TrackInfo* src);
ssize_t    fetch_album_art(esp_spotify_client_handle_t client, TrackInfo *track, uint8_t *out_buf, size_t buf_size);
