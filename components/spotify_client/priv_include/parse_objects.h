#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdbool.h>
#include <time.h>

#include "spotify_client.h"

/* Exported types ------------------------------------------------------------*/

/* Globally scoped variables declarations ------------------------------------*/

/* Exported functions prototypes ---------------------------------------------*/
void           parse_access_token(const char* js, char* access_token, int size);
void           parse_playlist(const char* js, PlaylistItem_t* playlist_item);
void           parse_available_devices(const char* js, List*);
void           parse_connection_id(const char* js, char** str);
SpotifyEvent_t parse_track(const char* js, TrackInfo** track_info, int initial_state);

#ifdef __cplusplus
}
#endif