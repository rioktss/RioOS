#include "filters.h"
#include "commands.h"
#include "fs.h"
#include "vfs.h"
#include "memory.h"
#include "string.h"
#include "uart.h"
#include "types.h"

#define FBUF 131072U
#define MAX_STAGES 8
#define MAX_LINES 4096

/* ---- output capture ---------------------------------------------------- */
static char* cap_buf = 0;
static uint32_t cap_len = 0;
static void cap_hook(char c)
{
    if (c == '\r' || !cap_buf) return;
    if (cap_len < FBUF - 1U) cap_buf[cap_len++] = c;
}

/* ---- tokenizer with quotes -------------------------------------------- */
static int tokenize(char* s, char** w, int cap)
{
    int n = 0;
    while (*s && n < cap)
    {
        while (*s == ' ' || *s == '\t') ++s;
        if (!*s) break;
        char* out = s; w[n++] = out;
        char q = 0;
        while (*s && (q || (*s != ' ' && *s != '\t')))
        {
            if (q) { if (*s == q) q = 0; else *out++ = *s; }
            else if (*s == '"' || *s == '\'') q = *s;
            else *out++ = *s;
            ++s;
        }
        if (*s) ++s;
        *out = 0;
    }
    return n;
}

static int to_int(const char* s, int fb)
{
    if (!s || !*s) return fb;
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; ++s; }
    if (*s < '0' || *s > '9') return fb;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; if (v > 100000000) break; }
    return neg ? -v : v;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int contains(const char* line, uint32_t len, const char* pat, int icase)
{
    uint32_t pl = (uint32_t)str_len(pat);
    if (pl == 0) return 1;
    for (uint32_t i = 0; i + pl <= len; ++i)
    {
        uint32_t k = 0;
        while (k < pl && (icase ? lower(line[i + k]) == lower(pat[k]) : line[i + k] == pat[k])) ++k;
        if (k == pl) return 1;
    }
    return 0;
}

static void put_num(char* out, uint32_t* o, uint64_t v)
{
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[(*o)++] = t[--n];
}

struct Lines { char** p; uint32_t* len; uint32_t n; };

static int split_lines(const char* in, uint32_t inlen, Lines* L)
{
    L->n = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= inlen; ++i)
    {
        if (i == inlen || in[i] == '\n')
        {
            if (i == inlen && start == inlen) break;
            if (L->n >= MAX_LINES) return -1;
            L->p[L->n] = (char*)in + start; L->len[L->n] = i - start; ++L->n;
            start = i + 1;
        }
    }
    return 0;
}

static int cmp_lines(const char* a, uint32_t al, const char* b, uint32_t bl, int numeric)
{
    if (numeric)
    {
        long x = 0, y = 0; int nx = 0, ny = 0; uint32_t i = 0;
        while (i < al && (a[i] == ' ')) ++i;
        if (i < al && a[i] == '-') { nx = 1; ++i; }
        for (; i < al && a[i] >= '0' && a[i] <= '9'; ++i) x = x * 10 + (a[i] - '0');
        i = 0; while (i < bl && b[i] == ' ') ++i;
        if (i < bl && b[i] == '-') { ny = 1; ++i; }
        for (; i < bl && b[i] >= '0' && b[i] <= '9'; ++i) y = y * 10 + (b[i] - '0');
        if (nx) x = -x; if (ny) y = -y;
        if (x != y) return x < y ? -1 : 1;
    }
    uint32_t n = al < bl ? al : bl;
    for (uint32_t i = 0; i < n; ++i) if (a[i] != b[i]) return (unsigned char)a[i] < (unsigned char)b[i] ? -1 : 1;
    return al == bl ? 0 : (al < bl ? -1 : 1);
}

static void emit_line(char* out, uint32_t* o, const char* l, uint32_t len)
{
    if (*o + len + 1 >= FBUF) return;
    for (uint32_t i = 0; i < len; ++i) out[(*o)++] = l[i];
    out[(*o)++] = '\n';
}

