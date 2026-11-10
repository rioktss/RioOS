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

#include "netserv.h"
#include "netstack.h"
#include "uart.h"
#include "fs.h"
#include "vfs.h"
#include "security.h"
#include "memory.h"
#include "string.h"
#include "timer.h"
#include "script.h"
#include "thread.h"

/* =============================== shared helpers =============================== */
namespace {

int g_abort = 0;
int abort_filter(int c) { if (c == 3 || c == 0x1D) { g_abort = 1; return 1; } return 0; }
bool check_abort() { uart_poll_job_keys(abort_filter); return g_abort != 0; }

void put_dec(uint64_t v) { char t[24]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); char o[24]; for (int i = 0; i < n; ++i) o[i] = t[n - 1 - i]; o[n] = 0; uart_puts(o); }
int parse_num(const char* s, int fallback) { if (!s || !*s) return fallback; int v = 0; for (; *s; ++s) { if (*s < '0' || *s > '9') return fallback; v = v * 10 + (*s - '0'); if (v > 100000000) return fallback; } return v; }
int split(char* s, char** w, int cap)
{
    int n = 0;
    while (*s && n < cap)
    {
        while (*s == ' ' || *s == '\t') ++s;
        if (!*s) break;
        w[n++] = s;
        while (*s && *s != ' ' && *s != '\t') ++s;
        if (*s) *s++ = 0;
    }
    return n;
}

struct Out { char* b; int n, cap; };
void o_s(Out* o, const char* s) { while (*s && o->n < o->cap - 1) o->b[o->n++] = *s++; o->b[o->n] = 0; }
void o_n(Out* o, uint64_t v) { char t[24]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n && o->n < o->cap - 1) o->b[o->n++] = t[--n]; o->b[o->n] = 0; }
void o_c(Out* o, char c) { if (o->n < o->cap - 1) { o->b[o->n++] = c; o->b[o->n] = 0; } }

int send_all(int c, const void* d, uint32_t n)
{
    const uint8_t* p = (const uint8_t*)d; uint32_t off = 0;
    while (off < n)
    {
        uint32_t chunk = n - off; if (chunk > 1400) chunk = 1400;
        int r = net_tcp_send(0, c, p + off, chunk, 4000U);
        if (r <= 0) return -1;
        off += (uint32_t)r;
        if (check_abort()) return -1;
    }
    return 0;
}

/* civil date from unix time */
void civil(uint64_t t, int* y, int* m, int* d, int* hh, int* mm, int* ss, int* wd)
{
    int64_t days = (int64_t)(t / 86400); int64_t rem = (int64_t)(t % 86400);
    *hh = (int)(rem / 3600); *mm = (int)(rem % 3600 / 60); *ss = (int)(rem % 60);
    *wd = (int)((days + 4) % 7);
    days += 719468; int64_t era = (days >= 0 ? days : days - 146096) / 146097; int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1); *m = (int)(mp < 10 ? mp + 3 : mp - 9); *y = (int)(yy + (*m <= 2));
}
const char* MON[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

/* =============================== HTTP =============================== */
const char* mime_of(const char* path)
{
    int l = str_len(path); int dot = -1;
    for (int i = l - 1; i >= 0 && path[i] != '/'; --i) if (path[i] == '.') { dot = i; break; }
    if (dot < 0) return "application/octet-stream";
    const char* e = path + dot + 1;
    struct M { const char* ext; const char* type; };
    static const M tab[] = {{"html","text/html; charset=utf-8"},{"htm","text/html; charset=utf-8"},{"txt","text/plain; charset=utf-8"},{"md","text/plain; charset=utf-8"},
        {"css","text/css"},{"js","application/javascript"},{"json","application/json"},{"xml","application/xml"},{"png","image/png"},{"jpg","image/jpeg"},{"jpeg","image/jpeg"},
        {"gif","image/gif"},{"svg","image/svg+xml"},{"ico","image/x-icon"},{"pdf","application/pdf"},{"tar","application/x-tar"},{"gz","application/gzip"},{"zip","application/zip"},
        {"c","text/plain; charset=utf-8"},{"h","text/plain; charset=utf-8"},{"cpp","text/plain; charset=utf-8"},{"sh","text/plain; charset=utf-8"},{"csv","text/csv"},{"rpk","application/octet-stream"}};
    for (const M& m : tab) if (str_equal(e, m.ext)) return m.type;
    return "application/octet-stream";
}

int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* decode target into out; returns -1 when it is unsafe */
int url_path(const char* in, char* out, int cap)
{
    int n = 0;
    for (; *in && *in != '?' && *in != '#'; ++in)
    {
        char c = *in;
        if (c == '%') { int a = hexv(in[1]), b = a >= 0 ? hexv(in[2]) : -1; if (b < 0) return -1; c = (char)(a * 16 + b); in += 2; }
        if (c == 0 || c == '\\') return -1;
        if (n >= cap - 1) return -1;
        out[n++] = c;
    }
    out[n] = 0;
    if (out[0] != '/') return -1;
    /* reject ".." segments */
    for (int i = 0; out[i]; ++i) if (out[i] == '/' && out[i + 1] == '.' && out[i + 2] == '.' && (out[i + 3] == '/' || out[i + 3] == 0)) return -1;
    return n;
}

void html_escape(Out* o, const char* s) { for (; *s; ++s) { if (*s == '<') o_s(o, "&lt;"); else if (*s == '>') o_s(o, "&gt;"); else if (*s == '&') o_s(o, "&amp;"); else if (*s == '"') o_s(o, "&quot;"); else o_c(o, *s); } }

static uint32_t g_pasv_lo = 30000, g_pasv_n = 2000;   /* passive data port range (ftpd -P lo-hi) */
static int g_http10 = 0;   /* answer HTTP/1.0 requests with an HTTP/1.0 status line */
struct HttpCfg { char root[96]; int hello_fallback; int quiet; };

int http_reply(int c, int code, const char* reason, const char* ctype, const uint8_t* body, uint64_t blen, int head_only, int keep, const char* extra)
{
    char h[512]; Out o = {h, 0, sizeof(h)};
    o_s(&o, g_http10 ? "HTTP/1.0 " : "HTTP/1.1 "); o_n(&o, (uint64_t)code); o_c(&o, ' '); o_s(&o, reason); o_s(&o, "\r\nServer: RioOS-httpd\r\nContent-Type: "); o_s(&o, ctype);
    o_s(&o, "\r\nContent-Length: "); o_n(&o, blen); o_s(&o, "\r\nAccept-Ranges: bytes\r\nConnection: "); o_s(&o, keep ? "keep-alive" : "close"); o_s(&o, "\r\n");
    if (extra) o_s(&o, extra);
    o_s(&o, "\r\n");
    if (send_all(c, h, (uint32_t)o.n) != 0) return -1;
    if (!head_only && blen && send_all(c, body, (uint32_t)blen) != 0) return -1;
    return 0;
}

int http_error(int c, int code, const char* reason, int head_only, int keep, const char* extra)
{
    char b[160]; Out o = {b, 0, sizeof(b)};
    o_s(&o, "<html><body><h1>"); o_n(&o, (uint64_t)code); o_c(&o, ' '); o_s(&o, reason); o_s(&o, "</h1></body></html>\n");
    return http_reply(c, code, reason, "text/html; charset=utf-8", (const uint8_t*)b, (uint64_t)o.n, head_only, keep, extra);
}

bool hdr_value(const char* req, const char* name, char* out, int cap)
{
    int nl = str_len(name);
    for (const char* p = req; *p; )
    {
        const char* e = p; while (*e && *e != '\n') ++e;
        bool m = true;
        for (int i = 0; i < nl; ++i) { char a = p[i], b = name[i]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32); if (b >= 'A' && b <= 'Z') b = (char)(b + 32); if (a != b) { m = false; break; } }
        if (m && p[nl] == ':')
        {
            const char* v = p + nl + 1; while (*v == ' ') ++v;
            int n = 0; while (v < e && *v != '\r' && n < cap - 1) out[n++] = *v++;
            out[n] = 0; return true;
        }
        p = *e ? e + 1 : e;
    }
    return false;
}

