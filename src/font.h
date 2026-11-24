/* SPDX-License-Identifier: GPL-3.0-or-later
 * VitaZoom - pinch-to-zoom kernel plugin for PS Vita (taiHEN)
 * Copyright (C) 2026 VitaZoom contributors
 */
#ifndef VZ_FONT_H
#define VZ_FONT_H
/* 8x16 bitmap glyphs for ASCII 32..126, MSB = leftmost pixel */
extern const unsigned char font8x16[95][16];
#define FONT_W 8
#define FONT_H 16
/* 12x24, 2 bits/pixel: ASCII 32..126 then Latin-1 160..255 */
extern const unsigned char font_big[191][72];
#endif
