#include "textutils.h"
#include "filters.h"
#include "memory.h"
#include "string.h"
#include "uart.h"

/* ============================================================================================
   Regular expressions (backtracking): . * + ? ^ $ [set] [^set] [a-z] \d \w \s \x  and  a|b
   ============================================================================================ */
static int is_wordc(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

/* length in bytes of the atom starting at re */
static int atom_len(const char* re)
{
    if (re[0] == '\\' && re[1]) return 2;
    if (re[0] == '[')
    {
        int i = 1;
        if (re[i] == '^') ++i;
        if (re[i] == ']') ++i;
        while (re[i] && re[i] != ']') { if (re[i] == '\\' && re[i + 1]) ++i; ++i; }
        return re[i] == ']' ? i + 1 : i;
    }
    return 1;
}

static int class_match(const char* re, int alen, int c)
{
    int i = 1, neg = 0, ok = 0;
    if (re[i] == '^') { neg = 1; ++i; }
    int end = alen - 1;                     /* index of the closing ] */
    if (re[end] != ']') end = alen;
    for (; i < end; ++i)
    {
        int lo = (unsigned char)re[i];
        if (re[i] == '\\' && i + 1 < end)
        {
            ++i; char e = re[i];
            if (e == 'd') { if (c >= '0' && c <= '9') ok = 1; continue; }
            if (e == 'w') { if (is_wordc(c)) ok = 1; continue; }
            if (e == 's') { if (c == ' ' || c == '\t') ok = 1; continue; }
            lo = (unsigned char)(e == 't' ? '\t' : e == 'n' ? '\n' : e);
        }
        if (i + 2 < end && re[i + 1] == '-')
        {
            int hi = (unsigned char)re[i + 2];
            if (c >= lo && c <= hi) ok = 1;
            i += 2;
        }
        else if (c == lo) ok = 1;
    }
    return neg ? !ok : ok;
}

static int atom_match(const char* re, int alen, int c)
{
    if (re[0] == '.') return c != '\n';
    if (re[0] == '[') return class_match(re, alen, c);
    if (re[0] == '\\')
    {
        char e = re[1];
        if (e == 'd') return c >= '0' && c <= '9';
        if (e == 'D') return !(c >= '0' && c <= '9');
        if (e == 'w') return is_wordc(c);
        if (e == 'W') return !is_wordc(c);
        if (e == 's') return c == ' ' || c == '\t';
        if (e == 'S') return !(c == ' ' || c == '\t');
        if (e == 't') return c == '\t';
        if (e == 'n') return c == '\n';
        return c == (unsigned char)e;
    }
    return c == (unsigned char)re[0];
}

static int match_here(const char* re, const char* re_end, const char* text, const char* tend, const char** endp, int depth)
{
    if (depth > 4000) return 0;
    if (re == re_end) { *endp = text; return 1; }
    if (*re == '$' && re + 1 == re_end) { if (text == tend) { *endp = text; return 1; } return 0; }
    int al = atom_len(re);
    char q = (re + al < re_end) ? re[al] : 0;
    if (q == '*' || q == '+' || q == '?')
    {
        int mn = q == '+' ? 1 : 0, mx = q == '?' ? 1 : 0x7fffffff;
        int k = 0;
        while (k < mx && text + k < tend && atom_match(re, al, (unsigned char)text[k])) ++k;
        for (; k >= mn; --k)
            if (match_here(re + al + 1, re_end, text + k, tend, endp, depth + 1)) return 1;
        return 0;
    }
    if (text < tend && atom_match(re, al, (unsigned char)*text)) return match_here(re + al, re_end, text + 1, tend, endp, depth + 1);
    return 0;
}

int re_search(const char* re, const char* text, int len, int* ms, int* me)
{
    const char* tend = text + len;
    /* split on top-level '|' (outside [] and not escaped) */
    const char* alt_start = re;
    int best_s = -1, best_e = -1;
    for (const char* p = re;; ++p)
    {
        if (*p == '[') { p += atom_len(p) - 1; continue; }
        if (*p == '\\' && p[1]) { ++p; continue; }
        if (*p == '|' || *p == 0)
        {
            const char* ra = alt_start; const char* rb = p;
            int anchored = 0;
            if (ra < rb && *ra == '^') { anchored = 1; ++ra; }
            for (int s = 0; s <= len; ++s)
            {
                if (anchored && s > 0) break;
                if (best_s >= 0 && s > best_s) break;
                const char* e = 0;
                if (match_here(ra, rb, text + s, tend, &e, 0))
                {
                    if (best_s < 0 || s < best_s) { best_s = s; best_e = (int)(e - text); }
                    break;
                }
            }
            if (*p == 0) break;
            alt_start = p + 1;
        }
    }
    if (best_s < 0) return 0;
    *ms = best_s; *me = best_e;
    return 1;
}

/* ============================================================================================
   output helper
   ============================================================================================ */
struct Out { char* b; uint32_t o; uint32_t cap; };
static void o_char(Out* o, char c) { if (o->o + 1 < o->cap) o->b[o->o++] = c; }
static void o_mem(Out* o, const char* s, uint32_t n) { for (uint32_t i = 0; i < n; ++i) o_char(o, s[i]); }
static void o_str(Out* o, const char* s) { while (*s) o_char(o, *s++); }
static void o_num(Out* o, long long v)
{
    char t[24]; int n = 0; unsigned long long u = v < 0 ? (unsigned long long)(-v) : (unsigned long long)v;
    if (v < 0) o_char(o, '-');
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) o_char(o, t[--n]);
}

#define TU_MAX_LINES 4096
static const char* split_next(const char* in, uint32_t inlen, uint32_t* pos, uint32_t* len)
{
    if (*pos >= inlen) return 0;
    uint32_t s = *pos, i = s;
    while (i < inlen && in[i] != '\n') ++i;
    *len = i - s; *pos = i + 1;
    return in + s;
}

/* ============================================================================================
   sed:  [-n] [-e script]... script   with  s/re/repl/[g|p]  d  p  q  y/abc/xyz/  =   and addresses N  $  /re/  N,M
   ============================================================================================ */
struct SedCmd
{
    char cmd; int a1_kind, a2_kind;      /* 0 none, 1 line number, 2 regex, 3 last line */
    long a1_n, a2_n; char a1_re[48], a2_re[48];
    char re[64]; char repl[96]; int global, print; char ysrc[48], ydst[48];
    int in_range; int neg;
};
static SedCmd sed_cmds[24];

static int sed_delim_field(const char* s, int* pos, char delim, char* dst, int cap)
{
    int k = 0;
    while (s[*pos] && s[*pos] != delim)
    {
        if (s[*pos] == '\\' && s[*pos + 1])
        {
            if (s[*pos + 1] == delim) { if (k < cap - 1) dst[k++] = delim; *pos += 2; continue; }
            if (k < cap - 1) dst[k++] = s[*pos];
            ++*pos;
        }
        if (k < cap - 1) dst[k++] = s[*pos];
        ++*pos;
    }
    dst[k] = 0;
    if (s[*pos] != delim) return -1;
    ++*pos;
    return 0;
}

static int sed_parse_addr(const char* s, int* pos, int* kind, long* nn, char* re)
{
    if (s[*pos] >= '0' && s[*pos] <= '9') { long v = 0; while (s[*pos] >= '0' && s[*pos] <= '9') v = v * 10 + (s[(*pos)++] - '0'); *kind = 1; *nn = v; return 0; }
    if (s[*pos] == '$') { *kind = 3; ++*pos; return 0; }
    if (s[*pos] == '/') { ++*pos; *kind = 2; return sed_delim_field(s, pos, '/', re, 48); }
    *kind = 0; return 0;
}