int serve_request(int c, const HttpCfg* cfg, char* req, int* keep_out, char* logline, int logcap)
{
    char method[8] = {0}; char target[300] = {0}; char ver[12] = {0}; int i = 0, k = 0;
    while (req[i] && req[i] != ' ' && k < 7) method[k++] = req[i++];
    while (req[i] == ' ') ++i; k = 0;
    while (req[i] && req[i] != ' ' && req[i] != '\r' && k < 299) target[k++] = req[i++];
    while (req[i] == ' ') ++i; k = 0;
    while (req[i] && req[i] != '\r' && k < 11) ver[k++] = req[i++];
    bool http11 = str_equal(ver, "HTTP/1.1");
    g_http10 = str_equal(ver, "HTTP/1.0") ? 1 : 0;
    char conn[32] = {0}; hdr_value(req, "Connection", conn, sizeof(conn));
    int keep = http11; if (conn[0] == 'c' || conn[0] == 'C') keep = 0; if ((conn[0] == 'k' || conn[0] == 'K') && !http11) keep = 1;
    *keep_out = keep;
    bool is_get = str_equal(method, "GET"), is_head = str_equal(method, "HEAD");
    Out lg = {logline, 0, logcap}; o_s(&lg, method); o_c(&lg, ' '); o_s(&lg, target); 
    if (!is_get && !is_head) { o_s(&lg, " 405"); return http_error(c, 405, "Method Not Allowed", 0, keep, "Allow: GET, HEAD\r\n"); }
    char path[300];
    if (!str_equal(ver, "HTTP/1.0") && !http11) { o_s(&lg, " 400"); *keep_out = 0; return http_error(c, 400, "Bad Request", is_head, 0, 0); }
    if (url_path(target, path, sizeof(path)) < 0) { o_s(&lg, " 400"); *keep_out = 0; return http_error(c, 400, "Bad Request", is_head, 0, 0); }
    char fspath[420]; fspath[0] = 0;
    { int rl = str_len(cfg->root); if (rl > 1) str_copy(fspath, cfg->root, sizeof(fspath)); str_copy(fspath + str_len(fspath), path, (int)sizeof(fspath) - str_len(fspath)); }
    FsStat st;
    int plen = str_len(path);
    if (fs_stat_path(fspath, vfs_cwd(), &st) != 0)
    {
        if (cfg->hello_fallback && str_equal(path, "/"))
        { const char* m = "Hello from RioOS httpd\n"; o_s(&lg, " 200"); return http_reply(c, 200, "OK", "text/plain; charset=utf-8", (const uint8_t*)m, (uint64_t)str_len(m), is_head, keep, 0); }
        o_s(&lg, " 404"); return http_error(c, 404, "Not Found", is_head, keep, 0);
    }
    if (st.type == FS_DIR)
    {
        if (path[plen - 1] != '/')
        {
            char loc[400]; Out lo = {loc, 0, sizeof(loc)}; o_s(&lo, "Location: "); o_s(&lo, path); o_s(&lo, "/\r\n");
            o_s(&lg, " 301"); return http_error(c, 301, "Moved Permanently", is_head, keep, loc);
        }
        char idx[460]; str_copy(idx, fspath, sizeof(idx)); str_copy(idx + str_len(idx), "index.html", (int)sizeof(idx) - str_len(idx));
        FsStat s2;
        if (fs_stat_path(idx, vfs_cwd(), &s2) == 0 && s2.type == FS_FILE) { str_copy(fspath, idx, sizeof(fspath)); st = s2; str_copy(path + plen, "index.html", (int)sizeof(path) - plen); }
        else
        {
            int id = fs_resolve(fspath, vfs_cwd());
            if (id < 0 || fs_check_access(id, FS_ACCESS_READ) != 0) { o_s(&lg, " 403"); return http_error(c, 403, "Forbidden", is_head, keep, 0); }
            int cnt = fs_get_child_count(id);
            uint8_t* body = (uint8_t*)kmalloc(256 + (uint32_t)cnt * 220);
            if (!body) { o_s(&lg, " 500"); return http_error(c, 500, "Internal Server Error", is_head, keep, 0); }
            Out o = {(char*)body, 0, (int)(256 + (uint32_t)cnt * 220)};
            o_s(&o, "<html><head><title>Index of "); html_escape(&o, path); o_s(&o, "</title></head><body><h1>Index of "); html_escape(&o, path); o_s(&o, "</h1><hr><pre>\n");
            if (plen > 1) o_s(&o, "<a href=\"../\">../</a>\n");
            for (int ci = 0; ci < cnt; ++ci)
            {
                int ch = fs_get_child(id, ci); if (ch < 0) continue; const char* nm = fs_get_name(ch);
                o_s(&o, "<a href=\""); html_escape(&o, nm); if (fs_get_type(ch) == FS_DIR) o_c(&o, '/'); o_s(&o, "\">"); html_escape(&o, nm); if (fs_get_type(ch) == FS_DIR) o_c(&o, '/');
                o_s(&o, "</a>"); if (fs_get_type(ch) == FS_FILE) { o_s(&o, "  "); o_n(&o, fs_file_size(ch)); }
                o_c(&o, '\n');
            }
            o_s(&o, "</pre><hr></body></html>\n");
            o_s(&lg, " 200"); int rc = http_reply(c, 200, "OK", "text/html; charset=utf-8", body, (uint64_t)o.n, is_head, keep, 0);
            kfree(body); return rc;
        }
    }
    if (st.type != FS_FILE) { o_s(&lg, " 403"); return http_error(c, 403, "Forbidden", is_head, keep, 0); }
    if (st.size > 8U * 1024U * 1024U) { o_s(&lg, " 500"); return http_error(c, 500, "Internal Server Error", is_head, keep, 0); }
    uint8_t* data = (uint8_t*)kmalloc(st.size + 1);
    if (!data) { o_s(&lg, " 500"); return http_error(c, 500, "Internal Server Error", is_head, keep, 0); }
    uint64_t got = 0; int rr = fs_read_file(fspath, vfs_cwd(), data, st.size, &got);
    if (rr != 0) { kfree(data); o_s(&lg, rr == -13 ? " 403" : " 500"); return rr == -13 ? http_error(c, 403, "Forbidden", is_head, keep, 0) : http_error(c, 500, "Internal Server Error", is_head, keep, 0); }
    /* Range: bytes=a-b | a- | -n */
    char range[64]; int rc;
    uint64_t from = 0, to = got ? got - 1 : 0; bool partial = false;
    if (hdr_value(req, "Range", range, sizeof(range)) && range[0] == 'b' && range[5] == '=')
    {
        const char* p = range + 6; uint64_t a = 0, b = 0; bool ha = false, hb = false;
        while (*p >= '0' && *p <= '9') { a = a * 10 + (uint64_t)(*p++ - '0'); ha = true; }
        if (*p == '-') { ++p; while (*p >= '0' && *p <= '9') { b = b * 10 + (uint64_t)(*p++ - '0'); hb = true; } }
        if (ha && !hb) { from = a; to = got ? got - 1 : 0; } else if (ha && hb) { from = a; to = b < got ? b : (got ? got - 1 : 0); } else if (!ha && hb) { from = b < got ? got - b : 0; to = got ? got - 1 : 0; } else from = got;
        if (got == 0 || from >= got || from > to)
        {
            char ex[64]; Out eo = {ex, 0, sizeof(ex)}; o_s(&eo, "Content-Range: bytes */"); o_n(&eo, got); o_s(&eo, "\r\n");
            kfree(data); o_s(&lg, " 416"); return http_error(c, 416, "Range Not Satisfiable", is_head, keep, ex);
        }
        partial = true;
    }
    if (partial)
    {
        char ex[96]; Out eo = {ex, 0, sizeof(ex)}; o_s(&eo, "Content-Range: bytes "); o_n(&eo, from); o_c(&eo, '-'); o_n(&eo, to); o_c(&eo, '/'); o_n(&eo, got); o_s(&eo, "\r\n");
        o_s(&lg, " 206"); rc = http_reply(c, 206, "Partial Content", mime_of(path), data + from, to - from + 1, is_head, keep, ex);
    }
    else { o_s(&lg, " 200"); rc = http_reply(c, 200, "OK", mime_of(path), data, got, is_head, keep, 0); }
    o_c(&lg, ' '); o_n(&lg, partial ? to - from + 1 : got);
    kfree(data);
    return rc;
}

}   // namespace

