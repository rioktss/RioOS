#include "filters.h"
#include "script.h"
#include "commands.h"
#include "env.h"
#include "fs.h"
#include "vfs.h"
#include "memory.h"
#include "string.h"
#include "uart.h"
#include "thread.h"
#include "types.h"

#define SCRIPT_MAX_DEPTH 4
#define SCRIPT_MAX_STMT  512
#define SCRIPT_MAX_ITER  100000

enum Flow { FLOW_NORMAL = 0, FLOW_BREAK = 1, FLOW_CONTINUE = 2, FLOW_EXIT = 3 };

static int g_status = 0;
static int g_depth = 0;
static int g_abort = 0;
static char* g_pos[10];       /* $0..$9 of the running script */
static int g_argc = 0;
static char g_arg_pool[10][64];

void script_set_status(int status) { g_status = status; }
int script_status() { return g_status; }

static int is_name_char(char c, int first)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') return 1;
    return !first && c >= '0' && c <= '9';
}

static void put_num(char* out, int* p, int cap, long v)
{
    char tmp[24]; int n = 0;
    int neg = v < 0; unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    do { tmp[n++] = (char)('0' + (u % 10UL)); u /= 10UL; } while (u && n < 22);
    if (neg && *p < cap - 1) out[(*p)++] = '-';
    while (n && *p < cap - 1) out[(*p)++] = tmp[--n];
}

