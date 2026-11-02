#include "textutil.h"
#include "string.h"

/*
 * Text utilities for the RioOS shell. No heap, no libc: fixed static scratch
 * arrays bound the work, and every limit is reported as an error instead of
 * silently truncating.
 */

#define TU_MAX_LINES 4096

static uint32_t g_start[TU_MAX_LINES];
static uint32_t g_len[TU_MAX_LINES];
static uint32_t g_order[TU_MAX_LINES];
static uint32_t g_tmp[TU_MAX_LINES];

/* ------------------------------------------------------------------ output */

struct Out
{
    uint8_t* buf;
    uint64_t cap;
    uint64_t n;
    int over;
};

static void out_byte(Out* o, uint8_t c)
{
    if (o->n < o->cap) o->buf[o->n++] = c;
    else o->over = 1;
}

static void out_bytes(Out* o, const uint8_t* s, uint64_t len)
{
    for (uint64_t i = 0; i < len; ++i) out_byte(o, s[i]);
}

static void out_line(Out* o, const uint8_t* s, uint64_t len)
{
    out_bytes(o, s, len);
    out_byte(o, (uint8_t)'\n');
}

static void out_u64_padded(Out* o, uint64_t v, int width)
{
    char tmp[24];
    int p = 0;
    if (v == 0) tmp[p++] = '0';
    while (v > 0 && p < 23) { tmp[p++] = (char)('0' + (v % 10)); v /= 10; }
    for (int pad = p; pad < width; ++pad) out_byte(o, (uint8_t)' ');
    while (p > 0) out_byte(o, (uint8_t)tmp[--p]);
}

/* ------------------------------------------------------------------- lines */

/* Returns the line count, or -1 when the input has too many lines. */
static int split_lines(const uint8_t* in, uint64_t len)
{
    int n = 0;
    uint64_t i = 0;
    while (i < len)
    {
        uint64_t j = i;
        while (j < len && in[j] != (uint8_t)'\n') ++j;
        if (n >= TU_MAX_LINES) return -1;
        g_start[n] = (uint32_t)i;
        uint64_t l = j - i;
        if (l > 0 && in[i + l - 1] == (uint8_t)'\r') --l; /* tolerate CRLF */
        g_len[n] = (uint32_t)l;
        ++n;
        i = j + 1;
    }
    return n;
}

/* ----------------------------------------------------------------- parsing */

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int parse_u(const char* s, uint64_t* v)
{
    if (!s || !is_digit(s[0])) return -1;
    uint64_t r = 0;
    for (int i = 0; s[i]; ++i)
    {
        if (!is_digit(s[i])) return -1;
        r = r * 10 + (uint64_t)(s[i] - '0');
        if (r > 100000000ULL) return -1;
    }
    *v = r;
    return 0;
}

/* Parse a leading signed integer for `sort -n`. Non-numeric lines count as 0. */
static int64_t leading_number(const uint8_t* s, uint32_t len)
{
    uint32_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) ++i;
    int neg = 0;
    if (i < len && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); ++i; }
    int64_t v = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9')
    {
        v = v * 10 + (s[i] - '0');
        if (v > 1000000000000000LL) break;
        ++i;
    }
    return neg ? -v : v;
}

/* --------------------------------------------------------------- name table */

struct FilterInfo { const char* name; const char* usage; };

static const FilterInfo filters[] = {
    { "head", "head [-n N] [file]" },
    { "tail", "tail [-n N] [file]" },
    { "sort", "sort [-r] [-n] [-u] [file]" },
    { "uniq", "uniq [-c] [-d] [file]" },
    { "rev",  "rev [file]" },
    { "tac",  "tac [file]" },
    { "nl",   "nl [file]" },
    { "cut",  "cut (-d C -f LIST | -c LIST) [file]" },
    { "tr",   "tr [-d] SET1 [SET2] [file]" },
    { "grep", "grep [-i] [-v] [-n] [-c] PATTERN [file]" },
    { "wc",   "wc [-l] [-w] [-c] [file]" },
};

