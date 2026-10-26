#include "ui.h"
#include "fb.h"
#include "vfs.h"
#include "commands.h"
#include "uart.h"
#include "history.h"
#include "string.h"

#define TERM_LINES 23
#define TERM_COLS  116
#define TERM_INPUT_CAP 256
#define TERM_TTY_ROWS 44
#define TERM_TTY_COLS 120

static char term_lines[TERM_LINES][TERM_COLS];
static int term_start = 0;
static int term_count = 0;
static char term_input[TERM_INPUT_CAP];
static int term_input_len = 0;
static int term_history_cursor = 0;
static char term_cwd[192];
static char term_output_line[TERM_COLS];
static int term_output_len = 0;
static int term_escape_state = 0;
static int term_interactive_mode = 0;
static char term_tty_cells[TERM_TTY_ROWS][TERM_TTY_COLS];
static int term_tty_row = 0;
static int term_tty_col = 0;
static int term_tty_escape_state = 0;
static char term_tty_csi[20];
static int term_tty_csi_len = 0;
static int term_tty_dirty = 0;

static void term_clear()
{
    for (int i = 0; i < TERM_LINES; ++i) term_lines[i][0] = '\0';
    term_start = 0;
    term_count = 0;
    term_output_len = 0;
    term_output_line[0] = '\0';
}

static void term_add_piece(const char* text, int length)
{
    if (text == 0) return;
    if (term_count < TERM_LINES)
    {
        int slot = (term_start + term_count) % TERM_LINES;
        int n = length < TERM_COLS - 1 ? length : TERM_COLS - 1;
        if (n < 0) n = 0;
        for (int i = 0; i < n; ++i) term_lines[slot][i] = text[i];
        term_lines[slot][n] = '\0';
        ++term_count;
    }
    else
    {
        int slot = term_start;
        int n = length < TERM_COLS - 1 ? length : TERM_COLS - 1;
        if (n < 0) n = 0;
        for (int i = 0; i < n; ++i) term_lines[slot][i] = text[i];
        term_lines[slot][n] = '\0';
        term_start = (term_start + 1) % TERM_LINES;
    }
}

static void term_print(const char* text)
{
    if (text == 0) return;
    int start = 0;
    int n = 0;
    while (text[n] != '\0')
    {
        if (text[n] == '\n')
        {
            term_add_piece(text + start, n - start);
            start = n + 1;
        }
        else if (n - start >= TERM_COLS - 1)
        {
            term_add_piece(text + start, n - start);
            start = n;
        }
        ++n;
    }
    if (n > start) term_add_piece(text + start, n - start);
    else if (n == 0) term_add_piece("", 0);
}

static int term_equal(const char* a, const char* b)
{
    if (a == 0 || b == 0) return 0;
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) ++i;
    return a[i] == '\0' && b[i] == '\0';
}


static void term_tty_render()
{
    fb_clear(0x071016U);
    for (int row = 0; row < TERM_TTY_ROWS; ++row)
    {
        term_tty_cells[row][TERM_TTY_COLS - 1] = '\0';
        fb_draw_ui_text(term_tty_cells[row], 8, 8 + row * 16, 0xE0F3F5U, 1);
    }
    fb_present();
    term_tty_dirty = 0;
}

static void term_tty_flush()
{
    if (term_interactive_mode && term_tty_dirty) term_tty_render();
}

static void term_tty_clear()
{
    for (int row = 0; row < TERM_TTY_ROWS; ++row)
        for (int col = 0; col < TERM_TTY_COLS; ++col)
            term_tty_cells[row][col] = ' ';
    term_tty_row = 0;
    term_tty_col = 0;
}

static void term_tty_scroll()
{
    for (int row = 1; row < TERM_TTY_ROWS; ++row)
        for (int col = 0; col < TERM_TTY_COLS; ++col)
            term_tty_cells[row - 1][col] = term_tty_cells[row][col];
    for (int col = 0; col < TERM_TTY_COLS; ++col)
        term_tty_cells[TERM_TTY_ROWS - 1][col] = ' ';
    term_tty_row = TERM_TTY_ROWS - 1;
    term_tty_dirty = 1;
}

