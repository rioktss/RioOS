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

#include "../libc/ulibc.h"
#include "../../include/user_api.h"
/* vi: modal editor. Normal: h j k l arrows 0 ^ $ w b e gg G {n}G x X dd dw d$ D cc cw C yy yw p P u (undo) ^R (redo) . (repeat)
   o O i a I A r R J ~ / ? n N ^F ^B ^L, counts. Ex: :w [f] :q :q! :wq :x :N :set nu/nonu :s/a/b/[g] :%s/a/b/[g] :d :$ :r file :e! */
#define ROWS 24
#define COLS 80
struct Ln { char* s; int n, cap; };
static Ln* L; static int nl, lcap;
static int cy, cx, top, want_x;
static char fname[200]; static int dirty, showno, ro;
static char msg[160];
static char* yank; static int yank_lines;   /* line-wise register (yank==joined with \n); */
static char* ychar; static int ychar_len;   /* char-wise register */
static char search[80]; static int search_dir = 1;
struct Snap { char** s; int n; int cy; };
static Snap undo_st[60]; static int nundo; static Snap redo_st[60]; static int nredo;

static void ln_set(Ln* l, const char* s, int n) { if (n + 1 > l->cap) { l->cap = n + 32; l->s = (char*)realloc(l->s, (unsigned long)l->cap); } memcpy(l->s, s, (size_t)n); l->n = n; l->s[n] = 0; }
static void ln_reserve(Ln* l, int n) { if (n + 1 > l->cap) { l->cap = n + 32; l->s = (char*)realloc(l->s, (unsigned long)l->cap); } }
static void buf_reserve(int n) { if (n > lcap) { lcap = n + 64; L = (Ln*)realloc(L, sizeof(Ln) * (unsigned long)lcap); } }
static void ins_line(int at, const char* s, int n) { buf_reserve(nl + 1); memmove(&L[at + 1], &L[at], sizeof(Ln) * (unsigned long)(nl - at)); L[at].s = 0; L[at].cap = 0; L[at].n = 0; ln_set(&L[at], s, n); ++nl; }
static void del_line(int at) { free(L[at].s); memmove(&L[at], &L[at + 1], sizeof(Ln) * (unsigned long)(nl - at - 1)); --nl; if (nl == 0) ins_line(0, "", 0); }

static void snap_free(Snap* s) { for (int i = 0; i < s->n; ++i) free(s->s[i]); free(s->s); s->s = 0; s->n = 0; }
static void snap_take(Snap* s)
{
    s->n = nl; s->cy = cy; s->s = (char**)malloc(sizeof(char*) * (unsigned long)nl);
    for (int i = 0; i < nl; ++i) { s->s[i] = (char*)malloc((unsigned long)L[i].n + 1); memcpy(s->s[i], L[i].s, (size_t)L[i].n + 1); }
}
static void snap_restore(Snap* s)
{
    for (int i = 0; i < nl; ++i) free(L[i].s);
    nl = 0; for (int i = 0; i < s->n; ++i) { buf_reserve(nl + 1); L[nl].s = 0; L[nl].cap = 0; L[nl].n = 0; ln_set(&L[nl], s->s[i], (int)strlen(s->s[i])); ++nl; }
    if (!nl) ins_line(0, "", 0);
    cy = s->cy < nl ? s->cy : nl - 1;
}
static void push_undo()
{
    if (nundo == 60) { snap_free(&undo_st[0]); memmove(&undo_st[0], &undo_st[1], sizeof(Snap) * 59); --nundo; }
    snap_take(&undo_st[nundo++]);
    for (int i = 0; i < nredo; ++i) snap_free(&redo_st[i]); nredo = 0;
    dirty = 1;
}
static void do_undo()
{
    if (!nundo) { snprintf(msg, sizeof(msg), "Already at oldest change"); return; }
    if (nredo < 60) snap_take(&redo_st[nredo++]);
    Snap* s = &undo_st[--nundo]; snap_restore(s); snap_free(s); dirty = 1; snprintf(msg, sizeof(msg), "undo");
}
static void do_redo()
{
    if (!nredo) { snprintf(msg, sizeof(msg), "Already at newest change"); return; }
    snap_take(&undo_st[nundo++]);
    Snap* s = &redo_st[--nredo]; snap_restore(s); snap_free(s); dirty = 1; snprintf(msg, sizeof(msg), "redo");
}

