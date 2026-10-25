#include "ui.h"
#include "fb.h"

static const uint32_t BOOT_BG = 0x070B18U;
static const uint32_t BOOT_WHITE = 0xFFFFFFFFU;
static const uint32_t BOOT_BLUE = 0x5EC8FFU;

void ui_draw_boot_screen()
{
    const int w = fb_get_width();
    const int h = fb_get_height();
    fb_clear(BOOT_BG);

    /* Pixel-rendered orbit mark: deliberately drawn from geometric primitives. */
    int cx = w / 2;
    int cy = h / 2 - 95;
    fb_draw_circle(cx, cy, 52, 0x142B49U);
    fb_draw_circle(cx, cy, 42, BOOT_BLUE);
    fb_draw_circle(cx, cy, 31, BOOT_BG);
    fb_draw_round_rect(cx - 8, cy - 29, 16, 58, 7, BOOT_WHITE);
    fb_draw_round_rect(cx - 28, cy - 8, 56, 16, 7, BOOT_WHITE);
    fb_draw_circle(cx + 30, cy - 28, 7, 0xA8EEFFU);

    const char* title = "RioOS";
    const int scale = 4;
    fb_draw_ui_text(title, (w - fb_ui_text_width(title, scale)) / 2, cy + 65, BOOT_WHITE, scale);
    const char* subtitle = "A BARE-METAL ARM64 EXPERIENCE";
    fb_draw_ui_text(subtitle, (w - fb_ui_text_width(subtitle, 2)) / 2, cy + 145, 0x9EB4D0U, 2);

    int bar_w = w / 3;
    int bar_x = (w - bar_w) / 2;
    int bar_y = h - 86;
    fb_draw_round_rect(bar_x, bar_y, bar_w, 5, 3, 0x20334BU);
    fb_draw_round_rect(bar_x, bar_y, bar_w / 3, 5, 3, BOOT_BLUE);
    const char* status = "STARTING MYKERNEL";
    fb_draw_ui_text(status, (w - fb_ui_text_width(status, 1)) / 2, bar_y + 20, 0x8EA5C1U, 1);
    fb_present();
}
