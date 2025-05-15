/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
The image used for the effect on the LCD in the SPI master example is stored in flash
as a jpeg file. This file contains the decode_image routine, which uses the tiny JPEG
decoder library to decode this JPEG into a format that can be sent to the display.

Keep in mind that the decoder library cannot handle progressive files (will give
``Image decoder: jd_prepare failed (8)`` as an error) so make sure to save in the correct
format if you want to use a different image file.
*/

#include "decode_jpg.h"
#include "jpeg_decoder.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include "freertos/FreeRTOS.h"

//Define the height and width of the jpeg file. Make sure this matches the actual jpeg
//dimensions.

const char *TAG = "ImageDec";

#define JPEG_WORK_BUF_SIZE  3100

//Decode the embedded image into pixel lines that can be used with the rest of the logic.
esp_err_t decode_image(uint16_t *pixels, const uint8_t *image_jpg, uint32_t image_jpg_size, size_t image_w, size_t image_h, esp_jpeg_image_scale_t scale)
{
    esp_err_t ret = ESP_OK;

    uint8_t *workbuf = NULL;

    workbuf = heap_caps_malloc(JPEG_WORK_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (workbuf == NULL)
    {
        return ESP_ERR_NO_MEM;
        //ESP_GOTO_ON_FALSE(workbuf, ESP_ERR_NO_MEM, err, TAG, "no mem for JPEG work buffer");
    }
    
    /* pixels = calloc(IMAGE_H * IMAGE_W, sizeof(uint16_t));
    ESP_GOTO_ON_FALSE((pixels), ESP_ERR_NO_MEM, err, TAG, "Error allocating memory for lines"); */

    //JPEG decode config
    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = (uint8_t *)image_jpg,
        .indata_size = image_jpg_size,
        .outbuf = (uint8_t*)pixels,
        .outbuf_size = image_w * image_h * sizeof(uint16_t),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale,
        .flags = {
            .swap_color_bytes = 1,
        },
        .advanced = {
            .working_buffer = (uint8_t*)workbuf,
            .working_buffer_size = JPEG_WORK_BUF_SIZE,
        }
    };

    //JPEG decode
    esp_jpeg_image_output_t outimg;
    esp_jpeg_decode(&jpeg_cfg, &outimg);

    ESP_LOGD(TAG, "JPEG image decoded! Size of the decoded image is: %dpx x %dpx", outimg.width, outimg.height);
    free(workbuf);
    return ret;
/* err:
    //Something went wrong! Exit cleanly, de-allocating everything we allocated.
    if (pixels != NULL) {
        free(*pixels);
    }
    return ret; */
}