void command_httpd(const char* argument)
{
    char buf[200]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[10]; int n = split(buf, w, 10);
    HttpCfg cfg; str_copy(cfg.root, "/", sizeof(cfg.root)); cfg.hello_fallback = 1; cfg.quiet = 0;
    int port = -1, limit = -1, pos = 0;
    for (int i = 0; i < n; ++i)
    {
        if (str_equal(w[i], "-d") && i + 1 < n) { str_copy(cfg.root, w[++i], sizeof(cfg.root)); cfg.hello_fallback = 0; int l = str_len(cfg.root); while (l > 1 && cfg.root[l - 1] == '/') cfg.root[--l] = 0; }
        else if (str_equal(w[i], "-q")) cfg.quiet = 1;
        else if (w[i][0] == '-') { port = -2; break; }
        else if (pos == 0) { port = parse_num(w[i], -2); ++pos; }
        else if (pos == 1) { limit = parse_num(w[i], -2); ++pos; }
        else { port = -2; break; }
    }
    if (!net_ready() || port == -1 || port == -2) { uart_puts("usage: httpd <port> [requests] [-d docroot] [-q]   (requests 0 or omitted = run until Ctrl-C)\r\n"); return; }
    if (port <= 0 || port > 65535 || limit == -2 || limit > 1000000) { uart_puts("httpd: bad arguments\r\n"); return; }
    if (limit == -1) limit = 1;                                    /* historic default: one request */
    if (cfg.root[0] != '/' ) { uart_puts("httpd: docroot must be an absolute path\r\n"); return; }
    { FsStat st; if (fs_stat_path(cfg.root, vfs_cwd(), &st) != 0 || st.type != FS_DIR) { uart_puts("httpd: docroot is not a directory\r\n"); return; } }
    int ls = net_tcp_socket(0);
    if (ls < 0 || net_tcp_listen(0, ls, (uint16_t)port) != 0) { uart_puts("httpd: cannot listen\r\n"); if (ls >= 0) net_tcp_close(0, ls); return; }
    uart_puts("httpd: listening on port "); put_dec((uint64_t)port); uart_puts(", root "); uart_puts(cfg.root);
    if (limit) { uart_puts(" ("); put_dec((uint64_t)limit); uart_puts(" request(s); Ctrl-C aborts)\r\n"); } else uart_puts(" (until Ctrl-C)\r\n");
    g_abort = 0; int served = 0, idle = 0;
    static char req[4096];
    while ((limit == 0 || served < limit) && !g_abort)
    {
        uint32_t pip; uint16_t pport;
        int c = net_tcp_accept(0, ls, &pip, &pport, 500U);
        if (c < 0) { check_abort(); if (limit && ++idle > 120) break; continue; }
        idle = 0;
        for (int reqn = 0; reqn < 50 && (limit == 0 || served < limit) && !g_abort; ++reqn)
        {
            int got = 0; uint64_t t0 = timer_millis(); uint32_t wait = reqn == 0 ? 4000U : 5000U; bool complete = false;
            while (got < (int)sizeof(req) - 1 && timer_millis() - t0 < wait)
            {
                int r = net_tcp_recv(0, c, req + got, (uint32_t)(sizeof(req) - 1 - (uint32_t)got), 100U);
                if (r == 0 || (r < 0 && r != -110)) { got = got ? got : -1; break; }
                if (r > 0) { got += r; req[got] = 0; for (int k = 3; k < got; ++k) if (req[k - 3] == '\r' && req[k - 2] == '\n' && req[k - 1] == '\r' && req[k] == '\n') { complete = true; break; } if (complete) break; }
                if (check_abort()) break;
            }
            if (got <= 0 || !complete) { if (got > 0 && !complete) { req[got] = 0; (void)http_error(c, 400, "Bad Request", 0, 0, 0); } break; }
            req[got] = 0; int keep = 0; char line[420]; line[0] = 0;
            int rc = serve_request(c, &cfg, req, &keep, line, sizeof(line));
            ++served;
            if (!cfg.quiet)
            {
                uart_puts("httpd: served ");
                /* "METHOD target status [bytes]": keep the historic "served <path>" prefix */
                const char* sp = line; while (*sp && *sp != ' ') ++sp; if (*sp == ' ') ++sp;
                uart_puts(sp); uart_puts("\r\n");
            }
            if (rc != 0 || !keep) break;
        }
        (void)net_tcp_close(0, c);
    }
    (void)net_tcp_close(0, ls);
    uart_puts("httpd: stopped ("); put_dec((uint64_t)served); uart_puts(" request(s) served)\r\n");
}

