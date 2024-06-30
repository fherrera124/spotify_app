/**
 ******************************************************************************
 * @file           : selection_list.c
 * @author         : Francisco Herrera
 * @date           : Oct 13, 2022
 * @brief          : Selection list with scroll option. Extracted from
 *                   u8g2_selection_list.c. Almost the same code, just adapted
 *                   to read events from a freertos queue instead.
 ******************************************************************************
 * @attention
 *
 * Universal 8bit Graphics Library (https://github.com/olikraus/u8g2/)
 *
 * Copyright (c) 2016, olikraus@gmail.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.

 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rotary_encoder.h"
#include "u8g2.h"

#include "selection_list.h"

/* Private macro -------------------------------------------------------------*/
#define MY_BORDER_SIZE 1

/* Imported function prototypes ----------------------------------------------*/
void u8g2_DrawSelectionList(u8g2_t* u8g2, u8sl_t* u8sl, u8g2_uint_t y, const char* s);

/* Private function prototypes -----------------------------------------------*/
uint8_t getMenuEvent(QueueHandle_t queue, TickType_t ticks_timeout);

/* Private variables ---------------------------------------------------------*/

/* Exported functions --------------------------------------------------------*/

/**
 * @brief Display a list of scrollable and selectable options. The user can select
 * one of the options. NOTE: Modified version of u8g2_UserInterfaceSelectionList(),
 * adapted for rotary encoder queue.
 *        Side effects (according to original function):
 *        -    u8g2_SetFontDirection(u8g2, 0);
 *        -    u8g2_SetFontPosBaseline(u8g2);
 *
 * @param u8g2 A pointer to the u8g2 structure
 * @param queue Handle of the rotary encoder queue
 * @param title NULL for no title, valid str for title line. Can contain
 *              mutliple lines, separated by '\\n'
 * @param start_pos default position for the cursor, first line is 1.
 * @param sl string list (list of strings separated by \n)
 *
 * @retval - 0 if user has pressed the home key
 * @retval - The selected line if user has pressed the select key
 */
uint8_t userInterfaceSelectionList(u8g2_t* u8g2, QueueHandle_t queue,
    const char* title, uint8_t start_pos, const char* sl, TickType_t ticks_timeout)
{
    u8sl_t      u8sl;
    u8g2_uint_t yy;

    uint8_t event;

    u8g2_uint_t line_height = u8g2_GetAscent(u8g2) - u8g2_GetDescent(u8g2) + MY_BORDER_SIZE;

    uint8_t title_lines = u8x8_GetStringLineCnt(title);
    uint8_t display_lines;

    if (start_pos > 0) /* issue 112 */
        start_pos--; /* issue 112 */

    if (title_lines > 0) {
        display_lines = (u8g2_GetDisplayHeight(u8g2) - 3) / line_height;
        u8sl.visible = display_lines;
        u8sl.visible -= title_lines;
    } else {
        display_lines = u8g2_GetDisplayHeight(u8g2) / line_height;
        u8sl.visible = display_lines;
    }

    u8sl.total = u8x8_GetStringLineCnt(sl);
    u8sl.first_pos = 0;
    u8sl.current_pos = start_pos;

    if (u8sl.current_pos >= u8sl.total)
        u8sl.current_pos = u8sl.total - 1;
    if (u8sl.first_pos + u8sl.visible <= u8sl.current_pos)
        u8sl.first_pos = u8sl.current_pos - u8sl.visible + 1;

    u8g2_SetFontPosBaseline(u8g2);

    for (;;) {
        u8g2_FirstPage(u8g2);
        do {
            yy = u8g2_GetAscent(u8g2);
            if (title_lines > 0) {
                yy += u8g2_DrawUTF8Lines(u8g2, 0, yy, u8g2_GetDisplayWidth(u8g2), line_height, title);

                u8g2_DrawHLine(u8g2, 0, yy - line_height - u8g2_GetDescent(u8g2) + 1, u8g2_GetDisplayWidth(u8g2));

                yy += 3;
            }
            u8g2_DrawSelectionList(u8g2, &u8sl, yy, sl);
        } while (u8g2_NextPage(u8g2));

#ifdef U8G2_REF_MAN_PIC
        return 0;
#endif

        for (;;) {
            event = getMenuEvent(queue, ticks_timeout);
            if (event == U8X8_MSG_GPIO_MENU_SELECT)
                return u8sl.current_pos + 1; /* +1, issue 112 */
            else if (event == U8X8_MSG_GPIO_MENU_HOME)
                return 0; /* issue 112: return 0 instead of start_pos */
            else if (event == U8X8_MSG_GPIO_MENU_NEXT || event == U8X8_MSG_GPIO_MENU_DOWN) {
                u8sl_Next(&u8sl);
                break;
            } else if (event == U8X8_MSG_GPIO_MENU_PREV || event == U8X8_MSG_GPIO_MENU_UP) {
                u8sl_Prev(&u8sl);
                break;
            } else if (event == MENU_EVENT_TIMEOUT) {
                return MENU_EVENT_TIMEOUT;
            }
        }
    }
}

/* Private functions ---------------------------------------------------------*/
uint8_t getMenuEvent(QueueHandle_t queue, TickType_t ticks_timeout)
{
    rotary_encoder_event_t queue_event = { 0 };

    if (pdTRUE == xQueueReceive(queue, &queue_event, ticks_timeout)) {
        if (queue_event.event_type == BUTTON_EVENT) {
            switch (queue_event.btn_event) {
            case SHORT_PRESS:
                return U8X8_MSG_GPIO_MENU_SELECT;
            case MEDIUM_PRESS:
            case LONG_PRESS:
                return U8X8_MSG_GPIO_MENU_HOME;
            }
        } else { /* ROTARY_ENCODER_EVENT */
            return queue_event.re_state.direction == ROTARY_ENCODER_DIRECTION_CLOCKWISE
                ? U8X8_MSG_GPIO_MENU_NEXT
                : U8X8_MSG_GPIO_MENU_PREV;
        }
    } else {
        return MENU_EVENT_TIMEOUT;
    }
    return 0; /* invalid message, no event */
}

/***************************** END OF FILE ************************************/