static int sed_parse(const char* s, int* ncmd)
{
    int p = 0, n = 0;
    for (;;)
    {
        while (s[p] == ' ' || s[p] == ';' || s[p] == '\n') ++p;
        if (!s[p]) break;
        if (n >= 24) return -1;
        SedCmd* c = &sed_cmds[n];
        c->cmd = 0; c->a1_kind = c->a2_kind = 0; c->global = c->print = 0; c->in_range = 0; c->neg = 0;
        c->re[0] = c->repl[0] = c->ysrc[0] = c->ydst[0] = 0;
        if (sed_parse_addr(s, &p, &c->a1_kind, &c->a1_n, c->a1_re) != 0) return -1;
        if (c->a1_kind && s[p] == ',') { ++p; if (sed_parse_addr(s, &p, &c->a2_kind, &c->a2_n, c->a2_re) != 0 || !c->a2_kind) return -1; }
        while (s[p] == ' ') ++p;
        if (s[p] == '!') { c->neg = 1; ++p; while (s[p] == ' ') ++p; }
        char ch = s[p++];
        if (ch == 's')
        {
            char d = s[p++]; if (!d) return -1;
            if (sed_delim_field(s, &p, d, c->re, 64) != 0) return -1;
            if (sed_delim_field(s, &p, d, c->repl, 96) != 0) return -1;
            while (s[p] && s[p] != ';' && s[p] != '\n' && s[p] != '}')
            { if (s[p] == 'g') c->global = 1; else if (s[p] == 'p') c->print = 1; else if (s[p] != ' ') return -1; ++p; }
        }
        else if (ch == 'y')
        {
            char d = s[p++]; if (!d) return -1;
            if (sed_delim_field(s, &p, d, c->ysrc, 48) != 0 || sed_delim_field(s, &p, d, c->ydst, 48) != 0) return -1;
            if (str_len(c->ysrc) != str_len(c->ydst)) return -1;
        }
        else if (ch == 'd' || ch == 'p' || ch == 'q' || ch == '=') { }
        else return -1;
        c->cmd = ch; ++n;
    }
    if (n == 0) return -1;
    *ncmd = n;
    return 0;
}

static int sed_addr_hit(int kind, long nn, const char* re, long lineno, int last, const char* line, int len)
{
    if (kind == 1) return lineno == nn;
    if (kind == 3) return last;
    int a, b; return re_search(re, line, len, &a, &b);
}

static int sed_run(char** w, int n, const char* in, uint32_t inlen, Out* out)
{
    char script[400]; script[0] = 0; int have = 0, quiet = 0;
    for (int i = 1; i < n; ++i)
    {
        if (str_equal(w[i], "-n")) quiet = 1;
        else if (str_equal(w[i], "-e") && i + 1 < n)
        { int l = str_len(script); if (l && l < 398) script[l++] = ';'; str_copy(script + l, w[++i], 400 - l); have = 1; }
        else if (!have && w[i][0] != '-') { str_copy(script, w[i], 400); have = 1; }
        else return -2;
    }
    int ncmd = 0;
    if (!have || sed_parse(script, &ncmd) != 0) return -2;
    uint32_t pos = 0, len = 0; long lineno = 0; const char* line;
    static char ps[1024], tmp[1024];
    int quit = 0;
    /* last-line detection needs lookahead */
    while (!quit && (line = split_next(in, inlen, &pos, &len)) != 0)
    {
        ++lineno;
        int last = pos >= inlen;
        int plen = (int)(len < 1000 ? len : 1000);
        for (int i = 0; i < plen; ++i) ps[i] = line[i];
        ps[plen] = 0;
        int deleted = 0;
        for (int ci = 0; ci < ncmd && !deleted; ++ci)
        {
            SedCmd* c = &sed_cmds[ci];
            int hit = 1;
            if (c->a1_kind)
            {
                if (!c->a2_kind) hit = sed_addr_hit(c->a1_kind, c->a1_n, c->a1_re, lineno, last, ps, plen);
                else
                {
                    if (!c->in_range) { if (sed_addr_hit(c->a1_kind, c->a1_n, c->a1_re, lineno, last, ps, plen)) { hit = 1; c->in_range = 1; if (c->a2_kind == 1 && lineno >= c->a2_n) c->in_range = 0; } else hit = 0; }
                    else { hit = 1; if (sed_addr_hit(c->a2_kind, c->a2_n, c->a2_re, lineno, last, ps, plen)) c->in_range = 0; }
                }
            }
            if (c->neg) hit = !hit;
            if (!hit) continue;
            if (c->cmd == 'd') deleted = 1;
            else if (c->cmd == 'p') { o_mem(out, ps, (uint32_t)plen); o_char(out, '\n'); }
            else if (c->cmd == '=') { o_num(out, lineno); o_char(out, '\n'); }
            else if (c->cmd == 'q') { quit = 1; break; }
            else if (c->cmd == 'y') { for (int i = 0; i < plen; ++i) for (int k = 0; c->ysrc[k]; ++k) if (ps[i] == c->ysrc[k]) { ps[i] = c->ydst[k]; break; } }
            else if (c->cmd == 's')
            {
                int from = 0, tl = 0, changed = 0;
                while (from <= plen)
                {
                    int ms, me;
                    if (!re_search(c->re, ps + from, plen - from, &ms, &me)) break;
                    ms += from; me += from;
                    for (int i = from; i < ms && tl < 1020; ++i) tmp[tl++] = ps[i];
                    for (const char* r = c->repl; *r && tl < 1020; ++r)
                    {
                        if (*r == '&') { for (int i = ms; i < me && tl < 1020; ++i) tmp[tl++] = ps[i]; }
                        else if (*r == '\\' && r[1]) { ++r; tmp[tl++] = *r == 'n' ? '\n' : *r == 't' ? '\t' : *r; }
                        else tmp[tl++] = *r;
                    }
                    changed = 1;
                    if (me == ms) { if (ms < plen && tl < 1020) tmp[tl++] = ps[ms]; from = ms + 1; } else from = me;
                    if (!c->global) break;
                }
                if (changed)
                {
                    for (int i = from; i < plen && tl < 1020; ++i) tmp[tl++] = ps[i];
                    for (int i = 0; i < tl; ++i) ps[i] = tmp[i];
                    plen = tl; ps[plen] = 0;
                    if (c->print) { o_mem(out, ps, (uint32_t)plen); o_char(out, '\n'); }
                }
            }
        }
        if (!deleted && !quiet) { o_mem(out, ps, (uint32_t)plen); o_char(out, '\n'); }
    }
    return 0;
}

/* ============================================================================================
   awk (integer arithmetic only): BEGIN/END, patterns (/re/, expr), print/printf, if/else, while, for, for-in,
   arrays, fields, NR NF FS, length substr index split sub gsub tolower toupper int sprintf, ~ !~ in, ++ -- op=
   ============================================================================================ */
struct AVal { int kind; long long n; char s[160]; };   /* kind 0 string, 1 number, 2 numeric string */
struct AVar { char name[16]; AVal v; };
struct AElem { char key[32]; AVal v; };
struct AArr { char name[16]; AElem e[40]; int n; };

#define AW_MAXVARS 40
#define AW_MAXARR 6
struct Awk
{
    const char* src; int pos; Out* out;
    AVar vars[AW_MAXVARS]; int nvars;
    AArr arrs[AW_MAXARR]; int narr;
    char rec[1024]; char fbuf[1024]; const char* fld[64]; int nf;
    int brk, cont, nxt, ex, err; long steps;
    int exit_code;
};
static Awk* aw;

static int aw_isnum_str(const char* s, long long* val)
{
    while (*s == ' ' || *s == '\t') ++s;
    int neg = 0; if (*s == '-' || *s == '+') { neg = *s == '-'; ++s; }
    if (*s < '0' || *s > '9') return 0;
    long long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; }
    if (*s == '.') { ++s; while (*s >= '0' && *s <= '9') ++s; }
    while (*s == ' ' || *s == '\t') ++s;
    if (*s) return 0;
    *val = neg ? -v : v; return 1;
}
static long long aw_str_to_num(const char* s)
{
    while (*s == ' ' || *s == '\t') ++s;
    int neg = 0; if (*s == '-' || *s == '+') { neg = *s == '-'; ++s; }
    long long v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; }
    return neg ? -v : v;
}
static void aw_tostr(const AVal* v, char* dst, int cap)
{
    if (v->kind == 1)
    {
        char t[24]; int n = 0; unsigned long long u = v->n < 0 ? (unsigned long long)(-v->n) : (unsigned long long)v->n; int k = 0;
        do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
        if (v->n < 0 && k < cap - 1) dst[k++] = '-';
        while (n && k < cap - 1) dst[k++] = t[--n];
        dst[k] = 0;
    }
    else str_copy(dst, v->s, cap);
}
static long long aw_num(const AVal* v) { return v->kind == 0 ? aw_str_to_num(v->s) : v->n; }
static AVal aw_mknum(long long n) { AVal v; v.kind = 1; v.n = n; v.s[0] = 0; return v; }
static AVal aw_mkstr(const char* s) { AVal v; v.kind = 0; v.n = 0; str_copy(v.s, s, 160); return v; }
static int aw_truth(const AVal* v)
{
    if (v->kind == 1) return v->n != 0;
    if (v->kind == 2) return v->n != 0;
    return v->s[0] != 0;
}

