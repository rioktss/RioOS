#include "ui.h"
#include "fb.h"

static void home_time_text(uint64_t epoch, char out[6])
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

void ui_draw_status_bar(uint64_t epoch)
{
    (void)epoch;
    int w = fb_get_width();
    int dark = ui_settings_is_dark();
    fb_fill_rect(0, 0, w, 34, dark ? 0x171D2CU : 0xE7EEF7U);
    fb_draw_ui_text("RIOOS", 24, 12, dark ? 0xFFFFFFFFU : 0x20344CU, 1);

    /* Pixel battery and wireless indicators (not decorative ASCII characters). */
    int bx = w - 47;
    uint32_t status_fg = dark ? 0xE9F0FFU : 0x253A54U;
    fb_draw_rect(bx, 11, 24, 12, status_fg);
    fb_fill_rect(bx + 24, 14, 3, 6, status_fg);
    fb_fill_rect(bx + 3, 14, 16, 6, 0x3BAF86U);
    fb_draw_line(w - 88, 22, w - 78, 10, status_fg);
    fb_draw_line(w - 78, 10, w - 68, 22, status_fg);
    fb_draw_line(w - 84, 22, w - 78, 15, status_fg);
    fb_draw_line(w - 78, 15, w - 72, 22, status_fg);
    fb_draw_circle(w - 78, 23, 2, status_fg);
}

void ui_draw_app_icon(int app_id, int cx, int cy, int size)
{
    if (size < 24) size = 24;
    int x = cx - size / 2, y = cy - size / 2;
    uint32_t base = 0x607D8BU;
    if (app_id == UI_APP_TERMINAL) base = 0x13252BU;
    else if (app_id == UI_APP_CALCULATOR) base = 0x3379D4U;
    else if (app_id == UI_APP_CLOCK) base = 0xDDE7F3U;
    else if (app_id == UI_APP_FILES) base = 0xE9AD43U;
    else if (app_id == UI_APP_LAUNCHER) base = 0xFFFFFFU;
    fb_draw_round_rect(x, y, size, size, size / 4, base);

    char icon_path[192];
    if (ui_app_icon_path(app_id, icon_path, sizeof(icon_path)) == 0 &&
        ui_image_draw_bmp(icon_path, x, y, size, size, size / 4) == 0)
        return;

    int unit = size / 10;
    if (unit < 2) unit = 2;
    if (app_id == UI_APP_TERMINAL)
    {
        fb_draw_round_rect(x + size / 6, y + size / 5, size * 2 / 3, size * 3 / 5,
                           size / 10, 0x263F48U);
        fb_draw_line(x + size / 4, y + size / 2, x + size / 3, y + size * 3 / 5, 0xA8F0C6U);
        fb_draw_line(x + size / 3, y + size * 3 / 5, x + size / 4, y + size * 7 / 10, 0xA8F0C6U);
        fb_fill_rect(x + size * 2 / 5, y + size * 7 / 10, size / 5, unit, 0xA8F0C6U);
    }
    else if (app_id == UI_APP_CALCULATOR)
    {
        fb_draw_round_rect(x + size / 5, y + size / 6, size * 3 / 5, size * 2 / 3, size / 12, 0xFFFFFFU);
        fb_fill_rect(x + size / 4, y + size / 4, size / 2, size / 10, 0x24466EU);
        for (int ry = 0; ry < 2; ++ry)
            for (int rx = 0; rx < 3; ++rx)
                fb_draw_circle(x + size / 3 + rx * size / 6, y + size / 2 + ry * size / 6,
                               unit, (rx == 2 && ry == 1) ? 0xE99A40U : 0x5B95DDU);
    }
    else if (app_id == UI_APP_CLOCK)
    {
        fb_draw_circle(cx, cy, size * 3 / 10, 0xFFFFFFU);
        fb_draw_circle(cx, cy, size * 3 / 10 - unit, 0x4778B5U);
        fb_draw_line(cx, cy, cx, cy - size / 5, 0x233E68U);
        fb_draw_line(cx, cy, cx + size / 6, cy + size / 10, 0x233E68U);
        fb_draw_circle(cx, cy, unit, 0xFFFFFFU);
    }
    else if (app_id == UI_APP_FILES)
    {
        fb_draw_round_rect(x + size / 6, y + size / 3, size * 2 / 3, size / 2, size / 12, 0xFFD26AU);
        fb_draw_round_rect(x + size / 6, y + size / 4, size / 3, size / 5, size / 12, 0xFFD26AU);
        fb_draw_line(x + size / 4, y + size / 2, x + size * 3 / 4, y + size / 2, 0xD18C26U);
    }
    else
    {
        uint32_t dots[9] = {0xF05A68U,0xF0B640U,0x59C98FU,0x5DA7E8U,0xA481E8U,
                            0xE56FB2U,0x62C7C6U,0x8A9CB1U,0xF08C5AU};
        int index = 0;
        for (int ry = -1; ry <= 1; ++ry)
            for (int rx = -1; rx <= 1; ++rx)
                fb_draw_circle(cx + rx * size / 5, cy + ry * size / 5, unit + 1, dots[index++]);
    }
}