static int load(const char* path)
{
    FILE* f = fopen(path, "r"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char* d = (char*)malloc((unsigned long)sz + 1); size_t got = sz ? fread(d, 1, (size_t)sz, f) : 0; fclose(f); d[got] = 0;
    size_t st = 0;
    for (size_t i = 0; i <= got; ++i)
        if (i == got || d[i] == '\n') { if (i == got && st == got) break; ins_line(nl, d + st, (int)(i - st)); st = i + 1; }
    free(d); return 0;
}
static int save(const char* path)
{
    long total = 0; for (int i = 0; i < nl; ++i) total += L[i].n + 1;
    char* b = (char*)malloc((unsigned long)total + 1); long p = 0;
    for (int i = 0; i < nl; ++i) { memcpy(b + p, L[i].s, (size_t)L[i].n); p += L[i].n; b[p++] = '\n'; }
    FILE* f = fopen(path, "w"); if (!f) { free(b); return -1; }
    size_t w = fwrite(b, 1, (size_t)total, f); fclose(f); free(b);
    return w == (size_t)total ? (int)total : -1;
}

/* ---- terminal ---- */
static char ob[16384]; static int on;
static void oflush() { int o = 0; while (o < on) { long w = write(1, ob + o, (size_t)(on - o)); if (w <= 0) { sleep_ms(1); continue; } o += (int)w; } on = 0; }
static void oc(char c) { if (on >= (int)sizeof(ob)) oflush(); ob[on++] = c; }
static void os(const char* s) { while (*s) oc(*s++); }
static int gutter() { if (!showno) return 0; int d = 1, n = nl; while (n >= 10) { n /= 10; ++d; } return d + 1; }
static int key_wait_ms(int ms)
{
    char c;
    for (int t = 0; ; ++t)
    {
        long n = read(0, &c, 1);
        if (n == 0) n = (long)user_read(&c, 1);
        if (n == 1) return (unsigned char)c;
        if (n < 0 && errno != EAGAIN && errno != EINTR) return -1;
        if (ms >= 0 && t * 5 >= ms) return -2;
        sleep_ms(5);
    }
}
enum { K_UP = 1000, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END, K_DEL, K_PGUP, K_PGDN };
static int pushback = -1;
static int getkey()
{
    int c; if (pushback >= 0) { c = pushback; pushback = -1; return c; }
    c = key_wait_ms(-1);
    if (c != 27) return c;
    int d = key_wait_ms(40); if (d == -2 || d == -1) return 27;
    if (d != '[' && d != 'O') { pushback = d; return 27; }
    int e = key_wait_ms(40); if (e < 0) return 27;
    if (e < '0' || (e > '9' && e < 'A') || e > 'Z') { pushback = e; return 27; }
    if (e >= '0' && e <= '9') { int e2 = key_wait_ms(40); if (e2 == '~') { switch (e) { case '1': case '7': return K_HOME; case '4': case '8': return K_END; case '3': return K_DEL; case '5': return K_PGUP; case '6': return K_PGDN; } } return 27; }
    switch (e) { case 'A': return K_UP; case 'B': return K_DOWN; case 'C': return K_RIGHT; case 'D': return K_LEFT; case 'H': return K_HOME; case 'F': return K_END; }
    return 27;
}

static int mode_insert, mode_replace;
static void clamp_x() { int mx = mode_insert ? L[cy].n : (L[cy].n ? L[cy].n - 1 : 0); if (cx > mx) cx = mx; if (cx < 0) cx = 0; }
static void redraw()
{
    int g = gutter(), tw = COLS - g, bodyrows = ROWS - 1;
    if (cy < top) top = cy; if (cy >= top + bodyrows) top = cy - bodyrows + 1;
    os("\x1b[?25l\x1b[H");
    for (int r = 0; r < bodyrows; ++r)
    {
        int li = top + r; os("\x1b[K");
        if (li < nl)
        {
            if (showno) { char t[16]; snprintf(t, sizeof(t), "%*d ", g - 1, li + 1); os(t); }
            int col = 0; for (int i = 0; i < L[li].n && col < tw; ++i) { char c = L[li].s[i]; if (c == '\t') { int k = 8 - col % 8; while (k-- && col < tw) { oc(' '); ++col; } } else if ((unsigned char)c < 32) { oc('^'); oc(c + 64); col += 2; } else { oc(c); ++col; } }
        }
        else oc('~');
        os("\r\n");
    }
    char st[200];
    snprintf(st, sizeof(st), "\x1b[7m%s %s%s %d/%d\x1b[0m  %s", fname[0] ? fname : "[No Name]", dirty ? "[+]" : "", mode_insert ? (mode_replace ? " -- REPLACE --" : " -- INSERT --") : "", cy + 1, nl, msg);
    os("\x1b[K"); os(st);
    int col = 0; for (int i = 0; i < cx && i < L[cy].n; ++i) { char c = L[cy].s[i]; if (c == '\t') col += 8 - col % 8; else if ((unsigned char)c < 32) col += 2; else ++col; }
    char p[32]; snprintf(p, sizeof(p), "\x1b[%d;%dH\x1b[?25h", cy - top + 1, col + g + 1); os(p); oflush();
}

/* ---- search ---- */
static int find_from(int y, int x, int dir, int* ry, int* rx)
{
    int sl = (int)strlen(search); if (!sl) return 0;
    for (int k = 0; k <= nl; ++k)
    {
        int yy = ((y + dir * k) % nl + nl) % nl; const char* s = L[yy].s; int n = L[yy].n;
        if (dir > 0) { int st = (k == 0) ? x + 1 : 0; for (int i = st; i + sl <= n; ++i) if (!memcmp(s + i, search, (size_t)sl)) { *ry = yy; *rx = i; return 1; } }
        else { int st = (k == 0) ? x - 1 : n - sl; for (int i = st; i >= 0; --i) if (i + sl <= n && !memcmp(s + i, search, (size_t)sl)) { *ry = yy; *rx = i; return 1; } }
    }
    return 0;
}
static void do_search(int dir)
{
    int y, x; if (find_from(cy, cx, dir, &y, &x)) { cy = y; cx = x; } else snprintf(msg, sizeof(msg), "Pattern not found: %s", search);
}

static int readline_prompt(char first, char* out, int cap)
{
    int n = 0;
    for (;;)
    {
        char t[200]; snprintf(t, sizeof(t), "\x1b[%d;1H\x1b[K%c%.*s", ROWS, first, n, out); os(t); oflush();
        int c = getkey();
        if (c == 27) return 0;
        if (c == '\r' || c == '\n') { out[n] = 0; return 1; }
        if (c == 127 || c == 8) { if (!n) return 0; --n; continue; }
        if (c >= 32 && c < 127 && n < cap - 1) out[n++] = (char)c;
    }
}

/* ---- ex ---- */
static void subst_line(int y, const char* pat, const char* rep, int global, int* count)
{
    int pl = (int)strlen(pat), rl = (int)strlen(rep); if (!pl) return;
    char* out = (char*)malloc((unsigned long)L[y].n * (unsigned long)(rl + 1) + 8); int on2 = 0, i = 0, did = 0;
    while (i < L[y].n)
    {
        if (!(did && !global) && i + pl <= L[y].n && !memcmp(L[y].s + i, pat, (size_t)pl)) { memcpy(out + on2, rep, (size_t)rl); on2 += rl; i += pl; did = 1; ++*count; }
        else out[on2++] = L[y].s[i++];
    }
    if (did) ln_set(&L[y], out, on2);
    free(out);
}
static int quit_flag;
static void ex_command(char* cmd)
{
    while (*cmd == ' ') ++cmd;
    if (!*cmd) return;
    int a = cy, b = cy; int ranged = 0;
    if (*cmd == '%') { a = 0; b = nl - 1; ranged = 1; ++cmd; }
    else if (*cmd >= '0' && *cmd <= '9')
    {
        int v = atoi(cmd); while (*cmd >= '0' && *cmd <= '9') ++cmd;
        if (*cmd == ',') { ++cmd; int w = 0; if (*cmd == '$') { w = nl; ++cmd; } else { w = atoi(cmd); while (*cmd >= '0' && *cmd <= '9') ++cmd; } a = v - 1; b = w - 1; ranged = 1; }
        else { cy = v - 1; if (cy >= nl) cy = nl - 1; if (cy < 0) cy = 0; cx = 0; return; }
    }
    else if (*cmd == '$' && !cmd[1]) { cy = nl - 1; cx = 0; return; }
    else if (*cmd == '.' && cmd[1] == ',') { cmd += 2; int w; if (*cmd == '$') { w = nl; ++cmd; } else { w = atoi(cmd); while (*cmd >= '0' && *cmd <= '9') ++cmd; } a = cy; b = w - 1; ranged = 1; }
    if (a < 0) a = 0; if (b >= nl) b = nl - 1; if (a > b) { int t = a; a = b; b = t; }
    (void)ranged;
    char* arg = cmd; while (*arg && *arg != ' ' && !(arg == cmd && 0)) { if (*arg == '!' ) { break; } ++arg; }
    char name[16]; int nlen = 0;
    char* p = cmd;
    if (*p == 's' && (p[1] == '/' || p[1] == '#' || p[1] == '|')) { name[0] = 's'; name[1] = 0; }
    else { while (*p && *p != ' ' && *p != '!' && nlen < 15) name[nlen++] = *p++; name[nlen] = 0; }
    int bang = (*p == '!'); if (bang) ++p;
    while (*p == ' ') ++p;
    if (!strcmp(name, "w") || !strcmp(name, "write")) { const char* f = *p ? p : fname; if (!f[0]) { snprintf(msg, sizeof(msg), "No file name"); return; } int r = save(f); if (r < 0) snprintf(msg, sizeof(msg), "Cannot write %s", f); else { snprintf(msg, sizeof(msg), "\"%s\" %dL, %dC written", f, nl, r); if (!fname[0]) strncpy(fname, f, sizeof(fname) - 1); if (!*p || !strcmp(f, fname)) dirty = 0; } }
    else if (!strcmp(name, "q") || !strcmp(name, "quit")) { if (dirty && !bang) snprintf(msg, sizeof(msg), "No write since last change (add ! to override)"); else quit_flag = 1; }
    else if (!strcmp(name, "wq") || !strcmp(name, "x") || !strcmp(name, "xit")) { const char* f = *p ? p : fname; if (!f[0]) { snprintf(msg, sizeof(msg), "No file name"); return; } if (!strcmp(name, "x") && !dirty) { quit_flag = 1; return; } if (save(f) < 0) snprintf(msg, sizeof(msg), "Cannot write %s", f); else { dirty = 0; quit_flag = 1; } }
    else if (!strcmp(name, "e") && bang) { if (!fname[0]) return; for (int i = 0; i < nl; ++i) free(L[i].s); nl = 0; if (load(fname) < 0) ins_line(0, "", 0); if (!nl) ins_line(0, "", 0); cy = 0; cx = 0; dirty = 0; snprintf(msg, sizeof(msg), "reloaded"); }
    else if (!strcmp(name, "r") || !strcmp(name, "read")) { if (!*p) return; Ln* save_L = 0; (void)save_L; FILE* f = fopen(p, "r"); if (!f) { snprintf(msg, sizeof(msg), "Cannot read %s", p); return; } fclose(f); push_undo(); int at = cy + 1; int before = nl; { int sy = nl; (void)sy; }
        FILE* g = fopen(p, "r"); fseek(g, 0, SEEK_END); long sz = ftell(g); fseek(g, 0, SEEK_SET); char* d = (char*)malloc((unsigned long)sz + 1); size_t got = sz ? fread(d, 1, (size_t)sz, g) : 0; fclose(g); size_t st = 0; int k = at;
        for (size_t i = 0; i <= got; ++i) if (i == got || d[i] == '\n') { if (i == got && st == got) break; ins_line(k++, d + st, (int)(i - st)); st = i + 1; }
        free(d); snprintf(msg, sizeof(msg), "%d lines read", nl - before); }
    else if (!strcmp(name, "d") || !strcmp(name, "delete")) { push_undo(); for (int i = b; i >= a; --i) del_line(i); if (cy >= nl) cy = nl - 1; snprintf(msg, sizeof(msg), "%d lines deleted", b - a + 1); }
    else if (!strcmp(name, "set")) { if (!strcmp(p, "nu") || !strcmp(p, "number")) showno = 1; else if (!strcmp(p, "nonu") || !strcmp(p, "nonumber")) showno = 0; else snprintf(msg, sizeof(msg), "Unknown option: %s", p); }
    else if (!strcmp(name, "s"))
    {
        char dl = cmd[1]; char* pat = cmd + 2; char* q = pat; while (*q && *q != dl) ++q; if (!*q) { snprintf(msg, sizeof(msg), "bad :s"); return; } *q++ = 0;
        char* rep = q; while (*q && *q != dl) ++q; int gl = 0; if (*q) { *q++ = 0; if (*q == 'g') gl = 1; }
        push_undo(); int cnt = 0; for (int i = a; i <= b; ++i) subst_line(i, pat, rep, gl, &cnt);
        if (!cnt) { snprintf(msg, sizeof(msg), "Pattern not found: %s", pat); if (nundo) { Snap* s = &undo_st[--nundo]; snap_free(s); } } else snprintf(msg, sizeof(msg), "%d substitutions", cnt);
    }
    else snprintf(msg, sizeof(msg), "Not an editor command: %s", cmd);
    if (cy >= nl) cy = nl - 1;
}

/* ---- motions ---- */
static int is_word(char c) { return isalnum((unsigned char)c) || c == '_'; }
static int cls(char c) { return c == ' ' || c == '\t' ? 0 : is_word(c) ? 1 : 2; }
static void m_word_fwd(int cnt)
{
    while (cnt--)
    {
        char* s = L[cy].s; int n = L[cy].n;
        if (cx < n) { int k = cls(s[cx]); if (k) while (cx < n && cls(s[cx]) == k) ++cx; }
        for (;;) { s = L[cy].s; n = L[cy].n; while (cx < n && cls(s[cx]) == 0) ++cx; if (cx < n) break; if (cy + 1 >= nl) { cx = n ? n - 1 : 0; return; } ++cy; cx = 0; if (!L[cy].n) break; }
    }
}
static void m_word_back(int cnt)
{
    while (cnt--)
    {
        for (;;) { if (cx == 0) { if (cy == 0) return; --cy; cx = L[cy].n; if (!L[cy].n) { cx = 0; return; } continue; } --cx; if (cls(L[cy].s[cx]) != 0) break; }
        int k = cls(L[cy].s[cx]); while (cx > 0 && cls(L[cy].s[cx - 1]) == k) --cx;
    }
}
static void m_word_end(int cnt)
{
    while (cnt--)
    {
        ++cx;
        for (;;) { while (cx < L[cy].n && cls(L[cy].s[cx]) == 0) ++cx; if (cx < L[cy].n) break; if (cy + 1 >= nl) { cx = L[cy].n ? L[cy].n - 1 : 0; return; } ++cy; cx = 0; }
        int k = cls(L[cy].s[cx]); while (cx + 1 < L[cy].n && cls(L[cy].s[cx + 1]) == k) ++cx;
    }
}
static void set_ychar(const char* s, int n) { free(ychar); ychar = (char*)malloc((unsigned long)n + 1); memcpy(ychar, s, (size_t)n); ychar[n] = 0; ychar_len = n; free(yank); yank = 0; yank_lines = 0; }
static void set_yline(int a, int b)
{
    free(yank); free(ychar); ychar = 0; ychar_len = 0; long t = 0; for (int i = a; i <= b; ++i) t += L[i].n + 1;
    yank = (char*)malloc((unsigned long)t + 1); long p = 0; for (int i = a; i <= b; ++i) { memcpy(yank + p, L[i].s, (size_t)L[i].n); p += L[i].n; yank[p++] = '\n'; } yank[p] = 0; yank_lines = b - a + 1;
}
static void del_chars(int y, int a, int b)   /* [a,b) */
{
    if (b > L[y].n) b = L[y].n; if (a >= b) return; set_ychar(L[y].s + a, b - a);
    memmove(L[y].s + a, L[y].s + b, (size_t)(L[y].n - b + 1)); L[y].n -= b - a;
}
static void put_text(int after)
{
    if (yank_lines)
    {
        push_undo(); int at = after ? cy + 1 : cy; const char* p = yank; int k = at;
        while (*p) { const char* e = p; while (*e != '\n') ++e; ins_line(k++, p, (int)(e - p)); p = e + 1; }
        cy = at; cx = 0;
    }
    else if (ychar)
    {
        push_undo(); int at = after && L[cy].n ? cx + 1 : cx; if (at > L[cy].n) at = L[cy].n;
        ln_reserve(&L[cy], L[cy].n + ychar_len + 1); memmove(L[cy].s + at + ychar_len, L[cy].s + at, (size_t)(L[cy].n - at + 1)); memcpy(L[cy].s + at, ychar, (size_t)ychar_len); L[cy].n += ychar_len; cx = at + ychar_len - 1;
    }
}

static int dot_buf[256], dot_n, dot_rec, replaying; static int dot_pending[256], dot_pn;
static int rawkey() { if (replaying && dot_pn) { return dot_pending[--dot_pn] ? 0 : 0; } return getkey(); }
static int pending_keys[512], pk_n, pk_i;
static int nextkey()
{
    if (pk_i < pk_n) return pending_keys[pk_i++];
    int k = getkey();
    if (dot_rec && dot_n < 255) dot_buf[dot_n++] = k;
    return k;
}
static void insert_session(int first_count)
{
    (void)first_count; mode_insert = 1;
    for (;;)
    {
        clamp_x(); redraw();
        int c = nextkey();
        if (c == 27) break;
        if (c == K_LEFT) { if (cx > 0) --cx; continue; } if (c == K_RIGHT) { if (cx < L[cy].n) ++cx; continue; }
        if (c == K_UP) { if (cy > 0) --cy; clamp_x(); continue; } if (c == K_DOWN) { if (cy + 1 < nl) ++cy; clamp_x(); continue; }
        if (c == K_HOME) { cx = 0; continue; } if (c == K_END) { cx = L[cy].n; continue; }
        if (c == K_DEL) { if (cx < L[cy].n) { memmove(L[cy].s + cx, L[cy].s + cx + 1, (size_t)(L[cy].n - cx)); --L[cy].n; } continue; }
        if (c == 127 || c == 8)
        {
            if (cx > 0) { memmove(L[cy].s + cx - 1, L[cy].s + cx, (size_t)(L[cy].n - cx + 1)); --L[cy].n; --cx; }
            else if (cy > 0) { int pn = L[cy - 1].n; ln_reserve(&L[cy - 1], pn + L[cy].n + 1); memcpy(L[cy - 1].s + pn, L[cy].s, (size_t)L[cy].n + 1); L[cy - 1].n = pn + L[cy].n; del_line(cy); --cy; cx = pn; }
            continue;
        }
        if (c == '\r' || c == '\n')
        {
            int rn = L[cy].n - cx; char* tail = (char*)malloc((unsigned long)rn + 1); memcpy(tail, L[cy].s + cx, (size_t)rn); tail[rn] = 0;
            L[cy].n = cx; L[cy].s[cx] = 0; ins_line(cy + 1, tail, rn); free(tail); ++cy; cx = 0; continue;
        }
        if (c == 23) { while (cx > 0 && L[cy].s[cx - 1] == ' ') { memmove(L[cy].s + cx - 1, L[cy].s + cx, (size_t)(L[cy].n - cx + 1)); --L[cy].n; --cx; } if (cx > 0) { int k = cls(L[cy].s[cx - 1]); while (cx > 0 && cls(L[cy].s[cx - 1]) == k) { memmove(L[cy].s + cx - 1, L[cy].s + cx, (size_t)(L[cy].n - cx + 1)); --L[cy].n; --cx; } } continue; }
        if (c == '\t' || (c >= 32 && c < 127))
        {
            if (mode_replace && cx < L[cy].n) { L[cy].s[cx++] = (char)c; continue; }
            ln_reserve(&L[cy], L[cy].n + 2); memmove(L[cy].s + cx + 1, L[cy].s + cx, (size_t)(L[cy].n - cx + 1)); L[cy].s[cx++] = (char)c; ++L[cy].n;
        }
    }
    mode_insert = 0; mode_replace = 0; if (cx > 0) --cx; clamp_x();
}

static void run_normal(int c, int cnt, int has_cnt);
static void exec_dot() { if (!dot_n) return; int k = dot_n; int tmp[256]; memcpy(tmp, dot_buf, sizeof(int) * (size_t)k); memcpy(pending_keys, tmp, sizeof(int) * (size_t)k); pk_n = k; pk_i = 0; int save_rec = dot_rec; dot_rec = 0; int c = pending_keys[pk_i++]; int cnt = 1, has = 0; while (c >= '1' && c <= '9') { cnt = 0; has = 1; while (c >= '0' && c <= '9') { cnt = cnt * 10 + c - '0'; c = pending_keys[pk_i++]; } } run_normal(c, cnt, has); pk_n = pk_i = 0; dot_rec = save_rec; }

static int is_edit_cmd(int c) { return strchr("xXdDcCpPoOiaIAARrJ~", c) != 0 && c != 0; }
static void run_normal(int c, int cnt, int has_cnt)
{
    switch (c)
    {
    case 'h': case K_LEFT: case 8: cx -= cnt; if (cx < 0) cx = 0; want_x = cx; break;
    case 'l': case K_RIGHT: case ' ': cx += cnt; clamp_x(); want_x = cx; break;
    case 'j': case K_DOWN: case '\r': case '\n': case 14: cy += cnt; if (cy >= nl) cy = nl - 1; cx = want_x; clamp_x(); break;
    case 'k': case K_UP: case 16: cy -= cnt; if (cy < 0) cy = 0; cx = want_x; clamp_x(); break;
    case '0': case K_HOME: cx = 0; want_x = 0; break;
    case '^': cx = 0; while (cx < L[cy].n && (L[cy].s[cx] == ' ' || L[cy].s[cx] == '\t')) ++cx; clamp_x(); break;
    case '$': case K_END: cx = L[cy].n ? L[cy].n - 1 : 0; want_x = 1 << 20; break;
    case 'w': m_word_fwd(cnt); want_x = cx; break;
    case 'b': m_word_back(cnt); want_x = cx; break;
    case 'e': m_word_end(cnt); want_x = cx; break;
    case 'G': cy = has_cnt ? cnt - 1 : nl - 1; if (cy >= nl) cy = nl - 1; if (cy < 0) cy = 0; cx = 0; break;
    case 'g': { int d = nextkey(); if (d == 'g') { cy = has_cnt ? cnt - 1 : 0; if (cy >= nl) cy = nl - 1; cx = 0; } break; }
    case 6: case K_PGDN: cy += (ROWS - 3) * cnt; if (cy >= nl) cy = nl - 1; cx = 0; break;
    case 2: case K_PGUP: cy -= (ROWS - 3) * cnt; if (cy < 0) cy = 0; cx = 0; break;
    case 12: os("\x1b[2J"); break;
    case 'x': case K_DEL: if (L[cy].n) { push_undo(); del_chars(cy, cx, cx + cnt); clamp_x(); } break;
    case 'X': if (cx > 0) { push_undo(); int a = cx - cnt; if (a < 0) a = 0; del_chars(cy, a, cx); cx = a; } break;
    case 'D': push_undo(); del_chars(cy, cx, L[cy].n); clamp_x(); break;
    case 'C': push_undo(); del_chars(cy, cx, L[cy].n); insert_session(1); break;
    case 'd': case 'c': case 'y':
    {
        int d = nextkey(); int n2 = 1; while (d >= '1' && d <= '9') { n2 = 0; while (d >= '0' && d <= '9') { n2 = n2 * 10 + d - '0'; d = nextkey(); } }
        int total = cnt * n2;
        if (d == c)
        {
            int a = cy, b = cy + total - 1; if (b >= nl) b = nl - 1;
            set_yline(a, b);
            if (c == 'y') { snprintf(msg, sizeof(msg), "%d lines yanked", b - a + 1); break; }
            push_undo();
            if (c == 'c') { for (int i = b; i > a; --i) del_line(i); ln_set(&L[a], "", 0); cy = a; cx = 0; insert_session(1); }
            else { for (int i = b; i >= a; --i) del_line(i); if (cy >= nl) cy = nl - 1; cx = 0; }
        }
        else if (d == 'w' || d == 'e' || d == '$' || d == '0' || d == 'b' || d == '^')
        {
            int sy = cy, sx = cx; int ey, ex;
            if (d == '$') { ey = cy; ex = L[cy].n; }
            else if (d == '0') { ey = cy; ex = 0; }
            else if (d == '^') { ey = cy; ex = 0; while (ex < L[cy].n && L[cy].s[ex] == ' ') ++ex; }
            else if (d == 'b') { m_word_back(total); ey = cy; ex = cx; cy = sy; cx = sx; }
            else { if (d == 'w' && c == 'c') d = 'e'; m_word_fwd(total); if (d == 'e') { cy = sy; cx = sx; m_word_end(total); ++cx; } ey = cy; ex = cx; if (d == 'w' && ey != sy) { ey = sy; ex = L[sy].n; } cy = sy; cx = sx; }
            int a = sx, b = ex; if (b < a) { int t = a; a = b; b = t; }
            if (ey != sy && d != 'w' && d != 'e') { break; }
            if (c == 'y') { set_ychar(L[sy].s + a, (b > L[sy].n ? L[sy].n : b) - a); cx = a; clamp_x(); break; }
            push_undo(); del_chars(sy, a, b); cx = a;
            if (c == 'c') insert_session(1); else clamp_x();
        }
        else if (d == 'j' || d == 'k')
        {
            int a = d == 'j' ? cy : cy - total, b = d == 'j' ? cy + total : cy; if (a < 0) a = 0; if (b >= nl) b = nl - 1; set_yline(a, b);
            if (c == 'y') break; push_undo(); for (int i = b; i >= a; --i) del_line(i); if (cy >= nl) cy = nl - 1; cy = a < nl ? a : nl - 1; cx = 0;
            if (c == 'c') { ins_line(cy, "", 0); insert_session(1); }
        }
        break;
    }
    case 'p': for (int i = 0; i < cnt; ++i) put_text(1); break;
    case 'P': for (int i = 0; i < cnt; ++i) put_text(0); break;
    case 'u': for (int i = 0; i < cnt; ++i) do_undo(); cx = 0; break;
    case 18: for (int i = 0; i < cnt; ++i) do_redo(); cx = 0; break;
    case 'i': push_undo(); insert_session(cnt); break;
    case 'a': push_undo(); if (L[cy].n) ++cx; insert_session(cnt); break;
    case 'I': push_undo(); cx = 0; while (cx < L[cy].n && (L[cy].s[cx] == ' ' || L[cy].s[cx] == '\t')) ++cx; insert_session(cnt); break;
    case 'A': push_undo(); cx = L[cy].n; insert_session(cnt); break;
    case 'o': push_undo(); ins_line(cy + 1, "", 0); ++cy; cx = 0; insert_session(cnt); break;
    case 'O': push_undo(); ins_line(cy, "", 0); cx = 0; insert_session(cnt); break;
    case 'R': push_undo(); mode_replace = 1; insert_session(cnt); break;
    case 'r': { int d = nextkey(); if (d >= 32 && d < 127 && cx + cnt <= L[cy].n) { push_undo(); for (int i = 0; i < cnt; ++i) L[cy].s[cx + i] = (char)d; cx += cnt - 1; } break; }
    case '~': if (L[cy].n) { push_undo(); for (int i = 0; i < cnt && cx < L[cy].n; ++i) { char ch = L[cy].s[cx]; L[cy].s[cx] = (char)(isupper((unsigned char)ch) ? tolower(ch) : toupper(ch)); if (cx + 1 < L[cy].n) ++cx; } } break;
    case 'J': for (int i = 0; i < cnt && cy + 1 < nl; ++i) { if (!i) push_undo(); int pn = L[cy].n; int sp = pn && L[cy + 1].n; int skip = 0; while (skip < L[cy + 1].n && L[cy + 1].s[skip] == ' ') ++skip; ln_reserve(&L[cy], pn + L[cy + 1].n + 2); if (sp) L[cy].s[L[cy].n++] = ' '; memcpy(L[cy].s + L[cy].n, L[cy + 1].s + skip, (size_t)(L[cy + 1].n - skip)); L[cy].n += L[cy + 1].n - skip; L[cy].s[L[cy].n] = 0; del_line(cy + 1); cx = pn; } clamp_x(); break;
    case 's': if (L[cy].n) { push_undo(); del_chars(cy, cx, cx + cnt); } else push_undo(); insert_session(1); break;
    case 'S': push_undo(); ln_set(&L[cy], "", 0); cx = 0; insert_session(1); break;
    case '/': case '?': { char q[80]; q[0] = 0; if (readline_prompt((char)c, q, sizeof(q)) && q[0]) strncpy(search, q, sizeof(search) - 1); else if (!q[0]) break; search_dir = c == '/' ? 1 : -1; do_search(search_dir); break; }
    case 'n': for (int i = 0; i < cnt; ++i) do_search(search_dir); break;
    case 'N': for (int i = 0; i < cnt; ++i) do_search(-search_dir); break;
    case '.': exec_dot(); break;
    case ':': { char q[120]; q[0] = 0; msg[0] = 0; if (readline_prompt(':', q, sizeof(q))) ex_command(q); break; }
    case 'Z': { int d = nextkey(); if (d == 'Z') { if (fname[0] && dirty) { if (save(fname) < 0) { snprintf(msg, sizeof(msg), "Cannot write"); break; } dirty = 0; } quit_flag = 1; } break; }
    default: break;
    }
}

int main(int argc, char** argv)
{
    (void)rawkey; (void)dot_pending; (void)dot_pn; (void)replaying; (void)is_edit_cmd; (void)yank_lines;
    for (int i = 1; i < argc; ++i) { if (argv[i][0] == '-' && argv[i][1]) { if (!strcmp(argv[i], "-R")) ro = 1; else { printf("usage: vi [-R] [file]\n"); return 2; } } else { strncpy(fname, argv[i], sizeof(fname) - 1); } }
    if (fname[0] && load(fname) < 0) snprintf(msg, sizeof(msg), "\"%s\" [New File]", fname);
    else if (fname[0]) snprintf(msg, sizeof(msg), "\"%s\" %dL", fname, nl);
    if (!nl) ins_line(0, "", 0);
    os("\x1b[2J"); 
    while (!quit_flag)
    {
        clamp_x(); redraw();
        int c = getkey(); int cnt = 0, has = 0;
        while ((c >= '1' && c <= '9') || (has && c == '0')) { has = 1; cnt = cnt * 10 + (c - '0'); if (cnt > 100000) cnt = 100000; c = getkey(); }
        if (!has) cnt = 1;
        int recordable = strchr("xXdDcCpPoOiaIAsSRr~J", c) && c < 256 && c > 0;
        if (recordable) { dot_rec = 1; dot_n = 0; if (has) { char t[16]; snprintf(t, sizeof(t), "%d", cnt); for (char* q = t; *q; ++q) dot_buf[dot_n++] = *q; } dot_buf[dot_n++] = c; }
        else dot_rec = 0;
        if (ro && c != ':' && strchr("xXdDcCpPoOiaIAsSRr~J", c) && c > 0) { snprintf(msg, sizeof(msg), "Read-only (vi -R)"); dot_rec = 0; continue; }
        if (c != 27) msg[0] = 0;
        run_normal(c, cnt, has);
        if (dot_rec) dot_rec = 0;
    }
    os("\x1b[2J\x1b[H"); oflush();
    return 0;
}
