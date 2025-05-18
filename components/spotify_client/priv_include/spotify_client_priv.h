/**
 * @file spotifyclient.h
 * @author Francisco Herrera (fherrera@lifia.info.unlp.edu.ar)
 * @brief
 * @version 0.1
 * @date 2022-11-06
 *
 * @copyright Copyright (c) 2022
 *
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Exported macro ------------------------------------------------------------*/
// eventgroup macros
#define ENABLE_PLAYER       (1 << 0)
#define DISABLE_PLAYER      (1 << 1)
#define WS_READY_FOR_DATA   (1 << 2)
#define WS_CONNECT_EVENT    (1 << 3)
#define WS_DISCONNECT_EVENT (1 << 4)
#define WS_DATA_EVENT       (1 << 5)
#define WS_DATA_CONSUMED    (1 << 6)
#define DO_PLAY             (1 << 7)
#define DO_PAUSE            (1 << 8)
#define DO_NEXT             (1 << 9)
#define DO_PREVIOUS         (1 << 10)
#define DO_PAUSE_UNPAUSE    (1 << 11)

/* Exported types ------------------------------------------------------------*/
typedef struct {
    uint8_t *buffer;
    size_t buffer_size;
    size_t current_size;
    void * ctx;
} evt_user_data_t;

/* Exported variables declarations -------------------------------------------*/

/* Exported macro ------------------------------------------------------------*/

/* Exported functions prototypes ---------------------------------------------*/

#ifdef __cplusplus
}
#endif