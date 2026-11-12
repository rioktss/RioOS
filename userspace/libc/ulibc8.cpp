/*
 * MIT License
 *
 * Copyright (c) 2026 Rio okta kusuma
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files, to deal in the Software
 * without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "ulibc.h"
#include "../../include/user_api.h"
/* RioOS libc part 8: getopt/getopt_long, POSIX regular expressions (BRE and ERE, leftmost-longest, back-references),
   strptime, string/locale helpers, UTF-8 multibyte and wide-string functions, syslog. */

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_digit_c(int c) { return c >= '0' && c <= '9'; }
static int is_word_c(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit_c(c) || c == '_'; }

/* ---- getopt ------------------------------------------------------------------------------------------------------------ */
char* optarg = 0; int optind = 1, opterr = 1, optopt = 0;
static int g_first_nonopt = 1, g_last_nonopt = 1, g_init = 0;
static char* g_nextchar = 0;
static int g_posix = 0, g_colon = 0;

static int opt_is_option(const char* a) { return a && a[0] == '-' && a[1] != 0; }
static void opt_reverse(char** v, int a, int b) { while (a < b) { char* t = v[a]; v[a] = v[b]; v[b] = t; ++a; --b; } }
static void opt_exchange(char** argv)
{
    int bottom = g_first_nonopt, middle = g_last_nonopt, top = optind;
    opt_reverse(argv, bottom, top - 1);
    opt_reverse(argv, bottom, bottom + (top - middle) - 1);
    opt_reverse(argv, bottom + (top - middle), top - 1);
    g_first_nonopt += optind - g_last_nonopt;
    g_last_nonopt = optind;
}
static const char* prog_name(char* const argv[]) { const char* a = argv[0] ? argv[0] : "?"; const char* s = strrchr(a, '/'); return s ? s + 1 : a; }

static int getopt_core(int argc, char* const argv_c[], const char* optstring, const struct option* longopts, int* longindex, int long_only)
{
    char** argv = (char**)argv_c;
    optarg = 0;
    if (optind == 0 || !g_init)
    {
        optind = 1; g_first_nonopt = g_last_nonopt = 1; g_nextchar = 0; g_init = 1;
        g_posix = g_colon = 0;
        if (optstring[0] == '+') { g_posix = 1; ++optstring; } else if (getenv("POSIXLY_CORRECT")) g_posix = 1;
        if (optstring[0] == ':') g_colon = 1;
    }
    else if (optstring[0] == '+') ++optstring;
    if (optstring[0] == ':') ++optstring;
    if (!g_nextchar || !*g_nextchar)
    {
        if (g_last_nonopt > optind) g_last_nonopt = optind;
        if (g_first_nonopt > optind) g_first_nonopt = optind;
        if (!g_posix)
        {
            if (g_first_nonopt != g_last_nonopt && g_last_nonopt != optind) opt_exchange(argv);
            else if (g_last_nonopt != optind) g_first_nonopt = optind;
            while (optind < argc && !opt_is_option(argv[optind]) ) ++optind;
            g_last_nonopt = optind;
        }
        if (optind != argc && !strcmp(argv[optind], "--"))
        {
            ++optind;
            if (g_first_nonopt != g_last_nonopt && g_last_nonopt != optind) opt_exchange(argv);
            else if (g_first_nonopt == g_last_nonopt) g_first_nonopt = optind;
            g_last_nonopt = argc; optind = argc;
        }
        if (optind == argc)
        {
            if (g_first_nonopt != g_last_nonopt) optind = g_first_nonopt;
            g_init = 0; return -1;
        }
        if (!opt_is_option(argv[optind])) { g_init = 0; return -1; }      /* POSIX mode: first non-option ends parsing */
        /* long option? */
        int dashes = argv[optind][1] == '-' ? 2 : 1;
        if (longopts && (dashes == 2 || long_only))
        {
            const char* name = argv[optind] + dashes; const char* eq = strchr(name, '=');
            size_t nl = eq ? (size_t)(eq - name) : strlen(name);
            int found = -1, exact = 0, ambig = 0;
            for (int i = 0; longopts[i].name; ++i)
            {
                if (strncmp(longopts[i].name, name, nl) != 0) continue;
                if (strlen(longopts[i].name) == nl) { found = i; exact = 1; break; }
                if (found < 0) found = i;
                else if (longopts[i].has_arg != longopts[found].has_arg || longopts[i].flag != longopts[found].flag || longopts[i].val != longopts[found].val) ambig = 1;
            }
            int single_ok = 0;
            if (dashes == 1 && long_only && found < 0 && nl == 1 && strchr(optstring, name[0])) single_ok = 1;
            if (dashes == 1 && long_only && found >= 0 && !exact && nl == 1 && strchr(optstring, name[0])) { found = -1; single_ok = 1; }
            if (!single_ok && (found >= 0 || dashes == 2 || long_only))
            {
                if (found < 0 || (ambig && !exact))
                {
                    if (opterr && !g_colon) dprintf(2, "%s: %s option '%s%.*s'\n", prog_name(argv_c), found < 0 ? "unrecognized" : "ambiguous", dashes == 2 ? "--" : "-", (int)nl, name);
                    g_nextchar = 0; ++optind; optopt = 0; return '?';
                }
                const struct option* o = &longopts[found];
                ++optind; g_nextchar = 0;
                if (eq)
                {
                    if (o->has_arg == no_argument)
                    {
                        if (opterr && !g_colon) dprintf(2, "%s: option '--%s' doesn't allow an argument\n", prog_name(argv_c), o->name);
                        optopt = o->val; return '?';
                    }
                    optarg = (char*)eq + 1;
                }
                else if (o->has_arg == required_argument)
                {
                    if (optind < argc) optarg = argv[optind++];
                    else
                    {
                        if (opterr && !g_colon) dprintf(2, "%s: option '--%s' requires an argument\n", prog_name(argv_c), o->name);
                        optopt = o->val; return g_colon ? ':' : '?';
                    }
                }
                if (longindex) *longindex = found;
                if (o->flag) { *o->flag = o->val; return 0; }
                return o->val;
            }
        }
        g_nextchar = argv[optind] + 1;
    }
    char c = *g_nextchar++;
    const char* spec = c == ':' ? 0 : strchr(optstring, c);
    if (!*g_nextchar) ++optind;
    if (!spec)
    {
        if (opterr && !g_colon) dprintf(2, "%s: invalid option -- '%c'\n", prog_name(argv_c), c);
        optopt = (unsigned char)c; return '?';
    }
    if (spec[1] == ':')
    {
        if (spec[2] == ':')                       /* optional argument: only when attached */
        {
            if (*g_nextchar) { optarg = g_nextchar; ++optind; }
            g_nextchar = 0;
        }
        else if (*g_nextchar) { optarg = g_nextchar; ++optind; g_nextchar = 0; }
        else if (optind >= argc)
        {
            if (opterr && !g_colon) dprintf(2, "%s: option requires an argument -- '%c'\n", prog_name(argv_c), c);
            optopt = (unsigned char)c; g_nextchar = 0; return g_colon ? ':' : '?';
        }
        else { optarg = argv[optind++]; g_nextchar = 0; }
    }
    return (unsigned char)c;
}
int getopt(int argc, char* const argv[], const char* optstring) { return getopt_core(argc, argv, optstring, 0, 0, 0); }
int getopt_long(int argc, char* const argv[], const char* optstring, const struct option* lo, int* li) { return getopt_core(argc, argv, optstring, lo, li, 0); }
int getopt_long_only(int argc, char* const argv[], const char* optstring, const struct option* lo, int* li) { return getopt_core(argc, argv, optstring, lo, li, 1); }

