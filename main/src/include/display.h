/**
 * @file display.h
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
#include "freertos/task.h"

/* Exported types ------------------------------------------------------------*/

/* Globally scoped variables declarations ------------------------------------*/
extern TaskHandle_t DISPLAY_TASK;

/* Exported macro ------------------------------------------------------------*/
#define NOTIFY_DISPLAY(event) xTaskNotify(DISPLAY_TASK, event, eSetBits)

/* Exported functions prototypes ---------------------------------------------*/
void display_init(UBaseType_t priority, QueueHandle_t encoder_queue_hlr);
void send_err(const char* msg);

#ifdef __cplusplus
}
#endif