static int term_tty_param(const char* text, int index, int fallback)
{
    int current = 0;
    int value = 0;
    int seen = 0;
    for (int i = 0; text[i]; ++i)
    {
        if (text[i] >= '0' && text[i] <= '9')
        {
            value = value * 10 + (text[i] - '0');
            if (value > 999) value = 999;
            seen = 1;
        }
        else if (text[i] == ';')
        {
            if (current == index) return seen ? value : fallback;
            ++current; value = 0; seen = 0;
        }
    }
    return current == index ? (seen ? value : fallback) : fallback;
}

static void term_tty_apply_csi(char final)
{
    int first = term_tty_param(term_tty_csi, 0, 0);
    if (final == 'J')
    {
        if (first == 2 || first == 3) term_tty_clear();
        else if (first == 0)
        {
            for (int row = term_tty_row; row < TERM_TTY_ROWS; ++row)
                for (int col = (row == term_tty_row ? term_tty_col : 0); col < TERM_TTY_COLS; ++col)
                    term_tty_cells[row][col] = ' ';
        }
        term_tty_dirty = 1;
        term_tty_render();
    }
    else if (final == 'H' || final == 'f')
    {
        int row = term_tty_param(term_tty_csi, 0, 1);
        int col = term_tty_param(term_tty_csi, 1, 1);
        term_tty_row = row > 0 ? row - 1 : 0;
        term_tty_col = col > 0 ? col - 1 : 0;
        if (term_tty_row >= TERM_TTY_ROWS) term_tty_row = TERM_TTY_ROWS - 1;
        if (term_tty_col >= TERM_TTY_COLS) term_tty_col = TERM_TTY_COLS - 1;
    }
    else if (final == 'A') { if (term_tty_row > first) term_tty_row -= first ? first : 1; }
    else if (final == 'B') { term_tty_row += first ? first : 1; if (term_tty_row >= TERM_TTY_ROWS) term_tty_row = TERM_TTY_ROWS - 1; }
    else if (final == 'C') { term_tty_col += first ? first : 1; if (term_tty_col >= TERM_TTY_COLS) term_tty_col = TERM_TTY_COLS - 1; }
    else if (final == 'D') { int amount = first ? first : 1; term_tty_col = term_tty_col > amount ? term_tty_col - amount : 0; }
    else if (final == 'K')
    {
        int start = first == 1 ? 0 : term_tty_col;
        int end = first == 1 ? term_tty_col + 1 : TERM_TTY_COLS;
        if (first == 2) { start = 0; end = TERM_TTY_COLS; }
        if (end > TERM_TTY_COLS) end = TERM_TTY_COLS;
        for (int col = start; col < end; ++col) term_tty_cells[term_tty_row][col] = ' ';
        term_tty_dirty = 1;
        term_tty_render();
    }
    /* 'm' (SGR colors) is intentionally accepted without styling. */
}

static void term_tty_capture(char value)
{
    unsigned char c = (unsigned char)value;
    if (term_tty_escape_state == 1)
    {
        if (c == '[') { term_tty_escape_state = 2; term_tty_csi_len = 0; term_tty_csi[0] = '\0'; }
        else term_tty_escape_state = 0;
        return;
    }
    if (term_tty_escape_state == 2)
    {
        if (c >= 0x40U && c <= 0x7EU)
        {
            term_tty_csi[term_tty_csi_len] = '\0';
            term_tty_apply_csi((char)c);
            term_tty_escape_state = 0;
        }
        else if (term_tty_csi_len < (int)sizeof(term_tty_csi) - 1)
        {
            term_tty_csi[term_tty_csi_len++] = (char)c;
            term_tty_csi[term_tty_csi_len] = '\0';
        }
        return;
    }
    if (c == 27U) { term_tty_escape_state = 1; return; }
    if (c == '\r') { term_tty_col = 0; return; }
    if (c == '\n')
    {
        term_tty_dirty = 1;
        term_tty_col = 0;
        if (++term_tty_row >= TERM_TTY_ROWS) term_tty_scroll();
        term_tty_render();
        return;
    }
    if (c == '\b' || c == 127U)
    {
        if (term_tty_col > 0) --term_tty_col;
        term_tty_cells[term_tty_row][term_tty_col] = ' ';
        term_tty_dirty = 1;
        term_tty_render();
        return;
    }
    if (c == '\t')
    {
        int spaces = 4 - (term_tty_col % 4);
        term_tty_dirty = 1;
        while (spaces-- > 0)
        {
            term_tty_cells[term_tty_row][term_tty_col++] = ' ';
            if (term_tty_col >= TERM_TTY_COLS) { term_tty_col = 0; if (++term_tty_row >= TERM_TTY_ROWS) term_tty_scroll(); }
        }
        return;
    }
    if (c >= 32U && c <= 126U)
    {
        term_tty_dirty = 1;
        term_tty_cells[term_tty_row][term_tty_col++] = (char)c;
        if (term_tty_col >= TERM_TTY_COLS) { term_tty_col = 0; if (++term_tty_row >= TERM_TTY_ROWS) term_tty_scroll(); }
    }
}

