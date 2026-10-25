#include "ui.h"
#include "fb.h"

static int launcher_selected = 0;
static int launcher_target = UI_APP_TERMINAL;
static int launcher_open_requested = 0;

void ui_launcher_init()
{
    launcher_selected = 0;
    launcher_target = UI_APP_TERMINAL;
    launcher_open_requested = 0;
}

void ui_launcher_render(uint64_t epoch)
{
    (void)epoch;
    ui_draw_app_frame("App launcher", "Installed RPK packages / built-in apps");
    const int columns = 4;
    const int cell_w = (fb_get_width() - 88) / columns;
    const int start_x = (fb_get_width() - columns * cell_w) / 2;
    for (int app = 0; app < UI_APP_COUNT; ++app)
    {
        int row = app / columns, col = app % columns;
        int x = start_x + col * cell_w;
        int y = 150 + row * 218;
        if (app == launcher_selected)
            fb_draw_round_rect(x + 8, y, cell_w - 16, 184, 18, 0x3B5874U);
        ui_draw_app_icon(app, x + cell_w / 2, y + 57, 72);
        const char* name = ui_app_name(app);
        fb_draw_ui_text(name, x + (cell_w - fb_ui_text_width(name, 1)) / 2, y + 108, 0xFFFFFFU, 1);
        int valid = ui_app_package_valid(app);
        const char* state = valid ? "RPK READY" : "PACKAGE MISSING";
        fb_draw_ui_text(state, x + (cell_w - fb_ui_text_width(state, 1)) / 2, y + 143,
                     valid ? 0x80E7B6U : 0xFFB2A5U, 1);
    }
    ui_draw_bottom_hint("A/D or W/S: Select   ENTER: Launch package   Q: Home");
    fb_present();
}

int ui_launcher_key(char key)
{
    if (key == 'q' || key == 'Q') return 1;
    if (key == 'a' || key == 'A') { if (launcher_selected > 0) --launcher_selected; }
    else if (key == 'd' || key == 'D') { if (launcher_selected + 1 < UI_APP_COUNT) ++launcher_selected; }
    else if (key == 'w' || key == 'W') { if (launcher_selected >= 4) launcher_selected -= 4; }
    else if (key == 's' || key == 'S') { if (launcher_selected + 4 < UI_APP_COUNT) launcher_selected += 4; }
    else if (key >= '1' && key <= '7') launcher_selected = key - '1';
    else if (key == '\r' || key == '\n')
    {
        if (ui_app_package_valid(launcher_selected))
        {
            launcher_target = launcher_selected;
            launcher_open_requested = 1;
            return 2;
        }
    }
    return 0;
}

int ui_launcher_get_target()
{
    (void)launcher_open_requested;
    return launcher_target;
}

int ui_launcher_pointer(int x, int y)
{
    const int columns = 4;
    const int cell_w = (fb_get_width() - 88) / columns;
    const int start_x = (fb_get_width() - columns * cell_w) / 2;
    if (cell_w <= 0) return 0;
    for (int app = 0; app < UI_APP_COUNT; ++app)
    {
        int row = app / columns, col = app % columns;
        int left = start_x + col * cell_w + 8;
        int top = 150 + row * 218;
        if (x >= left && x < left + cell_w - 16 && y >= top && y < top + 184)
        {
            launcher_selected = app;
            if (!ui_app_package_valid(app)) return 0;
            launcher_target = app;
            launcher_open_requested = 1;
            return 2;
        }
    }
    return 0;
}
