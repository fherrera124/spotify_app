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
 * @param image_w The width of the jpeg file
 * @param image_h The height of the jpeg file
 * @param scale The scale of the jpeg file. JPEG_IMAGE_SCALE_0 is no scale
 * @note The jpeg file must be in RGB565 format, and the decoder library cannot handle progressive files.
 * @return - ESP_ERR_NOT_SUPPORTED if image is malformed or a progressive jpeg file
 *         - ESP_ERR_NO_MEM if out of memory
 *         - ESP_OK on succesful decode
 */
esp_err_t decode_image(uint16_t *pixels, const uint8_t *image_jpg, uint32_t image_jpg_size, size_t image_w, size_t image_h, esp_jpeg_image_scale_t scale);