/* =============================== FTP =============================== */
namespace {

struct Ftp
{
    int ctl; int logged; uint32_t uid, gid; char user[48]; char root[96]; char cwd[240];
    int pasv_sock; int active; uint32_t act_ip; uint16_t act_port; int binary; uint64_t rest; char rnfr[260]; int anon; int ro;
};

int ftp_say(Ftp* f, const char* s) { uint32_t n = (uint32_t)str_len(s); if (send_all(f->ctl, s, n) != 0) return -1; return send_all(f->ctl, "\r\n", 2); }
int ftp_say_n(Ftp* f, int code, const char* text) { char b[300]; Out o = {b, 0, sizeof(b)}; o_n(&o, (uint64_t)code); o_c(&o, ' '); o_s(&o, text); return ftp_say(f, b); }

/* virtual path (absolute, normalised, confined to the root) */
void vnorm(const char* cwd, const char* arg, char* out, int cap)
{
    char tmp[500]; int n = 0;
    if (arg[0] == '/') str_copy(tmp, arg, sizeof(tmp)); else { str_copy(tmp, cwd, sizeof(tmp)); n = str_len(tmp); if (n && tmp[n - 1] != '/') { tmp[n++] = '/'; tmp[n] = 0; } str_copy(tmp + n, arg, (int)sizeof(tmp) - n); }
    char segs[24][64]; int ns = 0; int i = 0;
    while (tmp[i])
    {
        while (tmp[i] == '/') ++i; if (!tmp[i]) break;
        char seg[64]; int k = 0; while (tmp[i] && tmp[i] != '/') { if (k < 63) seg[k++] = tmp[i]; ++i; } seg[k] = 0;
        if (str_equal(seg, ".")) continue;
        if (str_equal(seg, "..")) { if (ns) --ns; continue; }
        if (ns < 24) str_copy(segs[ns++], seg, 64);
    }
    int o = 0; out[o++] = '/';
    for (int s = 0; s < ns; ++s) { for (const char* p = segs[s]; *p && o < cap - 2; ++p) out[o++] = *p; if (s + 1 < ns && o < cap - 2) out[o++] = '/'; }
    out[o] = 0;
}
void real_path(const Ftp* f, const char* v, char* out, int cap)
{
    int rl = str_len(f->root);
    if (rl <= 1) { str_copy(out, v, cap); return; }
    str_copy(out, f->root, cap); str_copy(out + str_len(out), v[1] ? v : "", cap - str_len(out));
}

int data_open(Ftp* f)
{
    int d = -1;
    if (f->pasv_sock >= 0)
    {
        uint32_t ip; uint16_t pt; d = net_tcp_accept(0, f->pasv_sock, &ip, &pt, 10000U);
        net_tcp_close(0, f->pasv_sock); f->pasv_sock = -1;
    }
    else if (f->active)
    {
        int s = net_tcp_socket(0);
        if (s >= 0 && net_tcp_connect(0, s, f->act_ip, f->act_port, 5000U) == 0) d = s; else if (s >= 0) net_tcp_close(0, s);
        f->active = 0;
    }
    return d;
}

void list_line(Out* o, int node, const char* name, bool full)
{
    if (!full) { o_s(o, name); o_s(o, "\r\n"); return; }
    uint32_t mode = fs_get_mode(node); int type = fs_get_type(node);
    char m[11]; m[0] = type == FS_DIR ? 'd' : type == FS_SYMLINK ? 'l' : '-';
    const char* rwx = "rwxrwxrwx"; for (int i = 0; i < 9; ++i) m[1 + i] = (mode & (1U << (8 - i))) ? rwx[i] : '-'; m[10] = 0;
    o_s(o, m); o_s(o, " 1 "); o_n(o, fs_get_uid(node)); o_c(o, ' '); o_n(o, fs_get_gid(node)); o_c(o, ' ');
    uint64_t sz = type == FS_FILE ? fs_file_size(node) : 0; char pad[16]; Out po = {pad, 0, sizeof(pad)}; o_n(&po, sz); for (int i = po.n; i < 10; ++i) o_c(o, ' '); o_s(o, pad);
    int y, mo, d, hh, mm, ss, wd; civil(fs_get_mtime(node), &y, &mo, &d, &hh, &mm, &ss, &wd);
    o_c(o, ' '); o_s(o, MON[mo - 1]); o_c(o, ' '); if (d < 10) o_c(o, ' '); o_n(o, (uint64_t)d); o_c(o, ' ');
    if (hh < 10) o_c(o, '0'); o_n(o, (uint64_t)hh); o_c(o, ':'); if (mm < 10) o_c(o, '0'); o_n(o, (uint64_t)mm);
    o_c(o, ' '); o_s(o, name); o_s(o, "\r\n");
}

bool may_write(Ftp* f) { return !f->ro; }

int ftp_session(int c, const char* root, int allow_anon, uint32_t peer_ip)
{
    static Ftp f; f.ctl = c; f.logged = 0; f.uid = f.gid = 0; f.user[0] = 0; str_copy(f.root, root, sizeof(f.root)); str_copy(f.cwd, "/", sizeof(f.cwd));
    f.pasv_sock = -1; f.active = 0; f.binary = 1; f.rest = 0; f.rnfr[0] = 0; f.anon = 0; f.ro = 0;
    uint32_t saved_uid = fs_current_uid(), saved_gid = fs_current_gid();
    if (ftp_say_n(&f, 220, "RioOS FTP server ready") != 0) return -1;
    char line[600]; int ln = 0; uint64_t last = timer_millis(); int pending_user = 0; char puser[48] = {0};
    for (;;)
    {
        if (check_abort()) break;
        if (timer_millis() - last > 120000) { ftp_say_n(&f, 421, "Idle timeout"); break; }
        char ch; int r = net_tcp_recv(0, c, &ch, 1, 200U);
        if (r == 0) break;
        if (r < 0) continue;
        last = timer_millis();
        if (ch == '\n') {
            line[ln] = 0; ln = 0;
            int l = str_len(line); if (l && line[l - 1] == '\r') line[--l] = 0;
            char cmd[8] = {0}; int i = 0; while (line[i] && line[i] != ' ' && i < 7) { char x = line[i]; cmd[i] = (char)(x >= 'a' && x <= 'z' ? x - 32 : x); ++i; }
            while (line[i] && line[i] != ' ') ++i; while (line[i] == ' ') ++i; const char* arg = line + i;
            bool quit = false;
            if (str_equal(cmd, "USER"))
            {
                f.logged = 0; pending_user = 1; str_copy(puser, arg, sizeof(puser));
                if (allow_anon && (str_equal(arg, "anonymous") || str_equal(arg, "ftp"))) ftp_say_n(&f, 331, "Anonymous login ok, send any password");
                else ftp_say_n(&f, 331, "Password required");
            }
            else if (str_equal(cmd, "PASS"))
            {
                if (!pending_user) ftp_say_n(&f, 503, "Login with USER first");
                else
                {
                    pending_user = 0; uint32_t u, g;
                    if (allow_anon && (str_equal(puser, "anonymous") || str_equal(puser, "ftp"))) { f.logged = 1; f.anon = 1; f.ro = 1; f.uid = 65534; f.gid = 65534; str_copy(f.user, "anonymous", sizeof(f.user)); ftp_say_n(&f, 230, "Anonymous access granted (read-only)"); }
                    else if (security_verify(puser, arg) == 0 && security_ids_of(puser, &u, &g) == 0) { f.logged = 1; f.uid = u; f.gid = g; str_copy(f.user, puser, sizeof(f.user)); ftp_say_n(&f, 230, "Login successful"); uart_puts("ftpd: login "); uart_puts(puser); uart_puts("\r\n"); }
                    else { ftp_say_n(&f, 530, "Login incorrect"); uart_puts("ftpd: failed login for "); uart_puts(puser); uart_puts("\r\n"); }
                }
            }
            else if (str_equal(cmd, "QUIT")) { ftp_say_n(&f, 221, "Goodbye"); quit = true; }
            else if (str_equal(cmd, "NOOP")) ftp_say_n(&f, 200, "OK");
            else if (str_equal(cmd, "SYST")) ftp_say_n(&f, 215, "UNIX Type: L8");
            else if (str_equal(cmd, "FEAT")) { ftp_say(&f, "211-Features:"); ftp_say(&f, " SIZE"); ftp_say(&f, " MDTM"); ftp_say(&f, " REST STREAM"); ftp_say(&f, " EPSV"); ftp_say(&f, " UTF8"); ftp_say(&f, "211 End"); }
            else if (str_equal(cmd, "OPTS")) ftp_say_n(&f, 200, "OK");
            else if (!f.logged) ftp_say_n(&f, 530, "Please login with USER and PASS");
            else
            {
                fs_set_credentials(f.uid, f.gid);
                if (str_equal(cmd, "PWD") || str_equal(cmd, "XPWD")) { char b[300]; Out o = {b, 0, sizeof(b)}; o_n(&o, 257); o_s(&o, " \""); o_s(&o, f.cwd); o_s(&o, "\""); ftp_say(&f, b); }
                else if (str_equal(cmd, "CWD") || str_equal(cmd, "CDUP"))
                {
                    char v[300], rp[420]; vnorm(f.cwd, str_equal(cmd, "CDUP") ? ".." : arg, v, sizeof(v)); real_path(&f, v, rp, sizeof(rp));
                    FsStat st; int id = fs_resolve(rp, vfs_cwd());
                    if (fs_stat_path(rp, vfs_cwd(), &st) == 0 && st.type == FS_DIR && id >= 0 && fs_check_access(id, FS_ACCESS_EXEC) == 0) { str_copy(f.cwd, v, sizeof(f.cwd)); ftp_say_n(&f, 250, "Directory changed"); }
                    else ftp_say_n(&f, 550, "No such directory");
                }
                else if (str_equal(cmd, "TYPE")) { f.binary = !(arg[0] == 'A' || arg[0] == 'a'); ftp_say_n(&f, 200, f.binary ? "Type set to I" : "Type set to A"); }
                else if (str_equal(cmd, "MODE") || str_equal(cmd, "STRU")) ftp_say_n(&f, 200, "OK");
                else if (str_equal(cmd, "PASV") || str_equal(cmd, "EPSV"))
                {
                    if (f.pasv_sock >= 0) { net_tcp_close(0, f.pasv_sock); f.pasv_sock = -1; }
                    int s = net_tcp_socket(0); uint16_t port = 0;
                    for (int tries = 0; s >= 0 && tries < 40; ++tries) { port = (uint16_t)(g_pasv_lo + (timer_millis() / 7 + (uint64_t)tries) % g_pasv_n); if (net_tcp_listen(0, s, port) == 0) break; port = 0; }
                    if (s < 0 || !port) { if (s >= 0) net_tcp_close(0, s); ftp_say_n(&f, 425, "Cannot open passive port"); }
                    else
                    {
                        f.pasv_sock = s; f.active = 0;
                        char b[120]; Out o = {b, 0, sizeof(b)};
                        if (cmd[0] == 'E') { o_s(&o, "229 Entering Extended Passive Mode (|||"); o_n(&o, port); o_s(&o, "|)"); }
                        else { uint32_t ip = net_local_ip(); o_s(&o, "227 Entering Passive Mode ("); o_n(&o, (ip >> 24) & 255); o_c(&o, ','); o_n(&o, (ip >> 16) & 255); o_c(&o, ','); o_n(&o, (ip >> 8) & 255); o_c(&o, ','); o_n(&o, ip & 255); o_c(&o, ','); o_n(&o, port >> 8); o_c(&o, ','); o_n(&o, port & 255); o_c(&o, ')'); }
                        ftp_say(&f, b);
                    }
                }
                else if (str_equal(cmd, "PORT"))
                {
                    uint32_t v[6]; int k = 0, cur = 0; bool ok = true; const char* p = arg;
                    for (;; ++p) { if (*p >= '0' && *p <= '9') cur = cur * 10 + (*p - '0'); else if (*p == ',' || *p == 0) { if (k >= 6 || cur > 255) { ok = false; break; } v[k++] = (uint32_t)cur; cur = 0; if (*p == 0) break; } else { ok = false; break; } }
                    uint32_t ip = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
                    if (!ok || k != 6) ftp_say_n(&f, 501, "Bad PORT");
                    else if (ip != peer_ip) ftp_say_n(&f, 501, "PORT address must match the control connection");
                    else { if (f.pasv_sock >= 0) { net_tcp_close(0, f.pasv_sock); f.pasv_sock = -1; } f.active = 1; f.act_ip = ip; f.act_port = (uint16_t)((v[4] << 8) | v[5]); ftp_say_n(&f, 200, "PORT ok"); }
                }
                else if (str_equal(cmd, "LIST") || str_equal(cmd, "NLST"))
                {
                    bool full = cmd[0] == 'L'; const char* a = arg; if (a[0] == '-') { while (*a && *a != ' ') ++a; while (*a == ' ') ++a; }
                    char v[300], rp[420]; vnorm(f.cwd, a[0] ? a : ".", v, sizeof(v)); real_path(&f, v, rp, sizeof(rp));
                    FsStat st; int id = fs_resolve(rp, vfs_cwd());
                    if (id < 0 || fs_stat_path(rp, vfs_cwd(), &st) != 0) { ftp_say_n(&f, 550, "No such file or directory"); }
                    else if (fs_check_access(id, FS_ACCESS_READ) != 0) { ftp_say_n(&f, 550, "Permission denied"); }
                    else if (f.pasv_sock < 0 && !f.active) ftp_say_n(&f, 425, "Use PASV or PORT first");
                    else
                    {
                        int cnt = st.type == FS_DIR ? fs_get_child_count(id) : 1; int cap = 512 + cnt * 140;
                        char* body = (char*)kmalloc((uint32_t)cap);
                        if (!body) ftp_say_n(&f, 451, "Out of memory");
                        else
                        {
                            Out o = {body, 0, cap};
                            if (st.type == FS_DIR) { for (int k = 0; k < cnt; ++k) { int ch2 = fs_get_child(id, k); if (ch2 >= 0) list_line(&o, ch2, fs_get_name(ch2), full); } }
                            else list_line(&o, id, fs_get_name(id), full);
                            ftp_say_n(&f, 150, "Here comes the directory listing");
                            int d = data_open(&f);
                            if (d < 0) ftp_say_n(&f, 425, "Cannot open data connection");
                            else { int rc = send_all(d, body, (uint32_t)o.n); net_tcp_close(0, d); if (rc == 0) ftp_say_n(&f, 226, "Directory send OK"); else ftp_say_n(&f, 426, "Transfer aborted"); }
                            kfree(body);
                        }
                    }
                }
                else if (str_equal(cmd, "RETR"))
                {
                    char v[300], rp[420]; vnorm(f.cwd, arg, v, sizeof(v)); real_path(&f, v, rp, sizeof(rp));
                    FsStat st;
                    if (fs_stat_path(rp, vfs_cwd(), &st) != 0 || st.type != FS_FILE) ftp_say_n(&f, 550, "No such file");
                    else if (f.pasv_sock < 0 && !f.active) ftp_say_n(&f, 425, "Use PASV or PORT first");
                    else if (st.size > 8U * 1024U * 1024U) ftp_say_n(&f, 552, "File too large");
                    else
                    {
                        uint8_t* data = (uint8_t*)kmalloc(st.size + 1); uint64_t got = 0;
                        if (!data) ftp_say_n(&f, 451, "Out of memory");
                        else if (fs_read_file(rp, vfs_cwd(), data, st.size, &got) != 0) { kfree(data); ftp_say_n(&f, 550, "Permission denied"); }
                        else
                        {
                            uint64_t off = f.rest < got ? f.rest : got; f.rest = 0;
                            ftp_say_n(&f, 150, "Opening data connection");
                            int d = data_open(&f);
                            if (d < 0) ftp_say_n(&f, 425, "Cannot open data connection");
                            else { int rc = send_all(d, data + off, (uint32_t)(got - off)); net_tcp_close(0, d); if (rc == 0) ftp_say_n(&f, 226, "Transfer complete"); else ftp_say_n(&f, 426, "Transfer aborted"); }
                            kfree(data);
                        }
                    }
                }
                else if (str_equal(cmd, "STOR") || str_equal(cmd, "APPE"))
                {
                    char v[300], rp[420]; vnorm(f.cwd, arg, v, sizeof(v)); real_path(&f, v, rp, sizeof(rp));
                    if (!may_write(&f)) ftp_say_n(&f, 550, "Read-only session");
                    else if (f.pasv_sock < 0 && !f.active) ftp_say_n(&f, 425, "Use PASV or PORT first");
                    else
                    {
                        bool app = cmd[0] == 'A'; FsStat st0; uint64_t base = 0; uint8_t* old = 0;
                        if (app && fs_stat_path(rp, vfs_cwd(), &st0) == 0 && st0.type == FS_FILE) base = st0.size;
                        const uint32_t CAP = 4U * 1024U * 1024U;
                        uint8_t* data = (uint8_t*)kmalloc(CAP + 16); (void)old;
                        if (!data) ftp_say_n(&f, 451, "Out of memory");
                        else
                        {
                            uint64_t n = 0; bool ok = true;
                            if (base) { if (fs_read_file(rp, vfs_cwd(), data, CAP, &n) != 0) { ok = false; } }
                            if (!ok) { kfree(data); ftp_say_n(&f, 550, "Cannot append"); }
                            else
                            {
                                ftp_say_n(&f, 150, "Ok to send data");
                                int d = data_open(&f);
                                if (d < 0) { kfree(data); ftp_say_n(&f, 425, "Cannot open data connection"); }
                                else
                                {
                                    bool full = false; uint64_t t1 = timer_millis();
                                    for (;;)
                                    {
                                        if (n + 1400 > CAP) { full = true; break; }
                                        int r = net_tcp_recv(0, d, data + n, 1400, 200U);
                                        if (r == 0) break;
                                        if (r > 0) { n += (uint64_t)r; t1 = timer_millis(); }
                                        else if (timer_millis() - t1 > 15000) { ok = false; break; }
                                        if (check_abort()) { ok = false; break; }
                                    }
                                    net_tcp_close(0, d);
                                    if (full) ftp_say_n(&f, 552, "File too large (4 MiB limit)");
                                    else if (!ok) ftp_say_n(&f, 426, "Transfer aborted");
                                    else if (fs_write_data(rp, vfs_cwd(), data, n) < 0) ftp_say_n(&f, 550, "Cannot write file (permission denied or no space)");
                                    else ftp_say_n(&f, 226, "Transfer complete");
                                    kfree(data);
                                }
                            }
                        }
                    }
                }
                else if (str_equal(cmd, "DELE")) { char v[300], rp[420]; vnorm(f.cwd, arg, v, sizeof(v)); real_path(&f, v, rp, sizeof(rp)); FsStat st; if (!may_write(&f)) ftp_say_n(&f, 550, "Read-only session"); else if (fs_stat_path(rp, vfs_cwd(), &st) != 0 || st.type == FS_DIR) ftp_say_n(&f, 550, "No such file"); else if (fs_rm(rp, vfs_cwd()) == 0) ftp_say_n(&f, 250, "Deleted"); else ftp_say_n(&f, 550, "Cannot delete"); }
                else if (str_equal(cmd, "MKD") || str_equal(cmd, "XMKD")) { char v[300], rp[420]; vnorm(f.cwd, arg, v, sizeof(v)); real_path(&f, v, rp, sizeof(rp)); if (!may_write(&f)) ftp_say_n(&f, 550, "Read-only session"); else if (fs_mkdir(rp, vfs_cwd()) >= 0) { char b[320]; Out o = {b, 0, sizeof(b)}; o_s(&o, "257 \""); o_s(&o, v); o_s(&o, "\" created"); ftp_say(&f, b); } else ftp_say_n(&f, 550, "Cannot create directory"); }
                else if (str_equal(cmd, "RMD") || str_equal(cmd, "XRMD")) { char v[300], rp[420]; vnorm(f.cwd, arg, v, sizeof(v)); real_path(&f, v, rp, sizeof(rp)); int id = fs_resolve(rp, vfs_cwd()); if (!may_write(&f)) ftp_say_n(&f, 550, "Read-only session"); else if (id < 0 || fs_get_type(id) != FS_DIR) ftp_say_n(&f, 550, "No such directory"); else if (fs_get_child_count(id) > 0) ftp_say_n(&f, 550, "Directory not empty"); else if (fs_rm(rp, vfs_cwd()) == 0) ftp_say_n(&f, 250, "Removed"); else ftp_say_n(&f, 550, "Cannot remove"); }
                else if (str_equal(cmd, "RNFR")) { char v[300], rp[420]; vnorm(f.cwd, arg, v, sizeof(v)); real_path(&f, v, rp, sizeof(rp)); FsStat st; if (!may_write(&f)) ftp_say_n(&f, 550, "Read-only session"); else if (fs_stat_path(rp, vfs_cwd(), &st) != 0) ftp_say_n(&f, 550, "No such file"); else { str_copy(f.rnfr, rp, sizeof(f.rnfr)); ftp_say_n(&f, 350, "Ready for RNTO"); } }
                else if (str_equal(cmd, "RNTO")) { char v[300], rp[420]; vnorm(f.cwd, arg, v, sizeof(v)); real_path(&f, v, rp, sizeof(rp)); if (!f.rnfr[0]) ftp_say_n(&f, 503, "RNFR first"); else if (fs_rename(f.rnfr, rp, vfs_cwd()) == 0) ftp_say_n(&f, 250, "Renamed"); else ftp_say_n(&f, 550, "Rename failed"); f.rnfr[0] = 0; }
                else if (str_equal(cmd, "SIZE")) { char v[300], rp[420]; vnorm(f.cwd, arg, v, sizeof(v)); real_path(&f, v, rp, sizeof(rp)); FsStat st; if (fs_stat_path(rp, vfs_cwd(), &st) == 0 && st.type == FS_FILE) { char b[40]; Out o = {b, 0, sizeof(b)}; o_s(&o, "213 "); o_n(&o, st.size); ftp_say(&f, b); } else ftp_say_n(&f, 550, "No such file"); }
                else if (str_equal(cmd, "MDTM")) { char v[300], rp[420]; vnorm(f.cwd, arg, v, sizeof(v)); real_path(&f, v, rp, sizeof(rp)); FsStat st; if (fs_stat_path(rp, vfs_cwd(), &st) == 0 && st.type == FS_FILE) { int y, mo, d, hh, mm, ss, wd; civil(st.mtime, &y, &mo, &d, &hh, &mm, &ss, &wd); char b[48]; Out o = {b, 0, sizeof(b)}; o_s(&o, "213 "); o_n(&o, (uint64_t)y); if (mo < 10) o_c(&o, '0'); o_n(&o, (uint64_t)mo); if (d < 10) o_c(&o, '0'); o_n(&o, (uint64_t)d); if (hh < 10) o_c(&o, '0'); o_n(&o, (uint64_t)hh); if (mm < 10) o_c(&o, '0'); o_n(&o, (uint64_t)mm); if (ss < 10) o_c(&o, '0'); o_n(&o, (uint64_t)ss); ftp_say(&f, b); } else ftp_say_n(&f, 550, "No such file"); }
                else if (str_equal(cmd, "REST")) { f.rest = (uint64_t)parse_num(arg, 0); ftp_say_n(&f, 350, "Restarting at the given offset"); }
                else ftp_say_n(&f, 502, "Command not implemented");
                fs_set_credentials(saved_uid, saved_gid);
            }
            if (quit) break;
        }
        else if (ch != '\r' && ln < (int)sizeof(line) - 1) line[ln++] = ch;
    }
    fs_set_credentials(saved_uid, saved_gid);
    if (f.pasv_sock >= 0) { net_tcp_close(0, f.pasv_sock); f.pasv_sock = -1; }
    return 0;
}

}   // namespace