int script_expand(const char* line, char* out, int cap)
{
    int ep = 0;
    for (int i = 0; line[i] && ep < cap - 1; )
    {
        if (line[i] == '\'' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t'))
        {
            int j = i + 1;
            while (line[j] && line[j] != '\'') ++j;
            if (line[j] == '\'')
            {
                /* single quotes: literal text, no expansion. Quotes are removed, except for awk/sed/xargs where
                   the quoted program must stay one word (the filter tokenizer strips them later). */
                int keep = 0;
                {
                    int st = i; while (st > 0 && line[st - 1] != '|' && line[st - 1] != ';' && line[st - 1] != '&') --st;
                    while (line[st] == ' ' || line[st] == '\t') ++st;
                    char nm[8]; int nn = 0; while (line[st] && line[st] != ' ' && line[st] != '\t' && nn < 7) nm[nn++] = line[st++];
                    nm[nn] = 0;
                    keep = str_equal(nm, "awk") || str_equal(nm, "sed") || str_equal(nm, "xargs") || str_equal(nm, "printf") || str_equal(nm, "alias") || str_equal(nm, "expr") || str_equal(nm, "curl") || str_equal(nm, "fat") || str_equal(nm, "fw");
                }
                if (keep && ep < cap - 1) out[ep++] = '\'';
                for (int k = i + 1; k < j && ep < cap - 1; ++k) out[ep++] = line[k];
                if (keep && ep < cap - 1) out[ep++] = '\'';
                i = j + 1;
                continue;
            }
        }
        if (line[i] == '`' || (line[i] == '$' && line[i + 1] == '('))
        {
            int bt = line[i] == '`';
            int st = i + (bt ? 1 : 2), j = st, depth = 1;
            while (line[j])
            {
                if (bt) { if (line[j] == '`') break; }
                else { if (line[j] == '(') ++depth; else if (line[j] == ')' && --depth == 0) break; }
                ++j;
            }
            if (line[j])
            {
                char cmd[256]; int cl = 0;
                for (int k = st; k < j && cl < 255; ++k) cmd[cl++] = line[k];
                cmd[cl] = 0;
                char* buf = (char*)kmalloc(4096);
                if (buf)
                {
                    int n = filters_capture(cmd, buf, 4096);
                    while (n > 0 && buf[n - 1] == '\n') buf[--n] = 0;
                    for (int k = 0; k < n && ep < cap - 1; ++k) out[ep++] = (buf[k] == '\n') ? ' ' : buf[k];
                    kfree(buf);
                }
                i = j + 1;
                continue;
            }
        }
        if (line[i] != '$') { out[ep++] = line[i++]; continue; }
        ++i;
        if (line[i] == '?') { ++i; put_num(out, &ep, cap, g_status); continue; }
        if (line[i] == '#') { ++i; put_num(out, &ep, cap, g_argc > 0 ? g_argc - 1 : 0); continue; }
        if (line[i] >= '0' && line[i] <= '9')
        {
            int d = line[i++] - '0';
            if (d < g_argc && g_pos[d]) for (int k = 0; g_pos[d][k] && ep < cap - 1; ++k) out[ep++] = g_pos[d][k];
            continue;
        }
        char name[32]; int np = 0;
        if (line[i] == '{')
        {
            ++i;
            while (line[i] && line[i] != '}' && np < 31) name[np++] = line[i++];
            if (line[i] == '}') ++i;
        }
        else
            while (line[i] && is_name_char(line[i], np == 0) && np < 31) name[np++] = line[i++];
        name[np] = 0;
        const char* val = env_get(name);
        if (val) { for (int k = 0; val[k] && ep < cap - 1; ++k) out[ep++] = val[k]; }
        else if (!name[0]) out[ep++] = '$';
    }
    out[ep] = 0;
    return ep;
}

/* ---- helpers --------------------------------------------------------- */
static char* skip_ws(char* s) { while (*s == ' ' || *s == '\t') ++s; return s; }

static int first_word_is(const char* s, const char* w)
{
    int n = str_len(w);
    for (int i = 0; i < n; ++i) if (s[i] != w[i]) return 0;
    return s[n] == '\0' || s[n] == ' ' || s[n] == '\t';
}

static long parse_int(const char* s, int* ok)
{
    long v = 0; int neg = 0, any = 0;
    if (*s == '-') { neg = 1; ++s; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; any = 1; }
    *ok = any && *s == '\0';
    return neg ? -v : v;
}

/* ---- integer expression parser for let ------------------------------- */
static const char* ex_p;
static int ex_err;
static long ex_expr();
static void ex_ws() { while (*ex_p == ' ') ++ex_p; }
static long ex_atom()
{
    ex_ws();
    if (*ex_p == '(') { ++ex_p; long v = ex_expr(); ex_ws(); if (*ex_p == ')') ++ex_p; else ex_err = 1; return v; }
    if (*ex_p == '-') { ++ex_p; return -ex_atom(); }
    if (*ex_p >= '0' && *ex_p <= '9') { long v = 0; while (*ex_p >= '0' && *ex_p <= '9') v = v * 10 + (*ex_p++ - '0'); return v; }
    if (is_name_char(*ex_p, 1))
    {
        char nm[32]; int n = 0;
        while (is_name_char(*ex_p, n == 0) && n < 31) nm[n++] = *ex_p++;
        nm[n] = 0;
        const char* v = env_get(nm);
        int ok = 0; long r = v ? parse_int(v, &ok) : 0;
        return ok ? r : 0;
    }
    ex_err = 1; return 0;
}
static long ex_term()
{
    long v = ex_atom();
    for (;;)
    {
        ex_ws();
        char c = *ex_p;
        if (c != '*' && c != '/' && c != '%') return v;
        ++ex_p;
        long r = ex_atom();
        if (c == '*') v *= r;
        else if (r == 0) { ex_err = 1; return 0; }
        else v = c == '/' ? v / r : v % r;
    }
}
static long ex_expr()
{
    long v = ex_term();
    for (;;)
    {
        ex_ws();
        char c = *ex_p;
        if (c != '+' && c != '-') return v;
        ++ex_p;
        long r = ex_term();
        v = c == '+' ? v + r : v - r;
    }
}

static int do_let(char* assign)
{
    char* eq = assign;
    while (*eq && *eq != '=') ++eq;
    if (!*eq) { uart_puts("let: usage: let NAME=expr\r\n"); return 1; }
    *eq = 0;
    char* name = skip_ws(assign);
    int n = str_len(name);
    while (n > 0 && name[n - 1] == ' ') name[--n] = 0;
    ex_p = eq + 1; ex_err = 0;
    long v = ex_expr(); ex_ws();
    if (ex_err || *ex_p) { uart_puts("let: bad expression\r\n"); return 1; }
    char buf[64]; int p = 0;
    for (int i = 0; name[i] && p < 30; ++i) buf[p++] = name[i];
    buf[p++] = '=';
    put_num(buf, &p, 63, v);
    buf[p] = 0;
    return env_set(buf) == 0 ? 0 : 1;
}

/* ---- test / [ ] -------------------------------------------------------- */
static int words_split(char* s, char** w, int cap)
{
    int n = 0;
    while (*s && n < cap)
    {
        while (*s == ' ' || *s == '\t') *s++ = 0;
        if (!*s) break;
        w[n++] = s;
        while (*s && *s != ' ' && *s != '\t') ++s;
    }
    return n;
}

static int test_eval(char* expr)       /* returns 0 = true, 1 = false, 2 = error */
{
    char* w[8]; int n = words_split(expr, w, 8);
    int neg = 0, b = 0;
    if (n > 0 && str_equal(w[0], "[")) { b = 1; }
    if (n > 0 && str_equal(w[0], "test")) b = 1;
    if (b) { ++w[0]; for (int i = 0; i + 1 < n; ++i) w[i] = w[i + 1]; --n; }
    if (n > 0 && str_equal(w[n - 1], "]")) --n;
    if (n > 0 && str_equal(w[0], "!")) { neg = 1; for (int i = 0; i + 1 < n; ++i) w[i] = w[i + 1]; --n; }
    int r = 1;
    if (n == 0) r = 1;
    else if (n == 1) r = w[0][0] ? 0 : 1;
    else if (n == 2 && w[0][0] == '-')
    {
        int id;
        if (str_equal(w[0], "-z")) r = w[1][0] ? 1 : 0;
        else if (str_equal(w[0], "-n")) r = w[1][0] ? 0 : 1;
        else if (str_equal(w[0], "-f")) { id = fs_resolve(w[1], vfs_cwd()); r = (id >= 0 && fs_get_type(id) == FS_FILE) ? 0 : 1; }
        else if (str_equal(w[0], "-d")) { id = fs_resolve(w[1], vfs_cwd()); r = (id >= 0 && fs_get_type(id) == FS_DIR) ? 0 : 1; }
        else if (str_equal(w[0], "-e")) { id = fs_resolve(w[1], vfs_cwd()); r = id >= 0 ? 0 : 1; }
        else if (str_equal(w[0], "-L")) { id = fs_resolve_nofollow(w[1], vfs_cwd()); r = (id >= 0 && fs_get_type(id) == FS_SYMLINK) ? 0 : 1; }
        else if (str_equal(w[0], "-r") || str_equal(w[0], "-w") || str_equal(w[0], "-x"))
        {
            id = fs_resolve(w[1], vfs_cwd());
            uint32_t bit = w[0][1] == 'r' ? FS_ACCESS_READ : (w[0][1] == 'w' ? FS_ACCESS_WRITE : FS_ACCESS_EXEC);
            r = (id >= 0 && fs_check_access(id, bit) == 0) ? 0 : 1;
        }
        else return 2;
    }
    else if (n == 2 || n == 3)
    {
        const char* l = n == 3 ? w[0] : "";
        const char* op = n == 3 ? w[1] : w[0];
        const char* rr = n == 3 ? w[2] : w[1];
        if (str_equal(op, "=") || str_equal(op, "==")) r = str_equal(l, rr) ? 0 : 1;
        else if (str_equal(op, "!=")) r = str_equal(l, rr) ? 1 : 0;
        else
        {
            int ok1, ok2; long a = parse_int(l, &ok1), c = parse_int(rr, &ok2);
            if (!ok1 || !ok2) return 2;
            if (str_equal(op, "-eq")) r = a == c ? 0 : 1;
            else if (str_equal(op, "-ne")) r = a != c ? 0 : 1;
            else if (str_equal(op, "-lt")) r = a < c ? 0 : 1;
            else if (str_equal(op, "-le")) r = a <= c ? 0 : 1;
            else if (str_equal(op, "-gt")) r = a > c ? 0 : 1;
            else if (str_equal(op, "-ge")) r = a >= c ? 0 : 1;
            else return 2;
        }
    }
    else return 2;
    return neg ? (r ? 0 : 1) : r;
}

int script_handle_simple(char* line)
{
    line = skip_ws(line);
    if (!*line) return 0;
    /* NAME=value */
    {
        int i = 0;
        while (is_name_char(line[i], i == 0)) ++i;
        if (i > 0 && line[i] == '=')
        {
            g_status = env_set(line) == 0 ? 0 : 1;
            return 1;
        }
    }
    if (first_word_is(line, "read"))
    {
        char* nm = skip_ws(line + 4);
        char var[32]; int vn = 0;
        while (*nm && *nm != ' ' && vn < 31) var[vn++] = *nm++;
        var[vn] = 0;
        if (!vn) { uart_puts("read: usage: read NAME\r\n"); g_status = 1; return 1; }
        char text[110]; int tn = 0;
        for (;;)
        {
            int c = uart_try_getc();
            if (c < 0) { thread_yield(); asm volatile("wfi"); continue; }
            if (c == '\r' || c == '\n') { uart_puts("\r\n"); break; }
            if (c == 3) { g_status = 130; uart_puts("^C\r\n"); return 1; }
            if ((c == 127 || c == 8) && tn > 0) { --tn; uart_puts("\b \b"); continue; }
            if (c >= 32 && c < 127 && tn < 100) { text[tn++] = (char)c; uart_putc((char)c); }
        }
        text[tn] = 0;
        char asg[160]; int ap = 0;
        for (int i = 0; var[i]; ++i) asg[ap++] = var[i];
        asg[ap++] = '=';
        for (int i = 0; text[i]; ++i) asg[ap++] = text[i];
        asg[ap] = 0;
        g_status = env_set(asg) == 0 ? 0 : 1;
        return 1;
    }
    if (first_word_is(line, "let")) { g_status = do_let(line + 3); return 1; }
    if (first_word_is(line, "true") || str_equal(line, ":")) { g_status = 0; return 1; }
    if (first_word_is(line, "false")) { g_status = 1; return 1; }
    if (first_word_is(line, "test") || line[0] == '[')
    {
        int r = test_eval(line);
        if (r == 2) { uart_puts("test: bad expression\r\n"); r = 2; }
        g_status = r;
        return 1;
    }
    if (g_depth > 0)
    {
        if (first_word_is(line, "exit")) { int ok; char* a = skip_ws(line + 4); long v = *a ? parse_int(a, &ok) : g_status; g_status = (int)v; g_abort = 2; return 1; }
        if (first_word_is(line, "break")) { g_abort = 3; return 1; }
        if (first_word_is(line, "continue")) { g_abort = 4; return 1; }
        if (first_word_is(line, "shift"))
        {
            if (g_argc > 1) { for (int i = 1; i + 1 < g_argc; ++i) g_pos[i] = g_pos[i + 1]; --g_argc; g_status = 0; }
            else g_status = 1;
            return 1;
        }
    }
    return 0;
}

/* ---- statement list + block executor --------------------------------- */
struct Prog { char** st; int n; };

static int run_range(Prog* pg, int s, int e);

static int find_end(Prog* pg, int from, int e, const char* open1, const char* open2, const char* close)
{
    int depth = 0;
    for (int i = from; i < e; ++i)
    {
        char* t = pg->st[i];
        if (first_word_is(t, open1) || (open2 && first_word_is(t, open2))) ++depth;
        else if (first_word_is(t, close)) { if (--depth == 0) return i; }
    }
    return -1;
}

static void exec_andor(const char* stmt);

static int eval_cond(const char* cond)
{
    char* t = skip_ws((char*)cond);
    int neg = 0;
    if (*t == '!' && (t[1] == ' ')) { neg = 1; t = skip_ws(t + 1); }
    g_status = 0;
    exec_andor(t);
    int ok = g_status == 0;
    return neg ? !ok : ok;
}

/* a && b || c : sequential evaluation by exit status (quotes protect operators) */
static void exec_andor(const char* stmt)
{
    char buf[512];
    str_copy(buf, stmt, sizeof(buf));
    char* seg = buf; char quote = 0; int op = 0;     /* 0 first, 1 '&&', 2 '||' */
    for (char* p = buf; ; ++p)
    {
        char c = *p;
        int end = c == '\0';
        int is_and = !end && !quote && c == '&' && p[1] == '&';
        int is_or = !end && !quote && c == '|' && p[1] == '|';
        if (!end && !is_and && !is_or) { if (quote) { if (c == quote) quote = 0; } else if (c == '"' || c == '\'') quote = c; continue; }
        *p = '\0';
        char* t = skip_ws(seg);
        int l = str_len(t);
        while (l > 0 && (t[l - 1] == ' ' || t[l - 1] == '\t')) t[--l] = 0;
        int run = op == 0 || (op == 1 && g_status == 0) || (op == 2 && g_status != 0);
        if (run && *t)
        {
            char cmd[512]; str_copy(cmd, t, sizeof(cmd));
            g_status = 0;
            execute_command(cmd);
        }
        if (end) break;
        op = is_and ? 1 : 2;
        ++p;
        seg = p + 1;
    }
}

static int exec_stmt(char* stmt)
{
    g_status = 0;
    exec_andor(stmt);
    return 0;
}

static int run_text(char* text, const char* name, const char* args);

/* ---- shell functions: name() { ...; }   function name { ...; }   return [n] -------------- */
#define FN_MAX 16
#define FN_BODY 768
struct ShFunc { char name[32]; char body[FN_BODY]; int used; };
static ShFunc g_funcs[FN_MAX];

static ShFunc* fn_find(const char* name)
{
    for (int i = 0; i < FN_MAX; ++i) if (g_funcs[i].used && str_equal(g_funcs[i].name, name)) return &g_funcs[i];
    return 0;
}

/* Is stmt a function header? On success fills name and points *rest after '{'. */
static int fn_header(char* t, char* name, char** rest)
{
    char* p = skip_ws(t); int n = 0;
    if (first_word_is(p, "function")) { p = skip_ws(p + 8); }
    while (is_name_char(*p, n == 0) && n < 31) name[n++] = *p++;
    name[n] = 0;
    if (!n) return 0;
    p = skip_ws(p);
    if (p[0] == '(' ) { p = skip_ws(p + 1); if (*p != ')') return 0; p = skip_ws(p + 1); }
    else if (!first_word_is(t, "function") && !(0)) return 0;
    if (*p != '{') return 0;
    *rest = p + 1;
    return 1;
}

static int fn_define(Prog* pg, int i, int e, char* name, char* rest)
{
    int j = i; char body[FN_BODY]; int bl = 0; int closed = 0;
    char* r = skip_ws(rest);
    int len = str_len(r);
    if (len > 0 && r[len - 1] == '}') { closed = 1; }
    for (int k = 0; r[k] && bl < FN_BODY - 4; ++k) { if (closed && r[k + 1] == 0 && r[k] == '}') break; body[bl++] = r[k]; }
    if (bl) body[bl++] = ';';
    while (!closed && ++j < e)
    {
        char* u = skip_ws(pg->st[j]);
        if (u[0] == '}' && (u[1] == 0 || u[1] == ' ')) { closed = 1; break; }
        for (int k = 0; u[k] && bl < FN_BODY - 4; ++k) body[bl++] = u[k];
        body[bl++] = ';';
    }
    if (!closed) { uart_puts("script: function: missing '}'\r\n"); g_status = 2; return -1; }
    body[bl] = 0;
    ShFunc* f = fn_find(name);
    if (!f) for (int k = 0; k < FN_MAX; ++k) if (!g_funcs[k].used) { f = &g_funcs[k]; break; }
    if (!f) { uart_puts("script: too many functions\r\n"); g_status = 2; return -1; }
    f->used = 1; str_copy(f->name, name, sizeof(f->name)); str_copy(f->body, body, sizeof(f->body));
    g_status = 0;
    return j;
}

int script_call_function(const char* line)
{
    char name[32]; int n = 0; const char* p = line;
    while (*p == ' ') ++p;
    while (is_name_char(*p, n == 0) && n < 31) name[n++] = *p++;
    name[n] = 0;
    if (!n || (*p && *p != ' ')) return 0;
    ShFunc* f = fn_find(name);
    if (!f) return 0;
    char* body = (char*)kmalloc(FN_BODY + 4);
    if (!body) return 1;
    str_copy(body, f->body, FN_BODY + 4);
    while (*p == ' ') ++p;
    char argsbuf[128]; str_copy(argsbuf, p, sizeof(argsbuf));
    (void)run_text(body, name, argsbuf);
    kfree(body);
    return 1;
}

static int ctrlc_filter(int c) { if (c == 3) { g_abort = 5; return 1; } return 0; }

static int run_range(Prog* pg, int s, int e)
{
    int i = s;
    while (i < e)
    {
        char* t = pg->st[i];
        int flow = FLOW_NORMAL;
        {
            char fname[32]; char* frest;
            if (fn_header(t, fname, &frest))
            {
                int j = fn_define(pg, i, e, fname, frest);
                if (j < 0) return FLOW_EXIT;
                i = j + 1;
                continue;
            }
        }
        if (first_word_is(t, "if"))
        {
            int fi = find_end(pg, i, e, "if", 0, "fi");
            if (fi < 0 || i + 1 >= e || !first_word_is(pg->st[i + 1], "then")) { uart_puts("script: syntax error near 'if'\r\n"); g_status = 2; return FLOW_EXIT; }
            int cur = i; int taken = 0; int body_start = i + 2;
            int cond_ok = eval_cond(skip_ws(t + 2));
            while (1)
            {
                /* find end of this branch: next elif/else/fi at depth 1 */
                int depth = 0, j = body_start, next = -1;
                for (; j < fi; ++j)
                {
                    char* u = pg->st[j];
                    if (first_word_is(u, "if")) ++depth;
                    else if (first_word_is(u, "fi")) --depth;
                    else if (depth == 0 && (first_word_is(u, "elif") || first_word_is(u, "else"))) { next = j; break; }
                }
                int branch_end = next >= 0 ? next : fi;
                if (!taken && cond_ok)
                {
                    taken = 1;
                    flow = run_range(pg, body_start, branch_end);
                    if (flow != FLOW_NORMAL) return flow;
                }
                if (next < 0) break;
                if (first_word_is(pg->st[next], "elif"))
                {
                    if (next + 1 >= fi || !first_word_is(pg->st[next + 1], "then")) { uart_puts("script: syntax error near 'elif'\r\n"); return FLOW_EXIT; }
                    cond_ok = taken ? 0 : eval_cond(skip_ws(pg->st[next] + 4));
                    body_start = next + 2;
                }
                else { cond_ok = !taken; body_start = next + 1; }
                cur = next;
            }
            (void)cur;
            i = fi + 1;
            continue;
        }
        if (first_word_is(t, "for") || first_word_is(t, "while"))
        {
            int is_for = first_word_is(t, "for");
            int done = find_end(pg, i, e, "for", "while", "done");
            if (done < 0 || i + 1 >= e || !first_word_is(pg->st[i + 1], "do")) { uart_puts("script: syntax error near loop\r\n"); g_status = 2; return FLOW_EXIT; }
            if (is_for)
            {
                char hdr[2048]; script_expand(skip_ws(t + 3), hdr, sizeof(hdr));
                char* p = hdr; char var[32]; int vn = 0;
                while (*p && *p != ' ' && vn < 31) var[vn++] = *p++;
                var[vn] = 0;
                p = skip_ws(p);
                if (p[0] == 'i' && p[1] == 'n' && (p[2] == ' ' || p[2] == 0)) p += 2;
                else { uart_puts("script: for: expected 'in'\r\n"); return FLOW_EXIT; }
                char* w[256]; int n = words_split(p, w, 256);
                for (int k = 0; k < n; ++k)
                {
                    uart_poll_job_keys(ctrlc_filter);
                    if (g_abort == 5) { g_abort = 0; g_status = 130; return FLOW_EXIT; }
                    char asg[160]; int ap = 0;
                    for (int c = 0; var[c] && ap < 30; ++c) asg[ap++] = var[c];
                    asg[ap++] = '=';
                    for (int c = 0; w[k][c] && ap < 150; ++c) asg[ap++] = w[k][c];
                    asg[ap] = 0;
                    (void)env_set(asg);
                    int f = run_range(pg, i + 2, done);
                    if (f == FLOW_EXIT) return f;
                    if (f == FLOW_BREAK) break;
                }
            }
            else
            {
                for (int it = 0; it < SCRIPT_MAX_ITER; ++it)
                {
                    uart_poll_job_keys(ctrlc_filter);
                    if (g_abort == 5) { g_abort = 0; g_status = 130; return FLOW_EXIT; }
                    if (!eval_cond(skip_ws(t + 5))) break;
                    int f = run_range(pg, i + 2, done);
                    if (f == FLOW_EXIT) return f;
                    if (f == FLOW_BREAK) break;
                    if (g_abort == 5) { g_abort = 0; g_status = 130; return FLOW_EXIT; }
                }
            }
            i = done + 1;
            continue;
        }
        (void)exec_stmt(t);
        if (g_abort == 2) return FLOW_EXIT;
        if (g_abort == 3) { g_abort = 0; return FLOW_BREAK; }
        if (g_abort == 4) { g_abort = 0; return FLOW_CONTINUE; }
        if (g_abort == 5) { g_abort = 0; g_status = 130; return FLOW_EXIT; }
        ++i;
    }
    return FLOW_NORMAL;
}

/* Split text (modified in place) into statements: newline and ';' separate,
   quotes protect, '#' at word start begins a comment. */
static int split_statements(char* text, char** st, int cap)
{
    int n = 0; char* p = text; char quote = 0; int word_start = 1;
    /* blank out comments first: '#' at the start of a word outside quotes */
    for (char* q = text; *q; ++q)
    {
        if (quote) { if (*q == quote) quote = 0; word_start = 0; continue; }
        if (*q == '"' || *q == '\'') { quote = *q; word_start = 0; continue; }
        if (*q == '#' && word_start) { while (*q && *q != '\n') *q++ = ' '; if (!*q) break; word_start = 1; continue; }
        word_start = (*q == ' ' || *q == '\t' || *q == '\n' || *q == ';');
    }
    quote = 0; word_start = 1;
    char* cur = p;
    for (;; ++p)
    {
        char c = *p;
        int end = (c == '\0');
        if (!end && quote) { if (c == quote) quote = 0; continue; }
        if (!end && (c == '"' || c == '\'')) { quote = c; word_start = 0; continue; }
        if (end || c == '\n' || c == ';')
        {
            *p = '\0';
            char* t = skip_ws(cur);
            int l = str_len(t);
            while (l > 0 && (t[l - 1] == ' ' || t[l - 1] == '\t' || t[l - 1] == '\r')) t[--l] = 0;
            if (*t && n < cap)
            {
                /* "then"/"do"/"else" may be followed by a command on the same stmt: split it off */
                st[n++] = t;
            }
            cur = p + 1; word_start = 1;
            if (end) break;
            continue;
        }
        word_start = (c == ' ' || c == '\t');
    }
    /* "then X" / "do X" / "else X" -> two statements */
    for (int i = 0; i < n; ++i)
    {
        const char* kw[3] = {"then", "do", "else"};
        for (int k = 0; k < 3; ++k)
        {
            int kl = str_len(kw[k]);
            if (first_word_is(st[i], kw[k]) && st[i][kl] != '\0')
            {
                if (n >= cap) break;
                for (int j = n; j > i + 1; --j) st[j] = st[j - 1];
                st[i + 1] = skip_ws(st[i] + kl);
                st[i][kl] = '\0';
                ++n;
                break;
            }
        }
    }
    return n;
}

static int run_text(char* text, const char* name, const char* args)
{
    if (g_depth >= SCRIPT_MAX_DEPTH) { uart_puts("script: nesting too deep\r\n"); return 1; }
    char** st = (char**)kmalloc(sizeof(char*) * (SCRIPT_MAX_STMT + 8));
    if (!st) return 1;
    int n = split_statements(text, st, SCRIPT_MAX_STMT);
    /* save positional context */
    char* save_pos[10]; int save_argc = g_argc; char save_pool[10][64];
    for (int i = 0; i < 10; ++i) { save_pos[i] = g_pos[i]; for (int k = 0; k < 64; ++k) save_pool[i][k] = g_arg_pool[i][k]; }
    g_argc = 1;
    str_copy(g_arg_pool[0], name, 64); g_pos[0] = g_arg_pool[0];
    if (args)
    {
        char tmp[128]; str_copy(tmp, args, sizeof(tmp));
        char* w[9]; int c = words_split(tmp, w, 9);
        for (int i = 0; i < c; ++i) { str_copy(g_arg_pool[i + 1], w[i], 64); g_pos[i + 1] = g_arg_pool[i + 1]; }
        g_argc = c + 1;
    }
    ++g_depth;
    int saved_abort = g_abort; g_abort = 0;
    Prog pg = { st, n };
    g_status = 0;
    (void)run_range(&pg, 0, n);
    g_abort = saved_abort == 5 ? 5 : 0;
    --g_depth;
    int result = g_status;
    for (int i = 0; i < 10; ++i) { g_pos[i] = save_pos[i]; for (int k = 0; k < 64; ++k) g_arg_pool[i][k] = save_pool[i][k]; }
    /* g_pos pointers into g_arg_pool remain valid (same storage) */
    for (int i = 0; i < 10; ++i) if (save_pos[i]) g_pos[i] = g_arg_pool[i];
    g_argc = save_argc;
    kfree(st);
    return result;
}

int script_run_file(const char* path, const char* args)
{
    int id = fs_resolve(path, vfs_cwd());
    if (id < 0 || fs_get_type(id) != FS_FILE) return -1;
    uint64_t size = fs_file_size(id);
    if (size == 0 || size > 32768) return size == 0 ? 0 : -2;
    char* buf = (char*)kmalloc(size + 2);
    if (!buf) return -3;
    uint64_t got = 0;
    if (fs_read_file(path, vfs_cwd(), (uint8_t*)buf, size, &got) != 0) { kfree(buf); return -4; }
    buf[got] = '\0';
    int rc = run_text(buf, path, args);
    kfree(buf);
    return rc;
}

static int has_list_ops(const char* line)
{
    char quote = 0;
    for (const char* p = line; *p; ++p)
    {
        char c = *p;
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == ';') return 1;
        if ((c == '&' && p[1] == '&') || (c == '|' && p[1] == '|')) return 1;
    }
    return 0;
}

