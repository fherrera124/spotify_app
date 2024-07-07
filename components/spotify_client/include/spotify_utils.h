#pragma once

#include <esp_err.h>

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
} DeviceItem_t; // TODO: merge with Device type

/* Exported functions prototypes ---------------------------------------------*/
List* spotify_create_empty_list(NodeType_t type);
Node* spotify_append_item_to_list(List* list, void* item);
void  spotify_free_nodes(List* list);