static AVal* aw_var(const char* name)
{
    for (int i = 0; i < aw->nvars; ++i) if (str_equal(aw->vars[i].name, name)) return &aw->vars[i].v;
    if (aw->nvars >= AW_MAXVARS) { aw->err = 1; return &aw->vars[0].v; }
    AVar* v = &aw->vars[aw->nvars++];
    str_copy(v->name, name, 16); v->v = aw_mkstr("");
    return &v->v;
}
static AArr* aw_arr(const char* name)
{
    for (int i = 0; i < aw->narr; ++i) if (str_equal(aw->arrs[i].name, name)) return &aw->arrs[i];
    if (aw->narr >= AW_MAXARR) { aw->err = 1; return &aw->arrs[0]; }
    AArr* a = &aw->arrs[aw->narr++]; str_copy(a->name, name, 16); a->n = 0; return a;
}
static AVal* aw_elem(AArr* a, const char* key, int create)
{
    for (int i = 0; i < a->n; ++i) if (str_equal(a->e[i].key, key)) return &a->e[i].v;
    if (!create) return 0;
    if (a->n >= 40) { aw->err = 1; return &a->e[0].v; }
    AElem* e = &a->e[a->n++]; str_copy(e->key, key, 32); e->v = aw_mkstr("");
    return &e->v;
}

static void aw_set_nf(int nf)
{
    *aw_var("NF") = aw_mknum(nf);
}
static void aw_split_record(const char* line)
{
    str_copy(aw->rec, line, (int)sizeof(aw->rec));
    aw->nf = 0;
    AVal* fs = aw_var("FS"); char fsb[40]; aw_tostr(fs, fsb, 40);
    char* d = aw->fbuf; int cap = (int)sizeof(aw->fbuf) - 1; int used = 0;
    const char* s = aw->rec;
    if (fsb[0] == 0 || (fsb[0] == ' ' && fsb[1] == 0))
    {
        while (*s)
        {
            while (*s == ' ' || *s == '\t') ++s;
            if (!*s) break;
            if (aw->nf >= 63) break;
            aw->fld[aw->nf++] = d + used;
            while (*s && *s != ' ' && *s != '\t' && used < cap) d[used++] = *s++;
            d[used++] = 0;
        }
    }
    else
    {
        int single = fsb[1] == 0;
        for (;;)
        {
            if (aw->nf >= 63) break;
            aw->fld[aw->nf++] = d + used;
            int ms = -1, me = -1;
            int rest = str_len(s);
            if (single) { for (int i = 0; i < rest; ++i) if (s[i] == fsb[0]) { ms = i; me = i + 1; break; } }
            else { int a, b; if (re_search(fsb, s, rest, &a, &b) && b > a) { ms = a; me = b; } }
            int take = ms < 0 ? rest : ms;
            for (int i = 0; i < take && used < cap; ++i) d[used++] = s[i];
            d[used++] = 0;
            if (ms < 0) break;
            s += me;
        }
    }
    aw_set_nf(aw->nf);
}
static AVal aw_field(long long i)
{
    if (i == 0) { AVal v = aw_mkstr(aw->rec); long long x; if (aw_isnum_str(aw->rec, &x)) { v.kind = 2; v.n = x; } return v; }
    if (i < 0 || i > aw->nf) return aw_mkstr("");
    AVal v = aw_mkstr(aw->fld[i - 1]); long long x; if (aw_isnum_str(v.s, &x)) { v.kind = 2; v.n = x; }
    return v;
}
static void aw_rebuild_record()
{
    char b[1024]; int k = 0;
    for (int i = 0; i < aw->nf; ++i)
    { if (i && k < 1022) b[k++] = ' '; for (const char* p = aw->fld[i]; *p && k < 1022; ++p) b[k++] = *p; }
    b[k] = 0;
    str_copy(aw->rec, b, (int)sizeof(aw->rec));
}
static void aw_set_field(long long i, const AVal* v)
{
    char t[160]; aw_tostr(v, t, 160);
    if (i == 0) { aw_split_record(t); return; }
    if (i < 0 || i > 62) { aw->err = 1; return; }
    static char store[64][160];
    /* snapshot all fields so rebuilding is safe */
    static char snap[64][160];
    for (int k = 0; k < aw->nf; ++k) str_copy(snap[k], aw->fld[k], 160);
    while (aw->nf < i) { snap[aw->nf][0] = 0; ++aw->nf; }
    str_copy(snap[i - 1], t, 160);
    int used = 0; int cap = (int)sizeof(aw->fbuf) - 1;
    for (int k = 0; k < aw->nf; ++k)
    {
        aw->fld[k] = aw->fbuf + used;
        for (const char* p = snap[k]; *p && used < cap; ++p) aw->fbuf[used++] = *p;
        aw->fbuf[used++] = 0;
    }
    (void)store;
    aw_set_nf(aw->nf);
    aw_rebuild_record();
}

/* ---- parser helpers ---- */
static void aw_ws(int nl)
{
    for (;;)
    {
        char c = aw->src[aw->pos];
        if (c == ' ' || c == '\t' || c == '\r') ++aw->pos;
        else if (nl && c == '\n') ++aw->pos;
        else if (c == '\\' && aw->src[aw->pos + 1] == '\n') aw->pos += 2;
        else if (c == '#') { while (aw->src[aw->pos] && aw->src[aw->pos] != '\n') ++aw->pos; }
        else break;
    }
}
static int aw_peek(char c) { aw_ws(0); return aw->src[aw->pos] == c; }
static int aw_eat(char c) { aw_ws(0); if (aw->src[aw->pos] == c) { ++aw->pos; return 1; } return 0; }
static int aw_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int aw_read_ident(char* dst, int cap)
{
    aw_ws(0);
    int k = 0, p = aw->pos;
    if (!aw_ident_start(aw->src[p])) return 0;
    while (aw_ident_start(aw->src[p]) || (aw->src[p] >= '0' && aw->src[p] <= '9')) { if (k < cap - 1) dst[k++] = aw->src[p]; ++p; }
    dst[k] = 0; aw->pos = p; return 1;
}
static int aw_keyword_at(const char* kw)
{
    aw_ws(0);
    int n = str_len(kw);
    for (int i = 0; i < n; ++i) if (aw->src[aw->pos + i] != kw[i]) return 0;
    char c = aw->src[aw->pos + n];
    return !(aw_ident_start(c) || (c >= '0' && c <= '9'));
}

static AVal aw_expr(int ex, int nogt);
static AVal aw_assign(int ex, int nogt);
static void aw_stmt(int ex);

static int aw_read_regex(char* dst, int cap)     /* after the opening / */
{
    int k = 0;
    while (aw->src[aw->pos] && aw->src[aw->pos] != '/' && aw->src[aw->pos] != '\n')
    {
        if (aw->src[aw->pos] == '\\' && aw->src[aw->pos + 1] == '/') { if (k < cap - 1) dst[k++] = '/'; aw->pos += 2; continue; }
        if (aw->src[aw->pos] == '[') { while (aw->src[aw->pos] && aw->src[aw->pos] != ']') { if (k < cap - 1) dst[k++] = aw->src[aw->pos]; ++aw->pos; } }
        if (aw->src[aw->pos] == '\\' && aw->src[aw->pos + 1]) { if (k < cap - 1) dst[k++] = aw->src[aw->pos]; ++aw->pos; }
        if (aw->src[aw->pos] && k < cap - 1) dst[k++] = aw->src[aw->pos];
        if (aw->src[aw->pos]) ++aw->pos;
    }
    dst[k] = 0;
    if (aw->src[aw->pos] != '/') { aw->err = 1; return -1; }
    ++aw->pos; return 0;
}
static int aw_read_string(char* dst, int cap)    /* after the opening quote */
{
    int k = 0;
    while (aw->src[aw->pos] && aw->src[aw->pos] != '"')
    {
        char c = aw->src[aw->pos++];
        if (c == '\\' && aw->src[aw->pos])
        {
            char e = aw->src[aw->pos++];
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
        }
        if (k < cap - 1) dst[k++] = c;
    }
    dst[k] = 0;
    if (aw->src[aw->pos] != '"') { aw->err = 1; return -1; }
    ++aw->pos; return 0;
}