int textutil_is_filter(const char* name)
{
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof(filters) / sizeof(filters[0]); ++i)
        if (str_equal(filters[i].name, name)) return 1;
    return 0;
}

const char* textutil_usage(const char* name)
{
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof(filters) / sizeof(filters[0]); ++i)
        if (str_equal(filters[i].name, name)) return filters[i].usage;
    return 0;
}

/* ----------------------------------------------------------------- filters */

/* Reads "-n N", "-nN" or "-N". Returns 0 and advances *idx on success. */
static int parse_count_option(int argc, char** argv, int* idx, uint64_t* count)
{
    const char* a = argv[*idx];
    if (a[0] != '-' || a[1] == '\0') return 1;           /* not an option */
    if (a[1] == 'n')
    {
        if (a[2] != '\0')
        {
            if (parse_u(a + 2, count) != 0) return -1;
            ++*idx;
            return 0;
        }
        if (*idx + 1 >= argc || parse_u(argv[*idx + 1], count) != 0) return -1;
        *idx += 2;
        return 0;
    }
    if (is_digit(a[1]))
    {
        if (parse_u(a + 1, count) != 0) return -1;
        ++*idx;
        return 0;
    }
    return -1;
}

static int do_head_tail(int is_tail, int argc, char** argv,
                        const uint8_t* in, uint64_t in_len, Out* o)
{
    uint64_t count = 10;
    int idx = 1;
    while (idx < argc)
    {
        int rc = parse_count_option(argc, argv, &idx, &count);
        if (rc < 0) return TEXTUTIL_ERR_USAGE;
        if (rc > 0) break;
    }
    if (idx < argc) return TEXTUTIL_ERR_USAGE;           /* stray operand */

    int n = split_lines(in, in_len);
    if (n < 0) return TEXTUTIL_ERR_OVERFLOW;
    int first = 0, last = n;
    if (!is_tail) { if (count < (uint64_t)n) last = (int)count; }
    else          { if (count < (uint64_t)n) first = n - (int)count; }
    for (int i = first; i < last; ++i) out_line(o, in + g_start[i], g_len[i]);
    return TEXTUTIL_OK;
}

