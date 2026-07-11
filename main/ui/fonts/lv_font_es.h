#pragma once

/* Custom-generated fonts covering Latin-1 Supplement (0xA0-0xFF: n/N with
 * tilde, accented vowels, u/U diaeresis, inverted ?/!, etc.) on top of
 * ASCII + LVGL's built-in symbol icons - the stock lv_font_montserrat_14/
 * _20 only cover ASCII + a couple of extras (degree, bullet) + icons, so
 * Spanish text rendered as missing/blank glyphs (ANALYSIS.md 1.25).
 *
 * Generated from the same sources LVGL's own built-in fonts use
 * (managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf
 * + FontAwesome5-Solid+Brands+Regular.woff, both open-licensed) via
 * lv_font_conv, range extended from "0x20-0x7F,0xB0,0x2022" to
 * "0x20-0x7F,0xA0-0xFF,0x2022". Regenerate with:
 *
 *   npx lv_font_conv --no-compress --no-prefilter --bpp 4 --size <14|20> \
 *     --font Montserrat-Medium.ttf -r 0x20-0x7F,0xA0-0xFF,0x2022 \
 *     --font "FontAwesome5-Solid+Brands+Regular.woff" -r <same symbol list \
 *     as built_in_font_gen.py's `syms`> --format lvgl \
 *     -o lv_font_es_<size>.c --force-fast-kern-format
 *
 * lv_font_es_14 replaces LV_FONT_DEFAULT (ui_init(), ui.c) - the app-wide
 * fallback for labels without an explicit font. lv_font_es_20 replaces
 * explicit &lv_font_montserrat_20 references (titles, nav icons). */

#include "lvgl.h"

extern const lv_font_t lv_font_es_14;
extern const lv_font_t lv_font_es_20;
