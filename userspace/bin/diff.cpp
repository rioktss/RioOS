#include "../libc/ulibc.h"
/* diff [-u|-U N] [-q] [-i] [-w] [-b] [-r] [-s] file1 file2   (normal and unified formats, exit 0 same / 1 differ / 2 error) */
struct Lines { char* data; unsigned long len; char** ln; unsigned* llen; int n; int nonl; };
static int opt_i = 0, opt_w = 0, opt_b = 0, opt_u = 0, opt_q = 0, opt_r = 0, opt_s = 0, ctxn = 3;

static int slurp(const char* path, Lines* L)
{
    FILE* f = fopen(path, "r"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    L->data = (char*)malloc((unsigned long)sz + 2); if (!L->data) { fclose(f); return -1; }
    size_t got = sz ? fread(L->data, 1, (size_t)sz, f) : 0; fclose(f);
    L->len = got; L->data[got] = 0;
    int cnt = 0; for (unsigned long i = 0; i < got; ++i) if (L->data[i] == '\n') ++cnt;
    L->nonl = got > 0 && L->data[got - 1] != '\n';
    L->n = cnt + L->nonl;
    L->ln = (char**)malloc(sizeof(char*) * (unsigned long)(L->n + 1)); L->llen = (unsigned*)malloc(sizeof(unsigned) * (unsigned long)(L->n + 1));
    if (!L->ln || !L->llen) return -1;
    int k = 0; unsigned long st = 0;
    for (unsigned long i = 0; i <= got && k < L->n; ++i)
        if (i == got || L->data[i] == '\n') { if (i == got && st == got) break; L->ln[k] = L->data + st; L->llen[k] = (unsigned)(i - st); ++k; st = i + 1; }
    return 0;
}
static int eqline(const Lines* a, int i, const Lines* b, int j)
{
    const char* p = a->ln[i]; const char* q = b->ln[j]; unsigned pl = a->llen[i], ql = b->llen[j];
    if (!opt_i && !opt_w && !opt_b)
    {
        if (pl != ql || memcmp(p, q, pl)) return 0;
    }
    else
    {
        unsigned x = 0, y = 0;
        for (;;)
        {
            if (opt_w) { while (x < pl && (p[x] == ' ' || p[x] == '\t')) ++x; while (y < ql && (q[y] == ' ' || q[y] == '\t')) ++y; }
            else if (opt_b)
            {
                int sp = x < pl && (p[x] == ' ' || p[x] == '\t'), sq = y < ql && (q[y] == ' ' || q[y] == '\t');
                if (sp != sq) return 0;
                if (sp) { while (x < pl && (p[x] == ' ' || p[x] == '\t')) ++x; while (y < ql && (q[y] == ' ' || q[y] == '\t')) ++y; }
            }
            if (x >= pl || y >= ql) { if (x >= pl && y >= ql) break; if (opt_b && !opt_w) return 0; return 0; }
            char c = p[x], d = q[y]; if (opt_i) { c = (char)tolower((unsigned char)c); d = (char)tolower((unsigned char)d); }
            if (c != d) return 0;
            ++x; ++y;
        }
    }
    int la = (i == a->n - 1 && a->nonl), lb = (j == b->n - 1 && b->nonl);
    return la == lb;
}
static void free_lines(Lines* L) { free(L->data); free(L->ln); free(L->llen); }
static void print_line(const char* pre, const Lines* L, int i)
{
    printf("%s%.*s\n", pre, (int)L->llen[i], L->ln[i]);
    if (i == L->n - 1 && L->nonl) printf("\\ No newline at end of file\n");
}
struct Op { unsigned char kind; };      /* 0 equal, 1 delete (from a), 2 insert (from b) */

static int diff_files(const char* pa, const char* pb, const char* na, const char* nb)
{
    Lines A, B; memset(&A, 0, sizeof(A)); memset(&B, 0, sizeof(B));
    if (slurp(pa, &A)) { printf("diff: %s: cannot read\n", pa); return 2; }
    if (slurp(pb, &B)) { printf("diff: %s: cannot read\n", pb); free_lines(&A); return 2; }
    int lo = 0; while (lo < A.n && lo < B.n && eqline(&A, lo, &B, lo)) ++lo;
    int ea = A.n, eb = B.n; while (ea > lo && eb > lo && eqline(&A, ea - 1, &B, eb - 1)) { --ea; --eb; }
    if (lo == A.n && lo == B.n) { if (opt_s) printf("Files %s and %s are identical\n", na, nb); free_lines(&A); free_lines(&B); return 0; }
    if (opt_q) { printf("Files %s and %s differ\n", na, nb); free_lines(&A); free_lines(&B); return 1; }
    int n = ea - lo, m = eb - lo;
    Op* ops = (Op*)malloc(sizeof(Op) * (unsigned long)(A.n + B.n + 2)); int nops = 0;
    for (int i = 0; i < lo; ++i) ops[nops++].kind = 0;
    if ((unsigned long)(n + 1) * (unsigned long)(m + 1) > 6000000UL)
    {
        printf("diff: %s and %s differ in too many lines for a line-by-line comparison\n", na, nb);
        free(ops); free_lines(&A); free_lines(&B); return 1;
    }
    unsigned short* T = (unsigned short*)malloc(sizeof(unsigned short) * (unsigned long)(n + 1) * (unsigned long)(m + 1));
    if (!T) { printf("diff: out of memory\n"); free(ops); free_lines(&A); free_lines(&B); return 2; }
    #define TT(i, j) T[(unsigned long)(i) * (unsigned long)(m + 1) + (unsigned long)(j)]
    for (int i = n; i >= 0; --i)
        for (int j = m; j >= 0; --j)
        {
            if (i == n || j == m) TT(i, j) = 0;
            else if (eqline(&A, lo + i, &B, lo + j)) TT(i, j) = (unsigned short)(TT(i + 1, j + 1) + 1);
            else TT(i, j) = TT(i + 1, j) >= TT(i, j + 1) ? TT(i + 1, j) : TT(i, j + 1);
        }
    { int i = 0, j = 0;
      while (i < n || j < m)
      {
          if (i < n && j < m && eqline(&A, lo + i, &B, lo + j)) { ops[nops++].kind = 0; ++i; ++j; }
          else if (i < n && (j == m || TT(i + 1, j) >= TT(i, j + 1))) { ops[nops++].kind = 1; ++i; }
          else { ops[nops++].kind = 2; ++j; }
      } }
    #undef TT
    free(T);
    for (int i = ea; i < A.n; ++i) ops[nops++].kind = 0;
    /* ops -> positions */
    int* ia = (int*)malloc(sizeof(int) * (unsigned long)(nops + 1)); int* ib = (int*)malloc(sizeof(int) * (unsigned long)(nops + 1));
    { int x = 0, y = 0; for (int k = 0; k < nops; ++k) { ia[k] = x; ib[k] = y; if (ops[k].kind != 2) ++x; if (ops[k].kind != 1) ++y; } ia[nops] = x; ib[nops] = y; }
    if (opt_u)
    {
        printf("--- %s\n+++ %s\n", na, nb);
        int k = 0;
        while (k < nops)
        {
            while (k < nops && ops[k].kind == 0) ++k;
            if (k >= nops) break;
            int start = k - ctxn; if (start < 0) start = 0;
            int end = k, lastchg = k;
            while (end < nops)
            {
                if (ops[end].kind != 0) { lastchg = end; ++end; continue; }
                int run = 0; int q = end; while (q < nops && ops[q].kind == 0) { ++run; ++q; }
                if (q >= nops || run > 2 * ctxn) break;
                end = q;
            }
            int stop = lastchg + 1 + ctxn; if (stop > nops) stop = nops;
            int sa = ia[start], sb = ib[start], ca = ia[stop] - sa, cb = ib[stop] - sb;
            printf("@@ -%d,%d +%d,%d @@\n", ca ? sa + 1 : sa, ca, cb ? sb + 1 : sb, cb);
            for (int q = start; q < stop; ++q)
            {
                if (ops[q].kind == 0) print_line(" ", &A, ia[q]);
                else if (ops[q].kind == 1) print_line("-", &A, ia[q]);
                else print_line("+", &B, ib[q]);
            }
            k = stop;
        }
    }
    else
    {
        int k = 0;
        while (k < nops)
        {
            while (k < nops && ops[k].kind == 0) ++k;
            if (k >= nops) break;
            int e = k; while (e < nops && ops[e].kind != 0) ++e;
            int a0 = ia[k], a1 = ia[e], b0 = ib[k], b1 = ib[e];
            int dels = a1 - a0, adds = b1 - b0;
            char c = dels && adds ? 'c' : (dels ? 'd' : 'a');
            if (dels > 1) printf("%d,%d", a0 + 1, a1); else if (dels == 1) printf("%d", a0 + 1); else printf("%d", a0);
            printf("%c", c);
            if (adds > 1) printf("%d,%d", b0 + 1, b1); else if (adds == 1) printf("%d", b0 + 1); else printf("%d", b0);
            printf("\n");
            for (int q = a0; q < a1; ++q) print_line("< ", &A, q);
            if (dels && adds) printf("---\n");
            for (int q = b0; q < b1; ++q) print_line("> ", &B, q);
            k = e;
        }
    }
    free(ia); free(ib); free(ops); free_lines(&A); free_lines(&B);
    return 1;
}

static int is_dir(const char* p) { struct RioStat st; return stat_path(p, &st) == 0 && st.type == 1; }
static int exists(const char* p) { struct RioStat st; return stat_path(p, &st) == 0; }
static char* list_dir(const char* p) { char* b = (char*)malloc(65536); long n = readdir_text(p, b, 65535); if (n < 0) { free(b); return 0; } b[n] = 0; return b; }
static int name_at(char** pp, char* kind, char* name, int cap)
{
    char* ln = *pp; if (!*ln) return 0;
    char* nl = ln; while (*nl && *nl != '\n') ++nl; if (*nl) *nl++ = 0;
    *kind = ln[0]; char* nm = ln + 1; while (*nm == ' ') ++nm; while (*nm >= '0' && *nm <= '9') ++nm; while (*nm == ' ') ++nm;
    strncpy(name, nm, (size_t)cap - 1); name[cap - 1] = 0; *pp = nl; return 1;
}
static int diff_paths(const char* a, const char* b, int top);
static int diff_dirs(const char* a, const char* b)
{
    char* la = list_dir(a); char* lb = list_dir(b); if (!la || !lb) { printf("diff: cannot list directory\n"); free(la); free(lb); return 2; }
    int rc = 0; char kind, name[100];
    char* pa = la;
    while (name_at(&pa, &kind, name, sizeof(name)))
    {
        char ca[256], cb[256]; snprintf(ca, sizeof(ca), "%s/%s", a, name); snprintf(cb, sizeof(cb), "%s/%s", b, name);
        if (!exists(cb)) { printf("Only in %s: %s\n", a, name); if (rc < 1) rc = 1; continue; }
        if (is_dir(ca) && is_dir(cb) && !opt_r) { printf("Common subdirectories: %s and %s\n", ca, cb); continue; }
        int r = diff_paths(ca, cb, 0); if (r > rc) rc = r;
    }
    char* pb = lb;
    while (name_at(&pb, &kind, name, sizeof(name)))
    {
        char ca[256]; snprintf(ca, sizeof(ca), "%s/%s", a, name);
        if (!exists(ca)) { printf("Only in %s: %s\n", b, name); if (rc < 1) rc = 1; }
    }
    free(la); free(lb);
    return rc;
}
static int diff_paths(const char* a, const char* b, int top)
{
    (void)top;
    if (is_dir(a) && is_dir(b)) return diff_dirs(a, b);
    if (is_dir(a) != is_dir(b)) { printf("File %s is a %s while file %s is a %s\n", a, is_dir(a) ? "directory" : "regular file", b, is_dir(b) ? "directory" : "regular file"); return 1; }
    if (!opt_q && !opt_s && !top) { /* header for recursive output */ printf("diff %s%s %s\n", opt_u ? "-u " : "", a, b); }
    return diff_files(a, b, a, b);
}
int main(int argc, char** argv)
{
    const char* f[2]; int nf = 0;
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (a[0] == '-' && a[1])
        {
            if (!strcmp(a, "-U") && i + 1 < argc) { opt_u = 1; ctxn = atoi(argv[++i]); continue; }
            for (const char* c = a + 1; *c; ++c)
            {
                if (*c == 'u') opt_u = 1; else if (*c == 'q') opt_q = 1; else if (*c == 'i') opt_i = 1; else if (*c == 'w') opt_w = 1;
                else if (*c == 'b') opt_b = 1; else if (*c == 'r') opt_r = 1; else if (*c == 's') opt_s = 1;
                else { printf("diff: invalid option -%c\n", *c); return 2; }
            }
        }
        else if (nf < 2) f[nf++] = a; else { printf("diff: extra operand %s\n", a); return 2; }
    }
    if (nf != 2) { printf("usage: diff [-u|-U N] [-q] [-i] [-w] [-b] [-r] [-s] file1 file2\n"); return 2; }
    if (!exists(f[0])) { printf("diff: %s: No such file or directory\n", f[0]); return 2; }
    if (!exists(f[1])) { printf("diff: %s: No such file or directory\n", f[1]); return 2; }
    return diff_paths(f[0], f[1], 1);
}
