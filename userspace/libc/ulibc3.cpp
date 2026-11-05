/* RioOS libc part 3: floating-point printf/strtod, full scanf family, locale, time, environment,
   extra ctype/stdlib, aligned allocation. Everything uses the soft-float core (no FPU). */
#include "ulibc.h"

extern "C" {

/* math functions from ulibm.cpp / softfloat.cpp */
double pow(double, double);
double fabs(double);
double floor(double);
double ldexp(double, int);

static inline unsigned long long d2u(double d) { unsigned long long r; __builtin_memcpy(&r, &d, 8); return r; }
static inline double u2d(unsigned long long r) { double d; __builtin_memcpy(&d, &r, 8); return d; }

/* ===================== exact double -> decimal ===================== */
#define DBUF 1700
static char dig_buf[DBUF];     /* all exact decimal digits: integer part then fraction */

/* Produces the exact decimal expansion of |v| (finite). *int_len = number of integer digits (>=1). Returns total length. */
static int exact_digits(double v, int* int_len)
{
    unsigned long long bits = d2u(v) & 0x7FFFFFFFFFFFFFFFULL;
    int e = (int)(bits >> 52);
    unsigned long long m = bits & 0xFFFFFFFFFFFFFULL;
    if (e) m |= 1ULL << 52; else e = 1;
    int e2 = e - 1075;
    int n = 0;
    static unsigned int limb[40];
    if (e2 >= 0)
    {
        int nl = 0; for (int i = 0; i < 40; ++i) limb[i] = 0;
        /* limbs = m << e2 */
        int ws = e2 / 32, bs = e2 % 32;
        unsigned long long lo = m << bs;
        unsigned long long hi = bs ? (m >> (64 - bs)) : 0;
        limb[ws] = (unsigned int)lo; limb[ws + 1] = (unsigned int)(lo >> 32); limb[ws + 2] = (unsigned int)hi;
        nl = ws + 3; while (nl > 0 && !limb[nl - 1]) --nl;
        char tmp[330]; int tn = 0;
        if (nl == 0) tmp[tn++] = '0';
        while (nl > 0)
        {
            unsigned long long rem = 0;
            for (int i = nl - 1; i >= 0; --i) { unsigned long long cur = (rem << 32) | limb[i]; limb[i] = (unsigned int)(cur / 1000000000ULL); rem = cur % 1000000000ULL; }
            while (nl > 0 && !limb[nl - 1]) --nl;
            for (int k = 0; k < 9; ++k) { tmp[tn++] = (char)('0' + rem % 10); rem /= 10; if (nl == 0 && rem == 0) break; }
        }
        while (tn > 1 && tmp[tn - 1] == '0') --tn;
        for (int i = tn - 1; i >= 0; --i) dig_buf[n++] = tmp[i];
        *int_len = n;
        return n;
    }
    int sh = -e2;
    unsigned long long ip = sh >= 64 ? 0 : (m >> sh);
    char tmp[24]; int tn = 0;
    if (!ip) tmp[tn++] = '0';
    while (ip) { tmp[tn++] = (char)('0' + ip % 10); ip /= 10; }
    for (int i = tn - 1; i >= 0; --i) dig_buf[n++] = tmp[i];
    *int_len = n;
    /* fraction F = m & (2^sh - 1); each step F *= 10, digit = F >> sh, F &= 2^sh - 1 */
    int wi = sh / 32, bi = sh % 32, nl = wi + 2;
    for (int i = 0; i < 40; ++i) limb[i] = 0;
    if (sh >= 64) { limb[0] = (unsigned int)m; limb[1] = (unsigned int)(m >> 32); }
    else { unsigned long long f = m & ((1ULL << sh) - 1); limb[0] = (unsigned int)f; limb[1] = (unsigned int)(f >> 32); }
    for (;;)
    {
        int zero = 1; for (int i = 0; i < nl; ++i) if (limb[i]) { zero = 0; break; }
        if (zero || n >= DBUF - 2) break;
        unsigned long long carry = 0;
        for (int i = 0; i < nl; ++i) { unsigned long long cur = (unsigned long long)limb[i] * 10ULL + carry; limb[i] = (unsigned int)cur; carry = cur >> 32; }
        unsigned long long two = (unsigned long long)limb[wi] | ((unsigned long long)limb[wi + 1] << 32);
        dig_buf[n++] = (char)('0' + (unsigned int)((two >> bi) & 0xF));
        limb[wi] &= bi ? ((1u << bi) - 1) : 0u;
        limb[wi + 1] = 0;
    }
    return n;
}

/* Round the digit string in place at keep digits (digits before index keep stay). Returns 1 if a carry added a leading digit. */
static int round_digits(char* d, int len, int keep)
{
    if (keep >= len) return 0;
    int up = 0;
    if (keep < 0) return 0;
    char nx = d[keep];
    if (nx > '5') up = 1;
    else if (nx == '5')
    {
        int rest = 0; for (int i = keep + 1; i < len; ++i) if (d[i] != '0') { rest = 1; break; }
        if (rest) up = 1; else up = (keep > 0 && ((d[keep - 1] - '0') & 1)) ? 1 : 0;
    }
    if (up)
    {
        int i = keep - 1;
        while (i >= 0) { if (d[i] == '9') { d[i] = '0'; --i; } else { ++d[i]; break; } }
        if (i < 0) return 1;
    }
    return 0;
}

/* Format value into out (no padding). conv: f F e E g G. Returns length. */
int rio_fmt_double(char* out, int cap, double v, char conv, int prec, int plus, int space, int alt)
{
    char* o = out; int n = 0;
#define PUT(c) do { if (n + 1 < cap) o[n] = (c); ++n; } while (0)
    int neg = (int)(d2u(v) >> 63);
    int upper = (conv == 'F' || conv == 'E' || conv == 'G');
    char lc = (char)(conv | 0x20);
    if (neg) PUT('-'); else if (plus) PUT('+'); else if (space) PUT(' ');
    unsigned long long bits = d2u(v) & 0x7FFFFFFFFFFFFFFFULL;
    if (bits >= 0x7FF0000000000000ULL)
    {
        const char* s = bits == 0x7FF0000000000000ULL ? (upper ? "INF" : "inf") : (upper ? "NAN" : "nan");
        if (bits != 0x7FF0000000000000ULL && neg) { /* sign of NaN not shown */ }
        while (*s) PUT(*s++);
        if (cap > 0) o[n < cap ? n : cap - 1] = 0;
        return n;
    }
    if (prec < 0) prec = 6;
    int ilen; int len = exact_digits(v, &ilen);
    char* d = dig_buf;
    /* decimal exponent of first significant digit */
    int first = 0; while (first < len && d[first] == '0') ++first;
    int is_zero = (first == len);
    int dexp = is_zero ? 0 : (ilen - 1 - first);       /* value = d[first].d[first+1]... * 10^dexp */
    if (lc == 'g')
    {
        int P = prec == 0 ? 1 : prec;
        int x = dexp;
        if (!is_zero)
        {
            /* exponent after rounding to P significant digits */
            static char tmpd[DBUF]; for (int i = 0; i < len; ++i) tmpd[i] = d[i];
            int carry = round_digits(tmpd + first, len - first, P);
            if (carry) ++x;
        }
        if (x < -4 || x >= P) { lc = 'e'; prec = P - 1; }
        else { lc = 'f'; prec = P - 1 - x; }
        /* strip zeros handled below via flag */
        int strip = !alt;
        /* format recursively through the common code with a strip flag */
        if (lc == 'f')
        {
            int keep = ilen + prec;
            char tmp[DBUF + 4]; int tl = len; for (int i = 0; i < len; ++i) tmp[i + 1] = d[i]; tmp[0] = '0'; tl = len + 1; ++keep;
            int c = round_digits(tmp, tl, keep); (void)c;
            int il = ilen + 1;                       /* with the extra leading digit */
            int s0 = 0; while (s0 < il - 1 && tmp[s0] == '0') ++s0;
            for (int i = s0; i < il; ++i) PUT(tmp[i]);
            int fl = prec;
            if (strip) { while (fl > 0 && (il + fl - 1 < tl ? tmp[il + fl - 1] : '0') == '0') --fl; }
            if (fl > 0 || alt) PUT('.');
            for (int i = 0; i < fl; ++i) PUT(il + i < tl ? tmp[il + i] : '0');
        }
        else
        {
            char tmp[DBUF + 4]; int tl = len - first; for (int i = 0; i < tl; ++i) tmp[i + 1] = d[first + i]; tmp[0] = '0'; ++tl;
            int c = round_digits(tmp, tl, 1 + prec + 1);
            int st = 1; int ex = dexp;
            if (c || tmp[0] != '0') { st = 0; if (c) ++ex; }
            if (is_zero) { st = 1; ex = 0; }
            PUT(tmp[st]);
            int fl = prec;
            if (strip) { while (fl > 0 && (st + fl < tl ? tmp[st + fl] : '0') == '0') --fl; }
            if (fl > 0 || alt) PUT('.');
            for (int i = 0; i < fl; ++i) PUT(st + 1 + i < tl ? tmp[st + 1 + i] : '0');
            PUT(upper ? 'E' : 'e'); PUT(ex < 0 ? '-' : '+'); int ax = ex < 0 ? -ex : ex;
            if (ax >= 100) PUT((char)('0' + ax / 100)); PUT((char)('0' + (ax / 10) % 10)); PUT((char)('0' + ax % 10));
        }
        if (cap > 0) o[n < cap ? n : cap - 1] = 0;
        return n;
    }
    if (lc == 'f')
    {
        static char tmp[DBUF + 4];
        tmp[0] = '0'; for (int i = 0; i < len; ++i) tmp[i + 1] = d[i];
        int tl = len + 1, il = ilen + 1;
        round_digits(tmp, tl, il + prec);
        int s0 = 0; while (s0 < il - 1 && tmp[s0] == '0') ++s0;
        for (int i = s0; i < il; ++i) PUT(tmp[i]);
        if (prec > 0 || alt) PUT('.');
        for (int i = 0; i < prec; ++i) PUT(il + i < tl ? tmp[il + i] : '0');
    }
    else   /* e */
    {
        static char tmp[DBUF + 4];
        int tl = is_zero ? 1 : len - first;
        tmp[0] = '0'; for (int i = 0; i < tl; ++i) tmp[i + 1] = is_zero ? '0' : d[first + i];
        ++tl;
        int c = round_digits(tmp, tl, 1 + prec + 1);
        int st = 1, ex = dexp;
        if (c || tmp[0] != '0') { st = 0; if (c) ++ex; }
        if (is_zero) ex = 0;
        PUT(tmp[st]);
        if (prec > 0 || alt) PUT('.');
        for (int i = 0; i < prec; ++i) PUT(st + 1 + i < tl ? tmp[st + 1 + i] : '0');
        PUT(upper ? 'E' : 'e'); PUT(ex < 0 ? '-' : '+'); int ax = ex < 0 ? -ex : ex;
        if (ax >= 100) PUT((char)('0' + ax / 100)); PUT((char)('0' + (ax / 10) % 10)); PUT((char)('0' + ax % 10));
    }
    if (cap > 0) o[n < cap ? n : cap - 1] = 0;
    return n;
#undef PUT
}

/* ===================== strtod ===================== */
static double pow10_i(int e)
{
    double r = 1.0, b = 10.0; int neg = e < 0; if (neg) e = -e;
    while (e) { if (e & 1) r *= b; b *= b; e >>= 1; }
    return neg ? 1.0 / r : r;
}

double strtod(const char* s, char** end)
{
    const char* p = s;
    while (isspace((unsigned char)*p)) ++p;
    int neg = 0;
    if (*p == '-' || *p == '+') { neg = *p == '-'; ++p; }
    double result;
    if ((p[0] == 'i' || p[0] == 'I') && (p[1] == 'n' || p[1] == 'N') && (p[2] == 'f' || p[2] == 'F'))
    {
        p += 3;
        if ((p[0] == 'i' || p[0] == 'I') && (p[1] == 'n' || p[1] == 'N') && (p[2] == 'i' || p[2] == 'I') && (p[3] == 't' || p[3] == 'T') && (p[4] == 'y' || p[4] == 'Y')) p += 5;
        if (end) *end = (char*)p;
        return u2d(neg ? 0xFFF0000000000000ULL : 0x7FF0000000000000ULL);
    }
    if ((p[0] == 'n' || p[0] == 'N') && (p[1] == 'a' || p[1] == 'A') && (p[2] == 'n' || p[2] == 'N'))
    {
        if (end) *end = (char*)(p + 3);
        return u2d(0x7FF8000000000000ULL);
    }
    unsigned long long w = 0; int ndig = 0, dexp = 0, any = 0, dropped_nz = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && (isxdigit((unsigned char)p[2]) || (p[2] == '.' && isxdigit((unsigned char)p[3]))))
    {
        p += 2; double m = 0.0; int bexp = 0, seen_dot = 0;
        for (;; ++p)
        {
            if (*p == '.' && !seen_dot) { seen_dot = 1; continue; }
            if (!isxdigit((unsigned char)*p)) break;
            int dv = isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10);
            m = m * 16.0 + (double)dv; if (seen_dot) bexp -= 4;
        }
        if (*p == 'p' || *p == 'P')
        {
            const char* q = p + 1; int en = 0, ng = 0;
            if (*q == '-' || *q == '+') { ng = *q == '-'; ++q; }
            if (isdigit((unsigned char)*q)) { while (isdigit((unsigned char)*q)) { if (en < 100000) en = en * 10 + (*q - '0'); ++q; } bexp += ng ? -en : en; p = q; }
        }
        if (end) *end = (char*)p;
        result = ldexp(m, bexp);
        return neg ? -result : result;
    }
    for (;; ++p)
    {
        if (*p >= '0' && *p <= '9')
        {
            any = 1;
            if (ndig < 19) { if (w || *p != '0') { w = w * 10 + (unsigned long long)(*p - '0'); ++ndig; } }
            else { ++dexp; if (*p != '0') dropped_nz = 1; }
        }
        else break;
    }
    if (*p == '.')
    {
        ++p;
        for (;; ++p)
        {
            if (*p >= '0' && *p <= '9')
            {
                any = 1;
                if (ndig < 19) { if (w || *p != '0') { w = w * 10 + (unsigned long long)(*p - '0'); ++ndig; } --dexp; }
                else if (*p != '0') dropped_nz = 1;
            }
            else break;
        }
    }
    if (!any) { if (end) *end = (char*)s; return 0.0; }
    if (*p == 'e' || *p == 'E')
    {
        const char* q = p + 1; int en = 0, ng = 0;
        if (*q == '-' || *q == '+') { ng = *q == '-'; ++q; }
        if (isdigit((unsigned char)*q)) { while (isdigit((unsigned char)*q)) { if (en < 100000) en = en * 10 + (*q - '0'); ++q; } dexp += ng ? -en : en; p = q; }
    }
    if (end) *end = (char*)p;
    (void)dropped_nz;
    if (w == 0) return neg ? -0.0 : 0.0;
    result = (double)w;
    if (dexp > 0 && dexp <= 22 + 0 && w < (1ULL << 53)) result *= pow10_i(dexp);
    else if (dexp < 0 && dexp >= -22 && w < (1ULL << 53)) result /= pow10_i(-dexp);
    else if (dexp != 0)
    {
        if (dexp > 309) result = u2d(0x7FF0000000000000ULL);
        else if (dexp < -340) result = 0.0;
        else if (dexp < -300) { result = (result * pow(10.0, (double)(dexp + 300))) * pow(10.0, -300.0); }
        else result = result * pow(10.0, (double)dexp);
    }
    return neg ? -result : result;
}
double atof(const char* s) { return strtod(s, 0); }
float strtof(const char* s, char** end) { return (float)strtod(s, end); }

