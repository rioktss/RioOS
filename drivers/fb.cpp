#include "fb.h"
#include "font.h"
#include "uart.h"
#include "types.h"

/*
 * Software framebuffer backend.
 *
 * RioOS runs in QEMU's text/serial mode by default, so the framebuffer is
 * deliberately opt-in: fb_putc()/fb_print() are safe before fb_init() and
 * simply do nothing until a backing RAM region is attached.
 *
 * The fixed backing address is inside the QEMU RAM window and is reserved by
 * memory.cpp. It is intentionally not exposed to user space.
 */
#define FB_BASE_ADDRESS 0x46800000ULL
#define FB_WIDTH  1024
#define FB_HEIGHT 768
#define FB_BPP 4
#define FB_STRIDE (FB_WIDTH * FB_BPP)
#define FB_BYTES ((uint64_t)FB_STRIDE * FB_HEIGHT)

static uint32_t* fb_base = 0;
static int fb_w = FB_WIDTH;
static int fb_h = FB_HEIGHT;
static int cur_x = 0;
static int cur_y = 0;
static int fb_ready = 0;

static void fb_draw_char(int x, int y, char c, uint32_t color)
{
    if (!fb_ready || fb_base == 0)
        return;

    unsigned int uc = (unsigned char)c;
    if (uc >= 128U)
        uc = '?';

    for (int row = 0; row < 16; ++row)
    {
        /* Expand the supplied 8x8 font to a readable 8x16 glyph. */
        uint8_t bits = font8x8[uc][row >> 1];
        for (int col = 0; col < 8; ++col)
        {
            if ((bits & (uint8_t)(0x80U >> col)) != 0U)
            {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < fb_w && py >= 0 && py < fb_h)
                    fb_base[(uint64_t)py * (uint64_t)fb_w + (uint64_t)px] = color;
            }
        }
    }
}

void fb_init()
{
    fb_base = (uint32_t*)FB_BASE_ADDRESS;
    fb_w = FB_WIDTH;
    fb_h = FB_HEIGHT;
    cur_x = 0;
    cur_y = 0;
    fb_ready = 1;
    fb_clear(0x00000000U);
    uart_puts("Framebuffer backend initialized (software RAM surface).\r\n");
}

void fb_clear(uint32_t color)
{
    if (!fb_ready || fb_base == 0)
    {
        cur_x = 0;
        cur_y = 0;
        return;
    }

    for (int y = 0; y < fb_h; ++y)
        for (int x = 0; x < fb_w; ++x)
            fb_base[(uint64_t)y * (uint64_t)fb_w + (uint64_t)x] = color;

    cur_x = 0;
    cur_y = 0;
}

void fb_set_cursor(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    cur_x = x - (x % 8);
    cur_y = y - (y % 16);
    if (cur_x >= fb_w) cur_x = fb_w - 8;
    if (cur_y >= fb_h) cur_y = fb_h - 16;
}

void fb_putc(char c, uint32_t color)
{
    if (!fb_ready || fb_base == 0)
        return;

    if (c == '\n')
    {
        cur_x = 0;
        cur_y += 16;
    }
    else if (c == '\r')
    {
        cur_x = 0;
    }
    else if (c == '\b')
    {
        if (cur_x >= 8)
        {
            cur_x -= 8;
            fb_draw_char(cur_x, cur_y, ' ', 0x00000000U);
        }
    }
    else if ((unsigned char)c >= 32U && (unsigned char)c < 128U)
    {
        fb_draw_char(cur_x, cur_y, c, color);
        cur_x += 8;
        if (cur_x >= fb_w - 8)
        {
            cur_x = 0;
            cur_y += 16;
        }
    }

    if (cur_y >= fb_h - 16)
        fb_clear(0x00000000U);
}

void fb_print(const char* s, uint32_t color)
{
    if (s == 0)
        return;
    while (*s)
        fb_putc(*s++, color);
}

int fb_get_width()  { return fb_w; }
int fb_get_height() { return fb_h; }
