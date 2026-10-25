#include "ui.h"
#include "fb.h"
#include "fs.h"
#include "string.h"

static const char* const app_names[UI_APP_COUNT] = {
    "Terminal", "Calculator", "Clock", "Files", "Launcher", "Wallpaper", "Settings"
};
static const char* const app_paths[UI_APP_COUNT] = {
    "/apps/terminal.rpk", "/apps/calculator.rpk", "/apps/clock.rpk",
    "/apps/files.rpk", "/apps/launcher.rpk", "/apps/wallpaper.rpk", "/apps/pengaturan.rpk"
};

const char* ui_app_name(int app_id)
{
    if (app_id < 0 || app_id >= UI_APP_COUNT) return "Unknown";
    return app_names[app_id];
}

const char* ui_app_package_path(int app_id)
{
    if (app_id < 0 || app_id >= UI_APP_COUNT) return "";
    return app_paths[app_id];
}

static int contains_text(const char* haystack, const char* needle)
{
    if (haystack == 0 || needle == 0 || needle[0] == '\0') return 0;
    for (int i = 0; haystack[i] != '\0'; ++i)
    {
        int j = 0;
        while (needle[j] != '\0' && haystack[i + j] == needle[j]) ++j;
        if (needle[j] == '\0') return 1;
    }
    return 0;
}

int ui_app_icon_path(int app_id, char* out, int capacity)
{
    if (app_id < 0 || app_id >= UI_APP_COUNT || out == 0 || capacity < 2) return -1;
    char data[384];
    uint64_t size = 0;
    if (fs_read_file(app_paths[app_id], fs_get_root(), (uint8_t*)data,
                     sizeof(data) - 1ULL, &size) != 0 || size == 0 || size >= sizeof(data))
        return -1;
    data[size] = '\0';
    const char key[] = "icon=";
    for (uint64_t i = 0; i + sizeof(key) - 1ULL < size; ++i)
    {
        if (i != 0 && data[i - 1] != '\n' && data[i - 1] != '\r') continue;
        int match = 1;
        for (uint32_t k = 0; k < sizeof(key) - 1U; ++k)
            if (data[i + k] != key[k]) { match = 0; break; }
        if (!match) continue;
        uint64_t begin = i + sizeof(key) - 1ULL, end = begin;
        while (end < size && data[end] != '\n' && data[end] != '\r') ++end;
        if (end == begin || end - begin >= (uint64_t)capacity) return -1;
        uint64_t out_len = end - begin;
        for (uint64_t n = 0; n < out_len; ++n) out[n] = data[begin + n];
        out[out_len] = '\0';
        return 0;
    }
    return -1;
}

int ui_app_package_valid(int app_id)
{
    if (app_id < 0 || app_id >= UI_APP_COUNT) return 0;
    char data[384];
    uint64_t size = 0;
    if (fs_read_file(app_paths[app_id], fs_get_root(), (uint8_t*)data,
                     sizeof(data) - 1ULL, &size) != 0 || size == 0 || size >= sizeof(data))
        return 0;
    data[size] = '\0';
    if (data[0] != 'R' || data[1] != 'I' || data[2] != 'O' || data[3] != 'P' ||
        data[4] != 'K' || data[5] != 'G' || data[6] != '/' || data[7] != '1')
        return 0;
    static const char* const ids[UI_APP_COUNT] = {
        "id=terminal", "id=calculator", "id=clock", "id=files", "id=launcher",
        "id=wallpaper", "id=settings"
    };
    static const char* const entries[UI_APP_COUNT] = {
        "entry=terminal", "entry=calculator", "entry=clock", "entry=files", "entry=launcher",
        "entry=wallpaper", "entry=settings"
    };
    return contains_text(data, ids[app_id]) && contains_text(data, entries[app_id]) &&
           contains_text(data, "type=builtin") && contains_text(data, "version=1.0.0");
}

