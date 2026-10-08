#include "shell.h"
#include "keyboard.h"
#include "commands.h"
#include "uart.h"
#include "fb.h"
#include "vfs.h"
#include "fs.h"
#include "security.h"
#include "history.h"
#include "string.h"

static const char* command_names[] = {
    "help","man","clear","version","mem","echo","hello","ls","pwd","cd","mkdir","touch",
    "write","cat","rm","cp","mv","tree","ps","uptime","sched","exec","nano","calc",
    "elf","syscall","user","userdemo","su","logout","exit","level","mmu","disk","diskread",
    "diskwrite","diskflush","apps","uname","whoami","net","ping","selftest","pages","tasks",
    "mount","stats","log","irq","grep","find","history","date","du","df","wc","chmod","env","export"
};

static void shell_prompt_text()
{
    char cwd[96];
    vfs_get_path(cwd, (int)sizeof(cwd));
    uart_putc('[');
    uart_puts(security_username());
    uart_puts("@RioOS ");
    uart_puts(cwd);
    uart_putc(']');
    uart_putc(security_prompt_char());
    uart_putc(' ');

    fb_print("[", 0x00FF00);
    fb_print(security_username(), 0x00FF00);
    fb_print("@RioOS ", 0x00FF00);
    fb_print(cwd, 0x00FF00);
    fb_print("]", 0x00FF00);
    fb_putc(security_prompt_char(), 0x00FF00);
    fb_putc(' ', 0x00FF00);
}

static void redraw_line(const char* input, int length)
{
    /* UART terminal: clear current row then redraw the prompt and current line. */
    uart_puts("\r\x1B[2K");
    shell_prompt_text();
    for (int i = 0; i < length; ++i) uart_putc(input[i]);
}

static int prefix_match(const char* a, const char* prefix)
{
    int i = 0;
    while (prefix[i]) { if (a[i] != prefix[i]) return 0; ++i; }
    return 1;
}

static int split_last_token(const char* input, int length, int* start)
{
    int i = length - 1;
    while (i >= 0 && (input[i] == ' ' || input[i] == '\t')) --i;
    while (i >= 0 && input[i] != ' ' && input[i] != '\t') --i;
    *start = i + 1;
    return length - *start;
}

static void complete_command_or_file(char* input, int* length)
{
    int start = 0;
    int token_len = split_last_token(input, *length, &start);
    char prefix[128];
    if (token_len >= (int)sizeof(prefix)) return;
    for (int i = 0; i < token_len; ++i) prefix[i] = input[start + i];
    prefix[token_len] = '\0';

    /* First word: command completion. */
    int is_command = 1;
    for (int i = 0; i < start; ++i)
        if (input[i] != ' ' && input[i] != '\t') { is_command = 0; break; }

    if (is_command)
    {
        const char* found = 0;
        int matches = 0;
        for (unsigned i = 0; i < sizeof(command_names)/sizeof(command_names[0]); ++i)
        {
            if (prefix_match(command_names[i], prefix)) { found = command_names[i]; ++matches; }
        }
        if (matches == 1 && found)
        {
            int suffix = str_len(found) - token_len;
            for (int i = 0; i < suffix && *length < 255; ++i) input[(*length)++] = found[token_len + i];
            input[*length] = '\0';
            redraw_line(input, *length);
        }
        else if (matches > 1)
        {
            uart_puts("\r\n");
            for (unsigned i = 0; i < sizeof(command_names)/sizeof(command_names[0]); ++i)
                if (prefix_match(command_names[i], prefix)) { uart_puts(command_names[i]); uart_puts("  "); }
            uart_puts("\r\n");
            redraw_line(input, *length);
        }
        return;
    }

    /* File/directory completion in the current directory or a path parent. */
    char parent_path[192];
    char base[96];
    int slash = -1;
    for (int i = token_len - 1; i >= 0; --i) if (prefix[i] == '/') { slash = i; break; }
    if (slash < 0)
    {
        str_copy(parent_path, ".", sizeof(parent_path));
        str_copy(base, prefix, sizeof(base));
    }
    else
    {
        for (int i = 0; i < slash && i < (int)sizeof(parent_path)-1; ++i) parent_path[i] = prefix[i];
        parent_path[slash] = '\0';
        str_copy(base, prefix + slash + 1, sizeof(base));
        if (slash == 0) str_copy(parent_path, "/", sizeof(parent_path));
    }

    int dir = fs_resolve(parent_path[0] ? parent_path : ".", vfs_cwd());
    if (dir < 0 || fs_get_type(dir) != FS_DIR) return;
    int matches = 0; int last_id = -1;
    for (int i = 0; i < fs_get_child_count(dir); ++i)
    {
        int id = fs_get_child(dir, i);
        if (id >= 0 && prefix_match(fs_get_name(id), base)) { ++matches; last_id = id; }
    }
    if (matches == 1 && last_id >= 0)
    {
        const char* name = fs_get_name(last_id);
        int extra = str_len(name) - str_len(base);
        for (int i = 0; i < extra && *length < 255; ++i) input[(*length)++] = name[str_len(base)+i];
        if (fs_get_type(last_id) == FS_DIR && *length < 255) input[(*length)++] = '/';
        input[*length] = '\0';
        redraw_line(input, *length);
    }
    else if (matches > 1)
    {
        uart_puts("\r\n");
        for (int i = 0; i < fs_get_child_count(dir); ++i)
        {
            int id = fs_get_child(dir, i);
            if (id >= 0 && prefix_match(fs_get_name(id), base)) { uart_puts(fs_get_name(id)); uart_puts("  "); }
        }
        uart_puts("\r\n");
        redraw_line(input, *length);
    }
}

