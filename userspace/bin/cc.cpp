#include "../libc/ulibc.h"
#include <stdarg.h>
/* cc: C-subset compiler to bytecode + virtual machine.
   cc [-S] [-c] [-o out.rbc] [-s steps] [-D name[=val]] file.c [args]   compile and run (default)
   cc -x prog.rbc [args]                                                run a compiled image
   Language: int/long/char/unsigned/void + pointers + arrays, enum, typedef, #define NAME value / NAME(a,b) body, #ifdef,
   functions/recursion, if/while/for/do/switch/break/continue/goto-free, all integer operators, ?:, sizeof, casts,
   string/char literals, initializers. Builtins: printf sprintf snprintf puts putchar getchar scanf malloc calloc realloc free
   strlen strcmp strncmp strcpy strncpy strcat strchr memset memcpy memcmp atoi abs exit rand srand is-and-to-functions.  No float/struct. */
enum Op { O_HALT, O_IMM, O_LEA, O_LD8, O_LD8U, O_LD32, O_LD32U, O_LD64, O_ST8, O_ST32, O_ST64, O_ADD, O_SUB, O_MUL, O_DIV, O_MOD, O_AND, O_OR, O_XOR,
          O_SHL, O_SHR, O_EQ, O_NE, O_LT, O_LE, O_GT, O_GE, O_NEG, O_BNOT, O_LNOT, O_JMP, O_JZ, O_JNZ, O_CALL, O_RET, O_ENTER, O_BUILTIN, O_POP, O_DUP,
          O_SWAP, O_SX8, O_SX32, O_ZX8, O_ZX32, O_NOP };
static const char* opname[] = { "halt", "imm", "lea", "ld8", "ld8u", "ld32", "ld32u", "ld64", "st8", "st32", "st64", "add", "sub", "mul", "div", "mod", "and", "or", "xor",
          "shl", "shr", "eq", "ne", "lt", "le", "gt", "ge", "neg", "bnot", "lnot", "jmp", "jz", "jnz", "call", "ret", "enter", "builtin", "pop", "dup",
          "swap", "sx8", "sx32", "zx8", "zx32", "nop" };
struct Ins { int op; int64_t a; int64_t b; };
enum { B_PRINTF, B_SPRINTF, B_SNPRINTF, B_PUTS, B_PUTCHAR, B_GETCHAR, B_SCANF, B_MALLOC, B_CALLOC, B_REALLOC, B_FREE, B_STRLEN, B_STRCMP, B_STRNCMP, B_STRCPY, B_STRNCPY, B_STRCAT,
       B_STRCHR, B_MEMSET, B_MEMCPY, B_MEMCMP, B_ATOI, B_ABS, B_EXIT, B_RAND, B_SRAND, B_ISDIGIT, B_ISALPHA, B_ISALNUM, B_ISSPACE, B_ISUPPER, B_ISLOWER, B_TOUPPER, B_TOLOWER, B_STRRCHR, B_STRSTR, B_NBUILTIN };
static const char* bname[] = { "printf", "sprintf", "snprintf", "puts", "putchar", "getchar", "scanf", "malloc", "calloc", "realloc", "free", "strlen", "strcmp", "strncmp", "strcpy", "strncpy", "strcat",
       "strchr", "memset", "memcpy", "memcmp", "atoi", "abs", "exit", "rand", "srand", "isdigit", "isalpha", "isalnum", "isspace", "isupper", "islower", "toupper", "tolower", "strrchr", "strstr" };

/* ---------------- types: base + 8*ptr ---------------- */
enum { T_INT, T_CHAR, T_VOID, T_LONG, T_UCHAR, T_UINT };
static inline int ty_ptr(int t) { return t >> 3; }
static inline int ty_base(int t) { return t & 7; }
static int ty_size(int t) { if (ty_ptr(t)) return 8; switch (ty_base(t)) { case T_CHAR: case T_UCHAR: case T_VOID: return 1; case T_LONG: return 8; default: return 4; } }
static int ty_elem(int t) { return ty_ptr(t) ? ty_size(t - 8) : 1; }
#define TY_PTR(t) ((t) + 8)

/* ---------------- lexer ---------------- */
enum { K_EOF, K_NUM, K_STR, K_ID, K_P };
struct Tok { int kind; long long val; char* s; int len; int line; };
static Tok* toks; static int ntok, tcap;
static const char* src_name = "input";
static void err(int line, const char* fmt, ...)
{
    char b[200]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    printf("%s:%d: error: %s\n", src_name, line, b); exit(1);
}
static void add_tok(int kind, long long v, const char* s, int len, int line)
{
    if (ntok == tcap) { tcap = tcap ? tcap * 2 : 4096; toks = (Tok*)realloc(toks, sizeof(Tok) * (size_t)tcap); }
    Tok* t = &toks[ntok++]; t->kind = kind; t->val = v; t->line = line; t->len = len;
    if (s) { t->s = (char*)malloc((size_t)len + 1); memcpy(t->s, s, (size_t)len); t->s[len] = 0; } else t->s = 0;
}
static int esc_char(const char* p, int* adv)
{
    if (*p != '\\') { *adv = 1; return (unsigned char)*p; }
    *adv = 2;
    switch (p[1]) { case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r'; case '0': if (p[2] >= '0' && p[2] <= '7') { int v = 0, k = 1; while (k < 4 && p[k] >= '0' && p[k] <= '7') v = v * 8 + p[k++] - '0'; *adv = k; return v & 255; } return 0;
        case 'a': return 7; case 'b': return 8; case 'f': return 12; case 'v': return 11; case 'e': return 27; case '\\': return '\\'; case '\'': return '\''; case '"': return '"'; case '?': return '?';
        case 'x': { int v = 0, k = 2; while (isxdigit((unsigned char)p[k])) { v = v * 16 + (isdigit((unsigned char)p[k]) ? p[k] - '0' : (tolower(p[k]) - 'a' + 10)); ++k; } *adv = k; return v & 255; }
        default: if (p[1] >= '1' && p[1] <= '7') { int v = 0, k = 1; while (k < 4 && p[k] >= '0' && p[k] <= '7') v = v * 8 + p[k++] - '0'; *adv = k; return v & 255; } return (unsigned char)p[1]; }
}
static void lex(const char* p)
{
    int line = 1;
    static const char* ops3[] = { "<<=", ">>=", "...", 0 };
    static const char* ops2[] = { "++", "--", "->", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", 0 };
    while (*p)
    {
        if (*p == '\n') { ++line; ++p; continue; }
        if (isspace((unsigned char)*p)) { ++p; continue; }
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') ++p; continue; }
        if (p[0] == '/' && p[1] == '*') { p += 2; while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') ++line; ++p; } if (!*p) err(line, "unterminated comment"); p += 2; continue; }
        if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1])))
        {
            long long v = 0; const char* q = p;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { q += 2; if (!isxdigit((unsigned char)*q)) err(line, "bad hex literal"); while (isxdigit((unsigned char)*q)) { v = v * 16 + (isdigit((unsigned char)*q) ? *q - '0' : (tolower(*q) - 'a' + 10)); ++q; } }
            else if (p[0] == '0' && isdigit((unsigned char)p[1])) { while (*q >= '0' && *q <= '7') v = v * 8 + *q++ - '0'; }
            else { while (isdigit((unsigned char)*q)) v = v * 10 + *q++ - '0'; if (*q == '.' || *q == 'e' || *q == 'E') err(line, "floating point is not supported"); }
            while (*q == 'u' || *q == 'U' || *q == 'l' || *q == 'L') ++q;
            add_tok(K_NUM, v, 0, 0, line); p = q; continue;
        }
        if (isalpha((unsigned char)*p) || *p == '_') { const char* q = p; while (isalnum((unsigned char)*q) || *q == '_') ++q; add_tok(K_ID, 0, p, (int)(q - p), line); p = q; continue; }
        if (*p == '\'') { ++p; int adv; if (!*p || *p == '\n') err(line, "bad character literal"); int c = esc_char(p, &adv); p += adv; if (*p != '\'') err(line, "unterminated character literal"); ++p; add_tok(K_NUM, (signed char)c, 0, 0, line); continue; }
        if (*p == '"')
        {
            char* buf = (char*)malloc(4096); int n = 0;
            for (;;)
            {
                ++p; while (*p && *p != '"') { if (*p == '\n') err(line, "newline in string literal"); int adv; int c = esc_char(p, &adv); p += adv; if (n < 4090) buf[n++] = (char)c; else err(line, "string literal too long"); }
                if (!*p) err(line, "unterminated string literal"); ++p;
                const char* q = p; while (isspace((unsigned char)*q)) { if (*q == '\n') ++line; ++q; }
                if (*q == '"') { p = q; continue; } break;
            }
            add_tok(K_STR, 0, buf, n, line); free(buf); continue;
        }
        int hit = 0;
        for (int i = 0; ops3[i]; ++i) if (!strncmp(p, ops3[i], 3)) { add_tok(K_P, 0, p, 3, line); p += 3; hit = 1; break; }
        if (hit) continue;
        for (int i = 0; ops2[i]; ++i) if (!strncmp(p, ops2[i], 2)) { add_tok(K_P, 0, p, 2, line); p += 2; hit = 1; break; }
        if (hit) continue;
        if (strchr("+-*/%&|^~!<>=?:;,.(){}[]", *p)) { add_tok(K_P, 0, p, 1, line); ++p; continue; }
        err(line, "unexpected character '%c'", *p);
    }
    add_tok(K_EOF, 0, 0, 0, line);
}