/* Apply one filter. Returns 0 ok, -1 unknown filter, -2 bad usage. */
static int apply(char** w, int n, const char* in, uint32_t inlen, char* out, uint32_t* outlen)
{
    uint32_t o = 0;
    char* name = w[0];
    static char* lp[MAX_LINES]; static uint32_t ll[MAX_LINES];
    Lines L = { lp, ll, 0 };

    if (str_equal(name, "grep"))
    {
        int inv = 0, num = 0, cnt = 0, ic = 0; const char* pat = 0;
        for (int i = 1; i < n; ++i)
        {
            if (w[i][0] == '-' && w[i][1])
            { for (int k = 1; w[i][k]; ++k) { char c = w[i][k]; if (c == 'v') inv = 1; else if (c == 'n') num = 1; else if (c == 'c') cnt = 1; else if (c == 'i') ic = 1; else return -2; } }
            else if (!pat) pat = w[i]; else return -2;
        }
        if (!pat) return -2;
        if (split_lines(in, inlen, &L) != 0) return -2;
        uint32_t matches = 0;
        for (uint32_t i = 0; i < L.n; ++i)
        {
            int m = contains(L.p[i], L.len[i], pat, ic); if (inv) m = !m;
            if (!m) continue;
            ++matches;
            if (cnt) continue;
            if (num) { put_num(out, &o, i + 1); out[o++] = ':'; }
            emit_line(out, &o, L.p[i], L.len[i]);
        }
        if (cnt) { put_num(out, &o, matches); out[o++] = '\n'; }
    }
    else if (str_equal(name, "wc"))
    {
        int fl = 0, fw = 0, fc = 0;
        for (int i = 1; i < n; ++i)
        { if (w[i][0] != '-') return -2; for (int k = 1; w[i][k]; ++k) { if (w[i][k] == 'l') fl = 1; else if (w[i][k] == 'w') fw = 1; else if (w[i][k] == 'c') fc = 1; else return -2; } }
        if (!fl && !fw && !fc) fl = fw = fc = 1;
        uint32_t lines = 0, words = 0; int inw = 0;
        for (uint32_t i = 0; i < inlen; ++i)
        {
            if (in[i] == '\n') ++lines;
            int sp = in[i] == ' ' || in[i] == '\t' || in[i] == '\n';
            if (sp) inw = 0; else if (!inw) { inw = 1; ++words; }
        }
        if (inlen && in[inlen - 1] != '\n') ++lines;
        int first = 1;
        if (fl) { put_num(out, &o, lines); first = 0; }
        if (fw) { if (!first) out[o++] = ' '; put_num(out, &o, words); first = 0; }
        if (fc) { if (!first) out[o++] = ' '; put_num(out, &o, inlen); }
        out[o++] = '\n';
    }
    else if (str_equal(name, "head") || str_equal(name, "tail"))
    {
        int count = 10;
        for (int i = 1; i < n; ++i)
        {
            if (str_equal(w[i], "-n") && i + 1 < n) { count = to_int(w[++i], -1); }
            else if (w[i][0] == '-' && w[i][1] >= '0' && w[i][1] <= '9') count = to_int(w[i] + 1, -1);
            else return -2;
        }
        if (count < 0) return -2;
        if (split_lines(in, inlen, &L) != 0) return -2;
        uint32_t a = 0, b = L.n;
        if (name[0] == 'h') { if ((uint32_t)count < b) b = (uint32_t)count; }
        else { if ((uint32_t)count < b) a = b - (uint32_t)count; }
        for (uint32_t i = a; i < b; ++i) emit_line(out, &o, L.p[i], L.len[i]);
    }
    else if (str_equal(name, "sort"))
    {
        int rev = 0, num = 0, uniq = 0;
        for (int i = 1; i < n; ++i)
        { if (w[i][0] != '-') return -2; for (int k = 1; w[i][k]; ++k) { if (w[i][k] == 'r') rev = 1; else if (w[i][k] == 'n') num = 1; else if (w[i][k] == 'u') uniq = 1; else return -2; } }
        if (split_lines(in, inlen, &L) != 0) return -2;
        /* shell sort on index permutation */
        for (uint32_t gap = L.n / 2; gap > 0; gap /= 2)
            for (uint32_t i = gap; i < L.n; ++i)
            {
                char* tp = L.p[i]; uint32_t tl = L.len[i]; uint32_t j = i;
                while (j >= gap)
                {
                    int c = cmp_lines(L.p[j - gap], L.len[j - gap], tp, tl, num);
                    if (rev) c = -c;
                    if (c <= 0) break;
                    L.p[j] = L.p[j - gap]; L.len[j] = L.len[j - gap]; j -= gap;
                }
                L.p[j] = tp; L.len[j] = tl;
            }
        for (uint32_t i = 0; i < L.n; ++i)
        {
            if (uniq && i > 0 && cmp_lines(L.p[i - 1], L.len[i - 1], L.p[i], L.len[i], 0) == 0) continue;
            emit_line(out, &o, L.p[i], L.len[i]);
        }
    }
    else if (str_equal(name, "uniq"))
    {
        int cnt = 0;
        for (int i = 1; i < n; ++i) { if (str_equal(w[i], "-c")) cnt = 1; else return -2; }
        if (split_lines(in, inlen, &L) != 0) return -2;
        for (uint32_t i = 0; i < L.n; )
        {
            uint32_t j = i + 1;
            while (j < L.n && cmp_lines(L.p[i], L.len[i], L.p[j], L.len[j], 0) == 0) ++j;
            if (cnt) { put_num(out, &o, j - i); out[o++] = ' '; }
            emit_line(out, &o, L.p[i], L.len[i]);
            i = j;
        }
    }
    else if (str_equal(name, "cut"))
    {
        char delim = '\t'; int field = 0;
        for (int i = 1; i < n; ++i)
        {
            if (str_equal(w[i], "-d") && i + 1 < n) delim = w[++i][0];
            else if (w[i][0] == '-' && w[i][1] == 'd' && w[i][2]) delim = w[i][2];
            else if (str_equal(w[i], "-f") && i + 1 < n) field = to_int(w[++i], 0);
            else if (w[i][0] == '-' && w[i][1] == 'f') field = to_int(w[i] + 2, 0);
            else return -2;
        }
        if (field < 1) return -2;
        if (split_lines(in, inlen, &L) != 0) return -2;
        for (uint32_t i = 0; i < L.n; ++i)
        {
            int f = 1; uint32_t s = 0, e = L.len[i]; int found = field == 1;
            for (uint32_t k = 0; k < L.len[i]; ++k)
                if (L.p[i][k] == delim) { if (f == field) { e = k; break; } ++f; s = k + 1; if (f == field) found = 1; }
            if (f < field) { e = s = 0; }
            (void)found;
            emit_line(out, &o, L.p[i] + s, e - s);
        }
    }
    else if (str_equal(name, "tr"))
    {
        if (n != 3 || str_len(w[1]) != str_len(w[2]) || str_len(w[1]) == 0) return -2;
        for (uint32_t i = 0; i < inlen; ++i)
        {
            char c = in[i];
            for (int k = 0; w[1][k]; ++k) if (c == w[1][k]) { c = w[2][k]; break; }
            if (o < FBUF - 1U) out[o++] = c;
        }
    }
    else if (str_equal(name, "tee"))
    {
        if (n != 2) return -2;
        if (fs_write_data(w[1], vfs_cwd(), (const uint8_t*)in, inlen) < 0) { uart_puts("tee: cannot write file\r\n"); }
        for (uint32_t i = 0; i < inlen; ++i) out[o++] = in[i];
    }
    else return -1;
    *outlen = o;
    return 0;
}