static int cmp_bytes(const uint8_t* a, uint32_t al, const uint8_t* b, uint32_t bl)
{
    uint32_t m = al < bl ? al : bl;
    for (uint32_t i = 0; i < m; ++i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    if (al == bl) return 0;
    return al < bl ? -1 : 1;
}

struct SortCtx { const uint8_t* in; int numeric; int reverse; };

static int sort_cmp(const SortCtx* c, uint32_t x, uint32_t y)
{
    int r;
    if (c->numeric)
    {
        int64_t nx = leading_number(c->in + g_start[x], g_len[x]);
        int64_t ny = leading_number(c->in + g_start[y], g_len[y]);
        r = nx < ny ? -1 : (nx > ny ? 1 : 0);
        if (r == 0) r = cmp_bytes(c->in + g_start[x], g_len[x], c->in + g_start[y], g_len[y]);
    }
    else
        r = cmp_bytes(c->in + g_start[x], g_len[x], c->in + g_start[y], g_len[y]);
    return c->reverse ? -r : r;
}

/* Stable bottom-up merge sort over g_order using g_tmp as scratch. */
static void merge_sort(const SortCtx* c, int n)
{
    for (int i = 0; i < n; ++i) g_order[i] = (uint32_t)i;
    for (int width = 1; width < n; width *= 2)
    {
        int k = 0;
        for (int lo = 0; lo < n; lo += 2 * width)
        {
            int mid = lo + width; if (mid > n) mid = n;
            int hi = lo + 2 * width; if (hi > n) hi = n;
            int a = lo, b = mid;
            while (a < mid && b < hi)
            {
                if (sort_cmp(c, g_order[b], g_order[a]) < 0) g_tmp[k++] = g_order[b++];
                else g_tmp[k++] = g_order[a++];
            }
            while (a < mid) g_tmp[k++] = g_order[a++];
            while (b < hi) g_tmp[k++] = g_order[b++];
        }
        for (int i = 0; i < n; ++i) g_order[i] = g_tmp[i];
    }
}

static int do_sort(int argc, char** argv, const uint8_t* in, uint64_t in_len, Out* o)
{
    SortCtx c; c.in = in; c.numeric = 0; c.reverse = 0;
    int unique = 0;
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (a[0] != '-' || a[1] == '\0') return TEXTUTIL_ERR_USAGE;
        for (int k = 1; a[k]; ++k)
        {
            if (a[k] == 'r') c.reverse = 1;
            else if (a[k] == 'n') c.numeric = 1;
            else if (a[k] == 'u') unique = 1;
            else return TEXTUTIL_ERR_USAGE;
        }
    }
    int n = split_lines(in, in_len);
    if (n < 0) return TEXTUTIL_ERR_OVERFLOW;
    merge_sort(&c, n);
    for (int i = 0; i < n; ++i)
    {
        uint32_t cur = g_order[i];
        if (unique && i > 0)
        {
            uint32_t prev = g_order[i - 1];
            if (cmp_bytes(in + g_start[prev], g_len[prev], in + g_start[cur], g_len[cur]) == 0)
                continue;
        }
        out_line(o, in + g_start[cur], g_len[cur]);
    }
    return TEXTUTIL_OK;
}

static int do_uniq(int argc, char** argv, const uint8_t* in, uint64_t in_len, Out* o)
{
    int count = 0, dups = 0;
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (a[0] != '-' || a[1] == '\0') return TEXTUTIL_ERR_USAGE;
        for (int k = 1; a[k]; ++k)
        {
            if (a[k] == 'c') count = 1;
            else if (a[k] == 'd') dups = 1;
            else return TEXTUTIL_ERR_USAGE;
        }
    }
    int n = split_lines(in, in_len);
    if (n < 0) return TEXTUTIL_ERR_OVERFLOW;
    int i = 0;
    while (i < n)
    {
        int j = i + 1;
        while (j < n && cmp_bytes(in + g_start[i], g_len[i], in + g_start[j], g_len[j]) == 0) ++j;
        int run = j - i;
        if (!dups || run > 1)
        {
            if (count) { out_u64_padded(o, (uint64_t)run, 4); out_byte(o, (uint8_t)' '); }
            out_line(o, in + g_start[i], g_len[i]);
        }
        i = j;
    }
    return TEXTUTIL_OK;
}

static int do_rev_tac_nl(int mode, int argc, const uint8_t* in, uint64_t in_len, Out* o)
{
    if (argc > 1) return TEXTUTIL_ERR_USAGE;
    int n = split_lines(in, in_len);
    if (n < 0) return TEXTUTIL_ERR_OVERFLOW;
    if (mode == 0)       /* rev */
    {
        for (int i = 0; i < n; ++i)
        {
            for (uint32_t k = g_len[i]; k > 0; --k) out_byte(o, in[g_start[i] + k - 1]);
            out_byte(o, (uint8_t)'\n');
        }
    }
    else if (mode == 1)  /* tac */
    {
        for (int i = n - 1; i >= 0; --i) out_line(o, in + g_start[i], g_len[i]);
    }
    else                 /* nl */
    {
        for (int i = 0; i < n; ++i)
        {
            out_u64_padded(o, (uint64_t)(i + 1), 6);
            out_byte(o, (uint8_t)' '); out_byte(o, (uint8_t)' ');
            out_line(o, in + g_start[i], g_len[i]);
        }
    }
    return TEXTUTIL_OK;
}

