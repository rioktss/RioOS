#include "shellext.h"
#include "fs.h"
#include "vfs.h"
#include "memory.h"
#include "string.h"
#include "uart.h"
#include "keyboard.h"
#include "history.h"
#include "commands.h"

/* ---- aliases ---------------------------------------------------------------------------- */
#define ALIAS_MAX 16
static char alias_name[ALIAS_MAX][24];
static char alias_value[ALIAS_MAX][160];

int shellext_alias_expand(const char* line, char* out, int cap)
{
    int st = 0; while (line[st] == ' ') ++st;
    char w[24]; int n = 0;
    while (line[st + n] && line[st + n] != ' ' && n < 23) w[n] = line[st + n], ++n;
    w[n] = 0;
    if (line[st + n] && line[st + n] != ' ') return 0;      /* word longer than any alias */
    if (!n) return 0;
    for (int i = 0; i < ALIAS_MAX; ++i)
    {
        if (!alias_name[i][0] || !str_equal(alias_name[i], w)) continue;
        int p = 0;
        for (const char* v = alias_value[i]; *v && p < cap - 1; ++v) out[p++] = *v;
        for (const char* r = line + st + n; *r && p < cap - 1; ++r) out[p++] = *r;
        out[p] = 0;
        return 1;
    }
    return 0;
}

static void cmd_alias(char* arg)
{
    if (!arg || !arg[0])
    {
        int any = 0;
        for (int i = 0; i < ALIAS_MAX; ++i) if (alias_name[i][0]) { uart_puts("alias "); uart_puts(alias_name[i]); uart_puts("='"); uart_puts(alias_value[i]); uart_puts("'\r\n"); any = 1; }
        if (!any) uart_puts("(no aliases)\r\n");
        return;
    }
    int eq = -1; for (int i = 0; arg[i]; ++i) if (arg[i] == '=') { eq = i; break; }
    if (eq < 0)
    {
        for (int i = 0; i < ALIAS_MAX; ++i) if (alias_name[i][0] && str_equal(alias_name[i], arg)) { uart_puts("alias "); uart_puts(arg); uart_puts("='"); uart_puts(alias_value[i]); uart_puts("'\r\n"); return; }
        uart_puts("alias: "); uart_puts(arg); uart_puts(": not found\r\n"); return;
    }
    if (eq == 0 || eq > 23) { uart_puts("alias: usage: alias name=command\r\n"); return; }
    char nm[24]; for (int i = 0; i < eq; ++i) nm[i] = arg[i]; nm[eq] = 0;
    for (int i = 0; nm[i]; ++i) if (nm[i] == ' ' || nm[i] == '|' || nm[i] == ';' || nm[i] == '&') { uart_puts("alias: invalid name\r\n"); return; }
    char* v = arg + eq + 1; int vl = str_len(v);
    if (vl >= 2 && ((v[0] == '\'' && v[vl - 1] == '\'') || (v[0] == '"' && v[vl - 1] == '"'))) { v[vl - 1] = 0; ++v; }
    if (str_equal(nm, "alias") || str_equal(nm, "unalias")) { uart_puts("alias: cannot alias that name\r\n"); return; }
    int slot = -1;
    for (int i = 0; i < ALIAS_MAX; ++i) if (alias_name[i][0] && str_equal(alias_name[i], nm)) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < ALIAS_MAX; ++i) if (!alias_name[i][0]) { slot = i; break; }
    if (slot < 0) { uart_puts("alias: too many aliases\r\n"); return; }
    str_copy(alias_name[slot], nm, 24); str_copy(alias_value[slot], v, 160);
}

static void cmd_unalias(char* arg)
{
    if (!arg || !arg[0]) { uart_puts("usage: unalias <name> | unalias -a\r\n"); return; }
    if (str_equal(arg, "-a")) { for (int i = 0; i < ALIAS_MAX; ++i) alias_name[i][0] = 0; return; }
    for (int i = 0; i < ALIAS_MAX; ++i) if (alias_name[i][0] && str_equal(alias_name[i], arg)) { alias_name[i][0] = 0; return; }
    uart_puts("unalias: "); uart_puts(arg); uart_puts(": not found\r\n");
}

/* ---- printf ----------------------------------------------------------------------------- */
static int words(char* s, char** w, int cap)
{
    int n = 0;
    while (*s && n < cap)
    {
        while (*s == ' ' || *s == '\t') ++s;
        if (!*s) break;
        char* o = s; w[n++] = o; char q = 0;
        while (*s && (q || (*s != ' ' && *s != '\t')))
        {
            if (q) { if (*s == q) q = 0; else *o++ = *s; }
            else if (*s == '"' || *s == '\'') q = *s;
            else if (*s == '\\' && s[1]) { *o++ = *s++; *o++ = *s; }
            else *o++ = *s;
            ++s;
        }
        if (*s) ++s;
        *o = 0;
    }
    return n;
}
static long long to_ll(const char* s)
{
    int neg = 0; while (*s == ' ') ++s;
    if (*s == '-') { neg = 1; ++s; } else if (*s == '+') ++s;
    long long v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')) { v = v * 16 + (*s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10); ++s; } }
    else while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
static int is_num(const char* s)
{
    if (*s == '-' || *s == '+') ++s;
    if (!*s) return 0;
    for (; *s; ++s) if (*s < '0' || *s > '9') return 0;
    return 1;
}
static void put_ll(long long v)
{
    char t[24]; int n = 0; unsigned long long u = v < 0 ? (unsigned long long)(-v) : (unsigned long long)v;
    if (v < 0) uart_putc('-');
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) uart_putc(t[--n]);
}

