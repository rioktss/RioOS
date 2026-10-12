#include "fb.h"
#include "font.h"
#ifndef HOST_TEST
#include "uart.h"
#endif

#define FB_BASE_ADDRESS 0x46800000ULL
#define FB_DEFAULT_WIDTH  1024
#define FB_DEFAULT_HEIGHT 768
#define FB_FONT_WIDTH     8
#define FB_FONT_HEIGHT    16
#define FB_BACKGROUND     0x00000000U

static uint32_t* fb_base = 0;
static int fb_w = FB_DEFAULT_WIDTH;
static int fb_h = FB_DEFAULT_HEIGHT;
static int cur_x = 0;
static int cur_y = 0;
static int fb_ready = 0;

static void fb_fill_rect(int x, int y, int width, int height, uint32_t color)
{
    if (!fb_ready || fb_base == 0 || width <= 0 || height <= 0) return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x >= fb_w || y >= fb_h) return;
    if (width > fb_w - x) width = fb_w - x;
    if (height > fb_h - y) height = fb_h - y;
    if (width <= 0 || height <= 0) return;

    for (int row = 0; row < height; ++row)
    {
        uint32_t* dst = fb_base + (uint64_t)(y + row) * (uint64_t)fb_w + (uint64_t)x;
        for (int col = 0; col < width; ++col) dst[col] = color;
    }
}

static void fb_scroll_one_line()
{
    if (!fb_ready || fb_base == 0 || fb_h <= FB_FONT_HEIGHT) return;

    const uint64_t row_pixels = (uint64_t)fb_w;
    const int remaining_rows = fb_h - FB_FONT_HEIGHT;
    for (int y = 0; y < remaining_rows; ++y)
    {
        uint32_t* dst = fb_base + (uint64_t)y * row_pixels;
        const uint32_t* src = fb_base + (uint64_t)(y + FB_FONT_HEIGHT) * row_pixels;
        for (int x = 0; x < fb_w; ++x) dst[x] = src[x];
    }
    fb_fill_rect(0, fb_h - FB_FONT_HEIGHT, fb_w, FB_FONT_HEIGHT, FB_BACKGROUND);
    cur_y = fb_h - FB_FONT_HEIGHT;
    if (cur_y < 0) cur_y = 0;
}

static void fb_advance_line()
{
    cur_x = 0;
    cur_y += FB_FONT_HEIGHT;
    if (cur_y + FB_FONT_HEIGHT > fb_h)
        fb_scroll_one_line();
    if (cur_y + FB_FONT_HEIGHT > fb_h)
        cur_y = fb_h >= FB_FONT_HEIGHT ? fb_h - FB_FONT_HEIGHT : 0;
}

static void fb_draw_char(int x, int y, char c, uint32_t color)
{
    if (!fb_ready || fb_base == 0) return;

    unsigned int uc = (unsigned char)c;
    if (uc >= 128U) uc = (unsigned int)'?';

    for (int row = 0; row < FB_FONT_HEIGHT; ++row)
    {
        uint8_t bits = font8x8[uc][row >> 1];
        for (int col = 0; col < FB_FONT_WIDTH; ++col)
        {
            int px = x + col;
            int py = y + row;
            if (px < 0 || px >= fb_w || py < 0 || py >= fb_h) continue;
            uint32_t pixel = (bits & (uint8_t)(0x80U >> col)) != 0U ? color : FB_BACKGROUND;
            fb_base[(uint64_t)py * (uint64_t)fb_w + (uint64_t)px] = pixel;
        }
    }
}

void fb_init()
{
#ifdef HOST_TEST
    /* Host tests must attach their own surface; never dereference a fixed MMIO-like address. */
    fb_base = 0;
    fb_ready = 0;
    cur_x = 0;
    cur_y = 0;
#else
    fb_base = (uint32_t*)FB_BASE_ADDRESS;
    fb_w = FB_DEFAULT_WIDTH;
    fb_h = FB_DEFAULT_HEIGHT;
    cur_x = 0;
    cur_y = 0;
    fb_ready = 1;
    fb_clear(FB_BACKGROUND);
    uart_puts("Framebuffer RAM surface initialized (not a QEMU display device).\r\n");
#endif
}

void fb_clear(uint32_t color)
{
    if (!fb_ready || fb_base == 0)
    {
        cur_x = 0;
        cur_y = 0;
        return;
    }
    fb_fill_rect(0, 0, fb_w, fb_h, color);
    cur_x = 0;
    cur_y = 0;
}

void fb_set_cursor(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    cur_x = x - (x % FB_FONT_WIDTH);
    cur_y = y - (y % FB_FONT_HEIGHT);

    int max_x = fb_w >= FB_FONT_WIDTH ? fb_w - FB_FONT_WIDTH : 0;
    int max_y = fb_h >= FB_FONT_HEIGHT ? fb_h - FB_FONT_HEIGHT : 0;
    if (cur_x > max_x) cur_x = max_x - (max_x % FB_FONT_WIDTH);
    if (cur_y > max_y) cur_y = max_y - (max_y % FB_FONT_HEIGHT);
    if (cur_x < 0) cur_x = 0;
    if (cur_y < 0) cur_y = 0;
}

void fb_putc(char c, uint32_t color)
{
    if (!fb_ready || fb_base == 0) return;

    if (c == '\n')
    {
        fb_advance_line();
        return;
    }
    if (c == '\r')
    {
        cur_x = 0;
        return;
    }
    if (c == '\b')
    {
        if (cur_x >= FB_FONT_WIDTH) cur_x -= FB_FONT_WIDTH;
        else if (cur_y >= FB_FONT_HEIGHT)
        {
            cur_y -= FB_FONT_HEIGHT;
            cur_x = fb_w >= FB_FONT_WIDTH ? (fb_w - FB_FONT_WIDTH) / FB_FONT_WIDTH * FB_FONT_WIDTH : 0;
        }
        fb_draw_char(cur_x, cur_y, ' ', color);
        return;
    }
    if ((unsigned char)c < 32U || (unsigned char)c >= 128U) return;
    if (fb_w < FB_FONT_WIDTH || fb_h < FB_FONT_HEIGHT) return;

    fb_draw_char(cur_x, cur_y, c, color);
    cur_x += FB_FONT_WIDTH;
    if (cur_x + FB_FONT_WIDTH > fb_w)
        fb_advance_line();
}

void fb_print(const char* s, uint32_t color)
{
    if (s == 0) return;
    while (*s) fb_putc(*s++, color);
}

int fb_get_width() { return fb_w; }
int fb_get_height() { return fb_h; }
int fb_is_ready() { return fb_ready && fb_base != 0; }

#ifdef HOST_TEST
int fb_test_attach(uint32_t* pixels, int width, int height)
{
    if (pixels == 0 || width < FB_FONT_WIDTH || height < FB_FONT_HEIGHT || width > 4096 || height > 4096)
    {
        fb_base = 0;
        fb_ready = 0;
        return -1;
    }
    fb_base = pixels;
    fb_w = width;
    fb_h = height;
    cur_x = 0;
    cur_y = 0;
    fb_ready = 1;
    fb_clear(FB_BACKGROUND);
    return 0;
}
#endif
