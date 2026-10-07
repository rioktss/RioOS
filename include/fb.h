#ifndef FB_H
#define FB_H

#include "types.h"

void fb_init();
void fb_clear(uint32_t color);
void fb_putc(char c, uint32_t color);
void fb_print(const char* s, uint32_t color);
void fb_set_cursor(int x, int y);
int fb_get_width();
int fb_get_height();

#endif
