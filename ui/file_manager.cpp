#include "ui.h"
#include "fb.h"
#include "fs.h"

static char file_path[192];
static int file_selected = 0;
static int file_offset = 0;
static int file_notice = 0;
static int file_preview = 0;
static int preview_error = 0;
static char preview_path[192];
static uint8_t preview_data[1024];
static uint64_t preview_size = 0;

static int current_dir_id()
{
    return fs_resolve(file_path, fs_get_root());
}

static void parent_directory()
{
    int n = 0; while (file_path[n]) ++n;
    if (n <= 1) { file_path[0] = '/'; file_path[1] = '\0'; return; }
    while (n > 1 && file_path[n - 1] != '/') --n;
    if (n <= 1) { file_path[0] = '/'; file_path[1] = '\0'; }
    else file_path[n - 1] = '\0';
}

static void path_append(const char* name)
{
    int n = 0; while (file_path[n] && n < (int)sizeof(file_path) - 2) ++n;
    if (n > 1 && file_path[n - 1] != '/') file_path[n++] = '/';
    for (int i = 0; name[i] && n < (int)sizeof(file_path) - 1; ++i) file_path[n++] = name[i];
    file_path[n] = '\0';
}

void ui_file_manager_init()
{
    file_path[0] = '/'; file_path[1] = '\0';
    file_selected = 0; file_offset = 0; file_notice = 0;
    file_preview = 0; preview_error = 0; preview_size = 0; preview_path[0] = '\0';
}

void ui_file_manager_render(uint64_t epoch)
{
    (void)epoch;
    ui_draw_app_frame("Files", "Browse the MyFS filesystem");
    fb_draw_round_rect(46, 116, fb_get_width() - 92, 40, 12, 0x253B53U);
    fb_draw_ui_text(file_path, 62, 132, 0xC3E3FFU, 1);

    if (file_preview)
    {
        fb_draw_round_rect(46, 170, fb_get_width() - 92, 430, 14, 0x07131FU);
        fb_draw_ui_text(preview_path, 62, 185, 0x83E3C3U, 1);
        if (preview_error)
            fb_draw_ui_text("Preview unavailable: permission denied or file exceeds 1023 bytes.", 62, 220, 0xFFB9A8U, 1);
        else
        {
            char line[116]; int used = 0; int y = 215;
            for (uint64_t i = 0; i < preview_size && y < 575; ++i)
            {
                char c = (char)preview_data[i];
                if (c == '\r') continue;
                if (c == '\n' || used >= (int)sizeof(line) - 1)
                {
                    line[used] = '\0';
                    fb_draw_ui_text(line, 62, y, 0xD8E6F5U, 1);
                    y += 18; used = 0;
                    if (c == '\n') continue;
                }
                if ((unsigned char)c >= 32U || c == '\t') line[used++] = c;
            }
            if (used > 0 && y < 575) { line[used] = '\0'; fb_draw_ui_text(line, 62, y, 0xD8E6F5U, 1); }
            if (preview_size == 0) fb_draw_ui_text("(empty file)", 62, 220, 0xAFC1D9U, 1);
        }
        ui_draw_bottom_hint("BACKSPACE: Return to folder   Q: Back to home");
        fb_present();
        return;
    }

    int dir = current_dir_id();
    if (dir < 0 || fs_get_type(dir) != FS_DIR)
    {
        fb_draw_ui_text("Cannot open this directory", 64, 190, 0xFFB7B7U, 2);
        ui_draw_bottom_hint("Q: Back to home"); fb_present(); return;
    }
    int count = fs_get_child_count(dir);
    int rows = 10;
    if (file_selected >= count) file_selected = count > 0 ? count - 1 : 0;
    if (file_selected < file_offset) file_offset = file_selected;
    if (file_selected >= file_offset + rows) file_offset = file_selected - rows + 1;

    fb_draw_round_rect(46, 170, fb_get_width() - 92, 430, 14, 0x1A2A3DU);
    if (count == 0)
        fb_draw_ui_text("This folder is empty", 70, 205, 0xA8C4E3U, 2);
    for (int row = 0; row < rows; ++row)
    {
        int index = file_offset + row;
        if (index >= count) break;
        int child = fs_get_child(dir, index);
        if (child < 0) continue;
        int y = 184 + row * 39;
        if (index == file_selected) fb_draw_round_rect(58, y, fb_get_width() - 116, 34, 8, 0x3A5270U);
        if (fs_get_type(child) == FS_DIR)
        {
            fb_draw_round_rect(73, y + 8, 24, 17, 4, 0xE7B65CU);
            fb_draw_round_rect(73, y + 5, 12, 7, 3, 0xF2CA7AU);
        }
        else
        {
            fb_draw_round_rect(77, y + 4, 17, 25, 3, 0xB8D8EDU);
            fb_draw_line(81, y + 12, 90, y + 12, 0x527C9FU);
            fb_draw_line(81, y + 17, 90, y + 17, 0x527C9FU);
        }
        fb_draw_ui_text(fs_get_name(child), 111, y + 11, 0xEEF5FFU, 1);
        if (fs_get_type(child) == FS_DIR)
            fb_draw_ui_text("FOLDER", fb_get_width() - 156, y + 11, 0x80B6D9U, 1);
        else
        {
            uint64_t size = fs_file_size(child);
            char number[24]; int n = 0;
            if (size == 0) number[n++] = '0';
            while (size > 0 && n < 20) { number[n++] = (char)('0' + size % 10ULL); size /= 10ULL; }
            char out[24]; int p = 0; while (n > 0) out[p++] = number[--n]; out[p] = '\0';
            fb_draw_ui_text(out, fb_get_width() - 142, y + 11, 0x80B6D9U, 1);
            fb_draw_ui_text("B", fb_get_width() - 80, y + 11, 0x80B6D9U, 1);
        }
    }
    if (file_notice)
    {
        fb_draw_round_rect(130, 617, fb_get_width() - 260, 28, 8, 0x314F68U);
        fb_draw_ui_text("Preview is read-only", 148, 627, 0xD6E9F7U, 1);
    }
    ui_draw_bottom_hint("W/S: Select   ENTER: Open folder   BACKSPACE: Parent   Q: Home");
    fb_present();
}

