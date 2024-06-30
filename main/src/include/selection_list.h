#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "u8g2.h"

/* Exported macro ------------------------------------------------------------*/
#define MENU_EVENT_TIMEOUT 127

/* Exported functions prototypes ---------------------------------------------*/
uint8_t userInterfaceSelectionList(u8g2_t* u8g2, QueueHandle_t queue,
    const char* title, uint8_t start_pos, const char* sl, TickType_t ticks_timeout);

#ifdef __cplusplus
}
#endif