int script_handle_compound(const char* line)
{
    while (*line == ' ') ++line;
    if (!has_list_ops(line) && script_call_function(line)) return 1;
    if (first_word_is(line, "return"))
    {
        const char* a = skip_ws((char*)line + 6); int ok = 1; long v = *a ? parse_int(a, &ok) : g_status;
        g_status = ok ? (int)v : 2; g_abort = 2; return 1;
    }
    if (has_list_ops(line) && !first_word_is(line, "if") && !first_word_is(line, "for") && !first_word_is(line, "while"))
    {
        int l = str_len(line);
        char* buf = (char*)kmalloc((uint64_t)l + 2);
        if (!buf) return 1;
        str_copy(buf, line, l + 1);
        int saved = g_status;
        (void)saved;
        (void)run_text(buf, "sh", 0);
        kfree(buf);
        return 1;
    }
    if (first_word_is(line, "if") || first_word_is(line, "for") || first_word_is(line, "while"))
    {
        int l = str_len(line);
        char* buf = (char*)kmalloc((uint64_t)l + 2);
        if (!buf) return 1;
        str_copy(buf, line, l + 1);
        (void)run_text(buf, "sh", 0);
        kfree(buf);
        return 1;
    }
    int is_sh = first_word_is(line, "sh") || first_word_is(line, "source") || (line[0] == '.' && (line[1] == ' ' || line[1] == '\0'));
    if (!is_sh) return 0;
    const char* p = line;
    while (*p && *p != ' ') ++p;
    p = skip_ws((char*)p);
    if (!*p) { uart_puts("usage: sh <script> [args]\r\n"); return 1; }
    char file[96]; int fp = 0;
    while (*p && *p != ' ' && fp < 95) file[fp++] = *p++;
    file[fp] = 0;
    p = skip_ws((char*)p);
    int rc = script_run_file(file, p);
    if (rc == -1) { uart_puts("sh: "); uart_puts(file); uart_puts(": not found\r\n"); script_set_status(127); }
    else if (rc < -1) { uart_puts("sh: cannot read script\r\n"); script_set_status(126); }
    return 1;
}
