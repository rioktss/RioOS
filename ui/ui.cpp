#include "ui.h"
#include "fb.h"
#include "uart.h"
#include "timer.h"
#include "rtc.h"
#include "virtio_input.h"
#include "netstack.h"

#define UI_STATE_LOCKED 0
#define UI_STATE_HOME 1
#define UI_STATE_APP 2
#define UI_STATE_CONTROL_CENTRE 3

static void ui_open_app(int app_id, int* state, int* active_app, int* selected)
{
    if (app_id < 0 || app_id >= UI_APP_COUNT || !ui_app_package_valid(app_id)) return;
    if (selected) *selected = app_id;
    if (active_app) *active_app = app_id;
    ui_apps_init(app_id);
    if (state) *state = UI_STATE_APP;
}

static void ui_control_tile_bounds(int tile, int* x, int* y, int* width, int* height)
{
    int screen_w = fb_get_width();
    const int margin = 64;
    const int gap = 16;
    const int tile_h = 84;
    int tile_w = (screen_w - margin * 2 - gap) / 2;
    if (x) *x = margin + (tile % 2) * (tile_w + gap);
    if (y) *y = 218 + (tile / 2) * 102;
    if (width) *width = tile_w;
    if (height) *height = tile_h;
}

static void ui_draw_control_centre(uint64_t epoch)
{
    int w = fb_get_width(), h = fb_get_height();
    int dark = ui_settings_is_dark();
    fb_fill_gradient(dark ? 0x101827U : 0xE9EFF7U, dark ? 0x1D3450U : 0xD5E3F1U);
    fb_draw_round_rect(36, 32, w - 72, h - 64, 28, dark ? 0x17263AU : 0xF4F7FCU);
    fb_draw_ui_text("Control Centre", 66, 62, dark ? 0xFFFFFFU : 0x1E344CU, 3);

    char time_text[6];
    ui_format_clock(epoch, time_text);
    fb_draw_ui_text(time_text, 66, 108, dark ? 0xD8E8FCU : 0x324C68U, 4);
    const char* connection = net_ready() ? "NETWORK READY" : "NETWORK NOT READY";
    fb_draw_ui_text(connection, w - fb_ui_text_width(connection, 1) - 74, 130,
                 net_ready() ? 0x46B78CU : 0xD47C70U, 1);

    for (int tile = 0; tile < 6; ++tile)
    {
        int x = 0, y = 0, tw = 0, th = 0;
        ui_control_tile_bounds(tile, &x, &y, &tw, &th);
        uint32_t color = dark ? 0x2B405BU : 0xDFE8F4U;
        if (tile == 1) color = dark ? 0x367F78U : 0xB8E1D8U;
        if (tile == 2) color = dark ? 0x354B73U : 0xC7D8F3U;
        if (tile == 3) color = dark ? 0x3D4964U : 0xD2DAEAU;
        fb_draw_round_rect(x, y, tw, th, 18, color);
        if (tile == 0)
        {
            fb_draw_circle(x + 31, y + 31, 11, net_ready() ? 0x7CE2B7U : 0xD98A80U);
            fb_draw_ui_text("Network", x + 55, y + 23, dark ? 0xFFFFFFU : 0x253A54U, 2);
            fb_draw_ui_text(net_ready() ? "Connected to QEMU user-net" : "Device not ready",
                         x + 55, y + 52, dark ? 0xBDD0E7U : 0x5D728A, 1);
        }
        else if (tile == 1)
        {
            fb_draw_circle(x + 32, y + 31, 12, 0xE8F2FFU);
            fb_draw_ui_text("Dark mode", x + 57, y + 22, dark ? 0xFFFFFFU : 0x253A54U, 2);
            fb_draw_ui_text(dark ? "ON - tap to switch to Light" : "OFF - tap to switch to Dark",
                         x + 57, y + 52, dark ? 0xD3E9E6U : 0x4E716DU, 1);
        }
        else if (tile == 2)
        {
            fb_draw_round_rect(x + 18, y + 19, 28, 31, 8, 0x9CBCE9U);
            fb_draw_circle(x + 32, y + 33, 8, 0xE4F3FFU);
            fb_draw_ui_text("Wallpaper", x + 57, y + 22, dark ? 0xFFFFFFU : 0x253A54U, 2);
            fb_draw_ui_text("Choose a local image", x + 57, y + 52, dark ? 0xBDD0E7U : 0x5D728A, 1);
        }
        else if (tile == 3)
        {
            fb_draw_circle(x + 32, y + 31, 12, 0x85A5D2U);
            fb_draw_circle(x + 32, y + 31, 5, dark ? 0x253A54U : 0xE7EEF7U);
            fb_draw_ui_text("Settings", x + 57, y + 22, dark ? 0xFFFFFFU : 0x253A54U, 2);
            fb_draw_ui_text("Appearance and display", x + 57, y + 52, dark ? 0xBDD0E7U : 0x5D728A, 1);
        }
        else if (tile == 4)
        {
            fb_draw_round_rect(x + 20, y + 17, 24, 34, 7, 0xD8E4F5U);
            fb_draw_circle(x + 32, y + 29, 5, 0x56749AU);
            fb_draw_ui_text("Lock screen", x + 57, y + 22, dark ? 0xFFFFFFU : 0x253A54U, 2);
            fb_draw_ui_text("Return to the lock page", x + 57, y + 52, dark ? 0xBDD0E7U : 0x5D728A, 1);
        }
        else
        {
            fb_draw_circle(x + 32, y + 31, 12, dark ? 0xDBE7F7U : 0x637D9CU);
            fb_draw_ui_text("Close panel", x + 57, y + 22, dark ? 0xFFFFFFU : 0x253A54U, 2);
            fb_draw_ui_text("Return to Home", x + 57, y + 52, dark ? 0xBDD0E7U : 0x5D728A, 1);
        }
    }
    fb_draw_ui_text("Swipe up or press C / ESC to close", (w - fb_ui_text_width("Swipe up or press C / ESC to close", 1)) / 2,
                 h - 52, dark ? 0xC3D4E8U : 0x506883U, 1);
    fb_present();
}

