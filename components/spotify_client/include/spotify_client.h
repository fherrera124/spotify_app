#pragma once

#include "esp_http_client.h"
#include "spotify_utils.h"

/* Exported macro ------------------------------------------------------------*/
/* Buffer size for TrackInfo.id below; also used by parse_track()
 * (parse_objects.c) as the max length it reads a Spotify id into, so the two
 * can't drift out of sync. */
#define SPOTIFY_ID_BUF_SIZE 30
/* HttpStatus_Code (esp_http_client.h) doesn't define 204: Spotify returns it
 * on GET_STATE when no device is currently playing anything, and on a
 * successful PUT /me/player/volume or /me/player/seek (spotify_set_volume(),
 * spotify_seek_to_position()). Public (not just used inside the component)
 * so callers of those two can tell a real success (200 or 204) apart from
 * an HTTP-level failure (e.g. 404 "no active device") using status_code
 * alone, without needing ANOTHER out-param just for that. */
#define HTTP_STATUS_NO_CONTENT 204
/* Desired Album.url_cover/Album.cover_size resolution: parse_track()
 * (parse_objects.c) picks the "images" entry closest to this size when the
 * response doesn't offer an exact match (episodes and some releases don't).
 * Public because the display pipeline (main/player_screen.c) needs the same
 * value to size its decode buffer and to tell a usable cover apart from a
 * fallback size it isn't prepared to decode - see Album.cover_size below. */
#define ALBUM_COVER_PREFERRED_SIZE 300

/* Exported types ------------------------------------------------------------*/

typedef struct esp_spotify_client *esp_spotify_client_handle_t;

/* Trimmed (ANALYSIS.md 2.9): dropped 7 unused values (DEVICE_STATE_CHANGED,
 * ACTIVE_DEVICES_FOUND, etc.) - never emitted nor consumed (device
 * listing/volume flow through their own mechanisms). Only add a case back
 * once something actually emits *and* consumes it. */
typedef enum {
    SAME_TRACK,
    NEW_TRACK,
    NO_PLAYER_ACTIVE,
    PLAYER_ERROR, /* recoverable network/API error; consumers can ignore it
                   * like UNKNOW if unhandled (ANALYSIS.md 2.5) */
    UNKNOW
} PlayerEvent_t;

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
    int   volume_percent; /* 0-100, or -1 if not known yet (device.volume_percent
                            * is only populated once parse_track() has seen at
                            * least one player-state response). */
} Device;

typedef struct
{
    char* name;
    char* url_cover;
    /* Actual (square) side length in px of url_cover's image, or 0 if
     * url_cover is NULL (no usable image at all). Not necessarily
     * ALBUM_COVER_PREFERRED_SIZE: parse_track() falls back to the closest
     * size available when there's no exact match, and consumers that assume
     * a fixed decode resolution (main/player_screen.c) need this to tell
     * "usable at our expected size" apart from "usable, but not a size we
     * can safely decode" without guessing from url_cover alone. */
    int   cover_size;
} Album;

typedef struct
{
    char   id[SPOTIFY_ID_BUF_SIZE];
    char*  name;
    List   artists;
    Album  album;
    time_t duration_ms;
    time_t progress_ms;
    bool   isPlaying;
    Device device;
} TrackInfo;

typedef struct {
    PlayerEvent_t player_event;
    void*   payload;
    int     error_code; /* for PLAYER_ERROR: the HTTP status code that caused
                          * it (e.g. 403 == Premium required for playback
                          * control), or 0 if the failure wasn't HTTP-level
                          * (e.g. couldn't start the websocket). Unused/0 for
                          * every other event type. */
} SpotifyEvent_t;

/* Exported functions prototypes ---------------------------------------------*/
esp_spotify_client_handle_t  spotify_client_init(UBaseType_t priority);
esp_err_t  spotify_client_deinit(esp_spotify_client_handle_t client);
esp_err_t  player_dispatch_event(esp_spotify_client_handle_t client, SendEvent_t event);
BaseType_t spotify_wait_event(esp_spotify_client_handle_t client, SpotifyEvent_t* event, TickType_t xTicksToWait);
esp_err_t  spotify_play_context_uri(esp_spotify_client_handle_t client, const char* uri, HttpStatus_Code* status_code);
esp_err_t  spotify_play_track_uri(esp_spotify_client_handle_t client, const char* uri, HttpStatus_Code* status_code);
esp_err_t  spotify_set_volume(esp_spotify_client_handle_t client, int volume_percent, HttpStatus_Code* status_code);
esp_err_t  spotify_seek_to_position(esp_spotify_client_handle_t client, int position_ms, HttpStatus_Code* status_code);
esp_err_t  spotify_transfer_playback(esp_spotify_client_handle_t client, const char* device_id, HttpStatus_Code* status_code);
List*      spotify_user_playlists(esp_spotify_client_handle_t client);
List*      spotify_available_devices(esp_spotify_client_handle_t client);
List*      spotify_search_tracks(esp_spotify_client_handle_t client, const char* query);
void       spotify_clear_track(TrackInfo* track);
esp_err_t  spotify_clone_track(TrackInfo* dest, const TrackInfo* src);
ssize_t    fetch_album_art(esp_spotify_client_handle_t client, TrackInfo *track, uint8_t *out_buf, size_t buf_size);
