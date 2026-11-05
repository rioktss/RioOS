#include "env.h"
#include "shell.h"
#include "keyboard.h"
#include "commands.h"
#include "uart.h"
#include "console.h"
#include "interrupt.h"
#include "vfs.h"
#include "fs.h"
#include "security.h"
#include "history.h"
#include "shellext.h"
#include "jobs.h"
#include "netstack.h"
#include "string.h"

static const char* command_names[] = {
    "help","man","clear","version","mem","echo","hello","ls","pwd","cd","mkdir","touch",
    "write","cat","rm","cp","mv","tree","ps","uptime","sched","exec","nano","calc",
    "elf","syscall","user","userdemo","su","logout","exit","level","mmu","disk","diskread",
    "diskwrite","diskflush","apps","uname","whoami","net","ping","selftest","pages","tasks",
    "mount","stats","log","irq","grep","find","history","date","du","df","wc","chmod","env","export",
    "ifconfig","nslookup","httpget","dnsserver","smp","fbinfo",
    "stat","chown","truncate","seq","basename","dirname","poweroff","reboot","head","tail","sort","uniq","cut","tr","tee","sleep","rmdir","free","which","dhcp","ping6","httpd","lspci","nvme","sata","umount","mkfs","fsck","fat","quota","awk","sed","xargs","rev","tac","nl","alias","unalias","printf","expr","more","less","hexdump","libctest","sync","ln","readlink","jtest","threads","thread-demo","unset","fds","sh","source","jobs","fg","bg","kill","let","test","true","false","read","useradd","userdel","passwd","id","users","return","nc","udp6","wget","telnet","usb","httpsget","curl","ntpdate","fw","gzip","gunzip","zcat","tar","diff","vi","pkg","cc"
};