/* ===================== scanf engine ===================== */
struct Src { const char* s; FILE* f; int pos; };
static int sget(Src* r) { if (r->f) { int c = fgetc(r->f); if (c >= 0) ++r->pos; return c; } int c = (unsigned char)r->s[r->pos]; if (!c) return -1; ++r->pos; return c; }
static void sunget(Src* r, int c) { if (c < 0) return; --r->pos; if (r->f) ungetc(c, r->f); }

static int scan_engine(Src* r, const char* fmt, va_list ap)
{
    int count = 0, anyread = 0;
    for (; *fmt; ++fmt)
    {
        if (isspace((unsigned char)*fmt)) { int c; while ((c = sget(r)) >= 0 && isspace(c)) {} sunget(r, c); continue; }
        if (*fmt != '%')
        {
            int c = sget(r);
            if (c < 0) return count ? count : (anyread ? count : -1);
            if (c != (unsigned char)*fmt) { sunget(r, c); return count; }
            anyread = 1; continue;
        }
        ++fmt;
        int suppress = 0; if (*fmt == '*') { suppress = 1; ++fmt; }
        int width = 0; while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        int lng = 0, shrt = 0; int isL = 0;
        for (;; ++fmt) { if (*fmt == 'l') ++lng; else if (*fmt == 'h') ++shrt; else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') lng = 1; else if (*fmt == 'L') isL = 1; else break; }
        char c = *fmt; if (!c) break;
        if (c == '%') { int ch = sget(r); if (ch != '%') { sunget(r, ch); return count; } continue; }
        if (c == 'n') { if (!suppress) { int* p = va_arg(ap, int*); *p = r->pos; } continue; }
        if (c != 'c' && c != '[') { int ch; while ((ch = sget(r)) >= 0 && isspace(ch)) {} sunget(r, ch); }
        int ch = sget(r);
        if (ch < 0) return count ? count : -1;
        sunget(r, ch);
        anyread = 1;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o' || c == 'p')
        {
            int base = (c == 'x' || c == 'X' || c == 'p') ? 16 : (c == 'o' ? 8 : 10); if (c == 'i') base = 0;
            int neg = 0, used = 0, any = 0; unsigned long long v = 0; int w = width ? width : 1 << 30;
            ch = sget(r);
            if ((ch == '-' || ch == '+') && used < w) { neg = ch == '-'; ++used; ch = sget(r); }
            if (ch == '0' && used < w && (base == 0 || base == 16))
            {
                int c2 = sget(r);
                if ((c2 == 'x' || c2 == 'X') && used + 1 < w) { base = 16; used += 2; ch = sget(r); }
                else { sunget(r, c2); if (base == 0) base = 8; any = 1; ++used; v = 0; ch = sget(r); }
            }
            if (base == 0) base = 10;
            while (ch >= 0 && used < w)
            {
                int dv;
                if (ch >= '0' && ch <= '9') dv = ch - '0'; else if (ch >= 'a' && ch <= 'f') dv = ch - 'a' + 10; else if (ch >= 'A' && ch <= 'F') dv = ch - 'A' + 10; else break;
                if (dv >= base) break;
                v = v * (unsigned long long)base + (unsigned long long)dv; any = 1; ++used; ch = sget(r);
            }
            sunget(r, ch);
            if (!any) return count;
            long long sv = neg ? -(long long)v : (long long)v;
            if (!suppress)
            {
                if (c == 'p') *va_arg(ap, void**) = (void*)(unsigned long)v;
                else if (c == 'd' || c == 'i')
                {
                    if (lng >= 2) *va_arg(ap, long long*) = sv; else if (lng == 1) *va_arg(ap, long*) = (long)sv;
                    else if (shrt >= 2) *va_arg(ap, signed char*) = (signed char)sv; else if (shrt == 1) *va_arg(ap, short*) = (short)sv; else *va_arg(ap, int*) = (int)sv;
                }
                else
                {
                    unsigned long long uv = neg ? (unsigned long long)(-(long long)v) : v;
                    if (lng >= 2) *va_arg(ap, unsigned long long*) = uv; else if (lng == 1) *va_arg(ap, unsigned long*) = (unsigned long)uv;
                    else if (shrt >= 2) *va_arg(ap, unsigned char*) = (unsigned char)uv; else if (shrt == 1) *va_arg(ap, unsigned short*) = (unsigned short)uv; else *va_arg(ap, unsigned*) = (unsigned)uv;
                }
                ++count;
            }
        }
        else if (c == 'f' || c == 'e' || c == 'g' || c == 'E' || c == 'G' || c == 'a')
        {
            char buf[400]; int n = 0; int w = width ? width : 380; if (w > 380) w = 380;
            ch = sget(r);
            if ((ch == '-' || ch == '+') && n < w) { buf[n++] = (char)ch; ch = sget(r); }
            int seen_e = 0, seen_dot = 0, digs = 0;
            while (ch >= 0 && n < w)
            {
                if (isdigit(ch)) { ++digs; buf[n++] = (char)ch; }
                else if (ch == '.' && !seen_dot && !seen_e) { seen_dot = 1; buf[n++] = '.'; }
                else if ((ch == 'e' || ch == 'E') && digs && !seen_e) { seen_e = 1; buf[n++] = 'e'; int nx = sget(r); if (nx == '-' || nx == '+') { buf[n++] = (char)nx; } else sunget(r, nx); }
                else if (!digs && (ch == 'i' || ch == 'I' || ch == 'n' || ch == 'N') && n < w - 2) { buf[n++] = (char)ch; }
                else if (!digs && n > 0 && isalpha(ch) && n < w) { buf[n++] = (char)ch; }
                else break;
                ch = sget(r);
            }
            sunget(r, ch);
            buf[n] = 0;
            char* e2; double dv = strtod(buf, &e2);
            if (e2 == buf) return count;
            if (!suppress)
            {
                if (lng >= 1 || isL) *va_arg(ap, double*) = dv; else *va_arg(ap, float*) = (float)dv;
                ++count;
            }
        }
        else if (c == 's')
        {
            char* d = suppress ? 0 : va_arg(ap, char*); int k = 0; int w = width ? width : 1 << 30;
            ch = sget(r);
            while (ch >= 0 && !isspace(ch) && k < w) { if (d) d[k] = (char)ch; ++k; ch = sget(r); }
            sunget(r, ch);
            if (d) d[k] = 0;
            if (!suppress) ++count;
        }
        else if (c == 'c')
        {
            char* d = suppress ? 0 : va_arg(ap, char*); int w = width ? width : 1;
            for (int k = 0; k < w; ++k) { ch = sget(r); if (ch < 0) return count ? count : -1; if (d) d[k] = (char)ch; }
            if (!suppress) ++count;
        }
        else if (c == '[')
        {
            ++fmt; int neg = 0; if (*fmt == '^') { neg = 1; ++fmt; }
            char set[256]; for (int i = 0; i < 256; ++i) set[i] = 0;
            int first = 1;
            while (*fmt && (*fmt != ']' || first))
            {
                first = 0;
                if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') { for (int x = (unsigned char)fmt[0]; x <= (unsigned char)fmt[2]; ++x) set[x] = 1; fmt += 3; }
                else { set[(unsigned char)*fmt] = 1; ++fmt; }
            }
            char* d = suppress ? 0 : va_arg(ap, char*); int k = 0; int w = width ? width : 1 << 30;
            ch = sget(r);
            while (ch >= 0 && k < w && (set[ch] != 0) != neg) { if (d) d[k] = (char)ch; ++k; ch = sget(r); }
            sunget(r, ch);
            if (!k) return count;
            if (d) d[k] = 0;
            if (!suppress) ++count;
            if (!*fmt) break;
        }
        else return count;
    }
    return count;
}
int vsscanf(const char* s, const char* fmt, va_list ap) { Src r = { s, 0, 0 }; return scan_engine(&r, fmt, ap); }
int sscanf(const char* s, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = vsscanf(s, fmt, ap); va_end(ap); return n; }
int vfscanf(FILE* f, const char* fmt, va_list ap) { Src r = { 0, f, 0 }; return scan_engine(&r, fmt, ap); }
int fscanf(FILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfscanf(f, fmt, ap); va_end(ap); return n; }
int vscanf(const char* fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }
int scanf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfscanf(stdin, fmt, ap); va_end(ap); return n; }