/* LIST syntax: N, N-M, N-, -M separated by commas; 1-based, up to 8 ranges. */
struct Range { uint32_t lo, hi; };
static int parse_list(const char* s, Range* r, int cap)
{
    int n = 0;
    int i = 0;
    while (s[i])
    {
        if (n >= cap) return -1;
        uint64_t a = 0, b = 0;
        int have_a = 0, have_b = 0, dash = 0;
        if (is_digit(s[i]))
        {
            while (is_digit(s[i])) { a = a * 10 + (uint64_t)(s[i] - '0'); if (a > 100000) return -1; ++i; }
            have_a = 1;
        }
        if (s[i] == '-')
        {
            dash = 1; ++i;
            if (is_digit(s[i]))
            {
                while (is_digit(s[i])) { b = b * 10 + (uint64_t)(s[i] - '0'); if (b > 100000) return -1; ++i; }
                have_b = 1;
            }
        }
        if (!have_a && !have_b) return -1;
        if (!dash) { b = a; have_b = 1; }
        if (have_a && a == 0) return -1;
        r[n].lo = have_a ? (uint32_t)a : 1;
        r[n].hi = have_b ? (uint32_t)b : 0xFFFFFFFFu;
        if (r[n].hi < r[n].lo) return -1;
        ++n;
        if (s[i] == ',') ++i;
        else if (s[i]) return -1;
    }
    return n;
}

static int in_ranges(const Range* r, int n, uint32_t pos)
{
    for (int i = 0; i < n; ++i) if (pos >= r[i].lo && pos <= r[i].hi) return 1;
    return 0;
}

static int do_cut(int argc, char** argv, const uint8_t* in, uint64_t in_len, Out* o)
{
    char delim = '\t';
    const char* flist = 0;
    const char* clist = 0;
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (a[0] != '-' || a[1] == '\0') return TEXTUTIL_ERR_USAGE;
        const char* val = a[2] ? a + 2 : 0;
        if (a[1] == 'd' || a[1] == 'f' || a[1] == 'c')
        {
            if (!val) { if (i + 1 >= argc) return TEXTUTIL_ERR_USAGE; val = argv[++i]; }
            if (a[1] == 'd') { if (val[0] == '\0' || val[1] != '\0') return TEXTUTIL_ERR_USAGE; delim = val[0]; }
            else if (a[1] == 'f') flist = val;
            else clist = val;
        }
        else return TEXTUTIL_ERR_USAGE;
    }
    if ((flist == 0) == (clist == 0)) return TEXTUTIL_ERR_USAGE;  /* exactly one */
    Range r[8];
    int rn = parse_list(flist ? flist : clist, r, 8);
    if (rn <= 0) return TEXTUTIL_ERR_USAGE;

    int n = split_lines(in, in_len);
    if (n < 0) return TEXTUTIL_ERR_OVERFLOW;
    for (int li = 0; li < n; ++li)
    {
        const uint8_t* s = in + g_start[li];
        uint32_t len = g_len[li];
        if (clist)
        {
            for (uint32_t k = 0; k < len; ++k) if (in_ranges(r, rn, k + 1)) out_byte(o, s[k]);
            out_byte(o, (uint8_t)'\n');
            continue;
        }
        /* -f: lines with no delimiter are passed through unchanged */
        int has = 0;
        for (uint32_t k = 0; k < len; ++k) if (s[k] == (uint8_t)delim) { has = 1; break; }
        if (!has) { out_line(o, s, len); continue; }
        uint32_t field = 1, fstart = 0;
        int first_out = 1;
        for (uint32_t k = 0; k <= len; ++k)
        {
            if (k == len || s[k] == (uint8_t)delim)
            {
                if (in_ranges(r, rn, field))
                {
                    if (!first_out) out_byte(o, (uint8_t)delim);
                    out_bytes(o, s + fstart, k - fstart);
                    first_out = 0;
                }
                ++field; fstart = k + 1;
            }
        }
        out_byte(o, (uint8_t)'\n');
    }
    return TEXTUTIL_OK;
}