int filters_is_filter(const char* name)
{
    static const char* f[] = {"grep", "wc", "head", "tail", "sort", "uniq", "cut", "tr", "tee"};
    for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); ++i) if (str_equal(name, f[i])) return 1;
    return 0;
}

static void print_buf(const char* b, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) { if (b[i] == '\n') uart_putc('\r'); uart_putc(b[i]); }
}

/* head/tail/sort/uniq/cut/tr/tee/grep/wc with a trailing FILE operand. */
int filters_run_file_command(const char* name, const char* args)
{
    if (!filters_is_filter(name) || str_equal(name, "grep") || str_equal(name, "wc") || str_equal(name, "tee")) return 0;   /* grep/wc/tee keep their dedicated implementations */
    char* in = (char*)kmalloc(FBUF); char* out = (char*)kmalloc(FBUF); char* line = (char*)kmalloc(512);
    if (!in || !out || !line) { if (in) kfree(in); if (out) kfree(out); if (line) kfree(line); return 1; }
    str_copy(line, name, 64);
    int p = str_len(line); line[p++] = ' ';
    str_copy(line + p, args ? args : "", 400);
    char* w[12]; int n = tokenize(line, w, 12);
    if (str_equal(name, "tr") ) { uart_puts("tr: use as a pipeline stage: <cmd> | tr a b\r\n"); kfree(in); kfree(out); kfree(line); return 1; }
    if (n < 2) { uart_puts(name); uart_puts(": missing file operand\r\n"); kfree(in); kfree(out); kfree(line); return 1; }
    const char* file = w[n - 1];
    uint64_t sz = 0;
    if (fs_read_file(file, vfs_cwd(), (uint8_t*)in, FBUF - 1U, &sz) != 0) { uart_puts(name); uart_puts(": cannot read file\r\n"); kfree(in); kfree(out); kfree(line); return 1; }
    uint32_t ol = 0;
    int rc = apply(w, n - 1, in, (uint32_t)sz, out, &ol);
    if (rc == -2) { uart_puts(name); uart_puts(": invalid usage\r\n"); }
    else if (rc == 0) print_buf(out, ol);
    kfree(in); kfree(out); kfree(line);
    return 1;
}

