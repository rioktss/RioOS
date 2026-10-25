#include "ui.h"
#include "fb.h"
#include "fs.h"

static int wallpaper_selected = 0;
static int wallpaper_offset = 0;
static int wallpaper_notice = 0;
static char wallpaper_notice_text[72];

static void set_notice(const char* text)
{
    int i = 0;
    while (text && text[i] && i < (int)sizeof(wallpaper_notice_text) - 1)
    {
        wallpaper_notice_text[i] = text[i];
        ++i;
    }
    wallpaper_notice_text[i] = '\0';
    wallpaper_notice = 1;
}

void ui_wallpaper_init()
{
    wallpaper_selected = 0;
    wallpaper_offset = 0;
    wallpaper_notice = 0;
    wallpaper_notice_text[0] = '\0';
    char active[192];
    ui_wallpaper_current_path(active, sizeof(active));
    int count = ui_wallpaper_count();
    for (int i = 0; i < count; ++i)
    {
        char path[192];
        if (ui_wallpaper_get_entry(i, path, sizeof(path), 0, 0) == 0 &&
            path[0] != '\0' && active[0] != '\0')
        {
            int a = 0;
            while (path[a] && active[a] && path[a] == active[a]) ++a;
            if (path[a] == '\0' && active[a] == '\0') { wallpaper_selected = i; break; }
        }
    }
}

static void apply_selected_wallpaper()
{
    char path[192];
    if (ui_wallpaper_get_entry(wallpaper_selected, path, sizeof(path), 0, 0) != 0)
    {
        set_notice("No local BMP wallpaper found in /wallpapers.");
        return;
    }
    if (ui_wallpaper_set_active(path) == 0) set_notice("Wallpaper applied and saved.");
    else set_notice("Cannot apply. Check BMP format, size, and permissions.");
}

void ui_wallpaper_render(uint64_t epoch)
{
    (void)epoch;
    int w = fb_get_width();
    ui_draw_app_frame("Wallpaper", "Choose a local BMP image from /wallpapers");
    int count = ui_wallpaper_count();
    const int cols = 3;
    const int margin = 38;
    int cell_w = (w - 2 * margin) / cols;
    const int card_h = 190;
    const int start_y = 132;
    if (count == 0)
    {
        fb_draw_ui_text("No wallpapers installed", 72, 190, 0xFFFFFFU, 2);
        fb_draw_ui_text("Place a BMP file in assets/wallpapers/ and rebuild.",
                     72, 232, 0xBCD0E8U, 1);
        fb_draw_ui_text("Supported: uncompressed 24-bit or 32-bit BMP, up to 1 MiB.",
                     72, 260, 0xBCD0E8U, 1);
    }
    else
    {
        if (wallpaper_selected >= count) wallpaper_selected = count - 1;
        wallpaper_offset = (wallpaper_selected / 6) * 6;
        for (int slot = 0; slot < 6; ++slot)
        {
            int i = wallpaper_offset + slot;
            if (i >= count) break;
            int row = slot / cols, col = slot % cols;
            int x = margin + col * cell_w + 6;
            int y = start_y + row * (card_h + 16);
            int selected = i == wallpaper_selected;
            fb_draw_round_rect(x, y, cell_w - 12, card_h, 18,
                               selected ? 0x486C91U : 0x25384FU);
            char path[192], name[64];
            if (ui_wallpaper_get_entry(i, path, sizeof(path), name, sizeof(name)) == 0)
            {
                if (ui_image_draw_bmp(path, x + 10, y + 10, cell_w - 32, 112, 12) != 0)
                {
                    fb_draw_round_rect(x + 10, y + 10, cell_w - 32, 112, 12, 0x35455CU);
                    fb_draw_ui_text("BMP ERROR", x + 24, y + 55, 0xFFBBAAU, 1);
                }
                fb_draw_ui_text(name, x + 14, y + 132, 0xFFFFFFU, 1);
                if (i == wallpaper_selected)
                    fb_draw_ui_text("SELECTED - ENTER TO APPLY", x + 14, y + 156, 0x8EF0C9U, 1);
                else
                    fb_draw_ui_text("TAP / ENTER TO APPLY", x + 14, y + 156, 0xC1D4E9U, 1);
            }
        }
        if (count > 6)
        {
            char page[48] = "Page ";
            int page_number = wallpaper_offset / 6 + 1;
            int n = 5;
            if (page_number >= 10) page[n++] = (char)('0' + (page_number / 10) % 10);
            page[n++] = (char)('0' + page_number % 10);
            page[n++] = '\0';
            fb_draw_ui_text(page, 54, 586, 0xBBD0E7U, 1);
        }
    }
    if (wallpaper_notice)
    {
        fb_draw_round_rect(64, 614, w - 128, 30, 8, 0x2E5168U);
        fb_draw_ui_text(wallpaper_notice_text, 80, 624, 0xF1F7FFU, 1);
    }
    ui_draw_bottom_hint("A/D/W/S: Select   ENTER: Apply   Q: Home");
    fb_present();
}

int ui_wallpaper_key(char key)
{
    int count = ui_wallpaper_count();
    if (key == 'q' || key == 'Q') return 1;
    if (key == 'a' || key == 'A') { if (wallpaper_selected > 0) --wallpaper_selected; wallpaper_notice = 0; }
    else if (key == 'd' || key == 'D') { if (wallpaper_selected + 1 < count) ++wallpaper_selected; wallpaper_notice = 0; }
    else if (key == 'w' || key == 'W') { if (wallpaper_selected >= 3) wallpaper_selected -= 3; wallpaper_notice = 0; }
    else if (key == 's' || key == 'S') { if (wallpaper_selected + 3 < count) wallpaper_selected += 3; wallpaper_notice = 0; }
    else if (key == '\r' || key == '\n') apply_selected_wallpaper();
    return 0;
}

int ui_wallpaper_pointer(int x, int y)
{
    int count = ui_wallpaper_count();
    const int cols = 3;
    int cell_w = (fb_get_width() - 76) / cols;
    const int card_h = 190;
    const int start_y = 132;
    int offset = (wallpaper_selected / 6) * 6;
    if (count <= 0 || cell_w <= 0) return 0;
    for (int slot = 0; slot < 6; ++slot)
    {
        int i = offset + slot;
        if (i >= count) break;
        int row = slot / cols, col = slot % cols;
        int left = 38 + col * cell_w + 6;
        int top = start_y + row * (card_h + 16);
        if (x >= left && x < left + cell_w - 12 && y >= top && y < top + card_h)
        {
            wallpaper_selected = i;
            apply_selected_wallpaper();
            return 0;
        }
    }
    return 0;
}