static int read_escape_sequence(char* out)
{
    out[0] = out[1] = out[2] = '\0';
    int loops = 0;
    while (!uart_has_data() && loops++ < 100000) asm volatile("yield");
    if (!uart_has_data()) return 0;
    out[0] = uart_getc();
    if (out[0] == '[')
    {
        loops = 0;
        while (!uart_has_data() && loops++ < 100000) asm volatile("yield");
        if (!uart_has_data()) return 1;
        out[1] = uart_getc();
    }
    return 2;
}

void shell()
{
    char input[256];
    history_init();

    while (1)
    {
        shell_prompt_text();
        int length = 0;
        int history_cursor = history_count();

        while (1)
        {
            char c = 0;
            if (uart_has_data()) c = uart_getc();
            else if (keyboard_has_data()) c = keyboard_getc();
            else { asm volatile("yield"); continue; }

            if (c == '\x1B')
            {
                char seq[3];
                if (read_escape_sequence(seq) >= 2 && seq[0] == '[')
                {
                    if (seq[1] == 'A') /* Up */
                    {
                        if (history_cursor > 0) --history_cursor;
                        const char* h = history_get(history_cursor);
                        if (h)
                        {
                            str_copy(input, h, sizeof(input));
                            length = str_len(input);
                            redraw_line(input, length);
                        }
                    }
                    else if (seq[1] == 'B') /* Down */
                    {
                        if (history_cursor < history_count()) ++history_cursor;
                        if (history_cursor == history_count())
                        {
                            input[0] = '\0'; length = 0; redraw_line(input, length);
                        }
                        else
                        {
                            const char* h = history_get(history_cursor);
                            if (h) { str_copy(input, h, sizeof(input)); length = str_len(input); redraw_line(input, length); }
                        }
                    }
                }
                continue;
            }

            if (c == '\t')
            {
                complete_command_or_file(input, &length);
                continue;
            }

            if (c == '\r' || c == '\n')
            {
                input[length] = '\0';
                uart_puts("\r\n");
                fb_print("\r\n", 0xFFFFFF);
                if (length > 0) history_add(input);
                break;
            }

            if (c == 8 || c == 127)
            {
                if (length > 0)
                {
                    --length;
                    input[length] = '\0';
                    uart_puts("\b \b");
                    fb_putc('\b', 0xFFFFFF);
                }
                continue;
            }

            if (c >= 32 && c <= 126 && length < 255)
            {
                input[length++] = c;
                input[length] = '\0';
                uart_putc(c);
                fb_putc(c, 0xFFFFFF);
            }
        }

        execute_command(input);
        uart_puts("\r\n");
        fb_print("\r\n", 0xFFFFFF);
    }
}
