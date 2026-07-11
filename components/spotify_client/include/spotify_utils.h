#pragma once

#include <esp_err.h>
#include <stdbool.h>

/* Exported macro ------------------------------------------------------------*/

/* Exported types ------------------------------------------------------------*/

typedef struct Node Node;

struct Node {
    void* data;
    Node* next;
};

typedef enum {
    STRING_LIST = 1,
    PLAYLIST_LIST,
    DEVICE_LIST,
    TRACK_LIST,
} NodeType_t;

typedef struct {
    Node*      first;
    Node*      last;
    size_t     count;
    NodeType_t type;
} List;

typedef struct {
    char* name;
    char* uri;
} PlaylistItem_t;

typedef struct {
    char* name;
    char* id;
    bool  is_active; /* true if this is the device Spotify currently considers
                       * active; lets a device-picker UI mark the current one. */
} DeviceItem_t; // TODO: merge with Device type

typedef struct {
    char* name;
    char* uri;
    /* Every "artists[].name" joined into a single ", "-separated string at
     * parse time (parse_search_results, parse_objects.c) - a search result
     * row only needs to display this, not walk it as a list like
     * TrackInfo.artists does, so a nested List would be unused complexity
     * here. NULL if the result had no artists array or none parsed. */
    char* artists;
} TrackSearchItem_t;

/* Exported functions prototypes ---------------------------------------------*/
List* spotify_create_empty_list(NodeType_t type);
Node* spotify_append_item_to_list(List* list, void* item);
void  spotify_free_nodes(List* list);
