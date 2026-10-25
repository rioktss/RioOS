#include <cstdint>
#include <cstdio>
#include <cstring>
#include "fb.h"
#include "fs.h"
#include "ui.h"

namespace
{
constexpr uint64_t BMP_SIZE = 70;
uint8_t good_bmp[BMP_SIZE];
uint8_t bad_bmp[BMP_SIZE];
uint8_t saved_config[192];
uint64_t saved_config_size = 0;
uint8_t saved_theme_config[16];
uint64_t saved_theme_size = 0;
uint32_t pixels[16 * 16];

void put16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
}

void put32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFU);
}

void make_bmp(uint8_t* out, uint16_t bpp)
{
    std::memset(out, 0, BMP_SIZE);
    out[0] = 'B'; out[1] = 'M';
    put32(out + 2, static_cast<uint32_t>(BMP_SIZE));
    put32(out + 10, 54);
    put32(out + 14, 40);
    put32(out + 18, 2);
    put32(out + 22, 2);
    put16(out + 26, 1);
    put16(out + 28, bpp);
    put32(out + 30, 0);
    put32(out + 34, 16);
    if (bpp == 24)
    {
        // BMP rows are bottom-up and padded to four-byte boundaries.
        // Bottom: blue, white. Top: red, green.
        const uint8_t data[16] = {
            255, 0, 0, 255, 255, 255, 0, 0,
            0, 0, 255, 0, 255, 0, 0, 0
        };
        std::memcpy(out + 54, data, sizeof(data));
    }
}
}

int fs_get_root() { return 0; }
int fs_resolve(const char* path, int)
{
    return path && std::strcmp(path, "/wallpapers") == 0 ? 1 : -1;
}
int fs_get_type(int id)
{
    if (id == 1) return FS_DIR;
    if (id >= 2 && id <= 5) return FS_FILE;
    return -1;
}
const char* fs_get_name(int id)
{
    switch (id)
    {
        case 2: return "test.bmp";
        case 3: return "broken.bmp";
        case 4: return "oversize.bmp";
        case 5: return "notes.txt";
        default: return "";
    }
}
int fs_get_child_count(int id) { return id == 1 ? 4 : 0; }
int fs_get_child(int id, int index)
{
    if (id != 1 || index < 0 || index >= 4) return -1;
    return index + 2;
}
int fs_stat_path(const char* path, int, FsStat* out)
{
    if (!path || !out) return -1;
    std::memset(out, 0, sizeof(*out));
    out->type = FS_FILE;
    out->mtime = 7;
    if (std::strcmp(path, "/wallpapers/test.bmp") == 0) out->size = BMP_SIZE;
    else if (std::strcmp(path, "/wallpapers/broken.bmp") == 0) { out->size = BMP_SIZE; out->mtime = 8; }
    else if (std::strcmp(path, "/wallpapers/oversize.bmp") == 0) { out->size = 1024ULL * 1024ULL + 1ULL; out->mtime = 9; }
    else return -1;
    return 0;
}
int fs_read_file(const char* path, int, uint8_t* out, uint64_t capacity, uint64_t* size)
{
    if (!path || !out || !size) return -1;
    if (std::strcmp(path, "/settings/wallpaper.cfg") == 0)
    {
        if (saved_config_size > capacity) return -1;
        std::memcpy(out, saved_config, static_cast<size_t>(saved_config_size));
        *size = saved_config_size;
        return saved_config_size ? 0 : -1;
    }
    if (std::strcmp(path, "/settings/theme.cfg") == 0)
    {
        if (saved_theme_size > capacity) return -1;
        std::memcpy(out, saved_theme_config, static_cast<size_t>(saved_theme_size));
        *size = saved_theme_size;
        return saved_theme_size ? 0 : -1;
    }
    const uint8_t* source = nullptr;
    if (std::strcmp(path, "/wallpapers/test.bmp") == 0) source = good_bmp;
    else if (std::strcmp(path, "/wallpapers/broken.bmp") == 0) source = bad_bmp;
    else return -1;
    if (capacity < BMP_SIZE) return -1;
    std::memcpy(out, source, BMP_SIZE);
    *size = BMP_SIZE;
    return 0;
}
int fs_write_data(const char* path, int, const uint8_t* data, uint64_t size)
{
    if (!path || !data) return -1;
    if (std::strcmp(path, "/settings/wallpaper.cfg") == 0 && size < sizeof(saved_config))
    {
        std::memcpy(saved_config, data, static_cast<size_t>(size));
        saved_config_size = size;
        return 0;
    }
    if (std::strcmp(path, "/settings/theme.cfg") == 0 && size < sizeof(saved_theme_config))
    {
        std::memcpy(saved_theme_config, data, static_cast<size_t>(size));
        saved_theme_size = size;
        return 0;
    }
    return -1;
}