/* ---- string helpers ---------------------------------------------------------------------------------------------------- */
char* stpcpy(char* d, const char* s) { while ((*d = *s++) != 0) ++d; return d; }
char* stpncpy(char* d, const char* s, size_t n)
{
    size_t i = 0; while (i < n && s[i]) { d[i] = s[i]; ++i; }
    char* end = d + i; while (i < n) d[i++] = 0;
    return end;
}
int strverscmp(const char* a, const char* b)
{
    while (*a && *b)
    {
        if (is_digit_c((unsigned char)*a) && is_digit_c((unsigned char)*b))
        {
            const char* ea = a; const char* eb = b;
            while (*a == '0' && is_digit_c((unsigned char)a[1])) ++a;      /* leading zeros do not count */
            while (*b == '0' && is_digit_c((unsigned char)b[1])) ++b;
            ea = a; eb = b; while (is_digit_c((unsigned char)*ea)) ++ea; while (is_digit_c((unsigned char)*eb)) ++eb;
            long la = (long)(ea - a), lb = (long)(eb - b);
            if (la != lb) return la < lb ? -1 : 1;
            int c = strncmp(a, b, (size_t)la); if (c) return c < 0 ? -1 : 1;
            a = ea; b = eb; continue;
        }
        if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        ++a; ++b;
    }
    return (unsigned char)*a == (unsigned char)*b ? 0 : (unsigned char)*a < (unsigned char)*b ? -1 : 1;
}
int strcoll(const char* a, const char* b) { return strcmp(a, b); }
size_t strxfrm(char* d, const char* s, size_t n)
{
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
int strerror_r(int err, char* buf, size_t cap)
{
    const char* m = strerror(err); size_t l = strlen(m);
    if (cap == 0) return ERANGE;
    if (l + 1 > cap) { memcpy(buf, m, cap - 1); buf[cap - 1] = 0; return ERANGE; }
    memcpy(buf, m, l + 1); return 0;
}
char* nl_langinfo(int item) { static char u[] = "UTF-8", e[] = ""; return item == CODESET ? u : e; }

/* ---- UTF-8 multibyte / wide strings ---------------------------------------------------------------------------------------- */
size_t wcslen(const wchar_t* s) { size_t n = 0; while (s[n]) ++n; return n; }
wchar_t* wcscpy(wchar_t* d, const wchar_t* s) { wchar_t* r = d; while ((*d++ = *s++) != 0) {} return r; }
wchar_t* wcsncpy(wchar_t* d, const wchar_t* s, size_t n) { size_t i = 0; while (i < n && s[i]) { d[i] = s[i]; ++i; } while (i < n) d[i++] = 0; return d; }
int wcscmp(const wchar_t* a, const wchar_t* b) { while (*a && *a == *b) { ++a; ++b; } return *a < *b ? -1 : *a > *b ? 1 : 0; }
wchar_t* wcscat(wchar_t* d, const wchar_t* s) { wcscpy(d + wcslen(d), s); return d; }
wchar_t* wcschr(const wchar_t* s, wchar_t c) { for (;; ++s) { if (*s == c) return (wchar_t*)s; if (!*s) return 0; } }
/* returns the sequence length (1-4), 0 for NUL, -1 invalid/incomplete (-2 when more bytes were needed) */
static int utf8_decode(const unsigned char* s, size_t n, unsigned* out)
{
    if (n == 0) return -2;
    unsigned c = s[0];
    if (c == 0) { *out = 0; return 0; }
    if (c < 0x80) { *out = c; return 1; }
    int len; unsigned cp, min;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
    else return -1;
    if (n < (size_t)len) return -2;
    for (int i = 1; i < len; ++i) { if ((s[i] & 0xC0) != 0x80) return -1; cp = (cp << 6) | (s[i] & 0x3F); }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
    *out = cp; return len;
}
int mbtowc(wchar_t* w, const char* s, size_t n)
{
    if (!s) return 0;
    unsigned cp; int l = utf8_decode((const unsigned char*)s, n, &cp);
    if (l < 0) { errno = EILSEQ; return -1; }
    if (w) *w = (wchar_t)cp;
    return l;
}
int mblen(const char* s, size_t n) { return mbtowc(0, s, n); }
int wctomb(char* s, wchar_t w)
{
    if (!s) return 0;
    unsigned c = (unsigned)w;
    if (c < 0x80) { s[0] = (char)c; return 1; }
    if (c < 0x800) { s[0] = (char)(0xC0 | (c >> 6)); s[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c >= 0xD800 && c <= 0xDFFF) { errno = EILSEQ; return -1; }
    if (c < 0x10000) { s[0] = (char)(0xE0 | (c >> 12)); s[1] = (char)(0x80 | ((c >> 6) & 0x3F)); s[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    if (c <= 0x10FFFF) { s[0] = (char)(0xF0 | (c >> 18)); s[1] = (char)(0x80 | ((c >> 12) & 0x3F)); s[2] = (char)(0x80 | ((c >> 6) & 0x3F)); s[3] = (char)(0x80 | (c & 0x3F)); return 4; }
    errno = EILSEQ; return -1;
}
size_t mbstowcs(wchar_t* d, const char* s, size_t n)
{
    size_t count = 0, len = strlen(s);
    while (*s)
    {
        unsigned cp; int l = utf8_decode((const unsigned char*)s, len, &cp);
        if (l <= 0) { errno = EILSEQ; return (size_t)-1; }
        if (d) { if (count >= n) return count; d[count] = (wchar_t)cp; }
        ++count; s += l; len -= (size_t)l;
    }
    if (d && count < n) d[count] = 0;
    return count;
}
size_t wcstombs(char* d, const wchar_t* s, size_t n)
{
    size_t out = 0; char tmp[4];
    for (; *s; ++s)
    {
        int l = wctomb(tmp, *s); if (l < 0) return (size_t)-1;
        if (d) { if (out + (size_t)l > n) return out; memcpy(d + out, tmp, (size_t)l); }
        out += (size_t)l;
    }
    if (d && out < n) d[out] = 0;
    return out;
}

/* ---- strptime ------------------------------------------------------------------------------------------------------------------ */
static const char* const mon_full[12] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
static const char* const day_full[7] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
/* Matches a full or 3-letter abbreviated name, case-insensitively; returns the index or -1 and advances *sp. */
static int match_name(const char** sp, const char* const* names, int count)
{
    const char* s = *sp;
    for (int i = 0; i < count; ++i)
    {
        size_t fl = strlen(names[i]), k = 0;
        while (k < fl && s[k] && lc((unsigned char)s[k]) == lc((unsigned char)names[i][k])) ++k;
        if (k == fl) { *sp = s + fl; return i; }
        if (k >= 3 && !((s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z'))) { *sp = s + k; return i; }
    }
    return -1;
}
static int get_num(const char** sp, int maxdigits, int lo, int hi, int* out)
{
    const char* s = *sp; int v = 0, n = 0;
    while (*s == ' ') ++s;
    while (n < maxdigits && is_digit_c((unsigned char)*s)) { v = v * 10 + (*s - '0'); ++s; ++n; }
    if (n == 0 || v < lo || v > hi) return 0;
    *out = v; *sp = s; return 1;
}
static int leap_year(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
char* strptime(const char* s, const char* fmt, struct tm* tm)
{
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int pm = -1, hour12 = 0, have_date = 0, century = -1, yy = -1;
    for (; *fmt; ++fmt)
    {
        if (*fmt == ' ' || (*fmt >= 9 && *fmt <= 13)) { while (*s == ' ' || (*s >= 9 && *s <= 13)) ++s; continue; }
        if (*fmt != '%') { if (*s != *fmt) return 0; ++s; continue; }
        ++fmt;
        while (*fmt == 'E' || *fmt == 'O' || *fmt == '-' || *fmt == '_' || *fmt == '0' || *fmt == '^' || *fmt == '#') ++fmt;
        int v;
        switch (*fmt)
        {
            case '%': if (*s != '%') return 0; ++s; break;
            case 'n': case 't': while (*s == ' ' || (*s >= 9 && *s <= 13)) ++s; break;
            case 'Y': if (!get_num(&s, 4, 0, 9999, &v)) return 0; tm->tm_year = v - 1900; have_date = 1; century = -1; yy = -1; break;
            case 'y': if (!get_num(&s, 2, 0, 99, &v)) return 0; yy = v; tm->tm_year = (v < 69 ? 100 : 0) + v; have_date = 1; break;
            case 'C': if (!get_num(&s, 2, 0, 99, &v)) return 0; century = v; break;
            case 'm': if (!get_num(&s, 2, 1, 12, &v)) return 0; tm->tm_mon = v - 1; have_date = 1; break;
            case 'd': case 'e': if (!get_num(&s, 2, 1, 31, &v)) return 0; tm->tm_mday = v; have_date = 1; break;
            case 'H': case 'k': if (!get_num(&s, 2, 0, 23, &v)) return 0; tm->tm_hour = v; hour12 = 0; break;
            case 'I': case 'l': if (!get_num(&s, 2, 1, 12, &v)) return 0; tm->tm_hour = v % 12; hour12 = 1; break;
            case 'M': if (!get_num(&s, 2, 0, 59, &v)) return 0; tm->tm_min = v; break;
            case 'S': if (!get_num(&s, 2, 0, 61, &v)) return 0; tm->tm_sec = v; break;
            case 'j': if (!get_num(&s, 3, 1, 366, &v)) return 0; tm->tm_yday = v - 1; break;
            case 'u': if (!get_num(&s, 1, 1, 7, &v)) return 0; tm->tm_wday = v % 7; break;
            case 'w': if (!get_num(&s, 1, 0, 6, &v)) return 0; tm->tm_wday = v; break;
            case 'p': case 'P':
                if (lc((unsigned char)s[0]) == 'a' && lc((unsigned char)s[1]) == 'm') pm = 0;
                else if (lc((unsigned char)s[0]) == 'p' && lc((unsigned char)s[1]) == 'm') pm = 1;
                else return 0;
                s += 2; break;
            case 'b': case 'B': case 'h': v = match_name(&s, mon_full, 12); if (v < 0) return 0; tm->tm_mon = v; have_date = 1; break;
            case 'a': case 'A': v = match_name(&s, day_full, 7); if (v < 0) return 0; tm->tm_wday = v; break;
            case 'z':
            {
                if (*s == 'Z') { ++s; break; }
                if (*s != '+' && *s != '-') return 0;
                ++s; int n = 0; while (n < 4 && is_digit_c((unsigned char)*s)) { ++s; ++n; if (n == 2 && *s == ':') ++s; }
                if (n != 4 && n != 2) return 0; break;
            }
            case 'Z': while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) ++s; break;
            case 's':
            {
                long t = 0; int n = 0; while (is_digit_c((unsigned char)*s)) { t = t * 10 + (*s - '0'); ++s; ++n; }
                if (!n) return 0;
                time_t tt = (time_t)t; if (!gmtime_r(&tt, tm)) return 0; have_date = 1; break;
            }
            case 'T': { char* r = strptime(s, "%H:%M:%S", tm); if (!r) return 0; s = r; break; }
            case 'R': { char* r = strptime(s, "%H:%M", tm); if (!r) return 0; s = r; break; }
            case 'r': { char* r = strptime(s, "%I:%M:%S %p", tm); if (!r) return 0; s = r; break; }
            case 'D': { char* r = strptime(s, "%m/%d/%y", tm); if (!r) return 0; s = r; break; }
            case 'F': { char* r = strptime(s, "%Y-%m-%d", tm); if (!r) return 0; s = r; break; }
            case 'c': { char* r = strptime(s, "%a %b %e %H:%M:%S %Y", tm); if (!r) return 0; s = r; break; }
            case 'x': { char* r = strptime(s, "%m/%d/%y", tm); if (!r) return 0; s = r; break; }
            case 'X': { char* r = strptime(s, "%H:%M:%S", tm); if (!r) return 0; s = r; break; }
            default: return 0;
        }
    }
    if (hour12 && pm == 1) tm->tm_hour += 12;
    else if (!hour12 && pm == 1 && tm->tm_hour < 12) tm->tm_hour += 12;
    if (century >= 0) tm->tm_year = century * 100 + (yy >= 0 ? yy : tm->tm_year % 100) - 1900;
    if (have_date)
    {
        int y = tm->tm_year + 1900, m = tm->tm_mon, d = tm->tm_mday;
        if (d < 1) d = 1;
        tm->tm_yday = cum[m] + d - 1 + ((m > 1 && leap_year(y)) ? 1 : 0);
        int yp = y - 1; long days = 365L * yp + yp / 4 - yp / 100 + yp / 400 + tm->tm_yday;   /* days since 0001-01-01 (a Monday) */
        tm->tm_wday = (int)((days + 1) % 7);
    }
    return (char*)s;
}

/* ---- syslog ---------------------------------------------------------------------------------------------------------------------- */
static char sl_ident[32] = "rio"; static int sl_opt = 0;
void openlog(const char* ident, int opt, int facility) { (void)facility; if (ident) { size_t i = 0; while (ident[i] && i < sizeof(sl_ident) - 1) { sl_ident[i] = ident[i]; ++i; } sl_ident[i] = 0; } sl_opt = opt; }
void closelog(void) {}
void syslog(int prio, const char* fmt, ...)
{
    static const char* const mon[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char msg[256]; __builtin_va_list ap; __builtin_va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap); __builtin_va_end(ap);
    time_t now = (time_t)time_epoch(); struct tm t; gmtime_r(&now, &t);
    char line[320];
    int n = (sl_opt & LOG_PID) ? snprintf(line, sizeof(line), "%s %2d %02d:%02d:%02d %s[%d]: <%d> %s\n", mon[t.tm_mon], t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, sl_ident, (int)getpid(), prio & 7, msg)
                               : snprintf(line, sizeof(line), "%s %2d %02d:%02d:%02d %s: <%d> %s\n", mon[t.tm_mon], t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, sl_ident, prio & 7, msg);
    if (n >= (int)sizeof(line)) { n = (int)sizeof(line) - 1; line[n - 1] = '\n'; }
    int saved = errno;
    (void)mkdir("/var", 0755); (void)mkdir("/var/log", 0755);
    int fd = open("/var/log/messages", O_WRONLY | O_CREAT | O_APPEND);
    if (fd >= 0) { write(fd, line, (size_t)n); close(fd); }
    errno = saved;
}

/* ---- POSIX regular expressions --------------------------------------------------------------------------------------------------------
   Parser -> AST -> backtracking VM program. The VM explores every alternative and keeps the longest match at the leftmost start
   (POSIX leftmost-longest). Without back-references, visited (split, position) states are memoised, so matching is O(program x text);
   with back-references an exploration step limit applies. The backtrack stack lives on the heap. */
enum { R_EMPTY, R_CHAR, R_ANY, R_SET, R_BOL, R_EOL, R_CAT, R_ALT, R_REP, R_GROUP, R_BACKREF, R_WORDB, R_NWORDB, R_WBEG, R_WEND, R_BUFB, R_BUFE };
struct RNode { uint8_t type; uint8_t ch; int a, b, min, max, idx; };
enum { I_CHAR, I_ANY, I_SET, I_BOL, I_EOL, I_SPLIT, I_JMP, I_SAVE, I_BACKREF, I_MATCH, I_LSET, I_LCHK, I_WORDB, I_NWORDB, I_WBEG, I_WEND, I_BUFB, I_BUFE };
struct Inst { int op; int x, y, z; };
typedef uint8_t RSet[32];
struct Prog { Inst* ins; int n, cap; RSet* sets; int nsets; int nsub; int cflags; int has_backref; int nsplit; int nlreg; };

#define RE_MAX_NODES 8192
#define RE_MAX_INST 24000
#define RE_DUP_MAX 255

struct RC
{
    const char* p; int ere, icase, newline; RNode* nodes; int nn; int nsub; uint8_t closed[10]; int err;
    RSet* sets; int nsets, setcap; int has_backref; int depth;
};
static int rc_node(RC* c, int type)
{
    if (c->nn >= RE_MAX_NODES) { c->err = REG_ESPACE; return 0; }
    RNode* n = &c->nodes[c->nn]; memset(n, 0, sizeof(*n)); n->type = (uint8_t)type; n->a = n->b = -1; n->max = -1;
    return c->nn++;
}
static int rc_set(RC* c)
{
    if (c->nsets == c->setcap)
    {
        int nc = c->setcap ? c->setcap * 2 : 8; RSet* ns = (RSet*)realloc(c->sets, (size_t)nc * sizeof(RSet));
        if (!ns) { c->err = REG_ESPACE; return -1; }
        c->sets = ns; c->setcap = nc;
    }
    memset(c->sets[c->nsets], 0, sizeof(RSet)); return c->nsets++;
}
static void set_add(RSet s, int ch) { s[ch >> 3] |= (uint8_t)(1U << (ch & 7)); }
static int set_has(const RSet s, int ch) { return (s[ch >> 3] >> (ch & 7)) & 1; }
static int class_has(const char* name, size_t l, int ch)
{
#define CL(n, cond) if (l == sizeof(n) - 1 && !memcmp(name, n, l)) return (cond)
    CL("alpha", (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')); CL("digit", ch >= '0' && ch <= '9');
    CL("alnum", (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')); CL("upper", ch >= 'A' && ch <= 'Z');
    CL("lower", ch >= 'a' && ch <= 'z'); CL("space", ch == ' ' || (ch >= 9 && ch <= 13)); CL("blank", ch == ' ' || ch == '\t');
    CL("punct", ch > 32 && ch < 127 && !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')));
    CL("print", ch >= 32 && ch < 127); CL("graph", ch > 32 && ch < 127); CL("cntrl", ch < 32 || ch == 127);
    CL("xdigit", (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'));
#undef CL
    return -1;
}
static int class_valid(const char* name, size_t l) { return class_has(name, l, 'a') >= 0; }

/* '[' already consumed */
static int parse_bracket(RC* c)
{
    int si = rc_set(c); if (si < 0) return -1;
    int neg = 0, first = 1;
    const char* p = c->p;
    if (*p == '^') { neg = 1; ++p; }
    for (;;)
    {
        if (!*p) { c->err = REG_EBRACK; return -1; }
        if (*p == ']' && !first) { ++p; break; }
        first = 0;
        int lo;
        if (p[0] == '[' && (p[1] == ':' || p[1] == '=' || p[1] == '.'))
        {
            char kind = p[1]; const char* e = p + 2;
            while (*e && !(e[0] == kind && e[1] == ']')) ++e;
            if (!*e) { c->err = REG_EBRACK; return -1; }
            size_t l = (size_t)(e - (p + 2));
            if (kind == ':')
            {
                if (!class_valid(p + 2, l)) { c->err = REG_ECTYPE; return -1; }
                for (int ch = 0; ch < 256; ++ch) if (class_has(p + 2, l, ch) > 0) set_add(c->sets[si], ch);
                p = e + 2; continue;
            }
            if (l != 1) { c->err = REG_ECOLLATE; return -1; }
            lo = (unsigned char)p[2]; p = e + 2;
            if (kind == '=') { set_add(c->sets[si], lo); continue; }
        }
        else lo = (unsigned char)*p++;
        int hi = lo;
        if (*p == '-' && p[1] && p[1] != ']')
        {
            ++p;
            if (p[0] == '[' && p[1] == '.')
            {
                const char* e = p + 2; while (*e && !(e[0] == '.' && e[1] == ']')) ++e;
                if (!*e || e - (p + 2) != 1) { c->err = REG_ECOLLATE; return -1; }
                hi = (unsigned char)p[2]; p = e + 2;
            }
            else hi = (unsigned char)*p++;
            if (hi < lo) { c->err = REG_ERANGE; return -1; }
        }
        for (int ch = lo; ch <= hi; ++ch) set_add(c->sets[si], ch);
    }
    if (c->icase) for (int ch = 'a'; ch <= 'z'; ++ch) if (set_has(c->sets[si], ch) || set_has(c->sets[si], ch - 32)) { set_add(c->sets[si], ch); set_add(c->sets[si], ch - 32); }
    if (neg) { for (int i = 0; i < 32; ++i) c->sets[si][i] = (uint8_t)~c->sets[si][i]; if (c->newline) c->sets[si][1] &= (uint8_t)~(1U << 2); }
    c->sets[si][0] &= (uint8_t)~1U;      /* NUL never matches */
    c->p = p;
    int n = rc_node(c, R_SET); if (c->err) return -1;
    c->nodes[n].idx = si; return n;
}
static int make_class_set(RC* c, int kind)      /* \w \W \s \S */
{
    int si = rc_set(c); if (si < 0) return -1;
    for (int ch = 1; ch < 256; ++ch)
    {
        int in = (kind == 'w' || kind == 'W') ? is_word_c(ch) : (ch == ' ' || (ch >= 9 && ch <= 13));
        if (kind == 'W' || kind == 'S') in = !in;
        if (in) set_add(c->sets[si], ch);
    }
    int n = rc_node(c, R_SET); if (c->err) return -1;
    c->nodes[n].idx = si; return n;
}

static int parse_alt(RC* c);
static int at_alt_op(RC* c) { return c->ere ? c->p[0] == '|' : (c->p[0] == '\\' && c->p[1] == '|'); }
static int at_close(RC* c) { return c->ere ? c->p[0] == ')' : (c->p[0] == '\\' && c->p[1] == ')'); }

static int parse_interval(RC* c, int* mn, int* mx)      /* after the opening brace */
{
    const char* p = c->p; int a = -1, b = -1, comma = 0;
    if (is_digit_c((unsigned char)*p)) { a = 0; while (is_digit_c((unsigned char)*p)) { a = a * 10 + (*p++ - '0'); if (a > 100000) a = 100000; } }
    if (*p == ',') { comma = 1; ++p; if (is_digit_c((unsigned char)*p)) { b = 0; while (is_digit_c((unsigned char)*p)) { b = b * 10 + (*p++ - '0'); if (b > 100000) b = 100000; } } }
    if (c->ere ? *p == '}' : (p[0] == '\\' && p[1] == '}')) p += c->ere ? 1 : 2;
    else { c->err = *p ? REG_BADBR : REG_EBRACE; return -1; }
    if (a < 0) { c->err = REG_BADBR; return -1; }
    if (!comma) b = a;
    if (a > RE_DUP_MAX || b > RE_DUP_MAX || (b >= 0 && b < a)) { c->err = REG_BADBR; return -1; }
    *mn = a; *mx = comma && b < 0 ? -1 : b; c->p = p; return 0;
}

static int parse_cat(RC* c)
{
    int head = -1; int start = 1;      /* start: at the beginning of a branch (BRE context rules) */
    for (;;)
    {
        if (!*c->p || at_alt_op(c)) break;
        if (at_close(c)) { if (c->depth == 0) { c->err = REG_EPAREN; return -1; } break; }
        int atom = -1; char ch = *c->p;
        if (c->ere)
        {
            if (ch == '(')
            {
                ++c->p; int g = ++c->nsub; ++c->depth;
                int inner = parse_alt(c); if (c->err) return -1;
                if (*c->p != ')') { c->err = REG_EPAREN; return -1; }
                ++c->p; --c->depth;
                atom = rc_node(c, R_GROUP); if (c->err) return -1;
                c->nodes[atom].a = inner; c->nodes[atom].idx = g; if (g < 10) c->closed[g] = 1;
            }
            else if (ch == '[') { ++c->p; atom = parse_bracket(c); if (c->err) return -1; }
            else if (ch == '.') { ++c->p; atom = rc_node(c, R_ANY); }
            else if (ch == '^') { ++c->p; atom = rc_node(c, R_BOL); }
            else if (ch == '$') { ++c->p; atom = rc_node(c, R_EOL); }
            else if (ch == '\\') goto escape;
            else if ((ch == '*' || ch == '+' || ch == '?') && start) { ++c->p; atom = rc_node(c, R_CHAR); if (c->err) return -1; c->nodes[atom].ch = (uint8_t)ch; }
            else if (ch == '{' && start) { ++c->p; atom = rc_node(c, R_CHAR); if (c->err) return -1; c->nodes[atom].ch = '{'; }
            else { ++c->p; atom = rc_node(c, R_CHAR); if (c->err) return -1; c->nodes[atom].ch = (uint8_t)ch; }
        }
        else
        {
            if (ch == '\\' && c->p[1] == '(')
            {
                c->p += 2; int g = ++c->nsub; ++c->depth;
                int inner = parse_alt(c); if (c->err) return -1;
                if (!(c->p[0] == '\\' && c->p[1] == ')')) { c->err = REG_EPAREN; return -1; }
                c->p += 2; --c->depth;
                atom = rc_node(c, R_GROUP); if (c->err) return -1;
                c->nodes[atom].a = inner; c->nodes[atom].idx = g; if (g < 10) c->closed[g] = 1;
            }
            else if (ch == '[') { ++c->p; atom = parse_bracket(c); if (c->err) return -1; }
            else if (ch == '.') { ++c->p; atom = rc_node(c, R_ANY); }
            else if (ch == '^' && start) { ++c->p; atom = rc_node(c, R_BOL); }
            else if (ch == '$' && (!c->p[1] || (c->p[1] == '\\' && (c->p[2] == ')' || c->p[2] == '|')))) { ++c->p; atom = rc_node(c, R_EOL); }
            else if (ch == '*' && start) { ++c->p; atom = rc_node(c, R_CHAR); if (c->err) return -1; c->nodes[atom].ch = '*'; }
            else if (ch == '\\') goto escape;
            else { ++c->p; atom = rc_node(c, R_CHAR); if (c->err) return -1; c->nodes[atom].ch = (uint8_t)ch; }
        }
        goto have_atom;
    escape:
        {
            char e = c->p[1];
            if (!e) { c->err = REG_EESCAPE; return -1; }
            c->p += 2;
            if (e >= '1' && e <= '9')
            {
                int g = e - '0';
                if (g > c->nsub || !c->closed[g]) { c->err = REG_ESUBREG; return -1; }
                atom = rc_node(c, R_BACKREF); if (c->err) return -1; c->nodes[atom].idx = g; c->has_backref = 1;
            }
            else if (e == 'w' || e == 'W' || e == 's' || e == 'S') { atom = make_class_set(c, e); if (c->err) return -1; }
            else if (e == 'b') atom = rc_node(c, R_WORDB);
            else if (e == 'B') atom = rc_node(c, R_NWORDB);
            else if (e == '<') atom = rc_node(c, R_WBEG);
            else if (e == '>') atom = rc_node(c, R_WEND);
            else if (e == '`') atom = rc_node(c, R_BUFB);
            else if (e == '\'') atom = rc_node(c, R_BUFE);
            else { atom = rc_node(c, R_CHAR); if (c->err) return -1; c->nodes[atom].ch = (uint8_t)e; }
        }
    have_atom:
        if (c->err) return -1;
        int was_anchor = c->nodes[atom].type == R_BOL;
        /* postfix operators */
        for (;;)
        {
            int mn, mx; const char* p = c->p;
            if (*p == '*') { mn = 0; mx = -1; ++c->p; }
            else if (c->ere && *p == '+') { mn = 1; mx = -1; ++c->p; }
            else if (c->ere && *p == '?') { mn = 0; mx = 1; ++c->p; }
            else if (!c->ere && p[0] == '\\' && p[1] == '+') { mn = 1; mx = -1; c->p += 2; }
            else if (!c->ere && p[0] == '\\' && p[1] == '?') { mn = 0; mx = 1; c->p += 2; }
            else if (c->ere && *p == '{' && is_digit_c((unsigned char)p[1])) { ++c->p; if (parse_interval(c, &mn, &mx) < 0) return -1; }
            else if (!c->ere && p[0] == '\\' && p[1] == '{') { c->p += 2; if (parse_interval(c, &mn, &mx) < 0) return -1; }
            else break;
            if (!c->ere && was_anchor && mn == 0 && mx == -1) { c->p--; break; }   /* BRE: '*' after a leading '^' is literal */
            int r = rc_node(c, R_REP); if (c->err) return -1;
            c->nodes[r].a = atom; c->nodes[r].min = mn; c->nodes[r].max = mx; atom = r;
        }
        if (head < 0) head = atom;
        else { int cat = rc_node(c, R_CAT); if (c->err) return -1; c->nodes[cat].a = head; c->nodes[cat].b = atom; head = cat; }
        start = was_anchor && !c->ere;      /* BRE: '*' directly after a leading '^' is literal */
        if (c->nodes[atom].type == R_BOL && !c->ere) start = 1; else start = 0;
    }
    if (head < 0) { head = rc_node(c, R_EMPTY); if (c->err) return -1; }
    return head;
}
static int parse_alt(RC* c)
{
    int left = parse_cat(c); if (c->err) return -1;
    while (at_alt_op(c))
    {
        c->p += c->ere ? 1 : 2;
        int right = parse_cat(c); if (c->err) return -1;
        int alt = rc_node(c, R_ALT); if (c->err) return -1;
        c->nodes[alt].a = left; c->nodes[alt].b = right; left = alt;
    }
    return left;
}

static int nullable(const RC* c, int n)
{
    const RNode* r = &c->nodes[n];
    switch (r->type)
    {
        case R_CHAR: case R_ANY: case R_SET: return 0;
        case R_CAT: return nullable(c, r->a) && nullable(c, r->b);
        case R_ALT: return nullable(c, r->a) || nullable(c, r->b);
        case R_REP: return r->min == 0 || nullable(c, r->a);
        case R_GROUP: return nullable(c, r->a);
        default: return 1;
    }
}
static int emit_inst(Prog* p, int* err, int op, int x, int y, int z)
{
    if (p->n >= RE_MAX_INST) { *err = REG_ESPACE; return -1; }
    if (p->n == p->cap)
    {
        int nc = p->cap ? p->cap * 2 : 64; Inst* ni = (Inst*)realloc(p->ins, (size_t)nc * sizeof(Inst));
        if (!ni) { *err = REG_ESPACE; return -1; }
        p->ins = ni; p->cap = nc;
    }
    p->ins[p->n].op = op; p->ins[p->n].x = x; p->ins[p->n].y = y; p->ins[p->n].z = z;
    return p->n++;
}
static void emit_node(RC* c, Prog* p, int n)
{
    if (c->err) return;
    const RNode* r = &c->nodes[n];
    switch (r->type)
    {
        case R_EMPTY: break;
        case R_CHAR: emit_inst(p, &c->err, I_CHAR, c->icase ? lc(r->ch) : r->ch, 0, 0); break;
        case R_ANY: emit_inst(p, &c->err, I_ANY, 0, 0, 0); break;
        case R_SET: emit_inst(p, &c->err, I_SET, r->idx, 0, 0); break;
        case R_BOL: emit_inst(p, &c->err, I_BOL, 0, 0, 0); break;
        case R_EOL: emit_inst(p, &c->err, I_EOL, 0, 0, 0); break;
        case R_WORDB: emit_inst(p, &c->err, I_WORDB, 0, 0, 0); break;
        case R_NWORDB: emit_inst(p, &c->err, I_NWORDB, 0, 0, 0); break;
        case R_WBEG: emit_inst(p, &c->err, I_WBEG, 0, 0, 0); break;
        case R_WEND: emit_inst(p, &c->err, I_WEND, 0, 0, 0); break;
        case R_BUFB: emit_inst(p, &c->err, I_BUFB, 0, 0, 0); break;
        case R_BUFE: emit_inst(p, &c->err, I_BUFE, 0, 0, 0); break;
        case R_BACKREF: emit_inst(p, &c->err, I_BACKREF, r->idx, 0, 0); break;
        case R_CAT: emit_node(c, p, r->a); emit_node(c, p, r->b); break;
        case R_GROUP:
            emit_inst(p, &c->err, I_SAVE, 2 * r->idx, 0, 0); emit_node(c, p, r->a); emit_inst(p, &c->err, I_SAVE, 2 * r->idx + 1, 0, 0); break;
        case R_ALT:
        {
            int sp = emit_inst(p, &c->err, I_SPLIT, 0, 0, p->nsplit++); if (sp < 0) return;
            p->ins[sp].x = p->n; emit_node(c, p, r->a);
            int j = emit_inst(p, &c->err, I_JMP, 0, 0, 0); if (j < 0) return;
            if (c->err) return;
            p->ins[sp].y = p->n; emit_node(c, p, r->b);
            if (c->err) return;
            p->ins[j].x = p->n; break;
        }
        case R_REP:
        {
            for (int i = 0; i < r->min && !c->err; ++i) emit_node(c, p, r->a);
            if (c->err) return;
            if (r->max < 0)
            {
                int null = nullable(c, r->a); int reg = -1;
                if (null && p->nlreg < 256) reg = p->nlreg++;
                int L = p->n; int sp = emit_inst(p, &c->err, I_SPLIT, 0, 0, p->nsplit++); if (sp < 0) return;
                p->ins[sp].x = p->n;
                if (reg >= 0) emit_inst(p, &c->err, I_LSET, reg, 0, 0);
                emit_node(c, p, r->a);
                if (reg >= 0) emit_inst(p, &c->err, I_LCHK, reg, 0, 0);
                emit_inst(p, &c->err, I_JMP, L, 0, 0);
                if (c->err) return;
                p->ins[sp].y = p->n;
            }
            else
            {
                int cnt = r->max - r->min; int sps[RE_DUP_MAX + 1];
                for (int i = 0; i < cnt && !c->err; ++i)
                {
                    sps[i] = emit_inst(p, &c->err, I_SPLIT, 0, 0, p->nsplit++); if (sps[i] < 0) return;
                    p->ins[sps[i]].x = p->n; emit_node(c, p, r->a);
                }
                if (c->err) return;
                for (int i = 0; i < cnt; ++i) p->ins[sps[i]].y = p->n;
            }
            break;
        }
    }
}

int regcomp(regex_t* re, const char* pattern, int cflags)
{
    RC c; memset(&c, 0, sizeof(c));
    c.p = pattern; c.ere = (cflags & REG_EXTENDED) != 0; c.icase = (cflags & REG_ICASE) != 0; c.newline = (cflags & REG_NEWLINE) != 0;
    c.nodes = (RNode*)malloc(sizeof(RNode) * RE_MAX_NODES);
    Prog* p = (Prog*)calloc(1, sizeof(Prog));
    int rc = 0;
    if (!c.nodes || !p) { rc = REG_ESPACE; goto done; }
    {
        int root = parse_alt(&c);
        if (!c.err && *c.p) c.err = REG_EPAREN;        /* an unmatched closing parenthesis stopped the parse */
        if (c.err) { rc = c.err; goto done; }
        emit_node(&c, p, root);
        if (!c.err) emit_inst(p, &c.err, I_MATCH, 0, 0, 0);
        if (c.err) { rc = c.err; goto done; }
    }
    p->sets = c.sets; p->nsets = c.nsets; p->nsub = c.nsub; p->cflags = cflags; p->has_backref = c.has_backref;
    c.sets = 0;
    re->__prog = p; re->re_nsub = (size_t)c.nsub; p = 0;
done:
    free(c.nodes); free(c.sets);
    if (p) { free(p->ins); free(p->sets); free(p); }
    return rc;
}

struct BT { int kind; int a; int b; };
static int re_wordat(const char* s, int len, int i) { return i >= 0 && i < len && is_word_c((unsigned char)s[i]); }

/* Runs the program from `start`. Returns 1 on match (best/bestcaps filled), 0 none, -1 resource limit. */
static int vm_run(const Prog* p, const char* s, int len, int start, int eflags, int* caps, int* best, int* bestcaps, uint8_t* memo, int* lreg)
{
    const int ncap = 2 * (p->nsub + 1);
    int cap_bt = 256, nbt = 0; BT* bt = (BT*)malloc(sizeof(BT) * (size_t)cap_bt);
    if (!bt) return -1;
    for (int i = 0; i < ncap; ++i) caps[i] = -1;
    *best = -1;
    long steps = 0; const long limit = 4000000L;
    int pc = 0, sp = start, result = 0;
    int nl = (p->cflags & REG_NEWLINE) != 0, icase = (p->cflags & REG_ICASE) != 0;
    for (;;)
    {
        int fail = 0;
        if (++steps > limit && !memo) { result = -1; break; }
        const Inst* in = &p->ins[pc];
        switch (in->op)
        {
            case I_CHAR: if (sp < len && (icase ? lc((unsigned char)s[sp]) : (unsigned char)s[sp]) == in->x) { ++sp; ++pc; } else fail = 1; break;
            case I_ANY: if (sp < len && !(nl && s[sp] == '\n')) { ++sp; ++pc; } else fail = 1; break;
            case I_SET: if (sp < len && set_has(p->sets[in->x], (unsigned char)s[sp])) { ++sp; ++pc; } else fail = 1; break;
            case I_BOL: if (sp == 0 ? !(eflags & REG_NOTBOL) : (nl && s[sp - 1] == '\n')) ++pc; else fail = 1; break;
            case I_EOL: if (sp == len ? !(eflags & REG_NOTEOL) : (nl && s[sp] == '\n')) ++pc; else fail = 1; break;
            case I_BUFB: if (sp == 0) ++pc; else fail = 1; break;
            case I_BUFE: if (sp == len) ++pc; else fail = 1; break;
            case I_WORDB: case I_NWORDB: case I_WBEG: case I_WEND:
            {
                int a = re_wordat(s, len, sp - 1), b = re_wordat(s, len, sp), ok;
                if (in->op == I_WORDB) ok = a != b; else if (in->op == I_NWORDB) ok = a == b; else if (in->op == I_WBEG) ok = !a && b; else ok = a && !b;
                if (ok) ++pc; else fail = 1; break;
            }
            case I_JMP: pc = in->x; break;
            case I_SPLIT:
                if (memo)
                {
                    size_t bit = (size_t)in->z * (size_t)(len + 1) + (size_t)sp;
                    if (memo[bit >> 3] & (1U << (bit & 7))) { fail = 1; break; }
                    memo[bit >> 3] |= (uint8_t)(1U << (bit & 7));
                }
                if (nbt + 1 >= cap_bt) { if (cap_bt >= (1 << 22)) { result = -1; goto out; } cap_bt *= 2; BT* nb = (BT*)realloc(bt, sizeof(BT) * (size_t)cap_bt); if (!nb) { result = -1; goto out; } bt = nb; }
                bt[nbt].kind = 0; bt[nbt].a = in->y; bt[nbt].b = sp; ++nbt; pc = in->x; break;
            case I_SAVE:
                if (nbt + 1 >= cap_bt) { if (cap_bt >= (1 << 22)) { result = -1; goto out; } cap_bt *= 2; BT* nb = (BT*)realloc(bt, sizeof(BT) * (size_t)cap_bt); if (!nb) { result = -1; goto out; } bt = nb; }
                bt[nbt].kind = 1; bt[nbt].a = in->x; bt[nbt].b = caps[in->x]; ++nbt; caps[in->x] = sp; ++pc; break;
            case I_LSET:
                if (memo) { ++pc; break; }
                if (nbt + 1 >= cap_bt) { if (cap_bt >= (1 << 22)) { result = -1; goto out; } cap_bt *= 2; BT* nb = (BT*)realloc(bt, sizeof(BT) * (size_t)cap_bt); if (!nb) { result = -1; goto out; } bt = nb; }
                bt[nbt].kind = 2; bt[nbt].a = in->x; bt[nbt].b = lreg[in->x]; ++nbt; lreg[in->x] = sp; ++pc; break;
            case I_LCHK: if (!memo && lreg[in->x] == sp) fail = 1; else ++pc; break;
            case I_BACKREF:
            {
                int g = in->x, so = caps[2 * g], eo = caps[2 * g + 1];
                if (so < 0 || eo < so) { fail = 1; break; }
                int l = eo - so;
                if (sp + l > len) { fail = 1; break; }
                int k = 0;
                for (; k < l; ++k) { int x = (unsigned char)s[so + k], y = (unsigned char)s[sp + k]; if (icase) { x = lc(x); y = lc(y); } if (x != y) break; }
                if (k < l) fail = 1; else { sp += l; ++pc; }
                break;
            }
            case I_MATCH:
                if (sp > *best) { *best = sp; for (int i = 0; i < ncap; ++i) bestcaps[i] = caps[i]; bestcaps[0] = start; bestcaps[1] = sp; }
                if (sp == len) { result = 1; goto out; }
                fail = 1; break;
        }
        if (!fail) continue;
        for (;;)
        {
            if (nbt == 0) { result = *best >= 0 ? 1 : 0; goto out; }
            BT e = bt[--nbt];
            if (e.kind == 1) caps[e.a] = e.b;
            else if (e.kind == 2) lreg[e.a] = e.b;
            else { pc = e.a; sp = e.b; break; }
        }
    }
out:
    if (result == 0 && *best >= 0) result = 1;
    free(bt);
    return result;
}

int regexec(const regex_t* re, const char* s, size_t nmatch, regmatch_t pm[], int eflags)
{
    const Prog* p = (const Prog*)re->__prog; if (!p) return REG_BADPAT;
    int len = (int)strlen(s), ncap = 2 * (p->nsub + 1);
    int* caps = (int*)malloc(sizeof(int) * (size_t)ncap * 2 + sizeof(int) * 256);
    if (!caps) return REG_ESPACE;
    int* bestcaps = caps + ncap; int* lreg = bestcaps + ncap;
    uint8_t* memo = 0;
    if (!p->has_backref && p->nsplit > 0)
    {
        size_t bits = (size_t)p->nsplit * (size_t)(len + 1);
        if (bits <= (1UL << 24)) { memo = (uint8_t*)calloc((bits + 7) / 8, 1); }
    }
    int found = 0, best = -1, rc = 0;
    for (int start = 0; start <= len; ++start)
    {
        for (int i = 0; i < 256; ++i) lreg[i] = -1;
        int r = vm_run(p, s, len, start, eflags, caps, &best, bestcaps, memo, lreg);
        if (r < 0) { rc = REG_ESPACE; break; }
        if (r > 0) { found = 1; break; }
    }
    free(memo);
    if (rc) { free(caps); return rc; }
    if (!found) { free(caps); return REG_NOMATCH; }
    if (!(p->cflags & REG_NOSUB) && pm)
        for (size_t i = 0; i < nmatch; ++i)
        {
            if ((int)i <= p->nsub && bestcaps[2 * i] >= 0 && bestcaps[2 * i + 1] >= bestcaps[2 * i]) { pm[i].rm_so = bestcaps[2 * i]; pm[i].rm_eo = bestcaps[2 * i + 1]; }
            else { pm[i].rm_so = -1; pm[i].rm_eo = -1; }
        }
    free(caps);
    return 0;
}
size_t regerror(int code, const regex_t* re, char* buf, size_t cap)
{
    (void)re;
    static const char* const msg[] = {"Success", "No match", "Invalid regular expression", "Invalid collation character", "Invalid character class name",
        "Trailing backslash", "Invalid back reference", "Unmatched [, [^, [:, [., or [=", "Unmatched ( or \\(", "Unmatched \\{", "Invalid content of \\{\\}",
        "Invalid range end", "Memory exhausted", "Invalid preceding regular expression"};
    const char* m = (code >= 0 && code < 14) ? msg[code] : "Unknown error";
    size_t l = strlen(m) + 1;
    if (cap) { size_t c = l < cap ? l : cap; memcpy(buf, m, c - 1); buf[c - 1] = 0; }
    return l;
}
void regfree(regex_t* re)
{
    Prog* p = (Prog*)re->__prog; if (!p) return;
    free(p->ins); free(p->sets); free(p); re->__prog = 0; re->re_nsub = 0;
}
