/*
 * MIT License
 *
 * Copyright (c) 2026 Rio okta kusuma
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files, to deal in the Software
 * without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef FB_H
#define FB_H

#include "types.h"

/* Minimal pixel surface (XRGB8888, 0x00RRGGBB). The text terminal lives in
   console.cpp; this layer only owns the framebuffer memory and its display
   hand-off (QEMU ramfb on AArch64). */
void fb_init();
void fb_clear(uint32_t color);
int fb_get_width();
int fb_get_height();
int fb_is_ready();
int fb_has_display();
uint32_t* fb_buffer();
int fb_stride_pixels();
void fb_draw_pixel(int x, int y, uint32_t color);
void fb_fill_rect(int x, int y, int width, int height, uint32_t color);
/* Make pixel rows [y0, y1) visible to the display device (cache clean). */
void fb_present_rows(int y0, int y1);
void fb_present();

#ifdef HOST_TEST
int fb_test_attach(uint32_t* pixels, int width, int height);
uint32_t fb_test_pixel(int x, int y);
#endif

#endif
