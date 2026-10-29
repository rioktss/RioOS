#include "console.h"
#include "console_font.h"
#include "fb.h"

/*
 * Framebuffer terminal.
 *
 * The text model is a ring of lines (scrollback) of 16-bit cells
 * (attribute << 8 | character). Rendering is lazy: console_putc() only edits
 * the model, console_flush() compares the model against what is already on
 * screen and repaints just the cells that differ, then cleans only the pixel
 * rows it touched. Mouse wheel / Shift+PageUp move a view window over the ring.
 */

#define CON_MARGIN_X   4
#define CON_MARGIN_Y   4
#define CON_MAX_COLS   160
#define CON_MAX_ROWS   64
#define CON_LINES      2000
#define CON_TAB        8
#define CON_FLUSH_BYTES 3072
#define CON_PARAMS     8
#define CON_BAR_WIDTH  4
#define CON_BG_DEFAULT 0
#define CON_FG_DEFAULT 7

/* Linux virtual-console palette (what an Arch Linux tty shows). */
static const uint32_t palette[16] = {
    0x000000U, 0xAA0000U, 0x00AA00U, 0xAA5500U,
    0x0000AAU, 0xAA00AAU, 0x00AAAAU, 0xAAAAAAU,
    0x555555U, 0xFF5555U, 0x55FF55U, 0xFFFF55U,
    0x5555FFU, 0xFF55FFU, 0x55FFFFU, 0xFFFFFFU
};

static uint16_t cells[CON_LINES][CON_MAX_COLS];
static uint32_t shown[CON_MAX_ROWS][CON_MAX_COLS]; /* what is on screen */

static int active = 0;
static int cols = 0;
static int rows = 0;
static int screen_base = 0;  /* absolute number of the top live line */
static int history_floor = 0; /* oldest absolute line that may be scrolled to */
static int view_back = 0;    /* lines scrolled back from live; 0 = live */
static int crow = 0, ccol = 0;
static int saved_row = 0, saved_col = 0;
static int wrap_pending = 0;
static int cursor_visible = 1;
static uint8_t cur_fg = CON_FG_DEFAULT, cur_bg = CON_BG_DEFAULT;
static int cur_bold = 0, cur_reverse = 0;
static int esc_state = 0; /* 0 normal, 1 ESC, 2 CSI, 3 ESC ( charset skip */
static int params[CON_PARAMS];
static int param_count = 0;
static int param_seen = 0;
static int csi_private = 0;
static int pending_bytes = 0;
static int dirty = 1;
static int bar_drawn = 0;

static uint16_t* line_ptr(int absolute)
{
    int slot = absolute % CON_LINES;
    if (slot < 0) slot += CON_LINES;
    return cells[slot];
}

static void clear_line_range(int absolute, int from, int to)
{
    uint16_t* line = line_ptr(absolute);
    uint16_t blank = (uint16_t)(((cur_bg & 15) << 4 | 7) << 8 | ' ');
    if (from < 0) from = 0;
    if (to > CON_MAX_COLS) to = CON_MAX_COLS;
    for (int i = from; i < to; ++i) line[i] = blank;
}

static int max_back()
{
    int back = screen_base - history_floor;
    if (back > CON_LINES - rows) back = CON_LINES - rows;
    return back < 0 ? 0 : back;
}

static void reset_attrs()
{
    cur_fg = CON_FG_DEFAULT; cur_bg = CON_BG_DEFAULT;
    cur_bold = 0; cur_reverse = 0;
}

static void scroll_screen_up()
{
    /* New bottom line becomes live; the old top line moves into history. */
    ++screen_base;
    clear_line_range(screen_base + rows - 1, 0, CON_MAX_COLS);
}

static void line_feed()
{
    if (crow + 1 >= rows) scroll_screen_up();
    else ++crow;
}

static void reverse_index()
{
    if (crow > 0) { --crow; return; }
    /* Scroll the visible screen down one line (history is untouched). */
    for (int r = rows - 1; r > 0; --r)
    {
        uint16_t* dst = line_ptr(screen_base + r);
        uint16_t* src = line_ptr(screen_base + r - 1);
        for (int c = 0; c < CON_MAX_COLS; ++c) dst[c] = src[c];
    }
    clear_line_range(screen_base, 0, CON_MAX_COLS);
}