static void cmd_printf(char* arg)
{
    char* w[24]; int n = arg ? words(arg, w, 24) : 0;
    if (n < 1) { uart_puts("usage: printf <format> [args...]   (%s %d %i %u %x %X %o %c %% \\n \\t \\\\)\r\n"); return; }
    const char* f = w[0]; int ai = 1;
    for (int pass = 0; pass < 8; ++pass)
    {
        int consumed = 0;
        for (const char* p = f; *p; ++p)
        {
            if (*p == '\\' && p[1])
            {
                ++p;
                if (*p == 'n') { uart_putc('\r'); uart_putc('\n'); } else if (*p == 't') uart_putc('\t');
                else if (*p == 'r') uart_putc('\r'); else uart_putc(*p);
                continue;
            }
            if (*p != '%') { uart_putc(*p); continue; }
            ++p;
            if (*p == '%') { uart_putc('%'); continue; }
            int left = 0, zero = 0, width = 0;
            for (;; ++p) { if (*p == '-') left = 1; else if (*p == '0') zero = 1; else break; }
            while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
            char conv = *p; if (!conv) break;
            const char* a = ai < n ? w[ai++] : ""; ++consumed;
            char t[80]; int tl = 0;
            if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o')
            {
                long long v = to_ll(a); unsigned base = (conv == 'x' || conv == 'X') ? 16 : conv == 'o' ? 8 : 10;
                int neg = 0; unsigned long long u;
                if ((conv == 'd' || conv == 'i') && v < 0) { neg = 1; u = (unsigned long long)(-v); } else u = (unsigned long long)v;
                char r[24]; int rn = 0;
                do { unsigned d = (unsigned)(u % base); r[rn++] = (char)(d < 10 ? '0' + d : (conv == 'X' ? 'A' : 'a') + d - 10); u /= base; } while (u);
                if (neg) t[tl++] = '-';
                while (rn) t[tl++] = r[--rn];
            }
            else if (conv == 'c') t[tl++] = a[0] ? a[0] : ' ';
            else if (conv == 's') { for (int i = 0; a[i] && tl < 78; ++i) t[tl++] = a[i]; }
            else { t[tl++] = '%'; t[tl++] = conv; }
            int pad = width > tl ? width - tl : 0;
            if (!left) for (int i = 0; i < pad; ++i) uart_putc((zero && conv != 's') ? '0' : ' ');
            for (int i = 0; i < tl; ++i) uart_putc(t[i]);
            if (left) for (int i = 0; i < pad; ++i) uart_putc(' ');
        }
        if (!consumed || ai >= n) break;        /* reuse the format while arguments remain */
    }
}

/* ---- expr ------------------------------------------------------------------------------- */
static void cmd_expr(char* arg)
{
    char* w[8]; int n = arg ? words(arg, w, 8) : 0;
    if (n == 2 && str_equal(w[0], "length")) { put_ll(str_len(w[1])); uart_puts("\r\n"); return; }
    if (n == 4 && str_equal(w[0], "substr"))
    {
        long long p = to_ll(w[2]), l = to_ll(w[3]); int sl = str_len(w[1]);
        for (long long i = p - 1; i >= 0 && i < sl && i < p - 1 + l; ++i) uart_putc(w[1][i]);
        uart_puts("\r\n"); return;
    }
    if (n == 1) { uart_puts(w[0]); uart_puts("\r\n"); return; }
    if (n != 3) { uart_puts("expr: usage: expr A op B | expr length S | expr substr S POS LEN   (op: + - * / % = != < > <= >=)\r\n"); return; }
    const char* op = w[1];
    if (op[0] == '\\' && op[1]) ++op;
    int numeric = is_num(w[0]) && is_num(w[2]);
    long long a = to_ll(w[0]), b = to_ll(w[2]);
    int cmp = 0; int is_cmp = 1;
    if (numeric) cmp = a < b ? -1 : a > b ? 1 : 0;
    else { const char* x = w[0]; const char* y = w[2]; while (*x && *x == *y) { ++x; ++y; } cmp = (unsigned char)*x < (unsigned char)*y ? -1 : (unsigned char)*x > (unsigned char)*y ? 1 : 0; }
    long long r = 0;
    if (str_equal(op, "=")) r = cmp == 0; else if (str_equal(op, "!=")) r = cmp != 0;
    else if (str_equal(op, "<")) r = cmp < 0; else if (str_equal(op, ">")) r = cmp > 0;
    else if (str_equal(op, "<=")) r = cmp <= 0; else if (str_equal(op, ">=")) r = cmp >= 0;
    else
    {
        is_cmp = 0;
        if (!numeric) { uart_puts("expr: non-numeric argument\r\n"); return; }
        if (str_equal(op, "+")) r = a + b; else if (str_equal(op, "-")) r = a - b; else if (str_equal(op, "*") || str_equal(op, "x")) r = a * b;
        else if (str_equal(op, "/") || str_equal(op, "%")) { if (b == 0) { uart_puts("expr: division by zero\r\n"); return; } r = op[0] == '/' ? a / b : a % b; }
        else { uart_puts("expr: unknown operator\r\n"); return; }
    }
    (void)is_cmp;
    put_ll(r); uart_puts("\r\n");
}