void ui_draw_app_frame(const char* title, const char* subtitle)
{
    int w = fb_get_width(), h = fb_get_height();
    int dark = ui_settings_is_dark();
    fb_fill_gradient(dark ? 0x101827U : 0xEAF0F8U, dark ? 0x192A43U : 0xD9E5F4U);
    fb_fill_rect(0, 0, w, 34, dark ? 0x222E42U : 0xD5E2F1U);
    fb_draw_circle(26, 17, 5, 0x83E3C3U);
    fb_draw_ui_text("RIOOS", 42, 12, dark ? 0xCFDFF4U : 0x263A54U, 1);
    fb_draw_round_rect(26, 52, w - 52, h - 96, 22, dark ? 0xF0132031U : 0xF8FBFFFFU);
    if (title != 0) fb_draw_ui_text(title, 50, 58, dark ? 0xFFFFFFFFU : 0x20344CU, 2);
    if (subtitle != 0) fb_draw_ui_text(subtitle, 52, 86, dark ? 0xA9BEDA : 0x506882U, 1);
}

void ui_draw_bottom_hint(const char* hint)
{
    int w = fb_get_width(), h = fb_get_height();
    fb_draw_rect(30, h - 43, w - 60, 1, 0x43516AU);
    if (hint != 0) fb_draw_ui_text(hint, 40, h - 29, 0xAFC1D9U, 1);
}

void ui_format_clock(uint64_t epoch, char out[6])
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

void ui_apps_init(int app_id)
{
    switch (app_id)
    {
        case UI_APP_TERMINAL: ui_terminal_init(); break;
        case UI_APP_CALCULATOR: ui_calculator_init(); break;
        case UI_APP_CLOCK: ui_clock_init(); break;
        case UI_APP_FILES: ui_file_manager_init(); break;
        case UI_APP_LAUNCHER: ui_launcher_init(); break;
        case UI_APP_WALLPAPER: ui_wallpaper_init(); break;
        case UI_APP_SETTINGS: ui_settings_init(); break;
        default: break;
    }
}

void ui_apps_render(int app_id, uint64_t epoch)
{
    switch (app_id)
    {
        case UI_APP_TERMINAL: ui_terminal_render(epoch); break;
        case UI_APP_CALCULATOR: ui_calculator_render(epoch); break;
        case UI_APP_CLOCK: ui_clock_render(epoch); break;
        case UI_APP_FILES: ui_file_manager_render(epoch); break;
        case UI_APP_LAUNCHER: ui_launcher_render(epoch); break;
        case UI_APP_WALLPAPER: ui_wallpaper_render(epoch); break;
        case UI_APP_SETTINGS: ui_settings_render(epoch); break;
        default: break;
    }
}

int ui_apps_key(int app_id, char key)
{
    if ((unsigned char)key == 27U) return 1;
    switch (app_id)
    {
        case UI_APP_TERMINAL: return ui_terminal_key(key);
        case UI_APP_CALCULATOR: return ui_calculator_key(key);
        case UI_APP_CLOCK: return ui_clock_key(key);
        case UI_APP_FILES: return ui_file_manager_key(key);
        case UI_APP_LAUNCHER: return ui_launcher_key(key);
        case UI_APP_WALLPAPER: return ui_wallpaper_key(key);
        case UI_APP_SETTINGS: return ui_settings_key(key);
        default: return 1;
    }
}

int ui_apps_pointer(int app_id, int x, int y)
{
    if (app_id == UI_APP_WALLPAPER) return ui_wallpaper_pointer(x, y);
    if (app_id == UI_APP_SETTINGS) return ui_settings_pointer(x, y);
    if (app_id == UI_APP_LAUNCHER) return ui_launcher_pointer(x, y);
    if (app_id == UI_APP_CALCULATOR) return ui_calculator_pointer(x, y);
    if (app_id == UI_APP_FILES) return ui_file_manager_pointer(x, y);
    return 0;
}

int ui_apps_get_target(int app_id)
{
    if (app_id == UI_APP_LAUNCHER) return ui_launcher_get_target();
    if (app_id == UI_APP_SETTINGS) return ui_settings_get_target();
    return UI_APP_TERMINAL;
}