static uint16_t make_cell(char ch)
{
    uint8_t fg = cur_fg;
    uint8_t bg = cur_bg;
    if (cur_bold && fg < 8) fg = (uint8_t)(fg + 8);
    if (cur_reverse) { uint8_t t = fg; fg = bg; bg = t; }
    return (uint16_t)((((bg & 15) << 4) | (fg & 15)) << 8 | (uint8_t)ch);
}

static void put_glyph(char ch)
{
    if (wrap_pending)
    {
        ccol = 0;
        line_feed();
        wrap_pending = 0;
    }
    uint16_t* line = line_ptr(screen_base + crow);
    line[ccol] = make_cell(ch);
    if (ccol + 1 >= cols) wrap_pending = 1;
    else ++ccol;
}

static int param(int index, int fallback)
{
    if (index < param_count && params[index] > 0) return params[index];
    if (index < param_count && params[index] == 0) return fallback;
    return fallback;
}

static void apply_sgr()
{
    if (param_count == 0) { reset_attrs(); return; }
    for (int i = 0; i < param_count; ++i)
    {
        int p = params[i];
        if (p == 0) reset_attrs();
        else if (p == 1) cur_bold = 1;
        else if (p == 22) cur_bold = 0;
        else if (p == 7) cur_reverse = 1;
        else if (p == 27) cur_reverse = 0;
        else if (p >= 30 && p <= 37) cur_fg = (uint8_t)(p - 30);
        else if (p == 39) cur_fg = CON_FG_DEFAULT;
        else if (p >= 40 && p <= 47) cur_bg = (uint8_t)(p - 40);
        else if (p == 49) cur_bg = CON_BG_DEFAULT;
        else if (p >= 90 && p <= 97) { cur_fg = (uint8_t)(p - 90 + 8); }
        else if (p >= 100 && p <= 107) { cur_bg = (uint8_t)(p - 100 + 8); }
    }
}

static void erase_display(int mode)
{
    if (mode == 0)
    {
        clear_line_range(screen_base + crow, ccol, CON_MAX_COLS);
        for (int r = crow + 1; r < rows; ++r) clear_line_range(screen_base + r, 0, CON_MAX_COLS);
    }
    else if (mode == 1)
    {
        for (int r = 0; r < crow; ++r) clear_line_range(screen_base + r, 0, CON_MAX_COLS);
        clear_line_range(screen_base + crow, 0, ccol + 1);
    }
    else
    {
        for (int r = 0; r < rows; ++r) clear_line_range(screen_base + r, 0, CON_MAX_COLS);
        if (mode == 3) history_floor = screen_base; /* forget scrollback */
    }
}

static void erase_line(int mode)
{
    if (mode == 0) clear_line_range(screen_base + crow, ccol, CON_MAX_COLS);
    else if (mode == 1) clear_line_range(screen_base + crow, 0, ccol + 1);
    else clear_line_range(screen_base + crow, 0, CON_MAX_COLS);
}

static void insert_lines(int n)
{
    if (n > rows - crow) n = rows - crow;
    for (int r = rows - 1; r >= crow + n; --r)
    {
        uint16_t* dst = line_ptr(screen_base + r);
        uint16_t* src = line_ptr(screen_base + r - n);
        for (int c = 0; c < CON_MAX_COLS; ++c) dst[c] = src[c];
    }
    for (int r = crow; r < crow + n; ++r) clear_line_range(screen_base + r, 0, CON_MAX_COLS);
}

static void delete_lines(int n)
{
    if (n > rows - crow) n = rows - crow;
    for (int r = crow; r + n < rows; ++r)
    {
        uint16_t* dst = line_ptr(screen_base + r);
        uint16_t* src = line_ptr(screen_base + r + n);
        for (int c = 0; c < CON_MAX_COLS; ++c) dst[c] = src[c];
    }
    for (int r = rows - n; r < rows; ++r) clear_line_range(screen_base + r, 0, CON_MAX_COLS);
}

static void delete_chars(int n)
{
    uint16_t* line = line_ptr(screen_base + crow);
    if (n > cols - ccol) n = cols - ccol;
    for (int c = ccol; c + n < cols; ++c) line[c] = line[c + n];
    clear_line_range(screen_base + crow, cols - n, cols);
}

