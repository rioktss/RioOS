#include "ui.h"
#include "fb.h"

static void lock_time_text(uint64_t epoch, char out[6])
{
    uint64_t seconds = epoch % 86400ULL;
    uint32_t hour = (uint32_t)(seconds / 3600ULL);
    uint32_t minute = (uint32_t)((seconds / 60ULL) % 60ULL);
    out[0] = (char)('0' + hour / 10U);
    out[1] = (char)('0' + hour % 10U);
    out[2] = ':';
    out[3] = (char)('0' + minute / 10U);
    out[4] = (char)('0' + minute % 10U);
    out[5] = '\0';
}

void ui_draw_lock_screen(uint64_t epoch)
{
    int w = fb_get_width(), h = fb_get_height();
    if (!ui_wallpaper_draw_background())
    {
        fb_fill_gradient(0x101D49U, 0x5D3A83U);
        fb_draw_circle(w - 80, 130, 200, 0x243C70U);
        fb_draw_circle(w - 35, 95, 142, 0x2A5682U);
        fb_draw_circle(100, h - 50, 230, 0x39417CU);
        fb_draw_circle(65, h - 75, 150, 0x4F488BU);
    }
    fb_draw_round_rect(70, 54, w - 140, 2, 1, 0x50FFFFFFU);

    ui_draw_status_bar(epoch);
    char time_text[6];
    lock_time_text(epoch, time_text);
    fb_draw_ui_text("WELCOME BACK", 80, h / 2 - 160, 0xD7E7FFU, 2);
    fb_draw_ui_text(time_text, 76, h / 2 - 98, 0xFFFFFFFFU, 6);
    fb_draw_ui_text("RioOS  |  MyKernel", 82, h / 2 - 24, 0xD6DFFFU, 2);

    int cx = w / 2;
    int bottom = h - 100;
    fb_draw_round_rect(cx - 35, bottom - 28, 70, 6, 3, 0xE5EAFFU);
    const char* prompt = "SWIPE UP OR PRESS ANY KEY TO UNLOCK";
    fb_draw_ui_text(prompt, (w - fb_ui_text_width(prompt, 2)) / 2, bottom + 5, 0xFFFFFFU, 2);
    fb_draw_ui_text("TOUCH / KEYBOARD / SERIAL", (w - fb_ui_text_width("TOUCH / KEYBOARD / SERIAL", 1)) / 2,
                 h - 37, 0xC4C9E8U, 1);
    fb_present();
}