static int ui_control_tile_hit(int px, int py)
{
    for (int tile = 0; tile < 6; ++tile)
    {
        int x = 0, y = 0, w = 0, h = 0;
        ui_control_tile_bounds(tile, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h) return tile;
    }
    return -1;
}

static void ui_control_action(int tile, int* state, int* active_app, int* selected, int* redraw)
{
    if (tile == 1)
    {
        if (ui_settings_toggle_theme() == 0) *redraw = 1;
        else *redraw = 1;
    }
    else if (tile == 2)
    {
        ui_open_app(UI_APP_WALLPAPER, state, active_app, selected);
        *redraw = 1;
    }
    else if (tile == 3)
    {
        ui_open_app(UI_APP_SETTINGS, state, active_app, selected);
        *redraw = 1;
    }
    else if (tile == 4)
    {
        *state = UI_STATE_LOCKED;
        *active_app = -1;
        *redraw = 1;
    }
    else if (tile == 5)
    {
        *state = UI_STATE_HOME;
        *active_app = -1;
        *redraw = 1;
    }
}

void ui_start()
{
    fb_init();
    if (!fb_has_display())
    {
        uart_puts("UI: graphical display unavailable; keeping the serial shell.\r\n");
        return;
    }

    if (virtio_input_keyboard_ready() && virtio_input_pointer_ready())
        uart_puts("UI: Android-like pixel UI ready (keyboard + pointer; serial fallback).\r\n");
    else if (virtio_input_keyboard_ready())
        uart_puts("UI: graphical UI ready; pointer unavailable (serial fallback remains).\r\n");
    else
        uart_puts("UI: graphical UI ready (serial input; QEMU keyboard unavailable).\r\n");
    ui_draw_boot_screen();
    timer_delay_ms(900U); /* Polling counter only; keep timer IRQ disabled. */

    int state = UI_STATE_LOCKED;
    int selected = 0;
    int active_app = -1;
    uint64_t last_render_minute = rtc_get_epoch() / 60ULL;
    int pointer_down = 0;
    int pointer_start_x = 0;
    int pointer_start_y = 0;
    ui_draw_lock_screen(rtc_get_epoch());

    for (;;)
    {
        int input = -1;
        int redraw = 0;
        struct RioInputEvent gui_event;
        if (virtio_input_poll(&gui_event))
        {
            if (gui_event.kind == RIO_INPUT_KEY)
            {
                input = (int)(unsigned char)gui_event.key;
            }
            else if (gui_event.kind == RIO_INPUT_POINTER)
            {
                int max_x = fb_get_width() - 1;
                int max_y = fb_get_height() - 1;
                int px = (int)(((uint64_t)(gui_event.x < 0 ? 0 : gui_event.x > 32767 ? 32767 : gui_event.x) * (uint64_t)max_x) / 32767ULL);
                int py = (int)(((uint64_t)(gui_event.y < 0 ? 0 : gui_event.y > 32767 ? 32767 : gui_event.y) * (uint64_t)max_y) / 32767ULL);
                if (gui_event.pressed && !pointer_down)
                {
                    pointer_start_x = px;
                    pointer_start_y = py;
                    pointer_down = 1;
                }
                else if (gui_event.pressed)
                {
                    pointer_down = 1;
                }
                else if (!gui_event.pressed && pointer_down)
                {
                    pointer_down = 0;
                    int dy = py - pointer_start_y;
                    if (state == UI_STATE_LOCKED && -dy > fb_get_height() / 14)
                    {
                        state = UI_STATE_HOME;
                        selected = 0;
                        redraw = 1;
                        uart_puts("UI: unlocked by pointer swipe.\r\n");
                    }
                    else if (state == UI_STATE_HOME && pointer_start_y < 52 && dy > fb_get_height() / 14)
                    {
                        state = UI_STATE_CONTROL_CENTRE;
                        redraw = 1;
                    }
                    else if (state == UI_STATE_HOME && py < 34)
                    {
                        state = UI_STATE_CONTROL_CENTRE;
                        redraw = 1;
                    }
                    else if (state == UI_STATE_HOME)
                    {
                        const int cols = 4;
                        int cell_w = (fb_get_width() - 160) / cols;
                        int hit = -1;
                        for (int app = 0; app < UI_APP_COUNT; ++app)
                        {
                            int col = app % cols, row = app / cols;
                            int cx = 80 + col * cell_w + cell_w / 2;
                            int cy = row == 0 ? 368 : 500;
                            int dx = px - cx, ddy = py - cy;
                            if (dx >= -54 && dx <= 54 && ddy >= -58 && ddy <= 59)
                            { hit = app; break; }
                        }
                        if (hit >= 0)
                        {
                            selected = hit;
                            ui_open_app(hit, &state, &active_app, &selected);
                            redraw = 1;
                        }
                    }
                    else if (state == UI_STATE_CONTROL_CENTRE)
                    {
                        int tile = ui_control_tile_hit(px, py);
                        if (tile >= 0) ui_control_action(tile, &state, &active_app, &selected, &redraw);
                        else { state = UI_STATE_HOME; redraw = 1; }
                    }
                    else if (state == UI_STATE_APP && py < 36 && px < 48)
                    {
                        state = UI_STATE_HOME;
                        active_app = -1;
                        redraw = 1;
                    }
                    else if (state == UI_STATE_APP)
                    {
                        int result = ui_apps_pointer(active_app, px, py);
                        if (result == 1)
                        {
                            state = UI_STATE_HOME;
                            active_app = -1;
                        }
                        else if (result == 2)
                        {
                            int target = ui_apps_get_target(active_app);
                            ui_open_app(target, &state, &active_app, &selected);
                        }
                        redraw = 1;
                    }
                    (void)pointer_start_x;
                }
            }
        }
        if (input < 0) input = uart_try_getc();
        if (input >= 0)
        {
            char key = (char)input;
            if (state == UI_STATE_LOCKED)
            {
                state = UI_STATE_HOME;
                selected = 0;
                redraw = 1;
                uart_puts("UI: unlocked.\r\n");
            }
            else if (state == UI_STATE_HOME)
            {
                if (key == 'x' || key == 'X')
                {
                    fb_clear(0x000000U);
                    fb_present();
                    uart_puts("UI: leaving graphical shell for UART command shell.\r\n");
                    return;
                }
                if (key == 'c' || key == 'C')
                {
                    state = UI_STATE_CONTROL_CENTRE;
                    redraw = 1;
                }
                else if (key == 'a' || key == 'A' || key == 'h' || key == 'H' || (unsigned char)key == 0x82U)
                {
                    if (selected > 0) --selected;
                    redraw = 1;
                }
                else if (key == 'd' || key == 'D' || (unsigned char)key == 0x83U)
                {
                    if (selected + 1 < UI_APP_COUNT) ++selected;
                    redraw = 1;
                }
                else if (key == 'w' || key == 'W' || (unsigned char)key == 0x80U)
                {
                    if (selected >= 4) selected -= 4;
                    redraw = 1;
                }
                else if (key == 's' || key == 'S' || (unsigned char)key == 0x81U)
                {
                    if (selected + 4 < UI_APP_COUNT) selected += 4;
                    redraw = 1;
                }
                else if (key == 'k' || key == 'K')
                {
                    state = UI_STATE_LOCKED;
                    redraw = 1;
                }
                else if (key >= '1' && key <= '7')
                {
                    selected = key - '1';
                    ui_open_app(selected, &state, &active_app, &selected);
                    redraw = 1;
                }
                else if (key == '\r' || key == '\n')
                {
                    ui_open_app(selected, &state, &active_app, &selected);
                    redraw = 1;
                }
            }
            else if (state == UI_STATE_CONTROL_CENTRE)
            {
                if ((unsigned char)key == 27U || key == 'c' || key == 'C' || key == 'q' || key == 'Q')
                {
                    state = UI_STATE_HOME;
                    redraw = 1;
                }
                else if (key >= '1' && key <= '6')
                {
                    ui_control_action(key - '1', &state, &active_app, &selected, &redraw);
                    if (!redraw) redraw = 1;
                }
            }
            else if (state == UI_STATE_APP)
            {
                int result = ui_apps_key(active_app, key);
                if (result == 1)
                {
                    state = UI_STATE_HOME;
                    active_app = -1;
                    redraw = 1;
                }
                else if (result == 2)
                {
                    int target = ui_apps_get_target(active_app);
                    ui_open_app(target, &state, &active_app, &selected);
                    redraw = 1;
                }
                else redraw = 1;
            }
        }

        uint64_t epoch = rtc_get_epoch();
        uint64_t minute = epoch / 60ULL;
        if (minute != last_render_minute)
        {
            last_render_minute = minute;
            /* Avoid repainting static pages every second: minute changes are enough for their clock labels. */
            if (state == UI_STATE_LOCKED || state == UI_STATE_HOME ||
                (state == UI_STATE_APP && active_app == UI_APP_CLOCK) ||
                state == UI_STATE_CONTROL_CENTRE)
                redraw = 1;
        }

        if (redraw)
        {
            if (state == UI_STATE_LOCKED) ui_draw_lock_screen(epoch);
            else if (state == UI_STATE_HOME) ui_draw_home_screen(selected, epoch);
            else if (state == UI_STATE_CONTROL_CENTRE) ui_draw_control_centre(epoch);
            else if (state == UI_STATE_APP && active_app >= 0) ui_apps_render(active_app, epoch);
        }
        asm volatile("yield");
    }
}
