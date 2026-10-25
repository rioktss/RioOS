#include "ui.h"
#include "fb.h"
#include "timer.h"

void ui_clock_init() {}

void ui_clock_render(uint64_t epoch)
{
    ui_draw_app_frame("Clock", "Local device clock from the QEMU PL031 RTC");
    char time_text[6];
    ui_format_clock(epoch, time_text);
    int w = fb_get_width();
    fb_draw_circle(w / 2, 320, 120, 0x263E5BU);
    fb_draw_circle(w / 2, 320, 105, 0xDCE9FBU);
    fb_draw_circle(w / 2, 320, 96, 0x263E5BU);
    fb_draw_line(w / 2, 320, w / 2, 255, 0xFFFFFFU);
    fb_draw_line(w / 2, 320, w / 2 + 49, 343, 0x83E3C3U);
    fb_draw_circle(w / 2, 320, 6, 0xFFFFFFU);
    fb_draw_ui_text(time_text, (w - fb_ui_text_width(time_text, 4)) / 2, 474, 0xFFFFFFU, 4);
    fb_draw_ui_text("24-HOUR CLOCK", (w - fb_ui_text_width("24-HOUR CLOCK", 2)) / 2, 535, 0xAFC1D9U, 2);
    fb_draw_ui_text("Uptime (seconds):", 370, 590, 0xAFC1D9U, 1);
    uint64_t uptime = timer_seconds();
    char digits[24]; int n = 0;
    if (uptime == 0) digits[n++] = '0';
    while (uptime > 0 && n < 20) { digits[n++] = (char)('0' + (uptime % 10ULL)); uptime /= 10ULL; }
    char out[24]; int k = 0;
    while (n > 0) out[k++] = digits[--n];
    out[k] = '\0';
    fb_draw_ui_text(out, 505, 590, 0xFFFFFFU, 1);
    ui_draw_bottom_hint("Q: Back to home   |   Clock refreshes automatically");
    fb_present();
}

int ui_clock_key(char key)
{
    if (key == 'q' || key == 'Q') return 1;
    return 0;
}