int ui_file_manager_key(char key)
{
    if (key == 'q' || key == 'Q') return 1;
    int dir = current_dir_id();
    if (dir < 0) return 0;
    int count = fs_get_child_count(dir);
    if (key == 'w' || key == 'W' || key == 'k' || key == 'K')
    {
        if (file_selected > 0) --file_selected;
        file_notice = 0;
    }
    else if (key == 's' || key == 'S' || key == 'j' || key == 'J')
    {
        if (file_selected + 1 < count) ++file_selected;
        file_notice = 0;
    }
    else if (key == '\b' || (unsigned char)key == 127U)
    {
        if (file_preview) { file_preview = 0; file_notice = 0; preview_size = 0; }
        else { parent_directory(); file_selected = 0; file_offset = 0; file_notice = 0; }
    }
    else if (key == '\r' || key == '\n')
    {
        if (file_selected >= 0 && file_selected < count)
        {
            int child = fs_get_child(dir, file_selected);
            if (child >= 0 && fs_get_type(child) == FS_DIR)
            {
                path_append(fs_get_name(child)); file_selected = 0; file_offset = 0; file_notice = 0;
            }
            else
            {
                int n = 0;
                while (file_path[n] && n < (int)sizeof(preview_path) - 2)
                { preview_path[n] = file_path[n]; ++n; }
                if (n > 1 && preview_path[n - 1] != '/') preview_path[n++] = '/';
                const char* name = fs_get_name(child);
                for (int j = 0; name && name[j] && n < (int)sizeof(preview_path) - 1; ++j)
                    preview_path[n++] = name[j];
                preview_path[n] = '\0';
                preview_size = 0; preview_error = 0;
                if (fs_read_file(preview_path, fs_get_root(), preview_data,
                                 sizeof(preview_data) - 1ULL, &preview_size) != 0)
                    preview_error = 1;
                else preview_data[preview_size] = 0;
                file_preview = 1;
            }
        }
    }
    return 0;
}

int ui_file_manager_pointer(int x, int y)
{
    if (file_preview)
    {
        if (y >= 116 && y < 158) (void)ui_file_manager_key('\b');
        return 0;
    }
    if (y < 184 || y >= 574 || x < 58 || x >= fb_get_width() - 58) return 0;
    int row = (y - 184) / 39;
    if (row < 0 || row >= 10) return 0;
    int index = file_offset + row;
    int dir = current_dir_id();
    if (dir < 0 || index >= fs_get_child_count(dir)) return 0;
    file_selected = index;
    (void)ui_file_manager_key('\r');
    return 0;
}
