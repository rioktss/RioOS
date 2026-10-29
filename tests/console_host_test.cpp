#include "console.h"
#include "console_font.h"
#include "fb.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); ++failures; } } while (0)

static void print_text(const char* s) { console_write(s); }

static char cell_char(int row, int col) { return (char)(console_test_cell(row, col) & 0xFF); }

static int row_matches(int row, const char* text)
{
    for (int i = 0; text[i]; ++i) if (cell_char(row, i) != text[i]) return 0;
    return 1;
}

static void dump_ppm(const char* path, const std::vector<uint32_t>& px, int w, int h)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i)
    {
        unsigned char rgb[3] = {(unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i]};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

int main()
{
    const int W = 1024, H = 768;
    std::vector<uint32_t> pixels((size_t)W * H, 0xDEADBEEFU);
    CHECK(fb_test_attach(pixels.data(), W, H) == 0, "attach");
    CHECK(console_start() == 0, "console_start");
    CHECK(console_active(), "console active");
    CHECK(console_columns() >= 100 && console_rows() >= 40, "geometry is a real terminal size");
    CHECK(fb_test_pixel(0, 0) == 0x000000U, "background cleared to black");

    print_text("Hello\r\nWorld");
    CHECK(row_matches(0, "Hello"), "first line text");
    CHECK(row_matches(1, "World"), "second line text");
    int r, c; console_test_cursor(&r, &c);
    CHECK(r == 1 && c == 5, "cursor after text");

    /* Bare LF behaves like CR+LF for kernel code that prints '\n' only. */
    print_text("\nX");
    console_test_cursor(&r, &c);
    CHECK(r == 2 && c == 1 && cell_char(2, 0) == 'X', "LF returns to column 0");

    /* Backspace, erase-line and carriage return redraw (shell line editing). */
    print_text("\r\x1b[2Kabc\b \bd");
    CHECK(row_matches(2, "abd"), "backspace/erase line editing");
    CHECK(cell_char(2, 3) == ' ', "tail cleared");

    /* Cursor positioning and clear. */
    print_text("\x1b[5;10Hq");
    CHECK(cell_char(4, 9) == 'q', "CSI H positions cursor");
    print_text("\x1b[2J\x1b[H");
    CHECK(cell_char(4, 9) == ' ' && cell_char(0, 0) == ' ', "CSI 2J clears the screen");

    /* SGR colours end up in the cell attribute (green fg = 2, bold -> bright). */
    print_text("\x1b[1;32mG\x1b[0mN");
    uint16_t g = console_test_cell(0, 0), n = console_test_cell(0, 1);
    CHECK(((g >> 8) & 15) == 10, "bold green maps to bright green");
    CHECK(((n >> 8) & 15) == 7, "reset returns to default foreground");

    /* Wrap + scroll: 200 lines fill history; first lines scroll off the live screen. */
    print_text("\x1b[2J\x1b[H");
    for (int i = 0; i < 200; ++i)
    {
        char line[32];
        std::snprintf(line, sizeof line, "line %03d\r\n", i);
        print_text(line);
    }
    CHECK(console_scrollback_lines() > 100, "scrollback accumulates");
    CHECK(console_scroll_offset() == 0, "live view by default");
    int bottom_row = console_rows() - 2;
    CHECK(row_matches(bottom_row, "line 199"), "newest line near the bottom");

    /* Wheel up goes back in history; wheel down returns. */
    console_scroll_lines(30);
    CHECK(console_scroll_offset() == 30, "scrolled back 30 lines");
    console_flush();
    console_scroll_lines(-10);
    CHECK(console_scroll_offset() == 20, "scrolled forward 10");
    console_scroll_lines(100000);
    CHECK(console_scroll_offset() == console_scrollback_lines(), "scroll clamps at oldest line");
    console_scroll_lines(-100000);
    CHECK(console_scroll_offset() == 0, "scroll clamps at live view");
    console_scroll_pages(1);
    CHECK(console_scroll_offset() == console_rows() - 1, "page up scrolls a page");

    /* New output snaps back to the live screen. */
    print_text("tail\r\n");
    CHECK(console_scroll_offset() == 0, "output snaps to bottom");

    /* The pixels really change when we scroll and are not left uninitialised. */
    console_flush();
    std::vector<uint32_t> live(pixels);
    console_scroll_lines(40);
    console_flush();
    CHECK(pixels != live, "scrolled view repaints differently");
    console_snap_to_bottom();
    console_flush();
    CHECK(pixels == live, "snap back restores the live frame exactly");

    /* Glyphs are anti-aliased: text produces intermediate (non pure) levels. */
    bool mid = false; bool fg = false;
    for (int y = 0; y < H && !(mid && fg); ++y)
        for (int x = 0; x < 200; ++x)
        {
            uint32_t p = pixels[(size_t)y * W + x] & 0xFF;
            if (p > 0 && p < 0xAA) mid = true;
            if (p == 0xAA) fg = true;
        }
    CHECK(mid && fg, "font is anti-aliased with full-intensity strokes");
    CHECK(pixels[(size_t)(H - 1) * W + (W - 1)] != 0xDEADBEEFU, "whole surface was initialised");

    /* Alignment contract of the generated font. */
    CHECK(CONSOLE_FONT_W == 9 && CONSOLE_FONT_H == 18, "font cell size");
    int ink = 0;
    for (int i = 0; i < CONSOLE_FONT_W * CONSOLE_FONT_H; ++i) ink += console_font_glyphs['M' - 32][i] > 128;
    CHECK(ink > 20, "glyph 'M' has ink");
    for (int i = 0; i < CONSOLE_FONT_W * CONSOLE_FONT_H; ++i)
        CHECK(console_font_glyphs[0][i] == 0, "space glyph is empty");

    /* Long line wraps at the right edge without losing characters. */
    print_text("\x1b[2J\x1b[H");
    std::vector<char> longline(console_columns() + 10, 'z');
    longline.push_back('\0');
    print_text(longline.data());
    CHECK(cell_char(0, console_columns() - 1) == 'z' && cell_char(1, 0) == 'z', "wrap at right edge");

    /* Hostile input: oversized/odd escape sequences must not crash or corrupt. */
    print_text("\x1b[999999;999999H\x1b[1;2;3;4;5;6;7;8;9;10;11mQ\x1b[?25l\x1b[?25h\x1b(B\x1b[99L\x1b[99M\x1b[99P\x1b[99@\x1b[99X");
    for (int i = 0; i < 70000; ++i) console_putc((char)(i * 7));
    console_flush();
    console_test_cursor(&r, &c);
    CHECK(r >= 0 && r < console_rows() && c >= 0 && c < console_columns(), "cursor stays in bounds under fuzz");

    if (std::getenv("RIOOS_CONSOLE_SNAPSHOT"))
    {
        print_text("\x1b[2J\x1b[H");
        print_text("\x1b[1;32m[root@RioOS \x1b[1;34m/\x1b[1;32m]\x1b[1;31m# \x1b[0mls -l\r\n");
        print_text("drwxr-xr-x 2 root root    0 bin\r\n-rw-r--r-- 1 user user   12 hello.txt\r\n");
        print_text("\x1b[1;32m[user@RioOS \x1b[1;34m/home/user\x1b[1;32m]$ \x1b[0mcalc\r\nRioOS CALCULATOR  0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~\r\nThe quick brown fox jumps over the lazy dog.\r\n");
        console_flush();
        dump_ppm(std::getenv("RIOOS_CONSOLE_SNAPSHOT"), pixels, W, H);
    }

    if (failures == 0) std::printf("console_host_test: PASS\n");
    return failures == 0 ? 0 : 1;
}