static void insert_chars(int n)
{
    uint16_t* line = line_ptr(screen_base + crow);
    if (n > cols - ccol) n = cols - ccol;
    for (int c = cols - 1; c >= ccol + n; --c) line[c] = line[c - n];
    clear_line_range(screen_base + crow, ccol, ccol + n);
}

static void clamp_cursor()
{
    if (crow < 0) crow = 0;
    if (crow >= rows) crow = rows - 1;
    if (ccol < 0) ccol = 0;
    if (ccol >= cols) ccol = cols - 1;
}

static void run_csi(char final)
{
    wrap_pending = 0;
    if (csi_private)
    {
        if (final == 'h' || final == 'l')
            for (int i = 0; i < param_count; ++i)
                if (params[i] == 25) cursor_visible = final == 'h';
        return;
    }
    switch (final)
    {
        case 'A': crow -= param(0, 1); clamp_cursor(); break;
        case 'B': crow += param(0, 1); clamp_cursor(); break;
        case 'C': ccol += param(0, 1); clamp_cursor(); break;
        case 'D': ccol -= param(0, 1); clamp_cursor(); break;
        case 'E': crow += param(0, 1); ccol = 0; clamp_cursor(); break;
        case 'F': crow -= param(0, 1); ccol = 0; clamp_cursor(); break;
        case 'G': ccol = param(0, 1) - 1; clamp_cursor(); break;
        case 'd': crow = param(0, 1) - 1; clamp_cursor(); break;
        case 'H': case 'f':
            crow = param(0, 1) - 1; ccol = param(1, 1) - 1; clamp_cursor(); break;
        case 'J': erase_display(param_count ? params[0] : 0); break;
        case 'K': erase_line(param_count ? params[0] : 0); break;
        case 'L': insert_lines(param(0, 1)); break;
        case 'M': delete_lines(param(0, 1)); break;
        case 'P': delete_chars(param(0, 1)); break;
        case '@': insert_chars(param(0, 1)); break;
        case 'X':
        {
            int n = param(0, 1);
            clear_line_range(screen_base + crow, ccol, ccol + n < cols ? ccol + n : cols);
            break;
        }
        case 'm': apply_sgr(); break;
        case 's': saved_row = crow; saved_col = ccol; break;
        case 'u': crow = saved_row; ccol = saved_col; clamp_cursor(); break;
        default: break; /* r, n, c, ... accepted and ignored */
    }
}

static void console_reset_state()
{
    for (int r = 0; r < CON_LINES; ++r) clear_line_range(r, 0, CON_MAX_COLS);
    screen_base = 0; history_floor = 0; view_back = 0;
    crow = 0; ccol = 0; saved_row = 0; saved_col = 0;
    wrap_pending = 0; cursor_visible = 1;
    reset_attrs();
    esc_state = 0; param_count = 0; param_seen = 0; csi_private = 0;
    for (int r = 0; r < CON_MAX_ROWS; ++r)
        for (int c = 0; c < CON_MAX_COLS; ++c) shown[r][c] = 0xFFFFFFFFU;
    bar_drawn = 0;
    dirty = 1;
}

static void geometry_from_fb()
{
    int w = fb_get_width(), h = fb_get_height();
    cols = (w - 2 * CON_MARGIN_X - CON_BAR_WIDTH) / CONSOLE_FONT_W;
    rows = (h - 2 * CON_MARGIN_Y) / CONSOLE_FONT_H;
    if (cols > CON_MAX_COLS) cols = CON_MAX_COLS;
    if (rows > CON_MAX_ROWS) rows = CON_MAX_ROWS;
    if (cols < 20) cols = 20;
    if (rows < 5) rows = 5;
}

int console_start()
{
    if (!fb_is_ready() || fb_buffer() == 0) return -1;
    geometry_from_fb();
    console_reset_state();
    fb_clear(palette[CON_BG_DEFAULT]);
    fb_present();
    active = 1;
    return 0;
}

int console_active() { return active; }
int console_columns() { return cols; }
int console_rows() { return rows; }
int console_scroll_offset() { return view_back; }
int console_scrollback_lines() { return max_back(); }

void console_snap_to_bottom()
{
    if (view_back != 0) { view_back = 0; dirty = 1; }
}

