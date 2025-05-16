/* Includes ------------------------------------------------------------------*/
#include "handler_callbacks.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spotify_client_priv.h"
#include <stdio.h>
#include <string.h>

/* Private macro -------------------------------------------------------------*/
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* Private types -------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/
static const char *TAG = "HANDLER_CALLBACKS";

/* External variables declarations -------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/
size_t static inline memcpy_trimmed(char *dest, int dest_size, const char *src, size_t src_len);

/* Exported functions --------------------------------------------------------*/
esp_err_t json_http_handler_cb(esp_http_client_event_t *evt)
{
    http_data_t *http_data = (http_data_t *)evt->user_data;
    char *buffer = (char *)http_data->buffer;
    size_t buffer_size = http_data->buffer_size;
    static int output_len; // Stores number of bytes read

    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        size_t stored = memcpy_trimmed(buffer + output_len, buffer_size - output_len, evt->data, evt->data_len);
        output_len += stored;
        http_data->received_size = output_len;
        break;
    case HTTP_EVENT_ON_FINISH:
        buffer[output_len] = 0;
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
        if (err != ESP_OK)
        {
            ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            buffer[output_len] = 0;
            output_len = 0;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

void default_ws_handler_cb(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    handler_args_t *args = (handler_args_t *)handler_args;
    char *buffer = args->buffer;
    size_t buffer_size = args->buffer_size;
    EventGroupHandle_t event_group = args->event_group;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    static int lock = 0;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGD(TAG, "WebSocket Connected");
        xEventGroupSetBits(event_group, WS_CONNECT_EVENT);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "WebSocket Disconnected");
        xEventGroupSetBits(event_group, WS_DISCONNECT_EVENT);
        break;
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGD(TAG, "WebSocket Closed cleanly");
        xEventGroupSetBits(event_group, WS_DISCONNECT_EVENT);
        break;
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGD(TAG, "WebSocket Data Received: Opcode=%d, Length=%d", data->op_code, data->data_len);

        if (data->op_code == 0xA)
        {
            break;
        }
        if (data->op_code == 0x1 || data->op_code == 0x2)
        {
            if (!lock)
            {
                xEventGroupWaitBits(
                    event_group,
                    WS_READY_FOR_DATA,
                    pdTRUE,
                    pdFALSE,
                    portMAX_DELAY);
                lock = 1;
            }
            assert((data->payload_len) + 1 <= buffer_size); // TODO: don't use assert
            memcpy(buffer + data->payload_offset, data->data_ptr, data->data_len);
            if (data->payload_offset + data->data_len == data->payload_len)
            {
                ESP_LOGD(TAG, "Complete message received");
                buffer[data->payload_len] = 0;
                ESP_LOGD(TAG, "%s", buffer);
                lock = 0;
                xEventGroupSetBits(event_group, WS_DATA_EVENT);
            }
        }

        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket Error");
        break;
    }
}

esp_err_t esp_http_client_event_handler(esp_http_client_event_t *evt)
{
    http_data_t *http_data = (http_data_t *)evt->user_data;
    char *output_buffer = (char *)http_data->buffer; // Buffer to store response of http request from event handler
    static int output_len;                           // Stores number of bytes read

    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (esp_http_client_is_chunked_response(evt->client))
        {
            ESP_LOGW(TAG, "Chunked response");
            break;
        }
        int copy_len = 0;
        // The last byte in output_buffer is kept for the NULL character in case of out-of-bound access.
        // TODO: revisar
        copy_len = MIN(evt->data_len, (http_data->buffer_size - output_len));
        if (copy_len)
        {
            memcpy(output_buffer + output_len, evt->data, copy_len);
        }
        output_len += copy_len;
        http_data->received_size = output_len;
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
        if (err != 0)
        {
            ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        output_len = 0;
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        esp_http_client_set_header(evt->client, "From", "user@example.com");
        esp_http_client_set_header(evt->client, "Accept", "text/html");
        esp_http_client_set_redirection(evt->client);
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* Private functions ---------------------------------------------------------*/
size_t static inline memcpy_trimmed(char *dest, int dest_size, const char *src, size_t src_len)
{
    size_t chars_stored = 0;
    for (size_t i = 0; i < src_len; i++)
    {
        // Skip unnecessary spaces
        if (isspace((unsigned char)src[i]))
        {
            char prev = i ? src[i - 1] : 0;
            char next = (i < src_len - 1) ? src[i + 1] : 0;
            if (prev == ',' && next == '\"')
                continue;
            if (prev == ':' && chars_stored > 1)
            {
                if (dest[chars_stored - 2] == '\"')
                    continue;
            }
            if (strchr(" \"[]{}", prev) || strchr(" \"[]{}", next))
                continue;
        }
        if ((int)chars_stored > dest_size - 1)
        {
            ESP_LOGE(TAG, "Buffer overflow, stoping writing!");
            return chars_stored;
        }
        dest[chars_stored++] = src[i];
    }
    return chars_stored;
}