/* ---- more / less ------------------------------------------------------------------------ */
static char wait_key()
{
    for (;;)
    {
        if (uart_has_data()) return uart_getc();
        if (keyboard_has_data()) return keyboard_getc();
    }
}

static void cmd_more(const char* name, char* arg)
{
    char* w[4]; int n = arg ? words(arg, w, 4) : 0;
    if (n != 1) { uart_puts(name); uart_puts(": usage: "); uart_puts(name); uart_puts(" <file>   (space = next page, enter = next line, q = quit)\r\n"); return; }
    uint8_t* buf = (uint8_t*)kmalloc(131072);
    if (!buf) { uart_puts(name); uart_puts(": out of memory\r\n"); return; }
    uint64_t sz = 0;
    if (fs_read_file(w[0], vfs_cwd(), buf, 131071, &sz) != 0) { uart_puts(name); uart_puts(": cannot read file\r\n"); kfree(buf); return; }
    int lines = 0; const int PAGE = 20;
    uint64_t i = 0, total = sz;
    int quit = 0;
    while (i < total && !quit)
    {
        while (i < total)
        {
            char c = (char)buf[i++];
            if (c == '\n') { uart_putc('\r'); uart_putc('\n'); ++lines; if (lines >= PAGE) break; }
            else if (c != '\r') uart_putc(c);
        }
        if (i >= total) break;
        uart_puts("--More-- ("); put_ll((long long)(i * 100 / (total ? total : 1))); uart_puts("%) ");
        char k = wait_key();
        uart_puts("\r                    \r");
        if (k == 'q' || k == 'Q' || k == 3) quit = 1;
        else if (k == '\r' || k == '\n') lines = PAGE - 1;
        else lines = 0;
    }
    if (!quit && total && buf[total - 1] != '\n') uart_puts("\r\n");
    kfree(buf);
}

/* ---- history persistence ---------------------------------------------------------------- */
#define HIST_FILE "/home/user/.history"
static void cmd_history_save()
{
    static char b[32 * 260]; int p = 0;
    for (int i = 0; i < history_count(); ++i)
    { const char* h = history_get(i); for (int k = 0; h[k] && p < (int)sizeof(b) - 2; ++k) b[p++] = h[k]; b[p++] = '\n'; }
    int rc = fs_write_data(HIST_FILE, 0, (const uint8_t*)b, (uint64_t)p);
    uart_puts(rc >= 0 ? "history saved to " HIST_FILE "\r\n" : "history: cannot write " HIST_FILE "\r\n");
}
void shellext_history_load(int verbose)
{
    static uint8_t b[32 * 260]; uint64_t sz = 0;
    if (fs_read_file(HIST_FILE, 0, b, sizeof(b) - 1, &sz) != 0) { if (verbose) uart_puts("history: no saved history\r\n"); return; }
    char line[256]; int l = 0;
    for (uint64_t i = 0; i <= sz; ++i)
    {
        if (i == sz || b[i] == '\n') { line[l] = 0; if (l) history_add(line); l = 0; }
        else if (l < 255) line[l++] = (char)b[i];
    }
    if (verbose) uart_puts("history loaded\r\n");
}

int shellext_handle(const char* command, char* argument)
{
    if (str_equal(command, "alias")) { cmd_alias(argument); return 1; }
    if (str_equal(command, "unalias")) { cmd_unalias(argument); return 1; }
    if (str_equal(command, "printf")) { cmd_printf(argument); return 1; }
    if (str_equal(command, "expr")) { cmd_expr(argument); return 1; }
    if (str_equal(command, "more") || str_equal(command, "less")) { cmd_more(command, argument); return 1; }
    if (str_equal(command, "history") && argument)
    {
        if (str_equal(argument, "-w")) { cmd_history_save(); return 1; }
        if (str_equal(argument, "-r")) { shellext_history_load(1); return 1; }
    }
    return 0;
}
