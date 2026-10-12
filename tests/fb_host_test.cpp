#include "fb.h"
#include <stdio.h>
#include <stdint.h>

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main()
{
    uint32_t pixels[16 * 32];
    for (unsigned int i = 0; i < sizeof(pixels) / sizeof(pixels[0]); ++i) pixels[i] = 0xDEADBEEFU;

    fb_init();
    if (fb_is_ready()) return fail("host init must not touch fixed physical surface");
    if (fb_test_attach(0, 16, 32) == 0) return fail("null surface rejected");
    if (fb_test_attach(pixels, 16, 32) != 0) return fail("valid test surface attached");
    if (!fb_is_ready() || fb_get_width() != 16 || fb_get_height() != 32) return fail("surface metadata");
    for (unsigned int i = 0; i < sizeof(pixels) / sizeof(pixels[0]); ++i)
        if (pixels[i] != 0U) return fail("attach clears surface");

    fb_putc('A', 0x00FFFFFFU);
    int saw_foreground = 0;
    for (int i = 0; i < 16 * 16; ++i) if (pixels[i] == 0x00FFFFFFU) saw_foreground = 1;
    if (!saw_foreground) return fail("glyph writes foreground pixels");

    /* A space overwrites all pixels of its cell, including old glyph pixels. */
    fb_set_cursor(0, 0);
    fb_putc(' ', 0x00FFFFFFU);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 8; ++x)
            if (pixels[y * 16 + x] != 0U) return fail("space clears stale foreground pixels");

    fb_set_cursor(0, 16);
    fb_putc('B', 0x0000FF00U);
    uint32_t expected[16 * 16];
    for (int i = 0; i < 16 * 16; ++i) expected[i] = pixels[16 * 16 + i];
    fb_set_cursor(0, 16);
    fb_putc('\n', 0xFFFFFFFFU);
    for (int i = 0; i < 16 * 16; ++i)
        if (pixels[i] != expected[i]) return fail("scroll moves next text row to first row");
    for (int i = 16 * 16; i < 16 * 32; ++i)
        if (pixels[i] != 0U) return fail("scroll clears final text row");

    fb_set_cursor(9999, 9999);
    if (fb_get_width() != 16 || fb_get_height() != 32) return fail("cursor clamp preserves surface dimensions");
    if (fb_test_attach(pixels, 7, 32) == 0) return fail("too-small surface rejected");
    puts("Framebuffer host test: PASS");
    return 0;
}