/* Expand a-z style ranges into out[]; returns length or -1. */
static int expand_set(const char* s, uint8_t* out, int cap)
{
    int n = 0;
    for (int i = 0; s[i]; ++i)
    {
        uint8_t c = (uint8_t)s[i];
        if (s[i + 1] == '-' && s[i + 2] && (uint8_t)s[i + 2] >= c)
        {
            for (int v = c; v <= (uint8_t)s[i + 2]; ++v)
            { if (n >= cap) return -1; out[n++] = (uint8_t)v; }
            i += 2;
        }
        else { if (n >= cap) return -1; out[n++] = c; }
    }
    return n;
}

static int do_tr(int argc, char** argv, const uint8_t* in, uint64_t in_len, Out* o)
{
    int del = 0, idx = 1;
    if (idx < argc && str_equal(argv[idx], "-d")) { del = 1; ++idx; }
    if (idx >= argc) return TEXTUTIL_ERR_USAGE;
    uint8_t s1[256], s2[256];
    int n1 = expand_set(argv[idx++], s1, 256);
    if (n1 <= 0) return TEXTUTIL_ERR_USAGE;
    int n2 = 0;
    if (!del)
    {
        if (idx >= argc) return TEXTUTIL_ERR_USAGE;
        n2 = expand_set(argv[idx++], s2, 256);
        if (n2 <= 0) return TEXTUTIL_ERR_USAGE;
    }
    if (idx != argc) return TEXTUTIL_ERR_USAGE;
    for (uint64_t i = 0; i < in_len; ++i)
    {
        uint8_t c = in[i];
        int hit = -1;
        for (int k = 0; k < n1; ++k) if (s1[k] == c) { hit = k; break; }
        if (hit < 0) { out_byte(o, c); continue; }
        if (del) continue;
        out_byte(o, hit < n2 ? s2[hit] : s2[n2 - 1]);
    }
    return TEXTUTIL_OK;
}

static uint8_t lower(uint8_t c) { return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c; }