static int term_is_interactive_command(const char* line)
{
    char word[24];
    int n = 0;
    while (*line == ' ' || *line == '\t') ++line;
    while (line[n] && line[n] != ' ' && line[n] != '\t' && n < (int)sizeof(word) - 1)
    { word[n] = line[n]; ++n; }
    word[n] = '\0';
    return term_equal(word, "nano") || term_equal(word, "calc") || term_equal(word, "exec") ||
           term_equal(word, "userdemo") || term_equal(word, "user") || term_equal(word, "su");
}

static void term_update_cwd()
{
    vfs_get_path(term_cwd, sizeof(term_cwd));
}

static void term_commit_output_line()
{
    term_output_line[term_output_len] = '\0';
    term_add_piece(term_output_line, term_output_len);
    term_output_len = 0;
    term_output_line[0] = '\0';
}

/* execute_command() writes through the same UART API as the serial shell.
 * While a command runs, capture that output into this app's scrollback. ANSI
 * control sequences are consumed here rather than displayed as raw characters. */
static void term_capture_output(char value)
{
    if (term_interactive_mode)
    {
        term_tty_capture(value);
        return;
    }
    unsigned char c = (unsigned char)value;
    if (term_escape_state == 1)
    {
        term_escape_state = c == '[' ? 2 : 0;
        return;
    }
    if (term_escape_state == 2)
    {
        if (c >= 0x40U && c <= 0x7EU)
        {
            if (c == 'J')
            {
                term_clear();
            }
            term_escape_state = 0;
        }
        return;
    }
    if (c == 27U) { term_escape_state = 1; return; }
    if (c == '\r') return;
    if (c == '\n') { term_commit_output_line(); return; }
    if (c == '\b' || c == 127U)
    {
        if (term_output_len > 0) term_output_line[--term_output_len] = '\0';
        return;
    }
    if (c == '\t')
    {
        int spaces = 4 - (term_output_len % 4);
        while (spaces-- > 0)
        {
            if (term_output_len >= TERM_COLS - 1) term_commit_output_line();
            term_output_line[term_output_len++] = ' ';
        }
        return;
    }
    if (c >= 32U && c <= 126U)
    {
        if (term_output_len >= TERM_COLS - 1) term_commit_output_line();
        term_output_line[term_output_len++] = (char)c;
        term_output_line[term_output_len] = '\0';
    }
}

static int term_execute()
{
    char command_line[TERM_INPUT_CAP];
    int i = 0;
    while (term_input[i] && i < (int)sizeof(command_line) - 1)
    {
        command_line[i] = term_input[i];
        ++i;
    }
    command_line[i] = '\0';
    while (command_line[0] == ' ' || command_line[0] == '\t')
    {
        for (int j = 0; command_line[j]; ++j) command_line[j] = command_line[j + 1];
    }
    int command_length = str_len(command_line);
    while (command_length > 0 && (command_line[command_length - 1] == ' ' || command_line[command_length - 1] == '\t'))
        command_line[--command_length] = '\0';
    if (command_line[0] == '\0') return 0;

    int interactive = term_is_interactive_command(command_line);
    term_print(command_line);
    history_add(command_line);
    term_history_cursor = history_count();
    term_output_len = 0;
    term_escape_state = 0;
    term_output_line[0] = '\0';
    term_interactive_mode = interactive;
    term_tty_escape_state = 0;
    term_tty_csi_len = 0;
    if (interactive)
    {
        term_tty_clear();
        term_tty_render();
    }
    uart_set_output_hook(term_capture_output, 0);
    uart_set_output_flush_hook(term_tty_flush);
    execute_command(command_line);
    uart_clear_output_hook();
    if (interactive && term_tty_dirty) term_tty_render();
    if (!interactive && term_output_len > 0) term_commit_output_line();
    term_interactive_mode = 0;
    term_escape_state = 0;
    term_update_cwd();
    return 0;
}

