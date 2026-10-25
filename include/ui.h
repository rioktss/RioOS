#ifndef RIOOS_UI_H
#define RIOOS_UI_H

#include "types.h"

enum UiAppId
{
    UI_APP_TERMINAL = 0,
    UI_APP_CALCULATOR = 1,
    UI_APP_CLOCK = 2,
    UI_APP_FILES = 3,
    UI_APP_LAUNCHER = 4,
    UI_APP_WALLPAPER = 5,
    UI_APP_SETTINGS = 6,
    UI_APP_COUNT = 7
};

void ui_start();
void ui_draw_boot_screen();
void ui_draw_lock_screen(uint64_t epoch);
void ui_draw_home_screen(int selected, uint64_t epoch);
void ui_draw_status_bar(uint64_t epoch);
void ui_draw_app_frame(const char* title, const char* subtitle);
void ui_draw_bottom_hint(const char* hint);
void ui_format_clock(uint64_t epoch, char out[6]);
void ui_draw_app_icon(int app_id, int cx, int cy, int size);
const char* ui_app_name(int app_id);
const char* ui_app_package_path(int app_id);
int ui_app_package_valid(int app_id);
int ui_app_icon_path(int app_id, char* out, int capacity);
int ui_image_draw_bmp(const char* path, int x, int y, int width, int height, int radius);
int ui_wallpaper_draw_background();
int ui_wallpaper_count();
int ui_wallpaper_get_entry(int index, char* path, int path_capacity, char* name, int name_capacity);
void ui_wallpaper_current_path(char* out, int capacity);
int ui_wallpaper_set_active(const char* path);
int ui_settings_is_dark();
int ui_settings_toggle_theme();
void ui_apps_init(int app_id);
void ui_apps_render(int app_id, uint64_t epoch);
/* Return non-zero when the application requests to close. */
int ui_apps_key(int app_id, char key);
int ui_launcher_get_target();

void ui_terminal_init();
void ui_terminal_render(uint64_t epoch);
int ui_terminal_key(char key);
void ui_calculator_init();
void ui_calculator_render(uint64_t epoch);
int ui_calculator_key(char key);
int ui_calculator_pointer(int x, int y);
void ui_clock_init();
void ui_clock_render(uint64_t epoch);
int ui_clock_key(char key);
void ui_file_manager_init();
void ui_file_manager_render(uint64_t epoch);
int ui_file_manager_key(char key);
int ui_file_manager_pointer(int x, int y);
void ui_launcher_init();
void ui_launcher_render(uint64_t epoch);
int ui_launcher_key(char key);
int ui_launcher_pointer(int x, int y);
int ui_settings_get_target();
int ui_wallpaper_key(char key);
int ui_wallpaper_pointer(int x, int y);
int ui_settings_key(char key);
int ui_settings_pointer(int x, int y);
int ui_apps_pointer(int app_id, int x, int y);
int ui_apps_get_target(int app_id);
void ui_wallpaper_init();
void ui_wallpaper_render(uint64_t epoch);
void ui_settings_init();
void ui_settings_render(uint64_t epoch);

#endif
