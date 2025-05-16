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
    size_t             buffer_size;
    EventGroupHandle_t event_group;
} handler_args_t;

typedef struct {
    uint8_t *buffer;
    size_t buffer_size;
    size_t received_size;
} http_data_t;

/* Exported functions prototypes ---------------------------------------------*/
esp_err_t json_http_handler_cb(esp_http_client_event_t* evt);
void default_ws_handler_cb(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
esp_err_t esp_http_client_event_handler(esp_http_client_event_t* evt);

#ifdef __cplusplus
}
#endif