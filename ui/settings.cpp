#include "ui.h"
#include "fb.h"
#include "fs.h"

static int theme_loaded = 0;
static int theme_dark = 1;
static int settings_target = UI_APP_WALLPAPER;
static int theme_notice = 0;

static int same_text(const char* a, const char* b)
{
    if (a == 0 || b == 0) return 0;
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) ++i;
    return a[i] == '\0' && b[i] == '\0';
}

static void load_theme()
{
    if (theme_loaded) return;
    uint8_t data[16];
    uint64_t size = 0;
    theme_dark = 1;
    if (fs_read_file("/settings/theme.cfg", fs_get_root(), data,
                      sizeof(data) - 1ULL, &size) == 0 && size < sizeof(data))
    {
        data[size] = 0;
        if (same_text((const char*)data, "light") || same_text((const char*)data, "light\n")) theme_dark = 0;
    }
    theme_loaded = 1;
}

int ui_settings_is_dark()
{
    load_theme();
    return theme_dark;
}

int ui_settings_toggle_theme()
{
    load_theme();
    int next = theme_dark ? 0 : 1;
    const char* value = next ? "dark" : "light";
    uint64_t len = next ? 4ULL : 5ULL;
    if (fs_write_data("/settings/theme.cfg", fs_get_root(), (const uint8_t*)value, len) != 0)
    {
        theme_notice = 1;
        return -1;
    }
    theme_dark = next;
    theme_notice = 0;
    return 0;
}

void ui_settings_init()
{
    load_theme();
    theme_notice = 0;
    settings_target = UI_APP_WALLPAPER;
}

void ui_settings_render(uint64_t epoch)
{
    (void)epoch;
    int dark = ui_settings_is_dark();
    ui_draw_app_frame("Settings", "Personalize RioOS / MyKernel");
    uint32_t panel = dark ? 0x25384FU : 0xE9EFF8U;
    uint32_t primary = dark ? 0xFFFFFFU : 0x203048U;
    uint32_t secondary = dark ? 0xBBD0E8U : 0x536A85U;
    fb_draw_round_rect(56, 132, fb_get_width() - 112, 102, 18, panel);
    fb_draw_circle(96, 180, 22, dark ? 0x54749BU : 0x86A9D9U);
    fb_draw_ui_text("Appearance", 136, 154, primary, 2);
    fb_draw_ui_text(dark ? "Dark mode is ON" : "Light mode is ON", 136, 190, secondary, 1);
    fb_draw_round_rect(fb_get_width() - 218, 162, 122, 36, 18, dark ? 0x3D9E87U : 0x597AB5U);
    fb_draw_ui_text("TOGGLE", fb_get_width() - 187, 174, 0xFFFFFFU, 1);

    fb_draw_round_rect(56, 254, fb_get_width() - 112, 102, 18, panel);
    fb_draw_round_rect(76, 280, 42, 42, 12, 0x5A80C6U);
    fb_draw_circle(97, 301, 12, 0xBFE7FFU);
    fb_draw_ui_text("Wallpaper", 136, 273, primary, 2);
    char active[192];
    ui_wallpaper_current_path(active, sizeof(active));
    fb_draw_ui_text(active, 138, 310, secondary, 1);
    fb_draw_round_rect(fb_get_width() - 218, 288, 122, 36, 18, 0x477DB4U);
    fb_draw_ui_text("OPEN", fb_get_width() - 184, 300, 0xFFFFFFU, 1);

    fb_draw_round_rect(56, 376, fb_get_width() - 112, 102, 18, panel);
    fb_draw_round_rect(76, 402, 42, 42, 12, 0x4A647FU);
    fb_draw_line(87, 423, 108, 423, 0xFFFFFFU);
    fb_draw_line(97, 412, 97, 434, 0xFFFFFFU);
    fb_draw_ui_text("Control Centre", 136, 394, primary, 2);
    fb_draw_ui_text("Swipe down from the top edge on Home", 138, 430, secondary, 1);
    fb_draw_ui_text("for quick theme, wallpaper, and lock controls.", 138, 449, secondary, 1);

    if (theme_notice)
    {
        fb_draw_round_rect(76, 512, fb_get_width() - 152, 32, 8, 0x7D423FU);
        fb_draw_ui_text("Could not save theme settings to /settings/theme.cfg", 90, 523, 0xFFFFFFU, 1);
    }
    ui_draw_bottom_hint("T: Toggle theme   W: Wallpaper   Q: Back to Home");
    fb_present();
}

int ui_settings_key(char key)
{
    if (key == 'q' || key == 'Q') return 1;
    if (key == 't' || key == 'T') { (void)ui_settings_toggle_theme(); return 0; }
    if (key == 'w' || key == 'W' || key == '\r' || key == '\n')
    {
        settings_target = UI_APP_WALLPAPER;
        return 2;
    }
    return 0;
}

int ui_settings_pointer(int x, int y)
{
    if (y >= 132 && y < 234 && x >= fb_get_width() - 244)
    {
        (void)ui_settings_toggle_theme();
    }
    else if (y >= 254 && y < 356)
    {
        settings_target = UI_APP_WALLPAPER;
        return 2;
    }
    return 0;
}

int ui_settings_get_target()
{
    return settings_target;
}
