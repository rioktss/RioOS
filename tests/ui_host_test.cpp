#include <cstdint>
#include <cstdio>
#include "fb.h"
#include "ui.h"

static uint32_t pixels[1024 * 768];

const char* ui_app_name(int app_id)
{
    static const char* const names[] = {"Terminal", "Calculator", "Clock", "Files", "Launcher", "Wallpaper", "Settings"};
    return app_id >= 0 && app_id < UI_APP_COUNT ? names[app_id] : "Unknown";
}

int ui_settings_is_dark() { return 1; }
int ui_wallpaper_draw_background() { return 0; }
int ui_app_icon_path(int, char*, int) { return -1; }
int ui_image_draw_bmp(const char*, int, int, int, int, int) { return -1; }

static int count_bright(int x0, int y0, int x1, int y1)
{
    int count = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
        {
            uint32_t c = pixels[y * 1024 + x];
            if (((c >> 16) & 255U) > 170U && ((c >> 8) & 255U) > 170U && (c & 255U) > 170U) ++count;
        }
    return count;
}

static int count_non_background(uint32_t background)
{
    int count = 0;
    for (int i = 0; i < 1024 * 768; ++i)
        if (pixels[i] != background) ++count;
    return count;
}

int main()
{
    if (fb_test_attach(pixels, 1024, 768) != 0)
    {
        std::fprintf(stderr, "UI framebuffer attach failed\n");
        return 1;
    }

    fb_clear(0x000000U);
    fb_draw_ui_text("RioOS", 24, 20, 0xFFFFFFFFU, 2);
    int antialiased_pixels = 0;
    for (int y = 20; y < 44; ++y)
        for (int x = 24; x < 180; ++x)
        {
            uint32_t c = pixels[y * 1024 + x];
            if (c != 0U && c != 0xFFFFFFU) ++antialiased_pixels;
        }
    if (antialiased_pixels < 8)
    {
        std::fprintf(stderr, "UI font edge coverage is not antialiased\n");
        return 5;
    }

    ui_draw_boot_screen();
    if (pixels[0] != 0x070B18U || count_bright(340, 345, 684, 425) < 300)
    {
        std::fprintf(stderr, "boot splash background/title pixels are missing\n");
        return 2;
    }

    ui_draw_lock_screen(1760000000ULL);
    int prompt_pixels = count_bright(280, 620, 760, 725);
    int lock_pixels = count_non_background(0x101D49U);
    if (prompt_pixels < 100 || lock_pixels < 200000)
    {
        std::fprintf(stderr, "lock screen pixel composition is missing (prompt=%d total=%d)\n", prompt_pixels, lock_pixels);
        return 3;
    }

    ui_draw_home_screen(0, 1760000000ULL);
    if (pixels[0] != 0x171D2CU || count_non_background(0x182D52U) < 200000)
    {
        std::fprintf(stderr, "home screen pixel composition is missing\n");
        return 4;
    }

    std::puts("UI HOST TEST: PASS");
    return 0;
}