/* getline / getdelim */
ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* f)
{
    if (!lineptr || !n || !f) { errno = EINVAL; return -1; }
    if (!*lineptr || *n < 16) { char* p = (char*)realloc(*lineptr, 128); if (!p) return -1; *lineptr = p; *n = 128; }
    size_t len = 0; int c;
    while ((c = fgetc(f)) >= 0)
    {
        if (len + 2 > *n) { char* p = (char*)realloc(*lineptr, *n * 2); if (!p) return -1; *lineptr = p; *n *= 2; }
        (*lineptr)[len++] = (char)c;
        if (c == delim) break;
    }
    if (!len) return -1;
    (*lineptr)[len] = 0;
    return (ssize_t)len;
}
ssize_t getline(char** lineptr, size_t* n, FILE* f) { return getdelim(lineptr, n, '\n', f); }

/* ===================== locale ===================== */
static struct lconv c_lconv = { (char*)".", (char*)"", (char*)"", (char*)"", (char*)"", (char*)"", (char*)"", (char*)"", (char*)"", (char*)"", 127, 127, 127, 127, 127, 127, 127, 127 };
static char cur_locale[8] = "C";
char* setlocale(int category, const char* locale)
{
    (void)category;
    if (!locale) return cur_locale;
    if (!*locale || !strcmp(locale, "C") || !strcmp(locale, "POSIX")) { strcpy(cur_locale, "C"); return cur_locale; }
    return 0;      /* only the C/POSIX locale is available */
}
struct lconv* localeconv(void) { return &c_lconv; }