void command_ftpd(const char* argument)
{
    char buf[200]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[10]; int n = split(buf, w, 10);
    int port = 21, sessions = 0, anon = 0; char root[96]; str_copy(root, "/", sizeof(root));
    for (int i = 0; i < n; ++i)
    {
        if (str_equal(w[i], "-p") && i + 1 < n) port = parse_num(w[++i], -1);
        else if (str_equal(w[i], "-n") && i + 1 < n) sessions = parse_num(w[++i], -1);
        else if (str_equal(w[i], "-d") && i + 1 < n) { str_copy(root, w[++i], sizeof(root)); int l = str_len(root); while (l > 1 && root[l - 1] == '/') root[--l] = 0; }
        else if (str_equal(w[i], "-a")) anon = 1;
        else if (str_equal(w[i], "-P") && i + 1 < n)
        {   /* -P lo-hi (or a single port): fixed passive data port range, so an emulator/NAT can forward it */
            const char* a = w[++i]; const char* dash = a; while (*dash && *dash != '-') ++dash;
            char lo_s[12]; int ll = 0; while (a + ll < dash && ll < 11) { lo_s[ll] = a[ll]; ++ll; } lo_s[ll] = 0; int lo = parse_num(lo_s, -1);
            int hi = *dash == '-' ? parse_num(dash + 1, -1) : lo;
            if (lo < 1024 || hi < lo || hi > 65535) { port = -1; break; }
            g_pasv_lo = (uint32_t)lo; g_pasv_n = (uint32_t)(hi - lo + 1);
        }
        else { port = -1; break; }
    }
    if (!net_ready() || port <= 0 || port > 65535 || sessions < 0 || root[0] != '/') { uart_puts("usage: ftpd [-p port] [-d root] [-n sessions] [-a] [-P lo-hi]   (-P fixes the passive data ports; -a allows read-only anonymous login; default: run until Ctrl-C)\r\n"); return; }
    { FsStat st; if (fs_stat_path(root, vfs_cwd(), &st) != 0 || st.type != FS_DIR) { uart_puts("ftpd: root is not a directory\r\n"); return; } }
    int ls = net_tcp_socket(0);
    if (ls < 0 || net_tcp_listen(0, ls, (uint16_t)port) != 0) { uart_puts("ftpd: cannot listen\r\n"); if (ls >= 0) net_tcp_close(0, ls); return; }
    uart_puts("ftpd: listening on port "); put_dec((uint64_t)port); uart_puts(", root "); uart_puts(root);
    uart_puts(anon ? ", anonymous read-only enabled" : ""); uart_puts(" (Ctrl-C stops)\r\n");
    g_abort = 0; int done = 0;
    while (!g_abort && (sessions == 0 || done < sessions))
    {
        uint32_t pip; uint16_t pport;
        int c = net_tcp_accept(0, ls, &pip, &pport, 500U);
        if (c < 0) { check_abort(); continue; }
        uart_puts("ftpd: connection from "); net_print_ipv4(pip); uart_puts("\r\n");
        ftp_session(c, root, anon, pip);
        net_tcp_close(0, c); ++done;
        uart_puts("ftpd: session closed\r\n");
    }
    net_tcp_close(0, ls);
    uart_puts("ftpd: stopped\r\n");
}