int filters_handle_pipeline(const char* text)
{
    /* find unquoted single '|' (not '||') */
    int has = 0; char q = 0;
    for (const char* p = text; *p; ++p)
    {
        if (q) { if (*p == q) q = 0; continue; }
        if (*p == '"' || *p == '\'') { q = *p; continue; }
        if (*p == '|') { if (p[1] == '|') { ++p; continue; } has = 1; break; }
    }
    if (!has) return 0;
    char* line = (char*)kmalloc(1024);
    if (!line) return 0;
    str_copy(line, text, 1024);
    char* stage[MAX_STAGES]; int ns = 0; q = 0;
    stage[ns++] = line;
    for (char* p = line; *p; ++p)
    {
        if (q) { if (*p == q) q = 0; continue; }
        if (*p == '"' || *p == '\'') { q = *p; continue; }
        if (*p == '|') { if (p[1] == '|') { ++p; continue; } *p = 0; if (ns >= MAX_STAGES) { kfree(line); return 0; } stage[ns++] = p + 1; }
    }
    /* every stage after the first must be a filter; otherwise leave it to the legacy handlers */
    for (int i = 1; i < ns; ++i)
    {
        char* s = stage[i]; while (*s == ' ') ++s;
        char nm[16]; int k = 0; while (s[k] && s[k] != ' ' && k < 15) { nm[k] = s[k]; ++k; } nm[k] = 0;
        if (!filters_is_filter(nm)) { kfree(line); return 0; }
    }
    char* a = (char*)kmalloc(FBUF); char* b = (char*)kmalloc(FBUF);
    if (!a || !b) { if (a) kfree(a); if (b) kfree(b); kfree(line); return 0; }
    /* stage 0: run the command with output captured */
    cap_buf = a; cap_len = 0;
    uart_set_output_hook(cap_hook, 0);
    {
        char cmd[512]; str_copy(cmd, stage[0], sizeof(cmd));
        execute_command(cmd);
    }
    uart_clear_output_hook();
    uint32_t len = cap_len;
    char* in = a; char* out = b;
    int failed = 0;
    for (int i = 1; i < ns && !failed; ++i)
    {
        char* fl = (char*)kmalloc(512);
        if (!fl) { failed = 1; break; }
        str_copy(fl, stage[i], 512);
        char* w[12]; int n = tokenize(fl, w, 12);
        uint32_t ol = 0;
        int rc = n > 0 ? apply(w, n, in, len, out, &ol) : -2;
        if (rc != 0) { uart_puts(n > 0 ? w[0] : "pipe"); uart_puts(": invalid usage in pipeline\r\n"); failed = 1; }
        else { len = ol; char* t = in; in = out; out = t; }
        kfree(fl);
    }
    if (!failed) print_buf(in, len);
    kfree(a); kfree(b); kfree(line);
    cap_buf = 0;
    return 1;
}