/* ===================== ctype / stdlib extras ===================== */
int isgraph(int c) { return c > 32 && c < 127; }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isblank(int c) { return c == ' ' || c == '\t'; }
int isascii(int c) { return c >= 0 && c < 128; }
int toascii(int c) { return c & 0x7F; }
div_t div(int a, int b) { div_t r; r.quot = a / b; r.rem = a % b; return r; }
ldiv_t ldiv(long a, long b) { ldiv_t r; r.quot = a / b; r.rem = a % b; return r; }
void* memmem(const void* h, size_t hl, const void* n, size_t nl)
{
    if (!nl) return (void*)h;
    const unsigned char* a = (const unsigned char*)h; const unsigned char* b = (const unsigned char*)n;
    for (size_t i = 0; i + nl <= hl; ++i) if (a[i] == b[0] && !memcmp(a + i, b, nl)) return (void*)(a + i);
    return 0;
}
char* strndup(const char* s, size_t n) { size_t l = strnlen(s, n); char* p = (char*)malloc(l + 1); if (p) { memcpy(p, s, l); p[l] = 0; } return p; }
char* strcasestr(const char* h, const char* n)
{
    size_t nl = strlen(n); if (!nl) return (char*)h;
    for (; *h; ++h) if (!strncasecmp(h, n, nl)) return (char*)h;
    return 0;
}
char* strsep(char** sp, const char* delim)
{
    char* s = *sp; if (!s) return 0;
    char* e = s + strcspn(s, delim);
    if (*e) { *e = 0; *sp = e + 1; } else *sp = 0;
    return s;
}
size_t strlcpy(char* d, const char* s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l >= n ? n - 1 : l; memcpy(d, s, c); d[c] = 0; } return l; }
size_t strlcat(char* d, const char* s, size_t n) { size_t dl = strnlen(d, n); if (dl == n) return n + strlen(s); return dl + strlcpy(d + dl, s, n - dl); }
void* mempcpy(void* d, const void* s, size_t n) { memcpy(d, s, n); return (char*)d + n; }
unsigned usleep(unsigned us) { sleep_ms((us + 999) / 1000); return 0; }
unsigned sleep(unsigned s) { sleep_ms(s * 1000); return 0; }

