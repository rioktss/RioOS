#ifndef FB_H
#define FB_H

#include "types.h"

/* A freestanding pixel renderer. Colors use 0x00RRGGBB (XRGB8888). */
void fb_init();
void fb_clear(uint32_t color);
void fb_putc(char c, uint32_t color);
void fb_print(const char* s, uint32_t color);
void fb_set_cursor(int x, int y);
int fb_get_width();
int fb_get_height();
int fb_is_ready();
int fb_has_display();
void fb_present();
void fb_draw_pixel(int x, int y, uint32_t color);
void fb_fill_rect(int x, int y, int width, int height, uint32_t color);
void fb_draw_rect(int x, int y, int width, int height, uint32_t color);
void fb_draw_round_rect(int x, int y, int width, int height, int radius, uint32_t color);
void fb_draw_circle(int cx, int cy, int radius, uint32_t color);
void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void fb_fill_gradient(uint32_t top, uint32_t bottom);
void fb_draw_text(const char* text, int x, int y, uint32_t color, int scale);
int fb_text_width(const char* text, int scale);
int fb_ui_text_width(const char* text, int scale);
void fb_draw_ui_text(const char* text, int x, int y, uint32_t color, int scale);

#ifdef HOST_TEST
int fb_test_attach(uint32_t* pixels, int width, int height);
uint32_t fb_test_pixel(int x, int y);
#endif

#endif
