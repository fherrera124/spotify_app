/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "jpeg_decoder.h"

/**
 * @brief Decode the jpeg ``image.jpg`` embedded into the program file into pixel data.
 *
 * @param pixels A pointer to a pointer for an array of rows, which themselves are an array of pixels.
 *        Effectively, you can get the pixel data by doing ``decode_image(&myPixels); pixelval=myPixels[ypos][xpos];``
 * @param image_jpg A pointer to the jpeg file
 * @param image_jpg_size The size of the jpeg file
 * @param image_w The width of the pixels buffer, used to size the decoder's output bound (not necessarily the jpeg's own width)
 * @param image_h The height of the pixels buffer, used to size the decoder's output bound (not necessarily the jpeg's own height)
 * @param scale The scale of the jpeg file. JPEG_IMAGE_SCALE_0 is no scale
 * @param out_width Set to the actual decoded width (post-scale), as reported by the decoder
 * @param out_height Set to the actual decoded height (post-scale), as reported by the decoder
 * @note The jpeg file must be in RGB565 format, and the decoder library cannot handle progressive files.
 * @return - ESP_ERR_NOT_SUPPORTED if image is malformed or a progressive jpeg file
 *         - ESP_ERR_NO_MEM if out of memory, or if the decoded image doesn't fit in image_w x image_h
 *         - ESP_OK on succesful decode
 */
esp_err_t decode_image(uint16_t *pixels, const uint8_t *image_jpg, uint32_t image_jpg_size, size_t image_w, size_t image_h, esp_jpeg_image_scale_t scale, uint16_t *out_width, uint16_t *out_height);