/* ===================== environment (process-local) ===================== */
#define ENV_MAX 64
static char* env_tab[ENV_MAX + 1];
char** environ = env_tab;
char* getenv(const char* name)
{
    size_t l = strlen(name);
    for (int i = 0; env_tab[i]; ++i) if (!strncmp(env_tab[i], name, l) && env_tab[i][l] == '=') return env_tab[i] + l + 1;
    return 0;
}
int setenv(const char* name, const char* value, int overwrite)
{
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    size_t l = strlen(name);
    for (int i = 0; env_tab[i]; ++i)
        if (!strncmp(env_tab[i], name, l) && env_tab[i][l] == '=')
        {
            if (!overwrite) return 0;
            char* e = (char*)malloc(l + strlen(value) + 2); if (!e) return -1;
            strcpy(e, name); e[l] = '='; strcpy(e + l + 1, value);
            free(env_tab[i]); env_tab[i] = e; return 0;
        }
    int n = 0; while (env_tab[n]) ++n;
    if (n >= ENV_MAX) { errno = ENOMEM; return -1; }
    char* e = (char*)malloc(l + strlen(value) + 2); if (!e) return -1;
    strcpy(e, name); e[l] = '='; strcpy(e + l + 1, value);
    env_tab[n] = e; env_tab[n + 1] = 0; return 0;
}
int unsetenv(const char* name)
{
    size_t l = strlen(name);
    for (int i = 0; env_tab[i]; ++i)
        if (!strncmp(env_tab[i], name, l) && env_tab[i][l] == '=')
        { free(env_tab[i]); for (int j = i; env_tab[j]; ++j) env_tab[j] = env_tab[j + 1]; return 0; }
    return 0;
}
int putenv(char* s)
{
    char* eq = strchr(s, '='); if (!eq) return unsetenv(s);
    char name[64]; size_t l = (size_t)(eq - s); if (l >= sizeof(name)) { errno = EINVAL; return -1; }
    memcpy(name, s, l); name[l] = 0; return setenv(name, eq + 1, 1);
}