static int line_contains(const uint8_t* s, uint32_t len, const char* pat, int icase)
{
    int pl = str_len(pat);
    if (pl == 0) return 1;
    if ((uint32_t)pl > len) return 0;
    for (uint32_t i = 0; i + (uint32_t)pl <= len; ++i)
    {
        int ok = 1;
        for (int k = 0; k < pl; ++k)
        {
            uint8_t a = s[i + (uint32_t)k], b = (uint8_t)pat[k];
            if (icase) { a = lower(a); b = lower(b); }
            if (a != b) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static int do_grep(int argc, char** argv, const uint8_t* in, uint64_t in_len, Out* o)
{
    int icase = 0, inv = 0, num = 0, cnt = 0;
    const char* pat = 0;
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (a[0] == '-' && a[1] != '\0')
        {
            for (int k = 1; a[k]; ++k)
            {
                if (a[k] == 'i') icase = 1;
                else if (a[k] == 'v') inv = 1;
                else if (a[k] == 'n') num = 1;
                else if (a[k] == 'c') cnt = 1;
                else return TEXTUTIL_ERR_USAGE;
            }
        }
        else if (!pat) pat = a;
        else return TEXTUTIL_ERR_USAGE;
    }
    if (!pat) return TEXTUTIL_ERR_USAGE;
    int n = split_lines(in, in_len);
    if (n < 0) return TEXTUTIL_ERR_OVERFLOW;
    uint64_t matches = 0;
    for (int i = 0; i < n; ++i)
    {
        int m = line_contains(in + g_start[i], g_len[i], pat, icase);
        if (inv) m = !m;
        if (!m) continue;
        ++matches;
        if (cnt) continue;
        if (num) { out_u64_padded(o, (uint64_t)(i + 1), 0); out_byte(o, (uint8_t)':'); }
        out_line(o, in + g_start[i], g_len[i]);
    }
    if (cnt) { out_u64_padded(o, matches, 0); out_byte(o, (uint8_t)'\n'); }
    return TEXTUTIL_OK;
}

static int do_wc(int argc, char** argv, const uint8_t* in, uint64_t in_len, Out* o)
{
    int want_l = 0, want_w = 0, want_c = 0;
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (a[0] != '-' || a[1] == '\0') return TEXTUTIL_ERR_USAGE;
        for (int k = 1; a[k]; ++k)
        {
            if (a[k] == 'l') want_l = 1;
            else if (a[k] == 'w') want_w = 1;
            else if (a[k] == 'c') want_c = 1;
            else return TEXTUTIL_ERR_USAGE;
        }
    }
    if (!want_l && !want_w && !want_c) want_l = want_w = want_c = 1;
    uint64_t lines = 0, words = 0;
    int in_word = 0;
    for (uint64_t i = 0; i < in_len; ++i)
    {
        uint8_t c = in[i];
        if (c == '\n') ++lines;
        int sp = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (sp) in_word = 0; else if (!in_word) { in_word = 1; ++words; }
    }
    if (in_len > 0 && in[in_len - 1] != '\n') ++lines;
    int first = 1;
    if (want_l) { out_u64_padded(o, lines, 0); first = 0; }
    if (want_w) { if (!first) out_byte(o, (uint8_t)' '); out_u64_padded(o, words, 0); first = 0; }
    if (want_c) { if (!first) out_byte(o, (uint8_t)' '); out_u64_padded(o, in_len, 0); }
    out_byte(o, (uint8_t)'\n');
    return TEXTUTIL_OK;
}

int textutil_filter(int argc, char** argv,
                    const uint8_t* in, uint64_t in_len,
                    uint8_t* out, uint64_t out_cap, uint64_t* out_len)
{
    if (argc < 1 || !argv || !argv[0] || !out || !out_len) return TEXTUTIL_ERR_USAGE;
    if (!in && in_len) return TEXTUTIL_ERR_USAGE;
    Out o; o.buf = out; o.cap = out_cap; o.n = 0; o.over = 0;
    const char* name = argv[0];
    int rc;
    if      (str_equal(name, "head")) rc = do_head_tail(0, argc, argv, in, in_len, &o);
    else if (str_equal(name, "tail")) rc = do_head_tail(1, argc, argv, in, in_len, &o);
    else if (str_equal(name, "sort")) rc = do_sort(argc, argv, in, in_len, &o);
    else if (str_equal(name, "uniq")) rc = do_uniq(argc, argv, in, in_len, &o);
    else if (str_equal(name, "rev"))  rc = do_rev_tac_nl(0, argc, in, in_len, &o);
    else if (str_equal(name, "tac"))  rc = do_rev_tac_nl(1, argc, in, in_len, &o);
    else if (str_equal(name, "nl"))   rc = do_rev_tac_nl(2, argc, in, in_len, &o);
    else if (str_equal(name, "cut"))  rc = do_cut(argc, argv, in, in_len, &o);
    else if (str_equal(name, "tr"))   rc = do_tr(argc, argv, in, in_len, &o);
    else if (str_equal(name, "grep")) rc = do_grep(argc, argv, in, in_len, &o);
    else if (str_equal(name, "wc"))   rc = do_wc(argc, argv, in, in_len, &o);
    else return TEXTUTIL_ERR_UNKNOWN;
    *out_len = o.n;
    if (rc == TEXTUTIL_OK && o.over) return TEXTUTIL_ERR_OVERFLOW;
    return rc;
}

/* -------------------------------------------------------------- expression */

struct Parser { const char* s; int pos; int err; int depth; };

static void skip_ws(Parser* p) { while (p->s[p->pos] == ' ' || p->s[p->pos] == '\t') ++p->pos; }
static int64_t parse_expr(Parser* p);

static int64_t parse_atom(Parser* p)
{
    skip_ws(p);
    char c = p->s[p->pos];
    if (c == '(')
    {
        if (++p->depth > 32) { p->err = 1; return 0; }
        ++p->pos;
        int64_t v = parse_expr(p);
        skip_ws(p);
        if (p->s[p->pos] != ')') { p->err = 1; return 0; }
        ++p->pos; --p->depth;
        return v;
    }
    if (c == '-') { ++p->pos; return (int64_t)(0ULL - (uint64_t)parse_atom(p)); }
    if (c == '+') { ++p->pos; return parse_atom(p); }
    if (!is_digit(c)) { p->err = 1; return 0; }
    uint64_t v = 0;
    while (is_digit(p->s[p->pos]))
    {
        v = v * 10ULL + (uint64_t)(p->s[p->pos] - '0');
        if (v > 9000000000000000000ULL) { p->err = 1; return 0; }
        ++p->pos;
    }
    return (int64_t)v;
}

static int64_t parse_term(Parser* p)
{
    int64_t v = parse_atom(p);
    for (;;)
    {
        skip_ws(p);
        char c = p->s[p->pos];
        if (c != '*' && c != '/' && c != '%') return v;
        ++p->pos;
        int64_t r = parse_atom(p);
        if (p->err) return 0;
        if (c == '*') v = (int64_t)((uint64_t)v * (uint64_t)r);
        else
        {
            if (r == 0) { p->err = 1; return 0; }
            if (c == '/') v = v / r; else v = v % r;
        }
    }
}

static int64_t parse_expr(Parser* p)
{
    int64_t v = parse_term(p);
    for (;;)
    {
        skip_ws(p);
        char c = p->s[p->pos];
        if (c != '+' && c != '-') return v;
        ++p->pos;
        int64_t r = parse_term(p);
        if (p->err) return 0;
        v = (c == '+') ? (int64_t)((uint64_t)v + (uint64_t)r)
                       : (int64_t)((uint64_t)v - (uint64_t)r);
    }
}

int textutil_eval(const char* expr, int64_t* result)
{
    if (!expr || !result) return TEXTUTIL_ERR_USAGE;
    Parser p; p.s = expr; p.pos = 0; p.err = 0; p.depth = 0;
    int64_t v = parse_expr(&p);
    skip_ws(&p);
    if (p.err || p.s[p.pos] != '\0') return TEXTUTIL_ERR_USAGE;
    *result = v;
    return TEXTUTIL_OK;
}

/* ------------------------------------------------------------- path helpers */

static void put_range(const char* s, int from, int to, char* out, int cap)
{
    int n = 0;
    for (int i = from; i < to && n < cap - 1; ++i) out[n++] = s[i];
    out[n] = '\0';
}

void textutil_basename(const char* path, char* out, int cap)
{
    if (!out || cap <= 0) return;
    if (!path || !path[0]) { out[0] = '\0'; return; }
    int end = str_len(path);
    while (end > 1 && path[end - 1] == '/') --end;
    if (end == 1 && path[0] == '/') { str_copy(out, "/", cap); return; }
    int start = end;
    while (start > 0 && path[start - 1] != '/') --start;
    put_range(path, start, end, out, cap);
}

void textutil_dirname(const char* path, char* out, int cap)
{
    if (!out || cap <= 0) return;
    if (!path || !path[0]) { str_copy(out, ".", cap); return; }
    int end = str_len(path);
    while (end > 1 && path[end - 1] == '/') --end;
    int i = end;
    while (i > 0 && path[i - 1] != '/') --i;      /* skip final component */
    if (i == 0) { str_copy(out, ".", cap); return; }
    while (i > 1 && path[i - 1] == '/') --i;      /* skip separators */
    put_range(path, 0, i, out, cap);
}
