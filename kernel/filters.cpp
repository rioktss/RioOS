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
struct CapFrame { char* buf; uint32_t len; uint32_t cap; };
static CapFrame cap_stack[6];
static int cap_depth = 0;
static void cap_hook(char c)
{
    if (c == '\r' || cap_depth <= 0) return;
    CapFrame* f = &cap_stack[cap_depth - 1];
    if (f->len < f->cap - 1U) f->buf[f->len++] = c;
}
static int cap_push(char* buf, uint32_t cap)
{
    if (cap_depth >= 6) return -1;
    cap_stack[cap_depth].buf = buf; cap_stack[cap_depth].len = 0; cap_stack[cap_depth].cap = cap;
    ++cap_depth;
    uart_set_output_hook(cap_hook, 0);
    return 0;
}
static uint32_t cap_pop()
{
    uint32_t n = cap_stack[--cap_depth].len;
    if (cap_depth == 0) uart_clear_output_hook();
    return n;
}

/* Run a command line and capture everything it prints (CR removed, NUL-terminated). */
int filters_capture(const char* cmd, char* out, int cap)
{
    if (!out || cap < 2) return -1;
    if (cap_push(out, (uint32_t)cap) != 0) { out[0] = 0; return -1; }
    char line[512]; str_copy(line, cmd, sizeof(line));
    execute_command(line);
    uint32_t n = cap_pop();
    out[n] = 0;
    return (int)n;
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

static int str_cmp_local(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
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
    if (cap_push(a, FBUF) != 0) { kfree(a); kfree(b); kfree(line); return 0; }
    {
        char cmd[512]; str_copy(cmd, stage[0], sizeof(cmd));
        execute_command(cmd);
    }
    uint32_t len = cap_pop();
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
    return 1;
}

/* ---- redirection: cmd > f, cmd >> f, cmd 2> f, cmd 2>> f, cmd &> f, 2>&1 ------------------ */
static int write_target(const char* path, const char* data, uint32_t len, int append)
{
    if (str_equal(path, "/dev/null")) return 0;
    uint32_t total = len;
    char* buf = (char*)kmalloc(FBUF + 16U);
    if (!buf) return -1;
    uint32_t old = 0;
    if (append)
    {
        uint64_t sz = 0;
        if (fs_read_file(path, vfs_cwd(), (uint8_t*)buf, FBUF - 1U, &sz) == 0) old = (uint32_t)sz;
    }
    if (old + len > FBUF) { kfree(buf); return -2; }
    for (uint32_t i = 0; i < len; ++i) buf[old + i] = data[i];
    total = old + len;
    int rc = fs_write_data(path, vfs_cwd(), (const uint8_t*)buf, total);
    kfree(buf);
    return rc < 0 ? rc : 0;
}

static int is_error_line(const char* l, uint32_t n, const char* cmdname)
{
    uint32_t cl = (uint32_t)str_len(cmdname);
    if (cl > 0 && n > cl && l[cl] == ':') { uint32_t i = 0; while (i < cl && l[i] == cmdname[i]) ++i; if (i == cl) return 1; }
    const char* unk = "Unknown command:";
    uint32_t i = 0; while (unk[i] && i < n && l[i] == unk[i]) ++i;
    return unk[i] == 0;
}

int filters_handle_redirect(const char* text)
{
    char* line = (char*)kmalloc(1024);
    if (!line) return 0;
    str_copy(line, text, 1024);
    char* w = line; while (*w == ' ') ++w;
    if ((w[0] == 'e' && w[1] == 'c' && w[2] == 'h' && w[3] == 'o' && (w[4] == ' ' || w[4] == 0))) { kfree(line); return 0; }  /* legacy echo handler */
    char out_t[160] = "", err_t[160] = ""; int out_app = 0, err_app = 0, merge = 0, found = 0;
    char cmd[512]; int cn = 0; char q = 0;
    for (int i = 0; line[i]; )
    {
        char c = line[i];
        if (q) { if (c == q) q = 0; if (cn < 510) cmd[cn++] = c; ++i; continue; }
        if (c == '"' || c == '\'') { q = c; if (cn < 510) cmd[cn++] = c; ++i; continue; }
        int fd = 1, j = i;
        if (c == '2' && line[i + 1] == '>' && (i == 0 || line[i - 1] == ' ')) { fd = 2; j = i + 1; }
        else if (c == '&' && line[i + 1] == '>') { fd = 3; j = i + 1; }
        else if (c != '>') { if (cn < 510) cmd[cn++] = c; ++i; continue; }
        /* j points at '>' */
        int app = line[j + 1] == '>';
        int k = j + 1 + (app ? 1 : 0);
        if (line[k] == '&' && line[k + 1] == '1') { merge = 1; found = 1; i = k + 2; continue; }
        while (line[k] == ' ') ++k;
        char tgt[160]; int t = 0;
        while (line[k] && line[k] != ' ' && line[k] != '\t' && t < 159) tgt[t++] = line[k++];
        tgt[t] = 0;
        if (!tgt[0]) { uart_puts("shell: redirection target missing\r\n"); kfree(line); return 1; }
        found = 1;
        if (fd == 2) { str_copy(err_t, tgt, sizeof(err_t)); err_app = app; }
        else { str_copy(out_t, tgt, sizeof(out_t)); out_app = app; if (fd == 3) { str_copy(err_t, tgt, sizeof(err_t)); err_app = app; merge = 1; } }
        i = k;
    }
    if (!found) { kfree(line); return 0; }
    while (cn > 0 && cmd[cn - 1] == ' ') --cn;
    cmd[cn] = 0;
    char* buf = (char*)kmalloc(FBUF);
    if (!buf) { kfree(line); return 0; }
    int n = filters_capture(cmd, buf, (int)FBUF);
    if (n < 0) n = 0;
    char name[32]; int nl = 0; { char* c = cmd; while (*c == ' ') ++c; while (c[nl] && c[nl] != ' ' && nl < 31) { name[nl] = c[nl]; ++nl; } name[nl] = 0; }
    char* so = (char*)kmalloc(FBUF); char* se = (char*)kmalloc(FBUF);
    if (!so || !se) { if (so) kfree(so); if (se) kfree(se); kfree(buf); kfree(line); return 1; }
    uint32_t ol = 0, el = 0;
    for (int i = 0; i < n; )
    {
        int e = i; while (e < n && buf[e] != '\n') ++e;
        int len = e - i; int has_nl = e < n;
        int err = is_error_line(buf + i, (uint32_t)len, name);
        char* dst = err && !merge ? se : so; uint32_t* dl = err && !merge ? &el : &ol;
        for (int k = 0; k < len; ++k) dst[(*dl)++] = buf[i + k];
        if (has_nl) dst[(*dl)++] = '\n';
        i = e + 1;
    }
    int rc = 0;
    if (out_t[0]) { if (write_target(out_t, so, ol, out_app) != 0) { uart_puts("redirect: cannot write "); uart_puts(out_t); uart_puts("\r\n"); rc = 1; } }
    else print_buf(so, ol);
    if (err_t[0] && !merge) { if (write_target(err_t, se, el, err_app) != 0) { uart_puts("redirect: cannot write "); uart_puts(err_t); uart_puts("\r\n"); rc = 1; } }
    else if (!merge) print_buf(se, el);
    (void)rc;
    kfree(so); kfree(se); kfree(buf); kfree(line);
    return 1;
}

/* ---- wildcard expansion (* ?) in unquoted words after the command name ------------------ */
static int wild_match(const char* p, const char* s)
{
    while (*p)
    {
        if (*p == '*')
        {
            while (*p == '*') ++p;
            if (!*p) return 1;
            for (; *s; ++s) if (wild_match(p, s)) return 1;
            return 0;
        }
        if (!*s) return 0;
        if (*p != '?' && *p != *s) return 0;
        ++p; ++s;
    }
    return *s == 0;
}

int filters_glob(const char* in, char* out, int cap)
{
    int op = 0, i = 0, first = 1;
    while (in[i] && op < cap - 1)
    {
        if (in[i] == ' ' || in[i] == '\t') { out[op++] = in[i++]; continue; }
        int start = i; char q = 0; int quoted = 0, wild = 0;
        while (in[i] && (q || (in[i] != ' ' && in[i] != '\t')))
        {
            if (q) { if (in[i] == q) q = 0; }
            else if (in[i] == '"' || in[i] == '\'') { q = in[i]; quoted = 1; }
            else if (in[i] == '*' || in[i] == '?') wild = 1;
            ++i;
        }
        int wl = i - start;
        int expanded = 0;
        if (!first && wild && !quoted && wl < 150 && in[start] != '-')
        {
            char word[160]; for (int k = 0; k < wl; ++k) word[k] = in[start + k]; word[wl] = 0;
            int slash = -1; for (int k = 0; k < wl; ++k) if (word[k] == '/') slash = k;
            char dir[160]; const char* pat;
            if (slash < 0) { dir[0] = '.'; dir[1] = 0; pat = word; }
            else { for (int k = 0; k < slash; ++k) dir[k] = word[k]; dir[slash] = 0; if (slash == 0) { dir[0] = '/'; dir[1] = 0; } pat = word + slash + 1; }
            int d = fs_resolve(dir, vfs_cwd());
            if (d >= 0 && fs_get_type(d) == FS_DIR && fs_check_access(d, FS_ACCESS_READ | FS_ACCESS_EXEC) == 0)
            {
                const char* names[64]; int nm = 0;
                int cnt = fs_get_child_count(d);
                for (int c = 0; c < cnt && nm < 64; ++c)
                {
                    int id = fs_get_child(d, c);
                    if (id < 0) continue;
                    const char* name = fs_get_name(id);
                    if (name[0] == '.' && pat[0] != '.') continue;
                    if (wild_match(pat, name)) names[nm++] = name;
                }
                for (int a = 1; a < nm; ++a) { const char* t = names[a]; int b = a; while (b > 0 && str_cmp_local(names[b - 1], t) > 0) { names[b] = names[b - 1]; --b; } names[b] = t; }
                if (nm > 0)
                {
                    for (int m = 0; m < nm; ++m)
                    {
                        if (m) { if (op < cap - 1) out[op++] = ' '; }
                        if (slash >= 0) for (int k = 0; k <= slash && op < cap - 1; ++k) out[op++] = word[k];
                        for (int k = 0; names[m][k] && op < cap - 1; ++k) out[op++] = names[m][k];
                    }
                    expanded = 1;
                }
            }
        }
        if (!expanded) for (int k = 0; k < wl && op < cap - 1; ++k) out[op++] = in[start + k];
        first = 0;
    }
    out[op] = 0;
    return op;
}