void console_scroll_lines(int lines)
{
    if (!active || lines == 0) return;
    int next = view_back + lines;
    if (next < 0) next = 0;
    if (next > max_back()) next = max_back();
    if (next != view_back) { view_back = next; dirty = 1; }
    console_flush();
}

void console_scroll_pages(int pages)
{
    console_scroll_lines(pages * (rows > 2 ? rows - 1 : 1));
}

static void feed(char ch)
{
    unsigned char c = (unsigned char)ch;
    if (esc_state == 1)
    {
        esc_state = 0;
        if (c == '[') { esc_state = 2; param_count = 0; param_seen = 0; csi_private = 0; for (int i = 0; i < CON_PARAMS; ++i) params[i] = 0; }
        else if (c == '(' || c == ')') esc_state = 3;
        else if (c == '7') { saved_row = crow; saved_col = ccol; }
        else if (c == '8') { crow = saved_row; ccol = saved_col; clamp_cursor(); wrap_pending = 0; }
        else if (c == 'M') { reverse_index(); }
        else if (c == 'D') { line_feed(); }
        else if (c == 'E') { ccol = 0; line_feed(); }
        else if (c == 'c')
        {
            reset_attrs();
            erase_display(2);
            crow = 0; ccol = 0; wrap_pending = 0; cursor_visible = 1;
        }
        return;
    }
    if (esc_state == 3) { esc_state = 0; return; }
    if (esc_state == 2)
    {
        if (c >= '0' && c <= '9')
        {
            if (param_count == 0) param_count = 1;
            int* p = &params[param_count - 1];
            if (*p < 10000) *p = *p * 10 + (c - '0');
            param_seen = 1;
        }
        else if (c == ';')
        {
            if (param_count == 0) param_count = 1;
            if (param_count < CON_PARAMS) ++param_count;
        }
        else if (c == '?') csi_private = 1;
        else if (c >= 0x40 && c <= 0x7E)
        {
            esc_state = 0;
            run_csi((char)c);
        }
        else if (c < 0x20) { /* control characters inside CSI: ignore */ }
        else if (c == ' ' || c == '!' || c == '"' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' ) { /* intermediates */ }
        else esc_state = 0;
        return;
    }

    switch (c)
    {
        case 27: esc_state = 1; return;
        case '\r': ccol = 0; wrap_pending = 0; return;
        case '\n': case 11: case 12: ccol = 0; wrap_pending = 0; line_feed(); return;
        case '\b': if (ccol > 0) --ccol; wrap_pending = 0; return;
        case '\t':
        {
            wrap_pending = 0;
            int next = (ccol / CON_TAB + 1) * CON_TAB;
            if (next >= cols) next = cols - 1;
            ccol = next;
            return;
        }
        case 7: case 0: case 127: case 14: case 15: return;
        default: break;
    }
    if (c < 32) return;
    if (c >= 127) { put_glyph('?'); return; }
    put_glyph((char)c);
}

void console_putc(char c)
{
    if (!active) return;
    /* New output while reading history jumps back to the live screen. */
    if (view_back != 0) { view_back = 0; }
    feed(c);
    dirty = 1;
    if (++pending_bytes >= CON_FLUSH_BYTES) console_flush();
}

void console_write(const char* text)
{
    if (!active || text == 0) return;
    while (*text) console_putc(*text++);
}

void console_write_attr(const char* text, int fg, int bold)
{
    if (!active || text == 0) return;
    uint8_t saved_fg = cur_fg;
    int saved_bold = cur_bold;
    cur_fg = (uint8_t)(fg & 7);
    cur_bold = bold != 0;
    console_write(text);
    cur_fg = saved_fg;
    cur_bold = saved_bold;
}

static uint32_t blend(uint32_t fg, uint32_t bg, uint32_t a)
{
    uint32_t ia = 255U - a;
    uint32_t r = ((((fg >> 16) & 255U) * a) + (((bg >> 16) & 255U) * ia) + 127U) / 255U;
    uint32_t g = ((((fg >> 8) & 255U) * a) + (((bg >> 8) & 255U) * ia) + 127U) / 255U;
    uint32_t b = (((fg & 255U) * a) + ((bg & 255U) * ia) + 127U) / 255U;
    return (r << 16) | (g << 8) | b;
}

static void paint_cell(int row, int col, uint16_t cell, int cursor)
{
    uint32_t* base = fb_buffer();
    int stride = fb_stride_pixels();
    uint8_t attr = (uint8_t)(cell >> 8);
    unsigned ch = cell & 0xFFU;
    uint32_t bg = palette[(attr >> 4) & 15];
    uint32_t fg = palette[attr & 15];
    if (cursor) { uint32_t t = fg; fg = bg; bg = t; if (fg == bg) fg = 0; }
    if (ch < CONSOLE_FONT_FIRST || ch >= CONSOLE_FONT_FIRST + CONSOLE_FONT_GLYPHS) ch = ' ';
    const uint8_t* glyph = console_font_glyphs[ch - CONSOLE_FONT_FIRST];
    int x0 = CON_MARGIN_X + col * CONSOLE_FONT_W;
    int y0 = CON_MARGIN_Y + row * CONSOLE_FONT_H;
    for (int gy = 0; gy < CONSOLE_FONT_H; ++gy)
    {
        uint32_t* dst = base + (uint64_t)(y0 + gy) * (uint64_t)stride + (uint64_t)x0;
        const uint8_t* src = glyph + gy * CONSOLE_FONT_W;
        for (int gx = 0; gx < CONSOLE_FONT_W; ++gx)
        {
            uint32_t a = src[gx];
            dst[gx] = a == 0U ? bg : (a == 255U ? fg : blend(fg, bg, a));
        }
    }
}

static int draw_scroll_bar()
{
    int h = fb_get_height();
    int x = fb_get_width() - CON_BAR_WIDTH;
    int track_top = CON_MARGIN_Y, track_h = rows * CONSOLE_FONT_H;
    int total = max_back() + rows;
    if (view_back > 0 && total > rows)
    {
        fb_fill_rect(x, 0, CON_BAR_WIDTH, h, palette[CON_BG_DEFAULT]);
        int thumb_h = (int)((long long)track_h * rows / total);
        if (thumb_h < 12) thumb_h = 12;
        int span = track_h - thumb_h;
        int pos_from_top = max_back() > 0
            ? (int)((long long)span * (max_back() - view_back) / max_back()) : 0;
        fb_fill_rect(x + 1, track_top + pos_from_top, CON_BAR_WIDTH - 2, thumb_h, 0x777777U);
        bar_drawn = 1;
        return 1;
    }
    else if (bar_drawn)
    {
        fb_fill_rect(x, 0, CON_BAR_WIDTH, h, palette[CON_BG_DEFAULT]);
        bar_drawn = 0;
        return 1;
    }
    return 0;
}

void console_flush()
{
    if (!active || !dirty) { pending_bytes = 0; return; }
    int first_row = rows, last_row = -1;
    int top_abs = screen_base - view_back;
    int show_cursor = cursor_visible && view_back == 0;
    for (int r = 0; r < rows; ++r)
    {
        int abs_line = top_abs + r;
        int row_changed = 0;
        const uint16_t* line = abs_line >= 0 ? line_ptr(abs_line) : 0;
        for (int c = 0; c < cols; ++c)
        {
            uint16_t cell = line ? line[c] : (uint16_t)0x0720;
            int cur = show_cursor && r == crow && (c == ccol);
            uint32_t key = (uint32_t)cell | (cur ? 0x10000U : 0U);
            if (shown[r][c] == key) continue;
            shown[r][c] = key;
            paint_cell(r, c, cell, cur);
            row_changed = 1;
        }
        if (row_changed)
        {
            if (r < first_row) first_row = r;
            if (r > last_row) last_row = r;
        }
    }
    int bar_changed = draw_scroll_bar();
    if (last_row >= 0)
        fb_present_rows(CON_MARGIN_Y + first_row * CONSOLE_FONT_H,
                        CON_MARGIN_Y + (last_row + 1) * CONSOLE_FONT_H);
    if (bar_changed)
        fb_present_rows(0, fb_get_height());
    dirty = 0;
    pending_bytes = 0;
}

#ifdef HOST_TEST
uint16_t console_test_cell(int row, int col)
{
    if (row < 0 || row >= rows || col < 0 || col >= cols) return 0;
    return line_ptr(screen_base + row)[col];
}
void console_test_cursor(int* row, int* col) { if (row) *row = crow; if (col) *col = ccol; }
void console_test_reset() { active = 0; }
#endif
