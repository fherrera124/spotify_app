#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "esp_http_client.h"
#include "esp_websocket_client.h"

/* Exported types ------------------------------------------------------------*/

typedef struct {
    char*              buffer;
    EventGroupHandle_t event_group;
} handler_args_t;

/* Exported functions prototypes ---------------------------------------------*/
void default_http_handler_cb(char* buff, esp_http_client_event_t* evt);
void default_ws_handler_cb(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);

#ifdef __cplusplus
}
#endif