/* ===================== time ===================== */
time_t time(time_t* t) { time_t v = (time_t)time_epoch(); if (t) *t = v; return v; }
clock_t clock(void) { return (clock_t)clock_ms() * (CLOCKS_PER_SEC / 1000); }
double difftime(time_t a, time_t b) { return (double)(a - b); }
static int is_leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static struct tm tm_buf;
struct tm* gmtime_r(const time_t* t, struct tm* r)
{
    long long v = (long long)*t; long long days = v / 86400, rem = v % 86400;
    if (rem < 0) { rem += 86400; --days; }
    r->tm_hour = (int)(rem / 3600); r->tm_min = (int)((rem % 3600) / 60); r->tm_sec = (int)(rem % 60);
    r->tm_wday = (int)((days + 4) % 7); if (r->tm_wday < 0) r->tm_wday += 7;
    long y = 1970;
    while (days < 0) { --y; days += is_leap(y) ? 366 : 365; }
    while (days >= (is_leap(y) ? 366 : 365)) { days -= is_leap(y) ? 366 : 365; ++y; }
    r->tm_year = (int)(y - 1900); r->tm_yday = (int)days;
    int m = 0; for (; m < 12; ++m) { int md = mdays[m] + (m == 1 && is_leap(y)); if (days < md) break; days -= md; }
    r->tm_mon = m; r->tm_mday = (int)days + 1; r->tm_isdst = 0;
    return r;
}
struct tm* gmtime(const time_t* t) { return gmtime_r(t, &tm_buf); }
struct tm* localtime(const time_t* t) { return gmtime_r(t, &tm_buf); }      /* no timezone database: local time is UTC */
struct tm* localtime_r(const time_t* t, struct tm* r) { return gmtime_r(t, r); }
time_t mktime(struct tm* tm)
{
    long y = tm->tm_year + 1900L, mon = tm->tm_mon;
    y += mon / 12; mon %= 12; if (mon < 0) { mon += 12; --y; }
    long long days = 0;
    if (y >= 1970) for (long i = 1970; i < y; ++i) days += is_leap(i) ? 366 : 365;
    else for (long i = y; i < 1970; ++i) days -= is_leap(i) ? 366 : 365;
    for (int m = 0; m < mon; ++m) days += mdays[m] + (m == 1 && is_leap(y));
    days += tm->tm_mday - 1;
    long long t = days * 86400LL + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec;
    time_t tt = (time_t)t; gmtime_r(&tt, tm);
    return tt;
}
static const char* wd_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char* mo_names[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
static int put2(char* o, size_t cap, size_t* n, int v, char pad) { char t[3] = { (char)(v >= 10 ? '0' + v / 10 % 10 : pad), (char)('0' + v % 10), 0 }; for (int i = 0; i < 2; ++i) { if (*n + 1 >= cap) return -1; o[(*n)++] = t[i]; } return 0; }
size_t strftime(char* out, size_t cap, const char* fmt, const struct tm* tm)
{
    size_t n = 0;
    for (; *fmt; ++fmt)
    {
        if (*fmt != '%') { if (n + 1 >= cap) return 0; out[n++] = *fmt; continue; }
        ++fmt; char tmp[64]; const char* add = 0; int rc = 0;
        switch (*fmt)
        {
            case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); add = tmp; break;
            case 'y': rc = put2(out, cap, &n, (tm->tm_year + 1900) % 100, '0'); break;
            case 'm': rc = put2(out, cap, &n, tm->tm_mon + 1, '0'); break;
            case 'd': rc = put2(out, cap, &n, tm->tm_mday, '0'); break;
            case 'e': rc = put2(out, cap, &n, tm->tm_mday, ' '); break;
            case 'H': rc = put2(out, cap, &n, tm->tm_hour, '0'); break;
            case 'I': rc = put2(out, cap, &n, tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, '0'); break;
            case 'M': rc = put2(out, cap, &n, tm->tm_min, '0'); break;
            case 'S': rc = put2(out, cap, &n, tm->tm_sec, '0'); break;
            case 'p': add = tm->tm_hour >= 12 ? "PM" : "AM"; break;
            case 'A': add = wd_names[tm->tm_wday % 7]; break;
            case 'a': snprintf(tmp, sizeof(tmp), "%.3s", wd_names[tm->tm_wday % 7]); add = tmp; break;
            case 'B': add = mo_names[tm->tm_mon % 12]; break;
            case 'b': case 'h': snprintf(tmp, sizeof(tmp), "%.3s", mo_names[tm->tm_mon % 12]); add = tmp; break;
            case 'j': snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1); add = tmp; break;
            case 'u': snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday ? tm->tm_wday : 7); add = tmp; break;
            case 'w': snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday); add = tmp; break;
            case 'F': snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); add = tmp; break;
            case 'T': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); add = tmp; break;
            case 'R': snprintf(tmp, sizeof(tmp), "%02d:%02d", tm->tm_hour, tm->tm_min); add = tmp; break;
            case 'D': snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, (tm->tm_year + 1900) % 100); add = tmp; break;
            case 'c': snprintf(tmp, sizeof(tmp), "%.3s %.3s %2d %02d:%02d:%02d %d", wd_names[tm->tm_wday % 7], mo_names[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900); add = tmp; break;
            case 'Z': add = "UTC"; break;
            case 'z': add = "+0000"; break;
            case 's': snprintf(tmp, sizeof(tmp), "%ld", (long)mktime((struct tm*)tm)); add = tmp; break;
            case 'n': add = "\n"; break;
            case 't': add = "\t"; break;
            case '%': add = "%"; break;
            default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = 0; add = tmp; break;
        }
        if (rc) return 0;
        if (add) { size_t l = strlen(add); if (n + l + 1 > cap) return 0; memcpy(out + n, add, l); n += l; }
        if (!*fmt) break;
    }
    if (n >= cap) return 0;
    out[n] = 0;
    return n;
}
char* asctime_r(const struct tm* tm, char* buf) { strftime(buf, 26, "%a %b %e %H:%M:%S %Y\n", tm); return buf; }
char* ctime_r(const time_t* t, char* buf) { struct tm tmv; gmtime_r(t, &tmv); return asctime_r(&tmv, buf); }

/* ===================== aligned allocation ===================== */
struct MBlockHdr { size_t size; size_t flags; void* next; void* prev; };   /* mirrors ulibc.cpp MBlock */
#define MB_ALIGNED 4
void* aligned_alloc(size_t align, size_t size)
{
    if (align < 16) align = 16;
    if (align & (align - 1)) { errno = EINVAL; return 0; }
    if (align == 16) return malloc(size);
    char* raw = (char*)malloc(size + align + sizeof(MBlockHdr));
    if (!raw) return 0;
    unsigned long a = ((unsigned long)raw + sizeof(MBlockHdr) + align - 1) & ~(unsigned long)(align - 1);
    MBlockHdr* h = (MBlockHdr*)a - 1;
    h->size = 0; h->flags = MB_ALIGNED; h->next = raw; h->prev = 0;
    return (void*)a;
}
int posix_memalign(void** out, size_t align, size_t size) { void* p = aligned_alloc(align, size); if (!p) return errno == EINVAL ? EINVAL : ENOMEM; *out = p; return 0; }
void* memalign(size_t align, size_t size) { return aligned_alloc(align, size); }

}  // extern "C"