void ui_terminal_init()
{
    term_clear();
    term_input_len = 0;
    term_input[0] = '\0';
    term_history_cursor = history_count();
    term_update_cwd();
    term_print("RioOS Terminal [terminal.rpk]");
    term_print("Shell dispatcher: same built-in commands as RioOS serial shell.");
    term_print("Type help to list commands; use exit to return home.");
}

void ui_terminal_render(uint64_t epoch)
{
    (void)epoch;
    int w = fb_get_width(), h = fb_get_height();
    fb_clear(0x071016U);
    fb_fill_rect(0, 0, w, 36, 0x172932U);
    fb_draw_circle(20, 18, 5, 0xFF6C65U);
    fb_draw_circle(38, 18, 5, 0xF5C451U);
    fb_draw_circle(56, 18, 5, 0x5BD79AU);
    fb_draw_ui_text("RioOS Terminal", 78, 12, 0xE9F4F7U, 1);
    fb_draw_ui_text("RPK / BUILT-IN", w - 178, 12, 0x80A7B0U, 1);

    int y = 61;
    for (int i = 0; i < term_count; ++i)
    {
        int slot = (term_start + i) % TERM_LINES;
        fb_draw_ui_text(term_lines[slot], 28, y, 0xA9F4D0U, 1);
        y += 23;
    }
    fb_draw_rect(24, h - 79, w - 48, 1, 0x28434BU);
    fb_draw_ui_text("rio@rioos:", 28, h - 61, 0x7DE1B1U, 1);
    fb_draw_ui_text(term_cwd, 108, h - 61, 0xB8D6FFU, 1);
    int x = 108 + fb_ui_text_width(term_cwd, 1);
    fb_draw_ui_text(" $ ", x, h - 61, 0xFFFFFFU, 1);
    x += fb_ui_text_width(" $ ", 1);
    fb_draw_ui_text(term_input, x, h - 61, 0xFFFFFFU, 1);
    fb_draw_round_rect(28, h - 31, 7, 7, 3, 0x68DDA8U);
    fb_draw_ui_text("READY", 42, h - 31, 0x81BFB0U, 1);
    fb_draw_ui_text("ESC: HOME", w - 108, h - 31, 0x81BFB0U, 1);
    fb_present();
}

int ui_terminal_key(char key)
{
    if ((unsigned char)key == 27U) return 1;
    if (key == '\r' || key == '\n')
    {
        term_input[term_input_len] = '\0';
        int should_exit = term_execute();
        term_input_len = 0;
        term_input[0] = '\0';
        term_update_cwd();
        return should_exit ? 1 : 0;
    }
    if ((unsigned char)key == 0x80U)
    {
        if (term_history_cursor > 0) --term_history_cursor;
        const char* old = history_get(term_history_cursor);
        if (old)
        {
            str_copy(term_input, old, sizeof(term_input));
            term_input_len = str_len(term_input);
        }
        return 0;
    }
    if ((unsigned char)key == 0x81U)
    {
        if (term_history_cursor < history_count()) ++term_history_cursor;
        if (term_history_cursor >= history_count())
        {
            term_input_len = 0;
            term_input[0] = '\0';
        }
        else
        {
            const char* old = history_get(term_history_cursor);
            if (old)
            {
                str_copy(term_input, old, sizeof(term_input));
                term_input_len = str_len(term_input);
            }
        }
        return 0;
    }
    if (key == '\b' || (unsigned char)key == 127U)
    {
        if (term_input_len > 0) term_input[--term_input_len] = '\0';
        term_history_cursor = history_count();
        return 0;
    }
    if (key >= 32 && (unsigned char)key <= 126U && term_input_len < (int)sizeof(term_input) - 1)
    {
        term_input[term_input_len++] = key;
        term_input[term_input_len] = '\0';
        term_history_cursor = history_count();
    }
    return 0;
}