/* ---------------- preprocessor (#define object/function-like, #ifdef family) ---------------- */
struct Macro { char name[48]; char* val; int np; char params[8][24]; };
static Macro macros[200]; static int nmac;
static Macro* find_macro(const char* n, size_t l) { for (int i = 0; i < nmac; ++i) if (strlen(macros[i].name) == l && !strncmp(macros[i].name, n, l)) return &macros[i]; return 0; }
static void define_macro(const char* n, size_t l, const char* v)
{
    Macro* m = find_macro(n, l); if (!m) { if (nmac >= 200) { printf("%s: too many macros\n", src_name); exit(1); } m = &macros[nmac++]; if (l > 47) l = 47; memcpy(m->name, n, l); m->name[l] = 0; m->val = 0; }
    m->np = -1; free(m->val); m->val = strdup(v);
}
static char* expand_line(const char* ln, int depth)
{
    size_t cap = strlen(ln) * 4 + 64; char* out = (char*)malloc(cap); size_t o = 0;
    for (const char* p = ln; *p;)
    {
        if (o + 256 > cap) { cap *= 2; out = (char*)realloc(out, cap); }
        if (p[0] == '/' && p[1] == '/') { while (*p) out[o++] = *p++; break; }
        if (*p == '"' || *p == '\'') { char q = *p; out[o++] = *p++; while (*p && *p != q) { if (*p == '\\' && p[1]) out[o++] = *p++; out[o++] = *p++; } if (*p) out[o++] = *p++; continue; }
        if (isalpha((unsigned char)*p) || *p == '_')
        {
            const char* s = p; while (isalnum((unsigned char)*p) || *p == '_') ++p;
            Macro* m = depth < 8 ? find_macro(s, (size_t)(p - s)) : 0;
            if (m && m->np >= 0)
            {
                const char* q = p; while (*q == ' ' || *q == '\t') ++q;
                if (*q != '(') { memcpy(out + o, s, (size_t)(p - s)); o += (size_t)(p - s); continue; }
                ++q; char* args[8]; int na = 0; int dd = 0; const char* as = q;
                for (;; ++q)
                {
                    if (!*q) { printf("%s: error: unterminated macro call %s\n", src_name, m->name); exit(1); }
                    if (*q == '"' || *q == '\'') { char qc = *q++; while (*q && *q != qc) { if (*q == '\\' && q[1]) ++q; ++q; } if (!*q) continue; continue; }
                    if (*q == '(') ++dd;
                    if ((*q == ',' && dd == 0) || (*q == ')' && dd == 0))
                    {
                        if (na >= 8) { printf("%s: error: too many macro arguments\n", src_name); exit(1); }
                        size_t al = (size_t)(q - as); args[na] = (char*)malloc(al + 1); memcpy(args[na], as, al); args[na][al] = 0; ++na; as = q + 1;
                        if (*q == ')') break;
                    }
                    else if (*q == ')') --dd;
                }
                if (na == 1 && m->np == 0) { bool blank = true; for (const char* z = args[0]; *z; ++z) if (!isspace((unsigned char)*z)) blank = false; if (blank) { free(args[0]); na = 0; } }
                if (na != m->np) { printf("%s: error: macro %s expects %d argument(s), got %d\n", src_name, m->name, m->np, na); exit(1); }
                size_t bl = strlen(m->val); size_t tot = bl + 16; for (int i = 0; i < na; ++i) tot += strlen(args[i]) * (bl + 1);
                char* body = (char*)malloc(tot); size_t bo = 0;
                for (const char* b = m->val; *b;)
                {
                    if (isalpha((unsigned char)*b) || *b == '_')
                    {
                        const char* bs = b; while (isalnum((unsigned char)*b) || *b == '_') ++b; int hit = -1;
                        for (int i = 0; i < m->np; ++i) if (strlen(m->params[i]) == (size_t)(b - bs) && !strncmp(m->params[i], bs, (size_t)(b - bs))) hit = i;
                        if (hit >= 0) { size_t al = strlen(args[hit]); body[bo++] = '('; memcpy(body + bo, args[hit], al); bo += al; body[bo++] = ')'; } else { memcpy(body + bo, bs, (size_t)(b - bs)); bo += (size_t)(b - bs); }
                    }
                    else body[bo++] = *b++;
                }
                body[bo] = 0; for (int i = 0; i < na; ++i) free(args[i]);
                char* e = expand_line(body, depth + 1); free(body); size_t el = strlen(e);
                if (o + el + 256 > cap) { cap = (o + el) * 2 + 256; out = (char*)realloc(out, cap); }
                memcpy(out + o, e, el); o += el; free(e); p = q + 1; continue;
            }
            if (m) { char* e = expand_line(m->val, depth + 1); size_t el = strlen(e); if (o + el + 256 > cap) { cap = (o + el) * 2 + 256; out = (char*)realloc(out, cap); } memcpy(out + o, e, el); o += el; free(e); }
            else { memcpy(out + o, s, (size_t)(p - s)); o += (size_t)(p - s); }
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0; return out;
}
static char* preprocess(const char* text)
{
    size_t n = strlen(text); char* out = (char*)malloc(n * 4 + 1024); size_t o = 0; int skipstack[32]; int sp = 0; int active = 1; int lineno = 0;
    const char* p = text;
    while (*p)
    {
        const char* e = p; while (*e && *e != '\n') ++e;
        char* ln = (char*)malloc((size_t)(e - p) + 1); memcpy(ln, p, (size_t)(e - p)); ln[e - p] = 0; ++lineno;
        char* t = ln; while (*t == ' ' || *t == '\t') ++t;
        if (*t == '#')
        {
            ++t; while (*t == ' ' || *t == '\t') ++t; char dir[16]; int dn = 0; while (isalpha((unsigned char)*t) && dn < 15) dir[dn++] = *t++; dir[dn] = 0; while (*t == ' ' || *t == '\t') ++t;
            char name[64]; int nn = 0; const char* ns = t; while ((isalnum((unsigned char)*t) || *t == '_') && nn < 63) name[nn++] = *t++; name[nn] = 0;
            if (!strcmp(dir, "define") && active)
            {
                if (!nn) err(lineno, "#define needs a name");
                int np = -1; char pn[8][24];
                if (*t == '(')
                {
                    ++t; np = 0; while (*t == ' ') ++t;
                    while (*t && *t != ')') { int k = 0; while ((isalnum((unsigned char)*t) || *t == '_') && k < 23) pn[np][k++] = *t++; pn[np][k] = 0; if (!k || np >= 8) err(lineno, "bad macro parameter list"); ++np; while (*t == ' ') ++t; if (*t == ',') { ++t; while (*t == ' ') ++t; } else if (*t != ')') err(lineno, "bad macro parameter list"); }
                    if (*t != ')') err(lineno, "bad macro parameter list"); ++t;
                }
                while (*t == ' ' || *t == '\t') ++t; size_t vl = strlen(t); while (vl && (t[vl - 1] == ' ' || t[vl - 1] == '\r')) --vl; char* v = (char*)malloc(vl + 1); memcpy(v, t, vl); v[vl] = 0;
                char* ex = np >= 0 ? v : expand_line(v, 0); define_macro(ns, (size_t)nn, ex); if (np >= 0) { Macro* mm = find_macro(ns, (size_t)nn); mm->np = np; for (int i = 0; i < np; ++i) strcpy(mm->params[i], pn[i]); } if (np < 0) free(ex); free(v);
            }
            else if (!strcmp(dir, "undef") && active) { Macro* m = find_macro(name, (size_t)nn); if (m) { free(m->val); *m = macros[--nmac]; } }
            else if (!strcmp(dir, "ifdef") || !strcmp(dir, "ifndef")) { if (sp >= 31) err(lineno, "#if nesting too deep"); skipstack[sp++] = active; int d = find_macro(name, (size_t)nn) != 0; if (dir[2] == 'n') d = !d; active = active && d; }
            else if (!strcmp(dir, "else")) { if (!sp) err(lineno, "#else without #if"); int parent = skipstack[sp - 1]; /* flip only if parent active */ active = parent && !active; }
            else if (!strcmp(dir, "endif")) { if (!sp) err(lineno, "#endif without #if"); active = skipstack[--sp]; }
            else if (!strcmp(dir, "if") || !strcmp(dir, "elif")) err(lineno, "#if expressions are not supported (use #ifdef)");
            /* include, pragma, others ignored */
            out[o++] = '\n';
        }
        else if (!active) out[o++] = '\n';
        else { char* ex = expand_line(ln, 0); size_t el = strlen(ex); memcpy(out + o, ex, el); o += el; out[o++] = '\n'; free(ex); }
        free(ln); p = *e ? e + 1 : e;
    }
    if (sp) err(lineno, "unterminated #ifdef");
    out[o] = 0; return out;
}

/* ---------------- program image ---------------- */
static Ins* code; static int ncode, ccap;
static unsigned char* data; static int ndata, dcap;
static int emit(int op, int64_t a = 0, int64_t b = 0)
{
    if (ncode == ccap) { ccap = ccap ? ccap * 2 : 4096; code = (Ins*)realloc(code, sizeof(Ins) * (size_t)ccap); }
    code[ncode].op = op; code[ncode].a = a; code[ncode].b = b; return ncode++;
}
static int data_alloc(int size, int align)
{
    if (align < 1) align = 1; ndata = (ndata + align - 1) / align * align;
    int at = ndata; ndata += size;
    if (ndata > dcap) { int nc = dcap ? dcap : 4096; while (nc < ndata) nc *= 2; data = (unsigned char*)realloc(data, (size_t)nc); memset(data + dcap, 0, (size_t)(nc - dcap)); dcap = nc; }
    return at + 16;      /* VM addresses start at 16; 0..15 reserved (NULL traps) */
}
static unsigned char* data_at(int vmaddr) { return data + (vmaddr - 16); }

/* ---------------- symbols ---------------- */
struct Sym { char name[48]; int ty; int arr; int off; int global; int depth; int is_enumc; };   /* arr = element count (0: scalar) */
static Sym* gsyms; static int ngs, gscap;
static Sym lsyms[256]; static int nls, depth;
struct Fn { char name[48]; int pc; int nparams; int ret; int defined; int line; };
static Fn fns[300]; static int nfn;
struct Typedef { char name[48]; int ty; };
static Typedef tdefs[64]; static int ntd;
static int frame_size, cur_nparams;

static int pos;
static Tok* T() { return &toks[pos]; }
static int is_p(const char* s) { return toks[pos].kind == K_P && !strcmp(toks[pos].s, s); }
static int is_kw(const char* s) { return toks[pos].kind == K_ID && !strcmp(toks[pos].s, s); }
static int accept(const char* s) { if (is_p(s)) { ++pos; return 1; } return 0; }
static void expect(const char* s) { if (!accept(s)) err(T()->line, "expected '%s' near '%s'", s, T()->kind == K_EOF ? "end of file" : T()->kind == K_NUM ? "number" : T()->kind == K_STR ? "string" : T()->s); }
static int find_td(const char* n) { for (int i = 0; i < ntd; ++i) if (!strcmp(tdefs[i].name, n)) return i; return -1; }
static int is_type_start()
{
    if (T()->kind != K_ID) return 0; const char* s = T()->s;
    return !strcmp(s, "int") || !strcmp(s, "char") || !strcmp(s, "long") || !strcmp(s, "short") || !strcmp(s, "unsigned") || !strcmp(s, "signed") || !strcmp(s, "void") || !strcmp(s, "const") || !strcmp(s, "static") || !strcmp(s, "extern") || !strcmp(s, "volatile") || !strcmp(s, "register") || !strcmp(s, "inline") || !strcmp(s, "float") || !strcmp(s, "double") || !strcmp(s, "struct") || !strcmp(s, "union") || find_td(s) >= 0;
}
static int parse_base_type()
{
    int uns = 0, base = -1, longs = 0, seen = 0;
    for (;;)
    {
        if (T()->kind != K_ID) break; const char* s = T()->s;
        if (!strcmp(s, "const") || !strcmp(s, "static") || !strcmp(s, "extern") || !strcmp(s, "volatile") || !strcmp(s, "register") || !strcmp(s, "inline")) { ++pos; continue; }
        if (!strcmp(s, "unsigned")) { uns = 1; seen = 1; ++pos; continue; }
        if (!strcmp(s, "signed")) { seen = 1; ++pos; continue; }
        if (!strcmp(s, "int")) { if (base < 0) base = T_INT; seen = 1; ++pos; continue; }
        if (!strcmp(s, "short")) { base = T_INT; seen = 1; ++pos; continue; }
        if (!strcmp(s, "long")) { ++longs; seen = 1; ++pos; continue; }
        if (!strcmp(s, "char")) { base = T_CHAR; seen = 1; ++pos; continue; }
        if (!strcmp(s, "void")) { base = T_VOID; seen = 1; ++pos; continue; }
        if (!strcmp(s, "float") || !strcmp(s, "double")) err(T()->line, "floating point types are not supported");
        if (!strcmp(s, "struct") || !strcmp(s, "union")) err(T()->line, "struct/union are not supported");
        if (!seen && find_td(s) >= 0) { int t = tdefs[find_td(s)].ty; ++pos; return t; }
        break;
    }
    if (!seen) err(T()->line, "expected a type");
    if (longs) base = T_LONG; else if (base < 0) base = T_INT;
    if (uns) { if (base == T_CHAR) base = T_UCHAR; else if (base == T_INT) base = T_UINT; }
    return base;
}
static int parse_stars(int t) { while (is_p("*")) { ++pos; t = TY_PTR(t); while (is_kw("const") || is_kw("volatile")) ++pos; } return t; }

/* ---------------- constant expressions (for initializers, array sizes, enum) ---------------- */
static long long const_expr();
static long long const_primary()
{
    if (T()->kind == K_NUM) { long long v = T()->val; ++pos; return v; }
    if (accept("(")) { long long v = const_expr(); expect(")"); return v; }
    if (accept("-")) return -const_primary(); if (accept("+")) return const_primary(); if (accept("~")) return ~const_primary(); if (accept("!")) return !const_primary();
    if (is_kw("sizeof")) { ++pos; expect("("); int t = parse_base_type(); t = parse_stars(t); expect(")"); return ty_size(t); }
    if (T()->kind == K_ID) { for (int i = nls - 1; i >= 0; --i) if (lsyms[i].is_enumc && !strcmp(lsyms[i].name, T()->s)) { ++pos; return lsyms[i].off; } for (int i = 0; i < ngs; ++i) if (gsyms[i].is_enumc && !strcmp(gsyms[i].name, T()->s)) { ++pos; return gsyms[i].off; } }
    err(T()->line, "constant expression expected"); return 0;
}
static long long const_expr()
{
    long long a = const_primary();
    for (;;)
    {
        if (accept("+")) a += const_primary(); else if (accept("-")) a -= const_primary(); else if (accept("*")) a *= const_primary();
        else if (accept("/")) { long long b = const_primary(); if (!b) err(T()->line, "division by zero in constant"); a /= b; }
        else if (accept("%")) { long long b = const_primary(); if (!b) err(T()->line, "division by zero in constant"); a %= b; }
        else if (accept("<<")) a <<= const_primary(); else if (accept(">>")) a >>= const_primary();
        else if (accept("|")) a |= const_primary(); else if (accept("&")) a &= const_primary(); else if (accept("^")) a ^= const_primary();
        else break;
    }
    return a;
}

/* ---------------- code generation helpers ---------------- */
struct Val { int ty; int lv; };
static void load_op(int ty) { if (ty_ptr(ty)) { emit(O_LD64); return; } switch (ty_base(ty)) { case T_CHAR: emit(O_LD8); break; case T_UCHAR: emit(O_LD8U); break; case T_LONG: emit(O_LD64); break; case T_UINT: emit(O_LD32U); break; default: emit(O_LD32); } }
static void store_op(int ty) { int s = ty_size(ty); emit(s == 1 ? O_ST8 : s == 4 ? O_ST32 : O_ST64); }
static void rv(Val& v) { if (v.lv) { load_op(v.ty); v.lv = 0; } }
static Sym* find_sym(const char* n)
{
    for (int i = nls - 1; i >= 0; --i) if (!strcmp(lsyms[i].name, n)) return &lsyms[i];
    for (int i = 0; i < ngs; ++i) if (!strcmp(gsyms[i].name, n)) return &gsyms[i];
    return 0;
}
static int find_fn(const char* n) { for (int i = 0; i < nfn; ++i) if (!strcmp(fns[i].name, n)) return i; return -1; }
static int get_fn(const char* n, int line)
{
    int i = find_fn(n); if (i >= 0) return i;
    if (nfn >= 300) err(line, "too many functions");
    Fn* f = &fns[nfn]; memset(f, 0, sizeof(*f)); strncpy(f->name, n, 47); f->pc = -1; f->nparams = -1; f->ret = T_INT; f->line = line; return nfn++;
}
static int find_builtin(const char* n) { for (int i = 0; i < B_NBUILTIN; ++i) if (!strcmp(bname[i], n)) return i; return -1; }
static int str_literal(const char* s, int len) { int a = data_alloc(len + 1, 1); memcpy(data_at(a), s, (size_t)len); data_at(a)[len] = 0; return a; }

/* loops */
struct Loop { int is_loop; int brk[64]; int nbrk; int cont[64]; int ncont; };
static Loop loops[32]; static int nloops;

static Val parse_assign();
static Val parse_expr() { Val v = parse_assign(); while (is_p(",")) { rv(v); ++pos; emit(O_POP); v = parse_assign(); } return v; }
static Val parse_unary();
static void emit_cast(int ty)
{
    if (ty_ptr(ty)) return;
    switch (ty_base(ty)) { case T_CHAR: emit(O_SX8); break; case T_UCHAR: emit(O_ZX8); break; case T_INT: emit(O_SX32); break; case T_UINT: emit(O_ZX32); break; default: break; }
}
static Val parse_primary()
{
    Tok* t = T(); Val v; v.ty = T_INT; v.lv = 0;
    if (t->kind == K_NUM) { ++pos; emit(O_IMM, t->val); if (t->val > 2147483647LL || t->val < -2147483648LL) v.ty = T_LONG; return v; }
    if (t->kind == K_STR) { ++pos; int a = str_literal(t->s, t->len); emit(O_IMM, a); v.ty = TY_PTR(T_CHAR); return v; }
    if (accept("(")) { v = parse_expr(); expect(")"); return v; }
    if (t->kind == K_ID)
    {
        const char* n = t->s; int line = t->line; ++pos;
        if (is_p("("))
        {
            ++pos; int nargs = 0;
            int fi = find_fn(n); int bi = fi < 0 || !fns[fi].defined ? find_builtin(n) : -1; if (fi >= 0 && fns[fi].defined) bi = -1; if (fi >= 0 && fns[fi].nparams >= 0) bi = -1;
            if (!is_p(")")) { for (;;) { Val a = parse_assign(); rv(a); ++nargs; if (!accept(",")) break; } }
            expect(")");
            if (bi >= 0) { if (fi < 0 || !fns[fi].defined) { emit(O_BUILTIN, bi, nargs); v.ty = (bi == B_MALLOC || bi == B_CALLOC || bi == B_REALLOC || bi == B_STRCPY || bi == B_STRNCPY || bi == B_STRCAT || bi == B_STRCHR || bi == B_MEMSET || bi == B_MEMCPY || bi == B_STRRCHR || bi == B_STRSTR) ? TY_PTR(T_CHAR) : T_INT; if (bi == B_STRLEN) v.ty = T_LONG; return v; } }
            fi = get_fn(n, line); if (fns[fi].nparams >= 0 && fns[fi].nparams != nargs) err(line, "function '%s' expects %d argument(s), got %d", n, fns[fi].nparams, nargs);
            emit(O_CALL, fi, nargs); v.ty = fns[fi].ret; return v;
        }
        Sym* s = find_sym(n);
        if (!s) err(line, "undeclared identifier '%s'", n);
        if (s->is_enumc) { emit(O_IMM, s->off); return v; }
        if (s->global) emit(O_IMM, s->off); else emit(O_LEA, s->off);
        if (s->arr) { v.ty = TY_PTR(s->ty); v.lv = 0; return v; }
        v.ty = s->ty; v.lv = 1; return v;
    }
    err(t->line, "unexpected '%s' in expression", t->kind == K_EOF ? "end of file" : t->s ? t->s : "token"); return v;
}
static Val parse_postfix()
{
    Val v = parse_primary();
    for (;;)
    {
        if (is_p("["))
        {
            ++pos; rv(v); if (!ty_ptr(v.ty)) err(T()->line, "subscripted value is not an array or pointer");
            int es = ty_elem(v.ty); int et = v.ty - 8; Val i = parse_expr(); rv(i); expect("]");
            if (es != 1) { emit(O_IMM, es); emit(O_MUL); } emit(O_ADD);
            if (ty_ptr(et) == 0 && ty_base(et) == T_VOID) err(T()->line, "cannot dereference void*");
            v.ty = et; v.lv = 1; continue;
        }
        if (is_p("++") || is_p("--"))
        {
            int inc = is_p("++"); ++pos; if (!v.lv) err(T()->line, "lvalue required for ++/--");
            int step = ty_ptr(v.ty) ? ty_elem(v.ty) : 1;
            emit(O_DUP); load_op(v.ty); emit(O_IMM, step); emit(inc ? O_ADD : O_SUB); store_op(v.ty); emit_cast(v.ty);
            emit(O_IMM, step); emit(inc ? O_SUB : O_ADD); emit_cast(v.ty); v.lv = 0; continue;
        }
        break;
    }
    return v;
}
static Val parse_unary()
{
    Val v; v.ty = T_INT; v.lv = 0;
    if (accept("-")) { v = parse_unary(); rv(v); emit(O_NEG); return v; }
    if (accept("+")) { v = parse_unary(); rv(v); return v; }
    if (accept("!")) { v = parse_unary(); rv(v); emit(O_LNOT); v.ty = T_INT; return v; }
    if (accept("~")) { v = parse_unary(); rv(v); emit(O_BNOT); return v; }
    if (accept("*")) { v = parse_unary(); rv(v); if (!ty_ptr(v.ty)) err(T()->line, "dereference of a non-pointer"); v.ty -= 8; if (!ty_ptr(v.ty) && ty_base(v.ty) == T_VOID) err(T()->line, "cannot dereference void*"); v.lv = 1; return v; }
    if (accept("&")) { int save = pos; (void)save; v = parse_unary(); if (!v.lv) err(T()->line, "cannot take the address of an rvalue"); v.ty = TY_PTR(v.ty); v.lv = 0; return v; }
    if (is_p("++") || is_p("--"))
    {
        int inc = is_p("++"); ++pos; v = parse_unary(); if (!v.lv) err(T()->line, "lvalue required for ++/--");
        int step = ty_ptr(v.ty) ? ty_elem(v.ty) : 1;
        emit(O_DUP); load_op(v.ty); emit(O_IMM, step); emit(inc ? O_ADD : O_SUB); store_op(v.ty); emit_cast(v.ty); v.lv = 0; return v;
    }
    if (is_kw("sizeof"))
    {
        ++pos;
        if (is_p("(") && (pos++, is_type_start())) { int t = parse_base_type(); t = parse_stars(t); expect(")"); emit(O_IMM, ty_size(t)); v.ty = T_LONG; return v; }
        else if (toks[pos - 1].kind == K_P && !strcmp(toks[pos - 1].s, "(")) --pos;
        if (T()->kind == K_ID && find_sym(T()->s) && toks[pos + 1].kind == K_P && strcmp(toks[pos + 1].s, "[") && strcmp(toks[pos + 1].s, "(")) { Sym* s = find_sym(T()->s); ++pos; emit(O_IMM, s->arr ? (long long)s->arr * ty_size(s->ty) : ty_size(s->ty)); v.ty = T_LONG; return v; }
        if (is_p("(") && toks[pos + 1].kind == K_ID && find_sym(toks[pos + 1].s) && toks[pos + 2].kind == K_P && !strcmp(toks[pos + 2].s, ")")) { Sym* s = find_sym(toks[pos + 1].s); pos += 3; emit(O_IMM, s->arr ? (long long)s->arr * ty_size(s->ty) : ty_size(s->ty)); v.ty = T_LONG; return v; }
        int at = ncode; Val x = parse_unary(); (void)x; ncode = at; emit(O_IMM, ty_size(x.ty)); v.ty = T_LONG; return v;
    }
    if (is_p("(") && toks[pos + 1].kind == K_ID)
    {
        int save = pos; ++pos;
        if (is_type_start()) { int t = parse_base_type(); t = parse_stars(t); expect(")"); v = parse_unary(); rv(v); emit_cast(t); v.ty = t; return v; }
        pos = save;
    }
    return parse_postfix();
}
static int prec_of(const char* s)
{
    static const struct { const char* op; int p; } tab[] = { {"*", 10}, {"/", 10}, {"%", 10}, {"+", 9}, {"-", 9}, {"<<", 8}, {">>", 8}, {"<", 7}, {"<=", 7}, {">", 7}, {">=", 7}, {"==", 6}, {"!=", 6}, {"&", 5}, {"^", 4}, {"|", 3}, {"&&", 2}, {"||", 1}, {0, 0} };
    for (int i = 0; tab[i].op; ++i) if (!strcmp(tab[i].op, s)) return tab[i].p; return 0;
}
static Val parse_bin(int minp)
{
    Val l = parse_unary();
    for (;;)
    {
        if (T()->kind != K_P) break; int p = prec_of(T()->s); if (!p || p < minp) break;
        char op[4]; strcpy(op, T()->s); ++pos; rv(l);
        if (!strcmp(op, "&&") || !strcmp(op, "||"))
        {
            int isand = op[0] == '&'; int j1 = emit(isand ? O_JZ : O_JNZ);
            Val r = parse_bin(p + 1); rv(r); int j2 = emit(isand ? O_JZ : O_JNZ);
            emit(O_IMM, isand ? 1 : 0); int je = emit(O_JMP); int lf = ncode; emit(O_IMM, isand ? 0 : 1); code[j1].a = lf; code[j2].a = lf; code[je].a = ncode; l.ty = T_INT; l.lv = 0; continue;
        }
        Val r = parse_bin(p + 1); rv(r);
        int lp = ty_ptr(l.ty), rp = ty_ptr(r.ty); int resty = (ty_base(l.ty) == T_LONG && !lp) || (ty_base(r.ty) == T_LONG && !rp) ? T_LONG : T_INT;
        if (!strcmp(op, "+") || !strcmp(op, "-"))
        {
            int sub = op[0] == '-';
            if (lp && !rp) { int es = ty_elem(l.ty); if (es != 1) { emit(O_IMM, es); emit(O_MUL); } emit(sub ? O_SUB : O_ADD); resty = l.ty; }
            else if (!lp && rp && !sub) { int es = ty_elem(r.ty); emit(O_SWAP); if (es != 1) { emit(O_IMM, es); emit(O_MUL); } emit(O_ADD); resty = r.ty; }
            else if (lp && rp && sub) { emit(O_SUB); int es = ty_elem(l.ty); if (es != 1) { emit(O_IMM, es); emit(O_DIV); } resty = T_LONG; }
            else if (lp && rp) err(T()->line, "cannot add two pointers");
            else emit(sub ? O_SUB : O_ADD);
        }
        else
        {
            static const struct { const char* n; int o; } m[] = { {"*", O_MUL}, {"/", O_DIV}, {"%", O_MOD}, {"<<", O_SHL}, {">>", O_SHR}, {"<", O_LT}, {"<=", O_LE}, {">", O_GT}, {">=", O_GE}, {"==", O_EQ}, {"!=", O_NE}, {"&", O_AND}, {"^", O_XOR}, {"|", O_OR}, {0, 0} };
            for (int i = 0; m[i].n; ++i) if (!strcmp(m[i].n, op)) { emit(m[i].o); break; }
            if (p == 6 || p == 7) resty = T_INT;
        }
        l.ty = resty; l.lv = 0;
    }
    return l;
}
static Val parse_ternary()
{
    Val c = parse_bin(1);
    if (!is_p("?")) return c;
    ++pos; rv(c); int jz = emit(O_JZ); Val a = parse_expr(); rv(a); int je = emit(O_JMP); expect(":"); code[jz].a = ncode; Val b = parse_ternary(); rv(b); code[je].a = ncode;
    Val r; r.ty = ty_ptr(a.ty) ? a.ty : ty_ptr(b.ty) ? b.ty : a.ty; r.lv = 0; return r;
}
static Val parse_assign()
{
    Val l = parse_ternary();
    if (T()->kind == K_P)
    {
        const char* o = T()->s; int comp = 0; const char* bop = 0;
        static const struct { const char* a; const char* b; } ca[] = { {"+=", "+"}, {"-=", "-"}, {"*=", "*"}, {"/=", "/"}, {"%=", "%"}, {"&=", "&"}, {"|=", "|"}, {"^=", "^"}, {"<<=", "<<"}, {">>=", ">>"}, {0, 0} };
        if (!strcmp(o, "=")) comp = 0; else { int f = 0; for (int i = 0; ca[i].a; ++i) if (!strcmp(ca[i].a, o)) { bop = ca[i].b; f = 1; break; } if (!f) return l; comp = 1; }
        ++pos; if (!l.lv) err(T()->line, "lvalue required as left operand of assignment");
        if (!comp) { Val r = parse_assign(); rv(r); store_op(l.ty); emit_cast(l.ty); l.lv = 0; return l; }
        emit(O_DUP); load_op(l.ty);
        Val r = parse_assign(); rv(r);
        int isptr = ty_ptr(l.ty);
        if (isptr && (bop[0] == '+' || bop[0] == '-')) { int es = ty_elem(l.ty); if (es != 1) { emit(O_IMM, es); emit(O_MUL); } }
        static const struct { const char* n; int o; } m[] = { {"+", O_ADD}, {"-", O_SUB}, {"*", O_MUL}, {"/", O_DIV}, {"%", O_MOD}, {"&", O_AND}, {"|", O_OR}, {"^", O_XOR}, {"<<", O_SHL}, {">>", O_SHR}, {0, 0} };
        for (int i = 0; m[i].n; ++i) if (!strcmp(m[i].n, bop)) { emit(m[i].o); break; }
        store_op(l.ty); emit_cast(l.ty); l.lv = 0; return l;
    }
    return l;
}

/* ---------------- statements ---------------- */
static void patch_here(int* list, int n) { for (int i = 0; i < n; ++i) code[list[i]].a = ncode; }
static int alloc_local(int bytes) { bytes = (bytes + 7) / 8 * 8; int off = frame_size; frame_size += bytes; return off; }
static Sym* add_local(const char* name, int ty, int arr)
{
    if (nls >= 256) err(T()->line, "too many local variables");
    for (int i = nls - 1; i >= 0 && lsyms[i].depth == depth; --i) if (!strcmp(lsyms[i].name, name) && !lsyms[i].is_enumc) err(T()->line, "redeclaration of '%s'", name);
    Sym* s = &lsyms[nls++]; memset(s, 0, sizeof(*s)); strncpy(s->name, name, 47); s->ty = ty; s->arr = arr; s->depth = depth; s->global = 0;
    s->off = alloc_local(arr ? arr * ty_size(ty) : 8); return s;
}
static void parse_statement();
static void local_init(Sym* s, int line)
{
    if (s->arr)
    {
        if (accept("{"))
        {
            int idx = 0;
            while (!is_p("}")) { if (idx >= s->arr) err(line, "too many initializers"); emit(O_LEA, s->off + idx * ty_size(s->ty)); Val v = parse_assign(); rv(v); store_op(s->ty); emit(O_POP); ++idx; if (!accept(",")) break; }
            expect("}");
        }
        else if (T()->kind == K_STR && ty_size(s->ty) == 1)
        {
            if (T()->len + 1 > s->arr) err(line, "initializer string too long"); const char* str = T()->s; int len = T()->len; ++pos;
            for (int i = 0; i <= len; ++i) { emit(O_LEA, s->off + i); emit(O_IMM, (unsigned char)str[i]); emit(O_ST8); emit(O_POP); }
        }
        else err(line, "array initializer must be a {list} or string");
    }
    else { emit(O_LEA, s->off); Val v = parse_assign(); rv(v); store_op(s->ty); emit(O_POP); }
}
static void parse_local_decl()
{
    int bt = parse_base_type();
    for (;;)
    {
        int ty = parse_stars(bt); if (T()->kind != K_ID) err(T()->line, "variable name expected"); char name[48]; strncpy(name, T()->s, 47); name[47] = 0; int line = T()->line; ++pos;
        int arr = 0; if (accept("[")) { if (is_p("]")) arr = -1; else arr = (int)const_expr(); expect("]"); if (arr == 0) err(line, "zero-size array"); }
        if (arr == -1)
        {
            if (!is_p("=")) err(line, "array size needed");
            if (toks[pos + 1].kind == K_STR) arr = toks[pos + 1].len + 1; else { int p2 = pos + 2, n = 0, d = 0; while (toks[p2].kind != K_EOF) { if (toks[p2].kind == K_P && !strcmp(toks[p2].s, "{")) ++d; if (toks[p2].kind == K_P && !strcmp(toks[p2].s, "}")) { if (!d) break; --d; } if (!d && toks[p2].kind == K_P && !strcmp(toks[p2].s, ",") ) ++n; ++p2; } arr = n + 1; if (toks[p2 - 1].kind == K_P && !strcmp(toks[p2 - 1].s, ",")) --arr; }
        }
        if (!ty_ptr(ty) && ty_base(ty) == T_VOID) err(line, "variable '%s' declared void", name);
        Sym* s = add_local(name, ty, arr);
        if (accept("=")) local_init(s, line);
        if (!accept(",")) break;
    }
    expect(";");
}
static void parse_block() { expect("{"); ++depth; int sv = nls; while (!is_p("}")) { if (T()->kind == K_EOF) err(T()->line, "missing '}'"); parse_statement(); } ++pos; --depth; nls = sv; }
struct SwCase { long long v; int at; };
static void parse_statement()
{
    if (is_p("{")) { parse_block(); return; }
    if (accept(";")) return;
    if (is_kw("typedef") ) { ++pos; int t = parse_base_type(); t = parse_stars(t); if (T()->kind != K_ID) err(T()->line, "typedef name expected"); if (ntd >= 64) err(T()->line, "too many typedefs"); strncpy(tdefs[ntd].name, T()->s, 47); tdefs[ntd++].ty = t; ++pos; expect(";"); return; }
    if (is_kw("enum")) { err(T()->line, "enum inside functions is not supported; declare it at file scope"); }
    if (is_type_start() && !(T()->kind == K_ID && find_td(T()->s) >= 0 && toks[pos + 1].kind == K_P && strcmp(toks[pos + 1].s, "*") && 0)) { parse_local_decl(); return; }
    if (is_kw("if"))
    {
        ++pos; expect("("); Val c = parse_expr(); rv(c); expect(")"); int jz = emit(O_JZ); parse_statement();
        if (is_kw("else")) { ++pos; int je = emit(O_JMP); code[jz].a = ncode; parse_statement(); code[je].a = ncode; } else code[jz].a = ncode;
        return;
    }
    if (is_kw("while"))
    {
        ++pos; int top = ncode; expect("("); Val c = parse_expr(); rv(c); expect(")"); int jz = emit(O_JZ);
        if (nloops >= 32) err(T()->line, "loops nested too deeply"); Loop* L = &loops[nloops++]; L->is_loop = 1; L->nbrk = L->ncont = 0;
        parse_statement(); for (int i = 0; i < L->ncont; ++i) code[L->cont[i]].a = top; emit(O_JMP, top); code[jz].a = ncode; patch_here(L->brk, L->nbrk); --nloops; return;
    }
    if (is_kw("do"))
    {
        ++pos; int top = ncode; if (nloops >= 32) err(T()->line, "loops nested too deeply"); Loop* L = &loops[nloops++]; L->is_loop = 1; L->nbrk = L->ncont = 0;
        parse_statement(); if (!is_kw("while")) err(T()->line, "'while' expected after do body"); ++pos; patch_here(L->cont, L->ncont); expect("("); Val c = parse_expr(); rv(c); expect(")"); expect(";");
        emit(O_JNZ, top); patch_here(L->brk, L->nbrk); --nloops; return;
    }
    if (is_kw("for"))
    {
        ++pos; expect("("); ++depth; int sv = nls;
        if (!is_p(";")) { if (is_type_start()) parse_local_decl(); else { Val v = parse_expr(); (void)v; emit(O_POP); expect(";"); } } else ++pos;
        int top = ncode; int jz = -1;
        if (!is_p(";")) { Val c = parse_expr(); rv(c); jz = emit(O_JZ); } expect(";");
        int step_pos = pos; int d = 0; while (!(d == 0 && is_p(")"))) { if (T()->kind == K_EOF) err(T()->line, "missing ')' in for"); if (is_p("(")) ++d; if (is_p(")")) --d; ++pos; }
        int after = pos + 1; pos = after;
        if (nloops >= 32) err(T()->line, "loops nested too deeply"); Loop* L = &loops[nloops++]; L->is_loop = 1; L->nbrk = L->ncont = 0;
        parse_statement();
        patch_here(L->cont, L->ncont); int end_pos = pos; pos = step_pos;
        if (!is_p(")")) { Val v = parse_expr(); (void)v; emit(O_POP); }
        pos = end_pos; emit(O_JMP, top); if (jz >= 0) code[jz].a = ncode; patch_here(L->brk, L->nbrk); --nloops; --depth; nls = sv; return;
    }
    if (is_kw("switch"))
    {
        ++pos; expect("("); Val c = parse_expr(); rv(c); expect(")");
        int tmp = alloc_local(8); emit(O_LEA, tmp); emit(O_SWAP); emit(O_ST64); emit(O_POP);
        int jd = emit(O_JMP);
        if (nloops >= 32) err(T()->line, "loops nested too deeply"); Loop* L = &loops[nloops++]; L->is_loop = 0; L->nbrk = L->ncont = 0;
        SwCase cases[128]; int nc = 0; int def = -1;
        expect("{"); ++depth; int sv = nls;
        while (!is_p("}"))
        {
            if (T()->kind == K_EOF) err(T()->line, "missing '}' in switch");
            if (is_kw("case")) { ++pos; long long v = const_expr(); expect(":"); if (nc >= 128) err(T()->line, "too many case labels"); for (int i = 0; i < nc; ++i) if (cases[i].v == v) err(T()->line, "duplicate case value"); cases[nc].v = v; cases[nc++].at = ncode; continue; }
            if (is_kw("default")) { ++pos; expect(":"); if (def >= 0) err(T()->line, "multiple default labels"); def = ncode; continue; }
            parse_statement();
        }
        ++pos; --depth; nls = sv;
        int jend = emit(O_JMP);
        code[jd].a = ncode;
        for (int i = 0; i < nc; ++i) { emit(O_LEA, tmp); emit(O_LD64); emit(O_IMM, cases[i].v); emit(O_EQ); emit(O_JNZ, cases[i].at); }
        emit(O_JMP, def >= 0 ? def : ncode + 1);
        code[jend].a = ncode; patch_here(L->brk, L->nbrk); --nloops; return;
    }
    if (is_kw("break")) { ++pos; expect(";"); if (!nloops) err(T()->line, "'break' outside loop or switch"); Loop* L = &loops[nloops - 1]; if (L->nbrk >= 64) err(T()->line, "too many breaks"); L->brk[L->nbrk++] = emit(O_JMP); return; }
    if (is_kw("continue")) { ++pos; expect(";"); int k = nloops - 1; while (k >= 0 && !loops[k].is_loop) --k; if (k < 0) err(T()->line, "'continue' outside loop"); Loop* L = &loops[k]; if (L->ncont >= 64) err(T()->line, "too many continues"); L->cont[L->ncont++] = emit(O_JMP); return; }
    if (is_kw("return")) { ++pos; if (is_p(";")) emit(O_IMM, 0); else { Val v = parse_expr(); rv(v); } expect(";"); emit(O_RET, cur_nparams); return; }
    if (is_kw("goto")) err(T()->line, "goto is not supported");
    Val v = parse_expr(); (void)v; expect(";"); emit(O_POP);
}

/* ---------------- top level ---------------- */
static Sym* add_global(const char* name, int ty, int arr, int line)
{
    for (int i = 0; i < ngs; ++i) if (!strcmp(gsyms[i].name, name) && !gsyms[i].is_enumc) err(line, "redefinition of '%s'", name);
    if (ngs == gscap) { gscap = gscap ? gscap * 2 : 64; gsyms = (Sym*)realloc(gsyms, sizeof(Sym) * (size_t)gscap); }
    Sym* s = &gsyms[ngs++]; memset(s, 0, sizeof(*s)); strncpy(s->name, name, 47); s->ty = ty; s->arr = arr; s->global = 1;
    return s;
}
static void put_value(int addr, int size, long long v) { unsigned char* p = data_at(addr); for (int i = 0; i < size; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void global_init(Sym* s, int line)
{
    int es = ty_size(s->ty);
    if (s->arr)
    {
        if (accept("{")) { int idx = 0; while (!is_p("}")) { if (idx >= s->arr) err(line, "too many initializers"); if (T()->kind == K_STR && ty_ptr(s->ty)) { int a = str_literal(T()->s, T()->len); ++pos; put_value(s->off + idx * es, es, a); } else put_value(s->off + idx * es, es, const_expr()); ++idx; if (!accept(",")) break; } expect("}"); }
        else if (T()->kind == K_STR && es == 1) { if (T()->len + 1 > s->arr) err(line, "initializer string too long"); memcpy(data_at(s->off), T()->s, (size_t)T()->len); ++pos; }
        else err(line, "array initializer must be a {list} or string");
    }
    else if (T()->kind == K_STR && ty_ptr(s->ty)) { int a = str_literal(T()->s, T()->len); ++pos; put_value(s->off, 8, a); }
    else put_value(s->off, es, const_expr());
}
static void parse_function(int ret, const char* name, int line)
{
    int fi = get_fn(name, line); Fn* f = &fns[fi];
    int ptys[16]; char pnames[16][48]; int np = 0;
    expect("(");
    if (is_kw("void") && toks[pos + 1].kind == K_P && !strcmp(toks[pos + 1].s, ")")) ++pos;
    else if (!is_p(")"))
    {
        for (;;)
        {
            if (is_p("...")) err(T()->line, "variadic user functions are not supported");
            int bt = parse_base_type(); int ty = parse_stars(bt); if (np >= 16) err(T()->line, "too many parameters (max 16)");
            pnames[np][0] = 0; if (T()->kind == K_ID) { strncpy(pnames[np], T()->s, 47); ++pos; }
            if (accept("[")) { if (!is_p("]")) const_expr(); expect("]"); ty = TY_PTR(ty); }
            ptys[np++] = ty; if (!accept(",")) break;
        }
    }
    expect(")");
    if (f->nparams >= 0 && f->nparams != np) err(line, "conflicting declaration of '%s'", name);
    f->nparams = np; f->ret = ret;
    if (accept(";")) return;
    if (!is_p("{")) err(T()->line, "function body or ';' expected");
    if (f->defined) err(line, "redefinition of function '%s'", name);
    f->defined = 1; f->pc = ncode; nls = 0; depth = 0; frame_size = 0; cur_nparams = np; nloops = 0;
    ++depth;
    for (int i = 0; i < np; ++i)
    {
        Sym* s = &lsyms[nls++]; memset(s, 0, sizeof(*s)); strncpy(s->name, pnames[i], 47); s->ty = ptys[i]; s->depth = depth; s->off = -16 - 8 * (np - i);
    }
    int enter = emit(O_ENTER, 0);
    parse_block();
    emit(O_IMM, 0); emit(O_RET, np);
    code[enter].a = (frame_size + 15) / 16 * 16;
    nls = 0; depth = 0;
}
static void parse_toplevel()
{
    if (is_kw("typedef")) { ++pos; int t = parse_base_type(); t = parse_stars(t); if (T()->kind != K_ID) err(T()->line, "typedef name expected"); if (ntd >= 64) err(T()->line, "too many typedefs"); strncpy(tdefs[ntd].name, T()->s, 47); tdefs[ntd++].ty = t; ++pos; expect(";"); return; }
    if (is_kw("enum"))
    {
        ++pos; if (T()->kind == K_ID) ++pos; expect("{"); long long v = 0;
        while (!is_p("}")) { if (T()->kind != K_ID) err(T()->line, "enumerator name expected"); Sym* s = add_global(T()->s, T_INT, 0, T()->line); ++pos; if (accept("=")) v = const_expr(); s->is_enumc = 1; s->off = (int)v++; if (!accept(",")) break; }
        expect("}"); expect(";"); return;
    }
    int bt = parse_base_type();
    for (;;)
    {
        int ty = parse_stars(bt); if (T()->kind != K_ID) err(T()->line, "name expected"); char name[48]; strncpy(name, T()->s, 47); name[47] = 0; int line = T()->line; ++pos;
        if (is_p("(")) { parse_function(ty, name, line); return; }
        int arr = 0; if (accept("[")) { if (is_p("]")) arr = -1; else arr = (int)const_expr(); expect("]"); if (arr == 0) err(line, "zero-size array"); }
        if (arr == -1)
        {
            if (!is_p("=")) err(line, "array size needed");
            if (toks[pos + 1].kind == K_STR) arr = toks[pos + 1].len + 1; else { int p2 = pos + 2, n = 0, d = 0; while (toks[p2].kind != K_EOF) { if (toks[p2].kind == K_P && !strcmp(toks[p2].s, "{")) ++d; if (toks[p2].kind == K_P && !strcmp(toks[p2].s, "}")) { if (!d) break; --d; } if (!d && toks[p2].kind == K_P && !strcmp(toks[p2].s, ",")) ++n; ++p2; } arr = n + 1; if (toks[p2 - 1].kind == K_P && !strcmp(toks[p2 - 1].s, ",")) --arr; }
        }
        if (!ty_ptr(ty) && ty_base(ty) == T_VOID) err(line, "variable '%s' declared void", name);
        Sym* s = add_global(name, ty, arr, line);
        s->off = data_alloc(arr ? arr * ty_size(ty) : ty_size(ty), arr ? (ty_size(ty) > 8 ? 8 : ty_size(ty)) : ty_size(ty));
        if (accept("=")) global_init(s, line);
        if (!accept(",")) break;
    }
    expect(";");
}
static int compile(const char* text, const char* name)
{
    src_name = name;
    char* pp = preprocess(text); lex(pp); free(pp);
    emit(O_HALT);
    pos = 0;
    while (T()->kind != K_EOF) parse_toplevel();
    for (int i = 0; i < nfn; ++i)
        if (!fns[i].defined && !(find_builtin(fns[i].name) >= 0 && fns[i].nparams >= 0 && 0))
        { int used = 0; for (int k = 0; k < ncode; ++k) if (code[k].op == O_CALL && code[k].a == i) used = 1; if (used) err(fns[i].line, "undefined function '%s'", fns[i].name); }
    for (int k = 0; k < ncode; ++k) if (code[k].op == O_CALL) code[k].a = fns[code[k].a].pc;
    int mi = find_fn("main"); if (mi < 0 || !fns[mi].defined) { printf("%s: error: no main() function\n", name); exit(1); }
    return mi;
}

/* ---------------- virtual machine ---------------- */
#define MEM_SIZE (2 * 1024 * 1024)
static unsigned char* mem; static int64_t heap_ptr, heap_end, last_block;
static long long steps_limit = 60000000LL;
static void vm_fail(const char* why, int64_t pc) { fflush(stdout); printf("\nruntime error: %s (pc=%ld)\n", why, (long)pc); exit(134); }
static inline void chk(int64_t a, int64_t n, int64_t pc) { if (a < 16 || n < 0 || a + n > MEM_SIZE) { char b[80]; snprintf(b, sizeof(b), "invalid memory access at address %ld", (long)a); vm_fail(b, pc); } }
static const char* vstr(int64_t a, int64_t pc) { chk(a, 1, pc); int64_t e = a; while (e < MEM_SIZE && mem[e]) ++e; if (e >= MEM_SIZE) vm_fail("unterminated string", pc); return (const char*)mem + a; }
static int64_t vm_malloc(int64_t n, int zero)
{
    if (n < 0 || n > MEM_SIZE) return 0; n = (n + 15) / 16 * 16;
    if (heap_ptr + 16 + n > heap_end) return 0;
    int64_t at = heap_ptr + 16; *(int64_t*)(mem + heap_ptr) = n; last_block = heap_ptr; heap_ptr += 16 + n; if (zero) memset(mem + at, 0, (size_t)n); return at;
}
static void vm_free(int64_t a) { if (!a) return; int64_t h = a - 16; if (h == last_block) { heap_ptr = h; last_block = -1; int64_t p = 0; (void)p; } }
static int do_format(char* out, size_t cap, const char* fmt, int64_t* args, int nargs, int64_t pc)
{
    size_t o = 0; int ai = 0;
#define PUT(c) do { if (o + 1 < cap) out[o] = (char)(c); ++o; } while (0)
    for (const char* p = fmt; *p; ++p)
    {
        if (*p != '%') { PUT(*p); continue; }
        char spec[32]; int sl = 0; spec[sl++] = *p++;
        while (*p && strchr("-+ #0", *p) && sl < 8) spec[sl++] = *p++;
        if (*p == '*') { int64_t w = ai < nargs ? args[ai++] : 0; sl += snprintf(spec + sl, 12, "%d", (int)w); ++p; } else while (isdigit((unsigned char)*p) && sl < 16) spec[sl++] = *p++;
        if (*p == '.') { spec[sl++] = *p++; if (*p == '*') { int64_t w = ai < nargs ? args[ai++] : 0; sl += snprintf(spec + sl, 12, "%d", (int)w); ++p; } else while (isdigit((unsigned char)*p) && sl < 24) spec[sl++] = *p++; }
        int lng = 0; while (*p == 'l' || *p == 'h' || *p == 'z') { if (*p == 'l' || *p == 'z') lng = 1; ++p; }
        char conv = *p; if (!conv) break;
        char piece[600]; int pn = 0;
        if (conv == '%') { PUT('%'); continue; }
        if (ai >= nargs && conv != '%') vm_fail("printf: not enough arguments for format", pc);
        int64_t v = args[ai++];
        if (conv == 'd' || conv == 'i') { spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = 'd'; spec[sl] = 0; long long x = lng ? v : (int32_t)v; pn = snprintf(piece, sizeof(piece), spec, x); }
        else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') { spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0; unsigned long long x = lng ? (uint64_t)v : (uint32_t)v; pn = snprintf(piece, sizeof(piece), spec, x); }
        else if (conv == 'c') { spec[sl++] = 'c'; spec[sl] = 0; pn = snprintf(piece, sizeof(piece), spec, (int)v); }
        else if (conv == 'p') { pn = snprintf(piece, sizeof(piece), "0x%llx", (unsigned long long)v); }
        else if (conv == 's') { const char* s = v ? vstr(v, pc) : "(null)"; spec[sl++] = 's'; spec[sl] = 0; char* tmp = (char*)malloc(strlen(s) + 700); pn = snprintf(tmp, strlen(s) + 700, spec, s); for (int i = 0; i < pn; ++i) PUT(tmp[i]); free(tmp); continue; }
        else if (conv == 'f' || conv == 'e' || conv == 'g') vm_fail("printf: floating point is not supported", pc);
        else { PUT('%'); PUT(conv); continue; }
        if (pn > (int)sizeof(piece) - 1) pn = (int)sizeof(piece) - 1;
        for (int i = 0; i < pn; ++i) PUT(piece[i]);
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return (int)o;
#undef PUT
}
static int do_scanf(const char* fmt, int64_t* args, int nargs, int64_t pc)
{
    int ai = 0, done = 0; int c = 0; int have = 0;
    auto peek = [&]() -> int { if (!have) { c = getchar(); have = 1; } return c; };
    auto take = [&]() { have = 0; };
    for (const char* p = fmt; *p; ++p)
    {
        if (isspace((unsigned char)*p)) { while (peek() >= 0 && isspace(c)) take(); continue; }
        if (*p != '%') { if (peek() != (unsigned char)*p) break; take(); continue; }
        ++p; int lng = 0; int width = 0; while (isdigit((unsigned char)*p)) width = width * 10 + *p++ - '0'; while (*p == 'l' || *p == 'h') { if (*p == 'l') lng = 1; ++p; }
        if (*p == '%') { if (peek() != '%') break; take(); continue; }
        if (ai >= nargs) vm_fail("scanf: not enough arguments", pc);
        if (*p == 'd' || *p == 'i' || *p == 'u' || *p == 'x')
        {
            while (peek() >= 0 && isspace(c)) take(); int neg = 0; int got = 0; long long v = 0; int base = *p == 'x' ? 16 : 10;
            if (peek() == '-' || c == '+') { neg = c == '-'; take(); }
            for (;;) { int d = peek(); int dv = d >= '0' && d <= '9' ? d - '0' : (base == 16 && d >= 'a' && d <= 'f') ? d - 'a' + 10 : (base == 16 && d >= 'A' && d <= 'F') ? d - 'A' + 10 : -1; if (dv < 0) break; v = v * base + dv; got = 1; take(); }
            if (!got) break; if (neg) v = -v; int64_t a = args[ai++]; chk(a, lng ? 8 : 4, pc); if (lng) memcpy(mem + a, &v, 8); else { int32_t x = (int32_t)v; memcpy(mem + a, &x, 4); } ++done;
        }
        else if (*p == 's')
        {
            while (peek() >= 0 && isspace(c)) take(); int64_t a = args[ai++]; int n = 0; if (peek() < 0) break;
            while (peek() >= 0 && !isspace(c) && (!width || n < width)) { chk(a + n, 2, pc); mem[a + n++] = (unsigned char)c; take(); } mem[a + n] = 0; ++done;
        }
        else if (*p == 'c') { int d = peek(); if (d < 0) break; take(); int64_t a = args[ai++]; chk(a, 1, pc); mem[a] = (unsigned char)d; ++done; }
        else break;
    }
    if (have && c >= 0) { /* one character of lookahead is dropped (no ungetc on a raw terminal); acceptable for line-oriented input */ }
    return done;
}
static int vm_run(int mi, int argc, char** argv)
{
    mem = (unsigned char*)calloc(MEM_SIZE, 1); if (!mem) { printf("cc: out of memory\n"); return 1; }
    if (ndata) memcpy(mem + 16, data, (size_t)ndata);
    int64_t dend = 16 + ndata; heap_ptr = (dend + 4095) / 4096 * 4096; heap_end = MEM_SIZE / 2; last_block = -1;
    int64_t sp = heap_end;
    int64_t* S = (int64_t*)mem;            /* word access helper: S[addr/8] (addresses used with stack are 8-aligned) */
    int64_t fp = 0, pc = fns[mi].pc; int np = fns[mi].nparams;
    if (np >= 1) { S[sp / 8] = argc; sp += 8; }
    if (np >= 2)
    {
        int64_t tbl = vm_malloc(8 * (argc + 1), 1); for (int i = 0; i < argc; ++i) { size_t l = strlen(argv[i]); int64_t a = vm_malloc((int64_t)l + 1, 0); memcpy(mem + a, argv[i], l + 1); memcpy(mem + tbl + 8 * i, &a, 8); }
        S[sp / 8] = tbl; sp += 8;
    }
    S[sp / 8] = 0; sp += 8; S[sp / 8] = 0; sp += 8; fp = sp;
    long long steps = 0;
    for (;;)
    {
        if (++steps > steps_limit) vm_fail("step limit exceeded (infinite loop? raise it with -s N)", pc);
        const Ins* in = &code[pc++];
        switch (in->op)
        {
        case O_HALT: { int rc = sp > 0 ? (int)S[(sp - 8) / 8] : 0; fflush(stdout); free(mem); return rc; }
        case O_IMM: S[sp / 8] = in->a; sp += 8; break;
        case O_LEA: S[sp / 8] = fp + in->a; sp += 8; break;
        case O_LD8: { int64_t a = S[(sp - 8) / 8]; chk(a, 1, pc); S[(sp - 8) / 8] = (int8_t)mem[a]; break; }
        case O_LD8U: { int64_t a = S[(sp - 8) / 8]; chk(a, 1, pc); S[(sp - 8) / 8] = mem[a]; break; }
        case O_LD32: { int64_t a = S[(sp - 8) / 8]; chk(a, 4, pc); int32_t x; memcpy(&x, mem + a, 4); S[(sp - 8) / 8] = x; break; }
        case O_LD32U: { int64_t a = S[(sp - 8) / 8]; chk(a, 4, pc); uint32_t x; memcpy(&x, mem + a, 4); S[(sp - 8) / 8] = x; break; }
        case O_LD64: { int64_t a = S[(sp - 8) / 8]; chk(a, 8, pc); int64_t x; memcpy(&x, mem + a, 8); S[(sp - 8) / 8] = x; break; }
        case O_ST8: { int64_t v = S[(sp - 8) / 8]; int64_t a = S[(sp - 16) / 8]; chk(a, 1, pc); mem[a] = (unsigned char)v; sp -= 8; S[(sp - 8) / 8] = v; break; }
        case O_ST32: { int64_t v = S[(sp - 8) / 8]; int64_t a = S[(sp - 16) / 8]; chk(a, 4, pc); int32_t x = (int32_t)v; memcpy(mem + a, &x, 4); sp -= 8; S[(sp - 8) / 8] = v; break; }
        case O_ST64: { int64_t v = S[(sp - 8) / 8]; int64_t a = S[(sp - 16) / 8]; chk(a, 8, pc); memcpy(mem + a, &v, 8); sp -= 8; S[(sp - 8) / 8] = v; break; }
#define BIN(expr) { int64_t b = S[(sp - 8) / 8]; int64_t a = S[(sp - 16) / 8]; sp -= 8; S[(sp - 8) / 8] = (expr); break; }
        case O_ADD: BIN(a + b) case O_SUB: BIN(a - b) case O_MUL: BIN((int64_t)((uint64_t)a * (uint64_t)b))
        case O_DIV: { int64_t b = S[(sp - 8) / 8]; int64_t a = S[(sp - 16) / 8]; if (!b) vm_fail("division by zero", pc); sp -= 8; S[(sp - 8) / 8] = (b == -1) ? -a : a / b; break; }
        case O_MOD: { int64_t b = S[(sp - 8) / 8]; int64_t a = S[(sp - 16) / 8]; if (!b) vm_fail("division by zero (modulo)", pc); sp -= 8; S[(sp - 8) / 8] = (b == -1) ? 0 : a % b; break; }
        case O_AND: BIN(a & b) case O_OR: BIN(a | b) case O_XOR: BIN(a ^ b)
        case O_SHL: BIN(b < 0 || b > 63 ? 0 : (int64_t)((uint64_t)a << b)) case O_SHR: BIN(b < 0 || b > 63 ? (a < 0 ? -1 : 0) : a >> b)
        case O_EQ: BIN(a == b) case O_NE: BIN(a != b) case O_LT: BIN(a < b) case O_LE: BIN(a <= b) case O_GT: BIN(a > b) case O_GE: BIN(a >= b)
        case O_NEG: S[(sp - 8) / 8] = -S[(sp - 8) / 8]; break;
        case O_BNOT: S[(sp - 8) / 8] = ~S[(sp - 8) / 8]; break;
        case O_LNOT: S[(sp - 8) / 8] = !S[(sp - 8) / 8]; break;
        case O_JMP: pc = in->a; break;
        case O_JZ: sp -= 8; if (!S[sp / 8]) pc = in->a; break;
        case O_JNZ: sp -= 8; if (S[sp / 8]) pc = in->a; break;
        case O_CALL: if (sp + 64 > MEM_SIZE - 4096) vm_fail("stack overflow (recursion too deep)", pc); S[sp / 8] = pc; sp += 8; S[sp / 8] = fp; sp += 8; fp = sp; pc = in->a; break;
        case O_ENTER: { if (sp + in->a + 4096 > MEM_SIZE) vm_fail("stack overflow (recursion too deep)", pc); memset(mem + sp, 0, (size_t)in->a); sp += in->a; break; }
        case O_RET: { int64_t r = S[(sp - 8) / 8]; int64_t base = fp - 16 - 8 * in->a; int64_t rpc = S[(fp - 16) / 8]; int64_t ofp = S[(fp - 8) / 8]; sp = base; S[sp / 8] = r; sp += 8; pc = rpc; fp = ofp; break; }
        case O_POP: sp -= 8; break;
        case O_DUP: S[sp / 8] = S[(sp - 8) / 8]; sp += 8; break;
        case O_SWAP: { int64_t t = S[(sp - 8) / 8]; S[(sp - 8) / 8] = S[(sp - 16) / 8]; S[(sp - 16) / 8] = t; break; }
        case O_SX8: S[(sp - 8) / 8] = (int8_t)S[(sp - 8) / 8]; break;
        case O_SX32: S[(sp - 8) / 8] = (int32_t)S[(sp - 8) / 8]; break;
        case O_ZX8: S[(sp - 8) / 8] = (uint8_t)S[(sp - 8) / 8]; break;
        case O_ZX32: S[(sp - 8) / 8] = (uint32_t)S[(sp - 8) / 8]; break;
        case O_NOP: break;
        case O_BUILTIN:
        {
            int id = (int)in->a, n = (int)in->b; int64_t args[24]; if (n > 24) vm_fail("too many arguments to builtin", pc);
            for (int i = n - 1; i >= 0; --i) { sp -= 8; args[i] = S[sp / 8]; }
            int64_t r = 0;
#define NEED(k) if (n < (k)) vm_fail("builtin called with too few arguments", pc)
            switch (id)
            {
            case B_PRINTF: { NEED(1); char* buf = (char*)malloc(8192); r = do_format(buf, 8192, vstr(args[0], pc), args + 1, n - 1, pc); if (r > 8191) r = 8191; fwrite(buf, 1, (size_t)r, stdout); free(buf); break; }
            case B_SPRINTF: { NEED(2); int64_t d = args[0]; const char* f = vstr(args[1], pc); char* buf = (char*)malloc(8192); r = do_format(buf, 8192, f, args + 2, n - 2, pc); if (r > 8191) r = 8191; chk(d, r + 1, pc); memcpy(mem + d, buf, (size_t)r + 1); free(buf); break; }
            case B_SNPRINTF: { NEED(3); int64_t d = args[0]; int64_t cap = args[1]; const char* f = vstr(args[2], pc); char* buf = (char*)malloc(8192); r = do_format(buf, 8192, f, args + 3, n - 3, pc); if (cap > 0) { int64_t cp = r < cap - 1 ? r : cap - 1; chk(d, cp + 1, pc); memcpy(mem + d, buf, (size_t)cp); mem[d + cp] = 0; } free(buf); break; }
            case B_PUTS: NEED(1); printf("%s\n", vstr(args[0], pc)); r = 1; break;
            case B_PUTCHAR: NEED(1); putchar((int)args[0]); r = args[0] & 255; break;
            case B_GETCHAR: fflush(stdout); r = getchar(); break;
            case B_SCANF: NEED(1); fflush(stdout); r = do_scanf(vstr(args[0], pc), args + 1, n - 1, pc); break;
            case B_MALLOC: NEED(1); r = vm_malloc(args[0], 0); break;
            case B_CALLOC: NEED(2); r = vm_malloc(args[0] * args[1], 1); break;
            case B_REALLOC: { NEED(2); if (!args[0]) { r = vm_malloc(args[1], 0); break; } chk(args[0] - 16, 16, pc); int64_t old = *(int64_t*)(mem + args[0] - 16); r = vm_malloc(args[1], 0); if (r) { memcpy(mem + r, mem + args[0], (size_t)(old < args[1] ? old : args[1])); } break; }
            case B_FREE: NEED(1); chk(args[0] ? args[0] - 16 : 16, 1, pc); vm_free(args[0]); break;
            case B_STRLEN: NEED(1); r = (int64_t)strlen(vstr(args[0], pc)); break;
            case B_STRCMP: NEED(2); r = strcmp(vstr(args[0], pc), vstr(args[1], pc)); break;
            case B_STRNCMP: NEED(3); r = strncmp(vstr(args[0], pc), vstr(args[1], pc), (size_t)args[2]); break;
            case B_STRCPY: { NEED(2); const char* s = vstr(args[1], pc); size_t l = strlen(s); chk(args[0], (int64_t)l + 1, pc); memmove(mem + args[0], s, l + 1); r = args[0]; break; }
            case B_STRNCPY: { NEED(3); const char* s = vstr(args[1], pc); size_t l = strlen(s); chk(args[0], args[2], pc); size_t k = (size_t)args[2]; for (size_t i = 0; i < k; ++i) mem[args[0] + (int64_t)i] = i <= l ? (unsigned char)s[i] : 0; r = args[0]; break; }
            case B_STRCAT: { NEED(2); const char* d = vstr(args[0], pc); size_t dl = strlen(d); const char* s = vstr(args[1], pc); size_t l = strlen(s); chk(args[0], (int64_t)(dl + l + 1), pc); memmove(mem + args[0] + dl, s, l + 1); r = args[0]; break; }
            case B_STRCHR: { NEED(2); const char* s = vstr(args[0], pc); const char* f = strchr(s, (int)args[1]); r = f ? (int64_t)(f - (const char*)mem) : 0; break; }
            case B_STRRCHR: { NEED(2); const char* s = vstr(args[0], pc); const char* f = 0; for (const char* q = s;; ++q) { if (*q == (char)args[1]) f = q; if (!*q) break; } r = f ? (int64_t)(f - (const char*)mem) : 0; break; }
            case B_STRSTR: { NEED(2); const char* f = strstr(vstr(args[0], pc), vstr(args[1], pc)); r = f ? (int64_t)(f - (const char*)mem) : 0; break; }
            case B_MEMSET: NEED(3); chk(args[0], args[2], pc); memset(mem + args[0], (int)args[1], (size_t)args[2]); r = args[0]; break;
            case B_MEMCPY: NEED(3); chk(args[0], args[2], pc); chk(args[1], args[2], pc); memmove(mem + args[0], mem + args[1], (size_t)args[2]); r = args[0]; break;
            case B_MEMCMP: NEED(3); chk(args[0], args[2], pc); chk(args[1], args[2], pc); r = memcmp(mem + args[0], mem + args[1], (size_t)args[2]); break;
            case B_ATOI: NEED(1); r = (int)strtol(vstr(args[0], pc), 0, 10); break;
            case B_ABS: NEED(1); r = args[0] < 0 ? -args[0] : args[0]; break;
            case B_EXIT: NEED(1); fflush(stdout); exit((int)args[0]);
            case B_RAND: r = rand(); break;
            case B_SRAND: NEED(1); srand((unsigned)args[0]); break;
            case B_ISDIGIT: NEED(1); r = isdigit((int)args[0]) != 0; break; case B_ISALPHA: NEED(1); r = isalpha((int)args[0]) != 0; break;
            case B_ISALNUM: NEED(1); r = isalnum((int)args[0]) != 0; break; case B_ISSPACE: NEED(1); r = isspace((int)args[0]) != 0; break;
            case B_ISUPPER: NEED(1); r = isupper((int)args[0]) != 0; break; case B_ISLOWER: NEED(1); r = islower((int)args[0]) != 0; break;
            case B_TOUPPER: NEED(1); r = toupper((int)args[0]); break; case B_TOLOWER: NEED(1); r = tolower((int)args[0]); break;
            }
            S[sp / 8] = r; sp += 8; break;
        }
        default: vm_fail("illegal instruction", pc);
        }
    }
}

/* ---------------- image save/load, listing ---------------- */
static int save_image(const char* path, int mi)
{
    FILE* f = fopen(path, "w"); if (!f) return -1;
    int64_t hdr[6] = { 0x31434252LL, ncode, ndata, fns[mi].pc, fns[mi].nparams, 0 };
    int ok = fwrite(hdr, 8, 6, f) == 6 && fwrite(code, sizeof(Ins), (size_t)ncode, f) == (size_t)ncode && (ndata == 0 || fwrite(data, 1, (size_t)ndata, f) == (size_t)ndata);
    fclose(f); return ok ? 0 : -1;
}
static int load_image(const char* path, int* mi)
{
    FILE* f = fopen(path, "r"); if (!f) return -1; int64_t hdr[6];
    if (fread(hdr, 8, 6, f) != 6 || hdr[0] != 0x31434252LL || hdr[1] <= 0 || hdr[1] > 4000000 || hdr[2] < 0 || hdr[2] > MEM_SIZE / 2) { fclose(f); return -2; }
    ncode = ccap = (int)hdr[1]; code = (Ins*)malloc(sizeof(Ins) * (size_t)ncode); ndata = dcap = (int)hdr[2]; data = (unsigned char*)malloc((size_t)ndata + 1);
    if (fread(code, sizeof(Ins), (size_t)ncode, f) != (size_t)ncode || (ndata && fread(data, 1, (size_t)ndata, f) != (size_t)ndata)) { fclose(f); return -2; }
    fclose(f);
    for (int i = 0; i < ncode; ++i) { if (code[i].op < 0 || code[i].op > O_NOP) return -2; if ((code[i].op == O_JMP || code[i].op == O_JZ || code[i].op == O_JNZ || code[i].op == O_CALL) && (code[i].a < 0 || code[i].a >= ncode)) return -2; if (code[i].op == O_BUILTIN && (code[i].a < 0 || code[i].a >= B_NBUILTIN)) return -2; }
    if (hdr[3] < 0 || hdr[3] >= ncode) return -2;
    memset(&fns[0], 0, sizeof(fns[0])); fns[0].pc = (int)hdr[3]; fns[0].nparams = (int)hdr[4]; *mi = 0; nfn = 1; return 0;
}
static void listing()
{
    for (int i = 0; i < ncode; ++i)
    {
        for (int k = 0; k < nfn; ++k) if (fns[k].defined && fns[k].pc == i) printf("%s:\n", fns[k].name);
        printf("%5d  %-8s", i, opname[code[i].op]);
        if (code[i].op == O_BUILTIN) printf(" %s/%ld", bname[code[i].a], (long)code[i].b); else if (code[i].op == O_IMM || code[i].op == O_LEA || code[i].op == O_ENTER || code[i].op == O_RET || code[i].op == O_JMP || code[i].op == O_JZ || code[i].op == O_JNZ || code[i].op == O_CALL) printf(" %ld", (long)code[i].a);
        printf("\n");
    }
    printf("; %d instructions, %d bytes of data\n", ncode, ndata);
}
static char* read_text(const char* path)
{
    FILE* f = fopen(path, "r"); if (!f) return 0; fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc((unsigned long)sz + 1); size_t g = sz ? fread(b, 1, (size_t)sz, f) : 0; fclose(f); b[g] = 0; return b;
}
int main(int argc, char** argv)
{
    int list = 0, check_only = 0, runimg = 0; const char* out = 0; const char* file = 0; int pa = argc;
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (!file && a[0] == '-' && a[1])
        {
            if (!strcmp(a, "-S")) list = 1; else if (!strcmp(a, "-c")) check_only = 1; else if (!strcmp(a, "-x")) runimg = 1;
            else if (!strcmp(a, "-o") && i + 1 < argc) out = argv[++i];
            else if (!strcmp(a, "-s") && i + 1 < argc) { steps_limit = atoll(argv[++i]); if (steps_limit < 1000) steps_limit = 1000; }
            else if (!strcmp(a, "-D") && i + 1 < argc) { char* d = argv[++i]; char* eq = strchr(d, '='); if (eq) define_macro(d, (size_t)(eq - d), eq + 1); else define_macro(d, strlen(d), "1"); }
            else { printf("cc: unknown option %s\n", a); return 2; }
        }
        else if (!file) { file = a; pa = i + 1; break; }
    }
    if (!file) { printf("usage: cc [-S] [-c] [-o out.rbc] [-s steps] [-D name[=val]] file.c [args]\n       cc -x prog.rbc [args]\n"); return 2; }
    int mi = 0;
    if (runimg)
    {
        int r = load_image(file, &mi); if (r == -1) { printf("cc: cannot open %s\n", file); return 2; } if (r < 0) { printf("cc: %s: not a valid bytecode image\n", file); return 2; }
        if (list) { listing(); return 0; }
    }
    else
    {
        char* text = read_text(file); if (!text) { printf("cc: %s: cannot read\n", file); return 2; }
        mi = compile(text, file); free(text);
        if (list) { listing(); if (!out) return 0; }
        if (out) { if (save_image(out, mi)) { printf("cc: cannot write %s\n", out); return 2; } printf("wrote %s (%d instructions, %d bytes data)\n", out, ncode, ndata); return 0; }
        if (check_only) { printf("%s: ok (%d instructions, %d functions)\n", file, ncode, nfn); return 0; }
    }
    return vm_run(mi, argc - pa + 1, argv + pa - 1);
}