/* formatted output: %d %i %s %c %x %X %o %u %% with flags - 0, width, .prec */
static void aw_format(Out* o, const char* fmt, AVal* args, int nargs)
{
    int ai = 0;
    for (const char* p = fmt; *p; ++p)
    {
        if (*p != '%') { o_char(o, *p); continue; }
        ++p;
        if (*p == '%') { o_char(o, '%'); continue; }
        int left = 0, zero = 0, width = 0, prec = -1;
        for (;; ++p) { if (*p == '-') left = 1; else if (*p == '0') zero = 1; else if (*p == '+' || *p == ' ' || *p == '#') { } else break; }
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (*p == '.') { ++p; prec = 0; while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0'); }
        while (*p == 'l' || *p == 'h') ++p;
        char conv = *p; if (!conv) break;
        char tmp[200]; int tl = 0;
        AVal dummy = aw_mkstr("");
        AVal* a = ai < nargs ? &args[ai++] : &dummy;
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o')
        {
            long long v = aw_num(a); unsigned long long u; int neg = 0;
            unsigned base = (conv == 'x' || conv == 'X') ? 16 : conv == 'o' ? 8 : 10;
            if ((conv == 'd' || conv == 'i') && v < 0) { neg = 1; u = (unsigned long long)(-v); } else u = (unsigned long long)v;
            char r[24]; int rn = 0;
            do { unsigned dg = (unsigned)(u % base); r[rn++] = (char)(dg < 10 ? '0' + dg : (conv == 'X' ? 'A' : 'a') + dg - 10); u /= base; } while (u);
            while (prec > rn && rn < 22) r[rn++] = '0';
            if (neg) tmp[tl++] = '-';
            while (rn) tmp[tl++] = r[--rn];
        }
        else if (conv == 'c')
        {
            if (a->kind == 0) tmp[tl++] = a->s[0] ? a->s[0] : ' '; else tmp[tl++] = (char)a->n;
        }
        else if (conv == 's')
        {
            char sbuf[160]; aw_tostr(a, sbuf, 160);
            for (int i = 0; sbuf[i] && (prec < 0 || i < prec) && tl < 190; ++i) tmp[tl++] = sbuf[i];
        }
        else { tmp[tl++] = '%'; tmp[tl++] = conv; }
        int pad = width > tl ? width - tl : 0;
        if (!left) { char pc = (zero && conv != 's') ? '0' : ' '; if (pc == '0' && tl && tmp[0] == '-') { o_char(o, '-'); for (int i = 0; i < pad; ++i) o_char(o, '0'); o_mem(o, tmp + 1, (uint32_t)tl - 1); pad = 0; tl = 0; } else for (int i = 0; i < pad; ++i) o_char(o, pc); }
        o_mem(o, tmp, (uint32_t)tl);
        if (left) for (int i = 0; i < pad; ++i) o_char(o, ' ');
    }
}

struct AwRef { int kind; char name[16]; long long idx; char key[32]; };  /* 0 var, 1 field, 2 array elem */

static AVal aw_ref_get(const AwRef* r)
{
    if (r->kind == 0) { AVal* v = aw_var(r->name); return *v; }
    if (r->kind == 1) return aw_field(r->idx);
    AVal* v = aw_elem(aw_arr(r->name), r->key, 1); return *v;
}
static void aw_ref_set(const AwRef* r, const AVal* val)
{
    if (r->kind == 0)
    {
        AVal* v = aw_var(r->name); *v = *val;
        if (str_equal(r->name, "NF")) { /* shrinking/growing NF is not supported; keep value */ }
    }
    else if (r->kind == 1) aw_set_field(r->idx, val);
    else *aw_elem(aw_arr(r->name), r->key, 1) = *val;
}

static AVal aw_primary(int ex, int nogt);

static int aw_parse_ref(int ex, AwRef* r)
{
    aw_ws(0);
    char c = aw->src[aw->pos];
    if (c == '$')
    {
        ++aw->pos;
        AVal v = aw_primary(ex, 1);
        r->kind = 1; r->idx = aw_num(&v); r->name[0] = 0; return 1;
    }
    if (aw_ident_start(c))
    {
        int save = aw->pos;
        char nm[16]; aw_read_ident(nm, 16);
        if (aw->src[aw->pos] == '(') { aw->pos = save; return 0; }       /* function call */
        if (aw->src[aw->pos] == '[')
        {
            ++aw->pos;
            AVal k = aw_expr(ex, 0);
            if (!aw_eat(']')) aw->err = 1;
            r->kind = 2; str_copy(r->name, nm, 16); aw_tostr(&k, r->key, 32); return 1;
        }
        r->kind = 0; str_copy(r->name, nm, 16); return 1;
    }
    return 0;
}

static AVal aw_call(const char* fn, int ex)
{
    /* arguments: parsed as expressions (regex args handled by callers below) */
    AVal args[4]; int na = 0;
    AwRef target; target.kind = 0; target.name[0] = 0; target.idx = 0; target.key[0] = 0; int has_target = 0; char regex[64]; int have_re = 0;
    aw_eat('(');
    int is_sub = str_equal(fn, "sub") || str_equal(fn, "gsub");
    int is_split = str_equal(fn, "split");
    if (!aw_peek(')'))
    {
        for (;;)
        {
            aw_ws(0);
            if (is_sub && na == 0)
            {
                if (aw->src[aw->pos] == '/') { ++aw->pos; aw_read_regex(regex, 64); have_re = 1; }
                else { AVal v = aw_assign(ex, 0); aw_tostr(&v, regex, 64); have_re = 1; }
                na = 1; args[0] = aw_mkstr("");
            }
            else if ((is_sub && na == 2) || (is_split && na == 1))
            {
                int save = aw->pos; AwRef rr;
                int ok = aw_parse_ref(ex, &rr);
                if (ok && (aw_peek(',') || aw_peek(')'))) { target = rr; has_target = 1; args[na] = aw_mkstr(""); ++na; }
                else { aw->pos = save; if (na < 4) args[na++] = aw_assign(ex, 0); else aw_assign(ex, 0); }
            }
            else if (na < 4) args[na++] = aw_assign(ex, 0);
            else aw_assign(ex, 0);
            if (!aw_eat(',')) break;
        }
    }
    if (!aw_eat(')')) { aw->err = 1; return aw_mknum(0); }
    if (!ex) return aw_mknum(0);
    char s0[160]; if (na > 0) aw_tostr(&args[0], s0, 160); else str_copy(s0, aw->rec, 160);
    if (str_equal(fn, "length")) { if (na == 0) return aw_mknum(str_len(aw->rec)); return aw_mknum(str_len(s0)); }
    if (str_equal(fn, "substr"))
    {
        long long st = na > 1 ? aw_num(&args[1]) : 1; long long ln = na > 2 ? aw_num(&args[2]) : 1000000;
        int sl = str_len(s0);
        if (st < 1) { ln += st - 1; st = 1; }
        if (ln < 0) ln = 0;
        if (st > sl) return aw_mkstr("");
        char t[160]; int k = 0;
        for (long long i = st - 1; i < sl && i < st - 1 + ln && k < 159; ++i) t[k++] = s0[i];
        t[k] = 0; return aw_mkstr(t);
    }
    if (str_equal(fn, "index"))
    {
        char s1[160]; if (na > 1) aw_tostr(&args[1], s1, 160); else s1[0] = 0;
        int sl = str_len(s0), tl = str_len(s1);
        for (int i = 0; i + tl <= sl; ++i) { int k = 0; while (k < tl && s0[i + k] == s1[k]) ++k; if (k == tl) return aw_mknum(i + 1); }
        return aw_mknum(0);
    }
    if (str_equal(fn, "tolower") || str_equal(fn, "toupper"))
    {
        int up = fn[2] == 'u';
        for (int i = 0; s0[i]; ++i) { if (up && s0[i] >= 'a' && s0[i] <= 'z') s0[i] = (char)(s0[i] - 32); if (!up && s0[i] >= 'A' && s0[i] <= 'Z') s0[i] = (char)(s0[i] + 32); }
        return aw_mkstr(s0);
    }
    if (str_equal(fn, "int")) return aw_mknum(na ? aw_num(&args[0]) : 0);
    if (str_equal(fn, "sprintf"))
    {
        static char sb[256]; Out so = { sb, 0, sizeof(sb) };
        aw_format(&so, s0, args + 1, na > 0 ? na - 1 : 0); sb[so.o] = 0; return aw_mkstr(sb);
    }
    if (is_split)
    {
        /* split(s, arr[, sep]) : the array name is the second argument identifier */
        return aw_mknum(0);
    }
    if (is_sub)
    {
        AwRef def; def.kind = 1; def.idx = 0; def.name[0] = 0;
        AwRef* t = has_target ? &target : &def;
        AVal cur = aw_ref_get(t); char text[200]; aw_tostr(&cur, text, 200);
        char repl[160]; if (na > 1) aw_tostr(&args[1], repl, 160); else repl[0] = 0;
        char res[400]; int rl = 0, from = 0, count = 0, tlen = str_len(text);
        int global = fn[0] == 'g';
        if (!have_re) return aw_mknum(0);
        while (from <= tlen)
        {
            int ms, me;
            if (!re_search(regex, text + from, tlen - from, &ms, &me)) break;
            ms += from; me += from;
            for (int i = from; i < ms && rl < 390; ++i) res[rl++] = text[i];
            for (const char* r = repl; *r && rl < 390; ++r)
            { if (*r == '&') { for (int i = ms; i < me && rl < 390; ++i) res[rl++] = text[i]; } else if (*r == '\\' && r[1]) { ++r; res[rl++] = *r; } else res[rl++] = *r; }
            ++count;
            if (me == ms) { if (ms < tlen && rl < 390) res[rl++] = text[ms]; from = ms + 1; } else from = me;
            if (!global) break;
        }
        if (count)
        {
            for (int i = from; i < tlen && rl < 390; ++i) res[rl++] = text[i];
            res[rl] = 0;
            AVal nv = aw_mkstr(res); aw_ref_set(t, &nv);
        }
        return aw_mknum(count);
    }
    aw->err = 1; return aw_mknum(0);
}

/* split() needs the array identifier, so it is parsed separately */
static AVal aw_split_call(int ex)
{
    aw_eat('(');
    AVal s = aw_assign(ex, 0);
    aw_eat(',');
    char an[16]; if (!aw_read_ident(an, 16)) { aw->err = 1; return aw_mknum(0); }
    char sep[40]; sep[0] = 0; int have_sep = 0;
    if (aw_eat(','))
    {
        aw_ws(0);
        if (aw->src[aw->pos] == '/') { ++aw->pos; aw_read_regex(sep, 40); have_sep = 1; }
        else { AVal v = aw_assign(ex, 0); aw_tostr(&v, sep, 40); have_sep = 1; }
    }
    if (!aw_eat(')')) { aw->err = 1; return aw_mknum(0); }
    if (!ex) return aw_mknum(0);
    if (!have_sep) { AVal* fs = aw_var("FS"); aw_tostr(fs, sep, 40); }
    AArr* a = aw_arr(an); a->n = 0;
    char text[200]; aw_tostr(&s, text, 200);
    int tl = str_len(text), from = 0, cnt = 0;
    int ws = sep[0] == 0 || (sep[0] == ' ' && sep[1] == 0);
    for (;;)
    {
        if (ws) { while (from < tl && (text[from] == ' ' || text[from] == '\t')) ++from; if (from >= tl) break; }
        int ms = -1, me = -1;
        if (ws) { int i = from; while (i < tl && text[i] != ' ' && text[i] != '\t') ++i; ms = i; me = i; if (i >= tl) ms = -1; }
        else if (sep[1] == 0) { for (int i = from; i < tl; ++i) if (text[i] == sep[0]) { ms = i; me = i + 1; break; } }
        else { int a2, b2; if (re_search(sep, text + from, tl - from, &a2, &b2) && b2 > a2) { ms = from + a2; me = from + b2; } }
        char piece[160]; int pl = 0; int e = ms < 0 ? tl : ms;
        for (int i = from; i < e && pl < 159; ++i) piece[pl++] = text[i];
        piece[pl] = 0;
        char key[12]; { AVal kv = aw_mknum(++cnt); aw_tostr(&kv, key, 12); }
        AVal pv = aw_mkstr(piece); long long x; if (aw_isnum_str(piece, &x)) { pv.kind = 2; pv.n = x; }
        *aw_elem(a, key, 1) = pv;
        if (ms < 0) break;
        from = me;
        if (from >= tl && !ws) { char k2[12]; AVal kv = aw_mknum(++cnt); aw_tostr(&kv, k2, 12); *aw_elem(a, k2, 1) = aw_mkstr(""); break; }
    }
    return aw_mknum(cnt);
}

static AVal aw_primary(int ex, int nogt)
{
    aw_ws(0);
    char c = aw->src[aw->pos];
    if (c == '"') { ++aw->pos; char b[160]; aw_read_string(b, 160); return aw_mkstr(b); }
    if (c >= '0' && c <= '9')
    {
        long long v = 0; while (aw->src[aw->pos] >= '0' && aw->src[aw->pos] <= '9') v = v * 10 + (aw->src[aw->pos++] - '0');
        if (aw->src[aw->pos] == '.' && aw->src[aw->pos + 1] >= '0' && aw->src[aw->pos + 1] <= '9') { ++aw->pos; while (aw->src[aw->pos] >= '0' && aw->src[aw->pos] <= '9') ++aw->pos; }
        return aw_mknum(v);
    }
    if (c == '(') { ++aw->pos; AVal v = aw_expr(ex, 0); if (!aw_eat(')')) aw->err = 1; return v; }
    if (c == '/') { ++aw->pos; char re[64]; aw_read_regex(re, 64); if (!ex) return aw_mknum(0); int a, b; return aw_mknum(re_search(re, aw->rec, str_len(aw->rec), &a, &b)); }
    if (c == '!') { ++aw->pos; AVal v = aw_primary(ex, nogt); return aw_mknum(!aw_truth(&v)); }
    if (c == '-') { ++aw->pos; AVal v = aw_primary(ex, nogt); return aw_mknum(-aw_num(&v)); }
    if (c == '+') { if (aw->src[aw->pos + 1] == '+') { aw->pos += 2; AwRef r; if (!aw_parse_ref(ex, &r)) { aw->err = 1; return aw_mknum(0); } if (!ex) return aw_mknum(0); AVal v = aw_ref_get(&r); v = aw_mknum(aw_num(&v) + 1); aw_ref_set(&r, &v); return v; } ++aw->pos; AVal v = aw_primary(ex, nogt); return aw_mknum(aw_num(&v)); }
    if (c == '$' || aw_ident_start(c))
    {
        if (aw_ident_start(c))
        {
            int save = aw->pos; char nm[16]; aw_read_ident(nm, 16);
            if (aw->src[aw->pos] == '(')
            {
                if (str_equal(nm, "split")) return aw_split_call(ex);
                return aw_call(nm, ex);
            }
            if (str_equal(nm, "length") ) { if (!ex) return aw_mknum(0); return aw_mknum(str_len(aw->rec)); }
            aw->pos = save;
        }
        AwRef r; if (!aw_parse_ref(ex, &r)) { aw->err = 1; return aw_mknum(0); }
        if (!ex) { if (aw->src[aw->pos] == '+' && aw->src[aw->pos + 1] == '+') aw->pos += 2; else if (aw->src[aw->pos] == '-' && aw->src[aw->pos + 1] == '-') aw->pos += 2; return aw_mknum(0); }
        AVal v = aw_ref_get(&r);
        if (aw->src[aw->pos] == '+' && aw->src[aw->pos + 1] == '+') { aw->pos += 2; AVal nv = aw_mknum(aw_num(&v) + 1); aw_ref_set(&r, &nv); return aw_mknum(aw_num(&v)); }
        if (aw->src[aw->pos] == '-' && aw->src[aw->pos + 1] == '-') { aw->pos += 2; AVal nv = aw_mknum(aw_num(&v) - 1); aw_ref_set(&r, &nv); return aw_mknum(aw_num(&v)); }
        return v;
    }
    aw->err = 1;
    return aw_mknum(0);
}

static AVal aw_mul(int ex, int nogt)
{
    AVal l = aw_primary(ex, nogt);
    for (;;)
    {
        aw_ws(0); char c = aw->src[aw->pos];
        if ((c == '*' || c == '/' || c == '%') && aw->src[aw->pos + 1] != '=')
        {
            ++aw->pos; AVal r = aw_primary(ex, nogt);
            if (ex)
            {
                long long a = aw_num(&l), b = aw_num(&r);
                if (c == '*') l = aw_mknum(a * b);
                else { if (b == 0) { aw->err = 2; l = aw_mknum(0); } else l = aw_mknum(c == '/' ? a / b : a % b); }
            }
        }
        else break;
    }
    return l;
}
static AVal aw_add(int ex, int nogt)
{
    AVal l = aw_mul(ex, nogt);
    for (;;)
    {
        aw_ws(0); char c = aw->src[aw->pos];
        if ((c == '+' || c == '-') && aw->src[aw->pos + 1] != '=' && aw->src[aw->pos + 1] != c)
        {
            ++aw->pos; AVal r = aw_mul(ex, nogt);
            if (ex) l = aw_mknum(c == '+' ? aw_num(&l) + aw_num(&r) : aw_num(&l) - aw_num(&r));
        }
        else break;
    }
    return l;
}
static int aw_concat_start(void)
{
    aw_ws(0); char c = aw->src[aw->pos];
    if (c == '"' || c == '$' || c == '(' || (c >= '0' && c <= '9')) return 1;
    if (c == '!' && aw->src[aw->pos + 1] != '=' && aw->src[aw->pos + 1] != '~') return 1;
    if (aw_ident_start(c))
    {
        if (aw_keyword_at("in") || aw_keyword_at("else") || aw_keyword_at("print") || aw_keyword_at("printf") || aw_keyword_at("if") || aw_keyword_at("while") || aw_keyword_at("for") || aw_keyword_at("next") || aw_keyword_at("exit") || aw_keyword_at("delete") || aw_keyword_at("break") || aw_keyword_at("continue")) return 0;
        return 1;
    }
    return 0;
}
static AVal aw_concat(int ex, int nogt)
{
    AVal l = aw_add(ex, nogt);
    while (aw_concat_start())
    {
        AVal r = aw_add(ex, nogt);
        if (ex) { char a[160], b[160]; aw_tostr(&l, a, 160); aw_tostr(&r, b, 160); int al = str_len(a); str_copy(a + al, b, 160 - al); l = aw_mkstr(a); }
    }
    return l;
}
static int aw_compare(const AVal* a, const AVal* b)
{
    if (a->kind != 0 && b->kind != 0) return a->n < b->n ? -1 : a->n > b->n ? 1 : 0;
    char x[160], y[160]; aw_tostr(a, x, 160); aw_tostr(b, y, 160);
    int i = 0; while (x[i] && x[i] == y[i]) ++i;
    return (unsigned char)x[i] < (unsigned char)y[i] ? -1 : (unsigned char)x[i] > (unsigned char)y[i] ? 1 : 0;
}
static AVal aw_cmp(int ex, int nogt)
{
    AVal l = aw_concat(ex, nogt);
    for (;;)
    {
        aw_ws(0); const char* s = aw->src + aw->pos; int op = 0, ol = 0;
        if (s[0] == '<' && s[1] == '=') { op = 1; ol = 2; } else if (s[0] == '>' && s[1] == '=') { op = 2; ol = 2; }
        else if (s[0] == '=' && s[1] == '=') { op = 3; ol = 2; } else if (s[0] == '!' && s[1] == '=') { op = 4; ol = 2; }
        else if (s[0] == '<') { op = 5; ol = 1; } else if (s[0] == '>' && !nogt) { op = 6; ol = 1; }
        if (!op) break;
        aw->pos += ol;
        AVal r = aw_concat(ex, nogt);
        if (ex)
        {
            int c = aw_compare(&l, &r); int res = 0;
            switch (op) { case 1: res = c <= 0; break; case 2: res = c >= 0; break; case 3: res = c == 0; break; case 4: res = c != 0; break; case 5: res = c < 0; break; default: res = c > 0; }
            l = aw_mknum(res);
        }
    }
    return l;
}
static AVal aw_match(int ex, int nogt)
{
    AVal l = aw_cmp(ex, nogt);
    for (;;)
    {
        aw_ws(0); int neg = 0;
        if (aw->src[aw->pos] == '~') { ++aw->pos; }
        else if (aw->src[aw->pos] == '!' && aw->src[aw->pos + 1] == '~') { aw->pos += 2; neg = 1; }
        else if (aw_keyword_at("in"))
        {
            aw->pos += 2; char an[16]; if (!aw_read_ident(an, 16)) { aw->err = 1; return l; }
            if (ex) { char k[40]; aw_tostr(&l, k, 40); l = aw_mknum(aw_elem(aw_arr(an), k, 0) != 0); }
            continue;
        }
        else break;
        char re[96]; aw_ws(0);
        if (aw->src[aw->pos] == '/') { ++aw->pos; aw_read_regex(re, 96); }
        else { AVal r = aw_cmp(ex, nogt); aw_tostr(&r, re, 96); }
        if (ex) { char t[200]; aw_tostr(&l, t, 200); int a, b; int m = re_search(re, t, str_len(t), &a, &b); l = aw_mknum(neg ? !m : m); }
    }
    return l;
}
static AVal aw_and(int ex, int nogt)
{
    AVal l = aw_match(ex, nogt);
    for (;;)
    {
        aw_ws(0);
        if (aw->src[aw->pos] == '&' && aw->src[aw->pos + 1] == '&')
        {
            aw->pos += 2; int lt = ex && aw_truth(&l);
            AVal r = aw_match(lt, nogt);
            if (ex) l = aw_mknum(lt && aw_truth(&r));
        }
        else break;
    }
    return l;
}
static AVal aw_or(int ex, int nogt)
{
    AVal l = aw_and(ex, nogt);
    for (;;)
    {
        aw_ws(0);
        if (aw->src[aw->pos] == '|' && aw->src[aw->pos + 1] == '|')
        {
            aw->pos += 2; int lf = ex && !aw_truth(&l);
            AVal r = aw_and(lf, nogt);
            if (ex) l = aw_mknum(aw_truth(&l) || aw_truth(&r));
        }
        else break;
    }
    return l;
}
static AVal aw_assign(int ex, int nogt)
{
    int save = aw->pos;
    aw_ws(0);
    char c = aw->src[aw->pos];
    if (c == '$' || aw_ident_start(c))
    {
        AwRef r;
        int old_err = aw->err;
        if (aw_parse_ref(ex, &r))
        {
            aw_ws(0); const char* s = aw->src + aw->pos; char op = 0; int ol = 0;
            if (s[0] == '=' && s[1] != '=') { op = '='; ol = 1; }
            else if ((s[0] == '+' || s[0] == '-' || s[0] == '*' || s[0] == '/' || s[0] == '%') && s[1] == '=') { op = s[0]; ol = 2; }
            if (op)
            {
                aw->pos += ol;
                AVal rhs = aw_assign(ex, nogt);
                if (!ex) return aw_mknum(0);
                if (op != '=')
                {
                    AVal cur = aw_ref_get(&r); long long a = aw_num(&cur), b = aw_num(&rhs);
                    if ((op == '/' || op == '%') && b == 0) { aw->err = 2; return aw_mknum(0); }
                    rhs = aw_mknum(op == '+' ? a + b : op == '-' ? a - b : op == '*' ? a * b : op == '/' ? a / b : a % b);
                }
                aw_ref_set(&r, &rhs);
                return rhs;
            }
        }
        aw->err = old_err; aw->pos = save;
    }
    return aw_or(ex, nogt);
}
static AVal aw_expr(int ex, int nogt) { return aw_assign(ex, nogt); }

static void aw_print_vals(int ex, int printf_mode)
{
    AVal vals[16]; int nv = 0;
    aw_ws(0);
    char c = aw->src[aw->pos];
    int paren = 0;
    if (c == '(')
    {
        /* print (a, b)  : try to parse as a parenthesised list */
        int save = aw->pos; ++aw->pos;
        AVal first = aw_expr(ex, 0);
        if (aw_peek(',')) { paren = 1; vals[nv++] = first; while (aw_eat(',')) { if (nv < 16) vals[nv++] = aw_expr(ex, 0); else aw_expr(ex, 0); } if (!aw_eat(')')) aw->err = 1; }
        else { aw->pos = save; }
    }
    if (!paren && c != ';' && c != '}' && c != '\n' && c != 0)
    {
        for (;;)
        {
            AVal v = aw_expr(ex, 1);
            if (nv < 16) vals[nv++] = v;
            if (!aw_eat(',')) break;
        }
    }
    if (!ex || aw->err) return;
    if (printf_mode)
    {
        if (nv == 0) return;
        char f[200]; aw_tostr(&vals[0], f, 200);
        aw_format(aw->out, f, vals + 1, nv - 1);
        return;
    }
    if (nv == 0) { o_str(aw->out, aw->rec); o_char(aw->out, '\n'); return; }
    for (int i = 0; i < nv; ++i)
    { if (i) o_char(aw->out, ' '); char t[160]; aw_tostr(&vals[i], t, 160); o_str(aw->out, t); }
    o_char(aw->out, '\n');
}

static void aw_end_stmt(void)
{
    aw_ws(0);
    char c = aw->src[aw->pos];
    if (c == ';' || c == '\n') { ++aw->pos; return; }
    /* '}' or end: fine */
}

static void aw_block(int ex)
{
    /* after '{' */
    for (;;)
    {
        aw_ws(1);
        while (aw->src[aw->pos] == ';') { ++aw->pos; aw_ws(1); }
        if (aw->src[aw->pos] == '}') { ++aw->pos; return; }
        if (!aw->src[aw->pos]) { aw->err = 1; return; }
        aw_stmt(ex && !aw->brk && !aw->cont && !aw->nxt && !aw->exit_code);
        if (aw->err) return;
    }
}

static int aw_tick(void) { if (++aw->steps > 400000) { aw->err = 3; return 1; } return 0; }

static void aw_stmt(int ex)
{
    aw_ws(1);
    if (aw->err) return;
    if (aw_eat('{')) { aw_block(ex); return; }
    if (aw_keyword_at("print")) { aw->pos += 5; aw_print_vals(ex, 0); aw_end_stmt(); return; }
    if (aw_keyword_at("printf")) { aw->pos += 6; aw_print_vals(ex, 1); aw_end_stmt(); return; }
    if (aw_keyword_at("next")) { aw->pos += 4; if (ex) aw->nxt = 1; aw_end_stmt(); return; }
    if (aw_keyword_at("break")) { aw->pos += 5; if (ex) aw->brk = 1; aw_end_stmt(); return; }
    if (aw_keyword_at("continue")) { aw->pos += 8; if (ex) aw->cont = 1; aw_end_stmt(); return; }
    if (aw_keyword_at("exit"))
    {
        aw->pos += 4; aw_ws(0);
        char c = aw->src[aw->pos];
        long long code = 0;
        if (c && c != ';' && c != '\n' && c != '}') { AVal v = aw_expr(ex, 0); code = aw_num(&v); }
        if (ex) { aw->exit_code = (int)(code ? code : -1000); aw->ex = 1; }
        aw_end_stmt(); return;
    }
    if (aw_keyword_at("delete"))
    {
        aw->pos += 6; char an[16]; if (!aw_read_ident(an, 16)) { aw->err = 1; return; }
        if (aw_eat('['))
        {
            AVal k = aw_expr(ex, 0); if (!aw_eat(']')) aw->err = 1;
            if (ex) { char ks[40]; aw_tostr(&k, ks, 40); AArr* a = aw_arr(an); for (int i = 0; i < a->n; ++i) if (str_equal(a->e[i].key, ks)) { a->e[i] = a->e[--a->n]; break; } }
        }
        else if (ex) aw_arr(an)->n = 0;
        aw_end_stmt(); return;
    }
    if (aw_keyword_at("if"))
    {
        aw->pos += 2; if (!aw_eat('(')) { aw->err = 1; return; }
        AVal c = aw_expr(ex, 0); if (!aw_eat(')')) { aw->err = 1; return; }
        int t = ex && aw_truth(&c);
        aw_stmt(t);
        aw_ws(1);
        if (aw_keyword_at("else")) { aw->pos += 4; aw_stmt(ex && !t); }
        return;
    }
    if (aw_keyword_at("while"))
    {
        aw->pos += 5; if (!aw_eat('(')) { aw->err = 1; return; }
        int cpos = aw->pos;
        for (;;)
        {
            aw->pos = cpos;
            AVal c = aw_expr(ex, 0); if (!aw_eat(')')) { aw->err = 1; return; }
            int t = ex && aw_truth(&c) && !aw->exit_code && !aw->nxt;
            aw_stmt(t);
            if (!t) break;
            if (aw->brk) { aw->brk = 0; int bp = aw->pos; (void)bp; aw->pos = cpos; AVal d = aw_expr(0, 0); (void)d; aw_eat(')'); aw_stmt(0); break; }
            aw->cont = 0;
            if (aw_tick() || aw->err) return;
        }
        return;
    }
    if (aw_keyword_at("for"))
    {
        aw->pos += 3; if (!aw_eat('(')) { aw->err = 1; return; }
        /* for (k in arr) */
        int save = aw->pos; char kn[16], an[16];
        if (aw_read_ident(kn, 16) && aw_keyword_at("in"))
        {
            aw->pos += 2;
            if (!aw_read_ident(an, 16) || !aw_eat(')')) { aw->err = 1; return; }
            int bpos = aw->pos;
            if (!ex) { aw_stmt(0); return; }
            AArr* a = aw_arr(an);
            int n = a->n; static char keys[40][32];
            char (*kk)[32] = keys; for (int i = 0; i < n; ++i) str_copy(kk[i], a->e[i].key, 32);
            for (int i = 0; i < n; ++i)
            {
                AVal kv = aw_mkstr(kk[i]); long long x; if (aw_isnum_str(kk[i], &x)) { kv.kind = 2; kv.n = x; }
                *aw_var(kn) = kv;
                aw->pos = bpos; aw_stmt(1);
                if (aw->brk) { aw->brk = 0; break; }
                aw->cont = 0;
                if (aw->exit_code || aw->nxt || aw->err) break;
            }
            aw->pos = bpos; aw_stmt(0);
            return;
        }
        aw->pos = save;
        if (!aw_peek(';')) aw_expr(ex, 0);
        if (!aw_eat(';')) { aw->err = 1; return; }
        int cpos = aw->pos;
        if (!aw_peek(';')) aw_expr(0, 0);
        if (!aw_eat(';')) { aw->err = 1; return; }
        int ipos = aw->pos;
        if (!aw_peek(')')) aw_expr(0, 0);
        if (!aw_eat(')')) { aw->err = 1; return; }
        int bpos = aw->pos;
        aw_stmt(0); int endpos = aw->pos;
        for (;;)
        {
            aw->pos = cpos; int t = ex;
            if (ex && !aw_peek(';')) { AVal c = aw_expr(1, 0); t = aw_truth(&c); }
            if (!t || aw->exit_code || aw->nxt || aw->err) break;
            aw->pos = bpos; aw_stmt(1);
            if (aw->brk) { aw->brk = 0; break; }
            aw->cont = 0;
            aw->pos = ipos; if (!aw_peek(')')) aw_expr(1, 0);
            if (aw_tick() || aw->err) return;
        }
        aw->pos = endpos;
        return;
    }
    AVal v = aw_expr(ex, 0); (void)v;
    aw_end_stmt();
}

struct AwItem { int kind; int pat; int block; };   /* kind 0 normal, 1 BEGIN, 2 END */

static int awk_run(char** w, int n, const char* in, uint32_t inlen, Out* out)
{
    const char* prog = 0; char fs[40]; fs[0] = ' '; fs[1] = 0;
    for (int i = 1; i < n; ++i)
    {
        if (w[i][0] == '-' && w[i][1] == 'F')
        {
            const char* f = w[i][2] ? w[i] + 2 : (i + 1 < n ? w[++i] : "");
            if (f[0] == '\\' && f[1] == 't') { fs[0] = '\t'; fs[1] = 0; } else str_copy(fs, f, 40);
        }
        else if (!prog && w[i][0] != '-') prog = w[i];
        else if (!prog && str_equal(w[i], "-f")) return -2;
        else return -2;
    }
    if (!prog) return -2;
    Awk* A = (Awk*)kmalloc(sizeof(Awk));
    if (!A) return -2;
    for (unsigned i = 0; i < sizeof(Awk); ++i) ((char*)A)[i] = 0;
    aw = A; A->src = prog; A->pos = 0; A->out = out;
    *aw_var("FS") = aw_mkstr(fs);
    aw_set_nf(0); *aw_var("NR") = aw_mknum(0);
    AwItem items[24]; int ni = 0;
    /* pre-scan */
    for (;;)
    {
        aw_ws(1);
        while (A->src[A->pos] == ';') { ++A->pos; aw_ws(1); }
        if (!A->src[A->pos]) break;
        if (ni >= 24) { A->err = 1; break; }
        AwItem* it = &items[ni]; it->kind = 0; it->pat = -1; it->block = -1;
        if (aw_keyword_at("BEGIN")) { A->pos += 5; it->kind = 1; }
        else if (aw_keyword_at("END")) { A->pos += 3; it->kind = 2; }
        else if (!aw_peek('{')) { it->pat = A->pos; aw_expr(0, 0); }
        aw_ws(0);
        if (A->src[A->pos] == '{') { it->block = A->pos + 1; ++A->pos; aw_block(0); }
        else if (it->kind) A->err = 1;
        ++ni;
        if (A->err) break;
    }
    int bad = A->err;
    if (!bad)
    {
        A->rec[0] = 0;
        for (int i = 0; i < ni && !A->ex && !A->err; ++i) if (items[i].kind == 1) { A->pos = items[i].block; aw_block(1); }
        uint32_t pos = 0, len = 0; const char* line; long long nr = 0;
        while (!A->ex && !A->err && (line = split_next(in, inlen, &pos, &len)) != 0)
        {
            char tmp[1024]; int l = (int)(len < 1020 ? len : 1020);
            for (int i = 0; i < l; ++i) tmp[i] = line[i];
            tmp[l] = 0;
            aw_split_record(tmp); *aw_var("NR") = aw_mknum(++nr);
            A->nxt = 0;
            for (int i = 0; i < ni && !A->nxt && !A->ex && !A->err; ++i)
            {
                if (items[i].kind) continue;
                int hit = 1;
                if (items[i].pat >= 0) { A->pos = items[i].pat; AVal v = aw_expr(1, 0); hit = aw_truth(&v); }
                if (!hit) continue;
                if (items[i].block >= 0) { A->pos = items[i].block; A->brk = A->cont = 0; aw_block(1); }
                else { o_str(out, A->rec); o_char(out, '\n'); }
            }
        }
        A->ex = 0;
        if (!A->err) for (int i = 0; i < ni && !A->err; ++i) if (items[i].kind == 2) { A->pos = items[i].block; aw_block(1); }
    }
    int err = A->err;
    kfree(A); aw = 0;
    if (err == 2) uart_puts("awk: division by zero\r\n");
    else if (err == 3) uart_puts("awk: loop limit exceeded\r\n");
    return err ? -2 : 0;
}

/* ============================================================================================
   rev  tac  nl  xargs
   ============================================================================================ */
static int xargs_run(char** w, int n, const char* in, uint32_t inlen, Out* out)
{
    int per = 0, ri = -1; const char* repl = 0; int i = 1;
    for (; i < n; ++i)
    {
        if (str_equal(w[i], "-n") && i + 1 < n) { per = 0; const char* s = w[++i]; while (*s >= '0' && *s <= '9') per = per * 10 + (*s++ - '0'); if (per <= 0) return -2; }
        else if (w[i][0] == '-' && w[i][1] == 'I') { repl = w[i][2] ? w[i] + 2 : "{}"; }
        else break;
    }
    char base[200]; base[0] = 0;
    for (int k = i; k < n; ++k) { int l = str_len(base); if (l) base[l++] = ' '; str_copy(base + l, w[k], 200 - l); }
    if (!base[0]) str_copy(base, "echo", 200);
    (void)ri;
    /* collect items */
    static char items[512][64]; int ni = 0;
    if (repl)
    {
        uint32_t pos = 0, len = 0; const char* line;
        while ((line = split_next(in, inlen, &pos, &len)) != 0 && ni < 512)
        { if (!len) continue; int l = (int)(len < 63 ? len : 63); for (int k = 0; k < l; ++k) items[ni][k] = line[k]; items[ni][l] = 0; ++ni; }
    }
    else
    {
        uint32_t p = 0;
        while (p < inlen && ni < 512)
        {
            while (p < inlen && (in[p] == ' ' || in[p] == '\n' || in[p] == '\t')) ++p;
            if (p >= inlen) break;
            int l = 0; while (p < inlen && in[p] != ' ' && in[p] != '\n' && in[p] != '\t') { if (l < 63) items[ni][l++] = in[p]; ++p; }
            items[ni][l] = 0; ++ni;
        }
    }
    if (ni == 0) return 0;
    char* cap = (char*)kmalloc(16384);
    if (!cap) return -2;
    int idx = 0;
    while (idx < ni)
    {
        char cmd[480]; int cl = 0;
        if (repl)
        {
            int rl = str_len(repl);
            for (const char* b = base; *b && cl < 470;)
            {
                int m = 1; for (int k = 0; k < rl; ++k) if (b[k] != repl[k]) { m = 0; break; }
                if (m) { for (const char* it = items[idx]; *it && cl < 470; ++it) cmd[cl++] = *it; b += rl; } else cmd[cl++] = *b++;
            }
            ++idx;
        }
        else
        {
            for (const char* b = base; *b && cl < 470; ++b) cmd[cl++] = *b;
            int take = per > 0 ? per : ni;
            for (int k = 0; k < take && idx < ni; ++k, ++idx)
            { cmd[cl++] = ' '; for (const char* it = items[idx]; *it && cl < 470; ++it) cmd[cl++] = *it; }
        }
        cmd[cl] = 0;
        int got = filters_capture(cmd, cap, 16384);
        if (got > 0) o_mem(out, cap, (uint32_t)got);
    }
    kfree(cap);
    return 0;
}

int textutils_is_filter(const char* name)
{
    static const char* f[] = { "sed", "awk", "rev", "tac", "nl", "xargs", "more", "less" };
    for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); ++i) if (str_equal(name, f[i])) return 1;
    return 0;
}