static void shell_prompt_text()
{
    char cwd[96];
    vfs_get_path(cwd, (int)sizeof(cwd));
    char tail[4];
    tail[0] = ']'; tail[1] = security_prompt_char(); tail[2] = ' '; tail[3] = '\0';

    /* The serial console gets plain text; the QEMU window gets the same text
       in Arch-style colours (green user@host, blue directory). */
    uart_putc_serial_only('[');
    uart_puts_serial_only(security_username());
    uart_puts_serial_only("@RioOS ");
    uart_puts_serial_only(cwd);
    uart_puts_serial_only(tail);

    console_write_attr("[", 2, 1);
    console_write_attr(security_username(), 2, 1);
    console_write_attr("@RioOS ", 2, 1);
    console_write_attr(cwd, 4, 1);
    console_write_attr("]", 2, 1);
    console_write_attr(tail + 1, security_prompt_char() == '#' ? 1 : 2, 1);
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

struct Cand { const char* name; int is_dir; };
static Cand cands[320];
static int ncand = 0;
static void cand_add(const char* n, int d) { if (ncand < 320 && n) { cands[ncand].name = n; cands[ncand].is_dir = d; ++ncand; } }
static void cand_words(const char* const* list, int n) { for (int i = 0; i < n; ++i) cand_add(list[i], 0); }

/* Finish the token `base` (already typed) from cands[]: unique -> complete it, several -> extend to the common prefix, else list them. */
static void apply_completion(char* input, int* length, const char* base)
{
    int blen = str_len(base);
    int m = 0; int first = -1;
    for (int i = 0; i < ncand; ++i) if (prefix_match(cands[i].name, base)) { if (m == 0) first = i; ++m; }
    if (m == 0) return;
    if (m == 1)
    {
        const char* name = cands[first].name;
        for (int i = blen; name[i] && *length < 254; ++i) input[(*length)++] = name[i];
        input[(*length)++] = cands[first].is_dir ? '/' : ' ';
        input[*length] = '\0';
        redraw_line(input, *length);
        return;
    }
    int common = str_len(cands[first].name);
    for (int i = 0; i < ncand; ++i)
    {
        if (!prefix_match(cands[i].name, base)) continue;
        int k = 0; while (k < common && cands[i].name[k] == cands[first].name[k] && cands[i].name[k]) ++k;
        common = k;
    }
    if (common > blen)
    {
        for (int i = blen; i < common && *length < 254; ++i) input[(*length)++] = cands[first].name[i];
        input[*length] = '\0';
        redraw_line(input, *length);
        return;
    }
    uart_puts("\r\n");
    for (int i = 0; i < ncand; ++i)
        if (prefix_match(cands[i].name, base)) { uart_puts(cands[i].name); if (cands[i].is_dir) uart_putc('/'); uart_puts("  "); }
    uart_puts("\r\n");
    redraw_line(input, *length);
}

static int word_is(const char* a, const char* b) { return str_equal(a, b); }

static void complete_command_or_file(char* input, int* length)
{
    int start = 0;
    int token_len = split_last_token(input, *length, &start);
    char prefix[128];
    if (token_len >= (int)sizeof(prefix)) return;
    for (int i = 0; i < token_len; ++i) prefix[i] = input[start + i];
    prefix[token_len] = '\0';

    /* Segment of the current simple command: after the last unquoted | ; & */
    int seg = 0; char q = 0;
    for (int i = 0; i < start; ++i)
    {
        char c = input[i];
        if (q) { if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == '|' || c == ';' || c == '&') seg = i + 1;
    }
    char argv0[24]; argv0[0] = 0; int argc_before = 0;
    {
        int i = seg;
        while (i < start)
        {
            while (i < start && (input[i] == ' ' || input[i] == '\t')) ++i;
            if (i >= start) break;
            int j = i; char tok[24]; int tn = 0;
            while (j < start && input[j] != ' ' && input[j] != '\t') { if (tn < 23) tok[tn++] = input[j]; ++j; }
            tok[tn] = 0;
            if (argc_before == 0) str_copy(argv0, tok, sizeof(argv0));
            ++argc_before; i = j;
        }
    }
    ncand = 0;

    if (argc_before == 0)                               /* command position */
    {
        cand_words(command_names, (int)(sizeof(command_names) / sizeof(command_names[0])));
        apply_completion(input, length, prefix);
        return;
    }
    if (prefix[0] == '$')                               /* environment variable names */
    {
        static char names[24][40];
        for (int i = 0; i < 24; ++i)
        {
            const char* n = env_name_at(i); if (!n) break;
            names[i][0] = '$'; str_copy(names[i] + 1, n, 38); cand_add(names[i], 0);
        }
        apply_completion(input, length, prefix);
        return;
    }
    int arg = argc_before;                              /* 1 = first argument */
    int files = 1, dirs_only = 0;
    static const char* const help_topics[] = { "files", "text", "shell", "system", "users", "storage", "network", "libc", "all" };
    static const char* const devs[] = { "ram0", "ram1", "nvme0n1", "sda" };
    static const char* const fw_sub[] = { "list", "add", "insert", "del", "flush", "default" };
    static const char* const dhcp_sub[] = { "status", "renew", "release" };
    static const char* const fat_sub[] = { "mkfs", "info", "ls", "cat", "get", "put", "mkdir", "rm" };
    static const char* const pkg_sub[] = { "list", "install", "remove", "info", "files", "search", "verify", "build", "update" };
    static const char* const tar_opt[] = { "-c", "-x", "-t", "-v", "-f", "-C" };
    if (word_is(argv0, "help") && arg == 1) { cand_words(help_topics, 9); files = 0; }
    else if ((word_is(argv0, "man") || word_is(argv0, "which")) && arg == 1) { cand_words(command_names, (int)(sizeof(command_names) / sizeof(command_names[0]))); files = 0; }
    else if (word_is(argv0, "mount") && arg <= 2 && arg == 1) { cand_words(devs, 4); static const char* const mo[] = { "-r", "-t", "-o" }; cand_words(mo, 3); }
    else if ((word_is(argv0, "mkfs")) && arg == 1) { cand_words(devs, 4); files = 0; }
    else if (word_is(argv0, "fat") && arg == 1) { cand_words(devs, 4); files = 0; }
    else if (word_is(argv0, "fat") && arg == 2) { cand_words(fat_sub, 8); files = 0; }
    else if (word_is(argv0, "fw") && arg == 1) { cand_words(fw_sub, 6); files = 0; }
    else if (word_is(argv0, "dhcp") && arg == 1) { cand_words(dhcp_sub, 3); files = 0; }
    else if (word_is(argv0, "pkg") && arg == 1) { cand_words(pkg_sub, 9); files = 0; }
    else if (word_is(argv0, "tar") && arg == 1) { cand_words(tar_opt, 6); }
    else if (word_is(argv0, "su") && arg == 1) { cand_add("root", 0); cand_add("user", 0); files = 0; }
    else if ((word_is(argv0, "export") || word_is(argv0, "unset")) && arg == 1)
    {
        static char names2[24][40];
        for (int i = 0; i < 24; ++i) { const char* n = env_name_at(i); if (!n) break; str_copy(names2[i], n, 38); cand_add(names2[i], 0); }
        files = 0;
    }
    else if (word_is(argv0, "cd") || word_is(argv0, "rmdir")) dirs_only = 1;
    if (files)
    {
        char parent_path[192]; char base[96]; int slash = -1;
        for (int i = token_len - 1; i >= 0; --i) if (prefix[i] == '/') { slash = i; break; }
        if (slash < 0) { str_copy(parent_path, ".", sizeof(parent_path)); str_copy(base, prefix, sizeof(base)); }
        else
        {
            for (int i = 0; i < slash && i < (int)sizeof(parent_path) - 1; ++i) parent_path[i] = prefix[i];
            parent_path[slash] = '\0';
            str_copy(base, prefix + slash + 1, sizeof(base));
            if (slash == 0) str_copy(parent_path, "/", sizeof(parent_path));
        }
        int dir = fs_resolve(parent_path[0] ? parent_path : ".", vfs_cwd());
        if (dir >= 0 && fs_get_type(dir) == FS_DIR && fs_check_access(dir, FS_ACCESS_READ | FS_ACCESS_EXEC) == 0)
        {
            for (int i = 0; i < fs_get_child_count(dir); ++i)
            {
                int id = fs_get_child(dir, i);
                if (id < 0) continue;
                int isd = fs_get_type(id) == FS_DIR;
                if (dirs_only && !isd) continue;
                cand_add(fs_get_name(id), isd);
            }
        }
        if (ncand == 0) return;
        apply_completion(input, length, base);     /* completes the part after the last '/' */
        return;
    }
    apply_completion(input, length, prefix);
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
    shellext_history_load(0);

    while (1)
    {
        jobs_poll();
        net_dhcp_maintain();
        shell_prompt_text();
        int length = 0;
        int history_cursor = history_count();

        while (1)
        {
            char c = 0;
            if (uart_has_data()) c = uart_getc();
            else if (keyboard_has_data()) c = keyboard_getc();
            else { console_flush(); interrupt_idle(); continue; }

            /* QEMU-window arrow keys arrive as single bytes (VirtIO / PS/2);
               serial terminals send ESC [ A / ESC [ B. Normalise both. */
            int arrow = 0;
            if ((unsigned char)c == 0x80U) arrow = 'A';
            else if ((unsigned char)c == 0x81U) arrow = 'B';
            if (c == '\x1B' || arrow != 0)
            {
                char seq[3];
                seq[0] = seq[1] = seq[2] = '\0';
                int have = 0;
                if (arrow != 0) { seq[0] = '['; seq[1] = (char)arrow; have = 2; }
                else have = read_escape_sequence(seq);
                if (have >= 2 && seq[0] == '[')
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
                }
                continue;
            }

            if (c >= 32 && c <= 126 && length < 255)
            {
                input[length++] = c;
                input[length] = '\0';
                uart_putc(c);
            }
        }

        execute_command(input);
        uart_puts("\r\n");
    }
}
