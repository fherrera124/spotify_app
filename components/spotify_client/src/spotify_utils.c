#include "spotify_utils.h"
#include <stdbool.h>
#include <string.h>

/* Private function prototypes -----------------------------------------------*/
Node* create_node(void* item);

/* Exported functions --------------------------------------------------------*/
List* spotify_create_empty_list(NodeType_t type)
{
    switch (type) {
    case STRING_LIST:
        break;
    case PLAYLIST_LIST:
        break;
    case DEVICE_LIST:
        break;
    default:
        return NULL;
    }
    List* list = calloc(sizeof(List), 1);
    if (!list) {
        return NULL;
    }
    list->type = type;
    return list;
}

/**
 * @brief Create a node, and append it to the list
 *
 * @param list list to append the node
 * @param item data to assign to the node
 * @return a pointer to the created node, NULL if failed
 */
Node* spotify_append_item_to_list(List* list, void* item)
{
    Node* node = create_node(item);
    if (!node) {
        return NULL;
    }
    if (!list->first) {
        list->first = node;
        list->last = node;
        list->count = 1;
    } else {
        list->last->next = node;
        list->last = node;
        list->count++;
    }
    return node;
}

void spotify_free_nodes(List* list)
{
    Node* node = list->first;
    Node* aux;

    while (node) {
        switch (list->type) {
        case STRING_LIST:
            free(node->data);
            break;
        case PLAYLIST_LIST:
            PlaylistItem_t* playlist_item = node->data;
            free(playlist_item->name);
            free(playlist_item->uri);
            free(playlist_item);
            break;
        case DEVICE_LIST:
            DeviceItem_t* device_item = node->data;
            free(device_item->name);
            free(device_item->id);
            free(device_item);
            break;
        default:
            return;
        }
        aux = node->next;
        free(node);
        node = aux;
    }
    list->first = NULL;
    list->last = NULL;
    list->count = 0;
}

/* Private functions ---------------------------------------------------------*/
Node* create_node(void* item)
{
    Node* node = malloc(sizeof(node));
    if (node) {
        node->data = item;
        node->next = NULL;
    }
    return node;
}