int textutils_apply(char** w, int n, const char* in, uint32_t inlen, char* out, uint32_t* outlen, uint32_t outcap)
{
    Out o = { out, 0, outcap };
    const char* name = w[0];
    int rc = 0;
    if (str_equal(name, "sed")) rc = sed_run(w, n, in, inlen, &o);
    else if (str_equal(name, "awk")) rc = awk_run(w, n, in, inlen, &o);
    else if (str_equal(name, "xargs")) rc = xargs_run(w, n, in, inlen, &o);
    else if (str_equal(name, "rev"))
    {
        if (n != 1) return -2;
        uint32_t pos = 0, len = 0; const char* l;
        while ((l = split_next(in, inlen, &pos, &len)) != 0) { for (uint32_t i = len; i > 0; --i) o_char(&o, l[i - 1]); o_char(&o, '\n'); }
    }
    else if (str_equal(name, "tac"))
    {
        if (n != 1) return -2;
        static const char* lp[TU_MAX_LINES]; static uint32_t ll[TU_MAX_LINES];
        uint32_t cnt = 0, pos = 0, len = 0; const char* l;
        while ((l = split_next(in, inlen, &pos, &len)) != 0 && cnt < TU_MAX_LINES) { lp[cnt] = l; ll[cnt] = len; ++cnt; }
        for (uint32_t i = cnt; i > 0; --i) { o_mem(&o, lp[i - 1], ll[i - 1]); o_char(&o, '\n'); }
    }
    else if (str_equal(name, "nl"))
    {
        if (n != 1) return -2;
        uint32_t pos = 0, len = 0; const char* l; long long k = 0;
        while ((l = split_next(in, inlen, &pos, &len)) != 0)
        {
            ++k; char t[16]; int tn = 0; long long v = k; do { t[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
            for (int pad = tn; pad < 6; ++pad) o_char(&o, ' ');
            while (tn) o_char(&o, t[--tn]);
            o_char(&o, '\t'); o_mem(&o, l, len); o_char(&o, '\n');
        }
    }
    else if (str_equal(name, "more") || str_equal(name, "less")) { if (n != 1) return -2; o_mem(&o, in, inlen); }
    else return -1;
    *outlen = o.o;
    return rc;
}