void ui_draw_home_screen(int selected, uint64_t epoch)
{
    int w = fb_get_width(), h = fb_get_height();
    int dark = ui_settings_is_dark();
    if (!ui_wallpaper_draw_background())
    {
        fb_fill_gradient(0x182D52U, 0x4B4789U);
        fb_draw_circle(w - 20, 130, 170, 0x275D86U);
        fb_draw_circle(w - 5, 115, 118, 0x2B7390U);
        fb_draw_circle(70, h - 10, 180, 0x2A6381U);
        fb_draw_circle(90, h - 4, 130, 0x3A7C91U);
    }
    ui_draw_status_bar(epoch);

    fb_draw_ui_text("Good day", 74, 65, dark ? 0xDCEBFFU : 0x294462U, 2);
    fb_draw_ui_text("Welcome to RioOS", 74, 94, dark ? 0xFFFFFFFFU : 0x182D47U, 2);

    fb_draw_round_rect(64, 148, w - 128, 144, 28, dark ? 0x1D2E48U : 0xF0F5FCU);
    fb_draw_round_rect(80, 164, 5, 110, 2, dark ? 0x75E0D3U : 0x3BAF9AU);
    char time_text[6];
    home_time_text(epoch, time_text);
    fb_draw_ui_text(time_text, 112, 172, dark ? 0xFFFFFFFFU : 0x172D47U, 4);
    fb_draw_ui_text("MYKERNEL  /  ARM64", 116, 235, dark ? 0xC4D8F2U : 0x48627F, 1);
    fb_draw_ui_text("Your apps, your system", 116, 258, dark ? 0xB0C8E8U : 0x5A7088U, 1);

    const int columns = 4;
    int cell_w = (w - 160) / columns;
    for (int app = 0; app < UI_APP_COUNT; ++app)
    {
        int col = app % columns;
        int row = app / columns;
        int cx = 80 + col * cell_w + cell_w / 2;
        int icon_y = row == 0 ? 368 : 500;
        if (app == selected)
            fb_draw_round_rect(cx - 51, icon_y - 48, 102, 100, 23,
                               dark ? 0x394963U : 0xD7E4F3U);
        ui_draw_app_icon(app, cx, icon_y, 62);
        const char* name = ui_app_name(app);
        fb_draw_ui_text(name, cx - fb_ui_text_width(name, 1) / 2, icon_y + 42,
                     dark ? 0xFFFFFFU : 0x203650U, 1);
    }

    fb_draw_round_rect(72, h - 100, w - 144, 64, 30, dark ? 0x1C2B41U : 0xECF2FAU);
    fb_draw_circle(w / 2 - 43, h - 68, 6, dark ? 0xE0E9F8U : 0x4D6682U);
    fb_draw_round_rect(w / 2 - 18, h - 75, 36, 14, 5, dark ? 0xE0E9F8U : 0x4D6682U);
    fb_draw_circle(w / 2 + 43, h - 68, 7, dark ? 0xE0E9F8U : 0x4D6682U);
    const char* hint = "SWIPE DOWN: CONTROL CENTRE   ENTER: OPEN   1-7: OPEN APP   X: SERIAL SHELL";
    fb_draw_ui_text(hint, (w - fb_ui_text_width(hint, 1)) / 2, h - 27,
                 dark ? 0xE2E9FAU : 0x304965U, 1);
    fb_present();
}
