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
#define MAX_HTTP_BUFFER 8192
// eventgroup macros
#define ENABLE_PLAYER       (1 << 0)
#define DISABLE_PLAYER      (1 << 1)
#define READY_FOR_DATA      (1 << 2)
#define WS_CONNECT_EVENT    (1 << 3)
#define WS_DISCONNECT_EVENT (1 << 4)
#define WS_DATA_EVENT       (1 << 5)
#define DATA_PROCESSED      (1 << 6)
#define DO_PLAY             (1 << 7)
#define DO_PAUSE            (1 << 8)
#define DO_NEXT             (1 << 9)
#define DO_PREVIOUS         (1 << 10)

/* Exported types ------------------------------------------------------------*/
typedef struct
{
    char   value[400];
    time_t expiresIn;
} AccessToken;

/* Exported variables declarations -------------------------------------------*/

/* Exported macro ------------------------------------------------------------*/

/* Exported functions prototypes ---------------------------------------------*/

#ifdef __cplusplus
}
#endif