void ui_draw_app_frame(const char*, const char*) {}
void ui_draw_bottom_hint(const char*) {}

int main()
{
    make_bmp(good_bmp, 24);
    make_bmp(bad_bmp, 16);
    if (fb_test_attach(pixels, 16, 16) != 0) return 1;

    if (ui_wallpaper_count() != 3)
    {
        std::fprintf(stderr, "wallpaper scan should ignore non-BMP files\n");
        return 2;
    }
    char path[192] = {}, name[64] = {};
    if (ui_wallpaper_get_entry(0, path, sizeof(path), name, sizeof(name)) != 0 ||
        std::strcmp(path, "/wallpapers/test.bmp") != 0 || std::strcmp(name, "test.bmp") != 0)
    {
        std::fprintf(stderr, "wallpaper manifest path lookup failed\n");
        return 3;
    }

    if (ui_image_draw_bmp(path, 0, 0, 2, 2, 0) != 0 ||
        fb_test_pixel(0, 0) != 0xFF0000U || fb_test_pixel(1, 0) != 0x00FF00U ||
        fb_test_pixel(0, 1) != 0x0000FFU || fb_test_pixel(1, 1) != 0xFFFFFFU)
    {
        std::fprintf(stderr, "BMP pixels/orientation decoded incorrectly\n");
        return 4;
    }
    if (ui_image_draw_bmp("/wallpapers/broken.bmp", 0, 0, 2, 2, 0) == 0)
    {
        std::fprintf(stderr, "bad BMP format was accepted\n");
        return 5;
    }
    if (ui_image_draw_bmp("/wallpapers/oversize.bmp", 0, 0, 2, 2, 0) == 0)
    {
        std::fprintf(stderr, "oversized BMP was accepted\n");
        return 9;
    }
    if (ui_wallpaper_set_active("/wallpapers/../apps/terminal.rpk") == 0)
    {
        std::fprintf(stderr, "wallpaper path traversal was accepted\n");
        return 10;
    }
    if (ui_wallpaper_set_active("/wallpapers/test.bmp") != 0)
    {
        std::fprintf(stderr, "valid local wallpaper should apply and persist\n");
        return 6;
    }
    char active[192] = {};
    ui_wallpaper_current_path(active, sizeof(active));
    if (std::strcmp(active, "/wallpapers/test.bmp") != 0)
    {
        std::fprintf(stderr, "saved wallpaper path did not round-trip\n");
        return 7;
    }
    if (ui_wallpaper_draw_background() != 1)
    {
        std::fprintf(stderr, "saved local wallpaper could not render\n");
        return 8;
    }
    if (ui_settings_is_dark() != 1 || ui_settings_toggle_theme() != 0 || ui_settings_is_dark() != 0 ||
        saved_theme_size != 5 || std::memcmp(saved_theme_config, "light", 5) != 0 ||
        ui_settings_toggle_theme() != 0 || ui_settings_is_dark() != 1 || saved_theme_size != 4 ||
        std::memcmp(saved_theme_config, "dark", 4) != 0)
    {
        std::fprintf(stderr, "theme settings failed to toggle and persist\n");
        return 11;
    }
    std::puts("UI IMAGE HOST TEST: PASS");
    return 0;
}
