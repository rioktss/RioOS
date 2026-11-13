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

#include "sshkey.h"
#include "crypto.h"
#include "ssh.h"
#include "tls.h"
#include "crypto.h"
#include "sha256.h"
#include "netstack.h"
#include "security.h"
#include "fs.h"
#include "vfs.h"
#include "memory.h"
#include "string.h"
#include "timer.h"
#include "uart.h"

namespace {

enum { MSG_DISCONNECT = 1, MSG_IGNORE = 2, MSG_UNIMPL = 3, MSG_DEBUG = 4, MSG_SERVICE_REQ = 5, MSG_SERVICE_ACC = 6,
       MSG_KEXINIT = 20, MSG_NEWKEYS = 21, MSG_ECDH_INIT = 30, MSG_ECDH_REPLY = 31,
       MSG_AUTH_REQ = 50, MSG_AUTH_FAIL = 51, MSG_AUTH_OK = 52, MSG_AUTH_BANNER = 53,
       MSG_GLOBAL_REQ = 80, MSG_REQ_FAIL = 82,
       MSG_CH_OPEN = 90, MSG_CH_CONFIRM = 91, MSG_CH_OPEN_FAIL = 92, MSG_CH_WINDOW = 93, MSG_CH_DATA = 94, MSG_CH_EXTDATA = 95,
       MSG_CH_EOF = 96, MSG_CH_CLOSE = 97, MSG_CH_REQUEST = 98, MSG_CH_SUCCESS = 99, MSG_CH_FAILURE = 100 };

const char* g_err = "";
int g_sk = -1;
uint32_t g_seq_out, g_seq_in;
int g_enc_out, g_enc_in;
struct Ctr { uint8_t rk[176]; uint8_t ctr[16]; uint8_t ks[16]; int pos; };
Ctr g_cout, g_cin;
uint8_t g_mac_out[32], g_mac_in[32];
uint8_t g_sid[32]; int g_have_sid;
uint8_t g_rx[36000], g_tx[36000];
uint8_t g_payload[36000];
uint32_t g_payload_len;
uint8_t g_peer_buf[4096]; uint32_t g_peer_len, g_peer_pos;     /* raw TCP read-ahead */

/* channel state */
uint32_t g_remote_ch, g_remote_window, g_remote_max;
uint32_t g_local_window;
int g_ch_open, g_got_eof, g_got_close, g_exit_status = -1;
uint8_t g_pend[36000]; uint32_t g_pend_len, g_pend_pos;       /* received data not yet handed out */
int g_pend_is_err;

void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
uint32_t get32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* growable-less writer into a fixed buffer */
struct W { uint8_t* b; uint32_t n, cap; bool bad; };
void w_u8(W* w, uint8_t v) { if (w->n + 1 > w->cap) { w->bad = true; return; } w->b[w->n++] = v; }
void w_u32(W* w, uint32_t v) { if (w->n + 4 > w->cap) { w->bad = true; return; } put32(w->b + w->n, v); w->n += 4; }
void w_raw(W* w, const void* d, uint32_t l) { if (w->n + l > w->cap) { w->bad = true; return; } const uint8_t* s = (const uint8_t*)d; for (uint32_t i = 0; i < l; ++i) w->b[w->n + i] = s[i]; w->n += l; }
void w_str(W* w, const void* d, uint32_t l) { w_u32(w, l); w_raw(w, d, l); }
void w_cstr(W* w, const char* s) { w_str(w, s, (uint32_t)str_len(s)); }
void w_mpint(W* w, const uint8_t* be, uint32_t l)     /* big-endian unsigned -> SSH mpint */
{
    while (l && be[0] == 0) { ++be; --l; }
    if (l == 0) { w_u32(w, 0); return; }
    bool pad = (be[0] & 0x80) != 0;
    w_u32(w, l + (pad ? 1U : 0U)); if (pad) w_u8(w, 0); w_raw(w, be, l);
}

struct R { const uint8_t* p; uint32_t n, pos; bool bad; };
uint8_t r_u8(R* r) { if (r->pos + 1 > r->n) { r->bad = true; return 0; } return r->p[r->pos++]; }
uint32_t r_u32(R* r) { if (r->pos + 4 > r->n) { r->bad = true; return 0; } uint32_t v = get32(r->p + r->pos); r->pos += 4; return v; }
const uint8_t* r_str(R* r, uint32_t* l) { uint32_t n = r_u32(r); if (r->bad || r->pos + n > r->n) { r->bad = true; *l = 0; return r->p; } const uint8_t* s = r->p + r->pos; r->pos += n; *l = n; return s; }

bool tx_time_left(uint64_t deadline) { return timer_millis() < deadline; }

/* ---- raw TCP ---- */
int tcp_fill(uint32_t n, uint32_t timeout_ms)        /* make >= n bytes available WITHOUT consuming: 0 ok, -1 timeout, -2 closed */
{
    uint64_t deadline = timer_millis() + timeout_ms;
    while (g_peer_len - g_peer_pos < n)
    {
        if (g_peer_pos)
        {
            uint32_t k = g_peer_len - g_peer_pos; for (uint32_t i = 0; i < k; ++i) g_peer_buf[i] = g_peer_buf[g_peer_pos + i];
            g_peer_len = k; g_peer_pos = 0;
        }
        int r = net_tcp_recv(0, g_sk, g_peer_buf + g_peer_len, sizeof(g_peer_buf) - g_peer_len, 50U);
        if (r == 0) return -2;
        if (r > 0) { g_peer_len += (uint32_t)r; continue; }
        if (!tx_time_left(deadline)) return -1;
    }
    return 0;
}
int tcp_get(uint8_t* out, uint32_t n, uint32_t timeout_ms)       /* exactly n bytes: 0 ok, -1 timeout, -2 closed/error */
{
    uint64_t deadline = timer_millis() + timeout_ms; uint32_t got = 0;
    while (got < n)
    {
        uint32_t want = n - got; if (want > 512) want = 512;
        uint32_t left = tx_time_left(deadline) ? (uint32_t)(deadline - timer_millis()) : 0;
        int r = tcp_fill(1, left);
        if (r != 0) return r;
        uint32_t avail = g_peer_len - g_peer_pos; if (avail > want) avail = want;
        for (uint32_t i = 0; i < avail; ++i) out[got + i] = g_peer_buf[g_peer_pos + i];
        g_peer_pos += avail; got += avail;
    }
    return 0;
}
int tcp_put(const uint8_t* d, uint32_t n)
{
    uint32_t off = 0;
    while (off < n)
    {
        uint32_t chunk = n - off; if (chunk > 1200) chunk = 1200;
        int r = net_tcp_send(0, g_sk, d + off, chunk, 5000U);
        if (r <= 0) return -1;
        off += (uint32_t)r;
    }
    return 0;
}

/* ---- ciphers ---- */
void ctr_init(Ctr* c, const uint8_t key[16], const uint8_t iv[16])
{
    tls_aes128_expand(c->rk, key);
    for (int i = 0; i < 16; ++i) c->ctr[i] = iv[i];
    c->pos = 16;
}
void ctr_xor(Ctr* c, uint8_t* d, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
    {
        if (c->pos == 16)
        {
            tls_aes128_encrypt_block(c->rk, c->ctr, c->ks); c->pos = 0;
            for (int k = 15; k >= 0; --k) if (++c->ctr[k]) break;
        }
        d[i] ^= c->ks[c->pos++];
    }
}

/* ---- packets ---- */
int send_packet(const uint8_t* payload, uint32_t plen)
{
    uint32_t block = g_enc_out ? 16 : 8;
    uint32_t pad = block - ((5 + plen) % block); if (pad < 4) pad += block;
    uint32_t total = 4 + 1 + plen + pad;
    if (total + 32 > sizeof(g_tx)) { g_err = "packet too large"; return -1; }
    put32(g_tx, total - 4); g_tx[4] = (uint8_t)pad;
    for (uint32_t i = 0; i < plen; ++i) g_tx[5 + i] = payload[i];
    tls_random(g_tx + 5 + plen, pad);
    uint32_t mlen = 0;
    if (g_enc_out)
    {
        static uint8_t mb[36000 + 4];
        put32(mb, g_seq_out); for (uint32_t i = 0; i < total; ++i) mb[4 + i] = g_tx[i];
        hmac_sha256(g_mac_out, 32, mb, total + 4, g_tx + total); mlen = 32;
        ctr_xor(&g_cout, g_tx, total);
    }
    ++g_seq_out;
    if (tcp_put(g_tx, total + mlen) != 0) { g_err = "connection lost while sending"; return -1; }
    return 0;
}

/* receive one packet into g_payload; 0 ok, -1 timeout, -2 error */
int recv_packet(uint32_t timeout_ms)
{
    uint8_t hdr[4];
    int r = tcp_fill(4, timeout_ms);
    if (r == 0) r = tcp_get(hdr, 4, 100);
    if (r == -1) return -1;
    if (r != 0) { g_err = "connection closed by the server"; return -2; }
    if (g_enc_in) ctr_xor(&g_cin, hdr, 4);
    uint32_t len = get32(hdr);
    if (len < 12 || len > 35000 || ((len + 4) & (g_enc_in ? 15U : 7U))) { g_err = "bad packet length from server"; return -2; }
    put32(g_rx, len);
    if (tcp_get(g_rx + 4, len + (g_enc_in ? 32U : 0U), 15000U) != 0) { g_err = "connection lost while receiving"; return -2; }
    if (g_enc_in)
    {
        ctr_xor(&g_cin, g_rx + 4, len);
        static uint8_t mb[36000 + 8]; uint8_t mac[32];
        put32(mb, g_seq_in); for (uint32_t i = 0; i < len + 4; ++i) mb[4 + i] = g_rx[i];
        hmac_sha256(g_mac_in, 32, mb, len + 8, mac);
        uint8_t diff = 0; for (int i = 0; i < 32; ++i) diff |= (uint8_t)(mac[i] ^ g_rx[4 + len + i]);
        if (diff) { g_err = "server MAC check failed"; return -2; }
    }
    ++g_seq_in;
    uint32_t pad = g_rx[4];
    if (pad + 1 > len) { g_err = "bad padding"; return -2; }
    g_payload_len = len - pad - 1;
    for (uint32_t i = 0; i < g_payload_len; ++i) g_payload[i] = g_rx[5 + i];
    return 0;
}
/* next packet that is not IGNORE/DEBUG/UNIMPL; handles DISCONNECT */
int next_msg(uint32_t timeout_ms)
{
    for (;;)
    {
        int r = recv_packet(timeout_ms); if (r != 0) return r;
        if (g_payload_len == 0) continue;
        uint8_t t = g_payload[0];
        if (t == MSG_IGNORE || t == MSG_DEBUG || t == MSG_UNIMPL) continue;
        if (t == MSG_DISCONNECT) { g_err = "server closed the connection (disconnect)"; return -2; }
        return 0;
    }
}

/* ---- known hosts ---- */
}   // namespace

void ssh_known_hosts_path(char* out, int cap)
{
    if (security_is_root()) { str_copy(out, "/etc/ssh_known_hosts", cap); return; }
    char t[96] = "/home/"; int n = 6;
    const char* u = security_username(); while (*u && n < 80) t[n++] = *u++;
    t[n] = 0; str_copy(t + n, "/.ssh_known_hosts", (int)sizeof(t) - n);
    str_copy(out, t, cap);
}

void ssh_fingerprint(const uint8_t* blob, uint32_t len, char* out, int cap)
{
    uint8_t h[32]; sha256(blob, len, h);
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char t[64]; str_copy(t, "SHA256:", 8); int n = 7;
    uint32_t acc = 0; int bits = 0;
    for (int i = 0; i < 32; ++i) { acc = (acc << 8) | h[i]; bits += 8; while (bits >= 6) { bits -= 6; t[n++] = b64[(acc >> bits) & 63]; } acc &= (1u << bits) - 1; }
    if (bits) t[n++] = b64[(acc << (6 - bits)) & 63];
    t[n] = 0; str_copy(out, t, cap);
}

namespace {

void hexs(const uint8_t* d, int n, char* out) { static const char* h = "0123456789abcdef"; for (int i = 0; i < n; ++i) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; } out[2 * n] = 0; }

void known_key(const char* host, uint32_t port, char* out, int cap)       /* "host:port" or "[host]:port" */
{
    char t[160]; int n = 0; t[n++] = '[';
    for (const char* p = host; *p && n < 130; ++p) t[n++] = *p;
    t[n++] = ']'; t[n++] = ':';
    char num[12]; int k = 0; uint32_t v = port; char rev[12]; int rn = 0; do { rev[rn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (rn) num[k++] = rev[--rn]; num[k] = 0;
    for (int i = 0; num[i]; ++i) t[n++] = num[i];
    t[n] = 0; str_copy(out, t, cap);
}

/* returns 1 known+match, 0 unknown, -1 known but different */
int known_lookup(const char* key, const char* keyhex)
{
    char path[128]; ssh_known_hosts_path(path, sizeof(path));
    static uint8_t buf[8192]; uint64_t sz = 0;
    if (fs_read_file(path, vfs_cwd(), buf, sizeof(buf) - 1, &sz) != 0) return 0;
    buf[sz] = 0;
    uint32_t i = 0; int klen = str_len(key);
    while (i < sz)
    {
        uint32_t e = i; while (e < sz && buf[e] != '\n') ++e;
        if ((int)(e - i) > klen && buf[i + (uint32_t)klen] == ' ')
        {
            bool m = true; for (int k = 0; k < klen; ++k) if (buf[i + (uint32_t)k] != (uint8_t)key[k]) { m = false; break; }
            if (m)
            {
                const char* val = (const char*)buf + i + klen + 1; uint32_t vl = e - i - (uint32_t)klen - 1;
                if ((int)vl == str_len(keyhex)) { bool eq = true; for (uint32_t k = 0; k < vl; ++k) if (val[k] != keyhex[k]) { eq = false; break; } return eq ? 1 : -1; }
                return -1;
            }
        }
        i = e + 1;
    }
    return 0;
}
int known_store(const char* key, const char* keyhex, int remove_only)
{
    char path[128]; ssh_known_hosts_path(path, sizeof(path));
    static uint8_t buf[8192]; uint64_t sz = 0;
    if (fs_read_file(path, vfs_cwd(), buf, sizeof(buf) - 400, &sz) != 0) sz = 0;
    /* drop the old line for this key */
    static uint8_t out[8192 + 400]; uint32_t on = 0; int klen = str_len(key); int removed = 0;
    uint32_t i = 0;
    while (i < sz)
    {
        uint32_t e = i; while (e < sz && buf[e] != '\n') ++e;
        bool m = (int)(e - i) > klen && buf[i + (uint32_t)klen] == ' ';
        if (m) for (int k = 0; k < klen; ++k) if (buf[i + (uint32_t)k] != (uint8_t)key[k]) { m = false; break; }
        if (m) removed = 1; else { for (uint32_t k = i; k < e; ++k) out[on++] = buf[k]; out[on++] = '\n'; }
        i = e + 1;
    }
    if (!remove_only) { for (int k = 0; key[k]; ++k) out[on++] = (uint8_t)key[k]; out[on++] = ' '; for (int k = 0; keyhex[k]; ++k) out[on++] = (uint8_t)keyhex[k]; out[on++] = '\n'; }
    else if (!removed) return -1;
    (void)fs_mkdir_p("/etc", fs_get_root());
    return fs_write_data(path, vfs_cwd(), out, on) < 0 ? -2 : 0;
}

bool name_in_list(const uint8_t* list, uint32_t ll, const char* name)
{
    uint32_t nl = (uint32_t)str_len(name), i = 0;
    while (i <= ll)
    {
        uint32_t e = i; while (e < ll && list[e] != ',') ++e;
        if (e - i == nl) { bool m = true; for (uint32_t k = 0; k < nl; ++k) if (list[i + k] != (uint8_t)name[k]) { m = false; break; } if (m) return true; }
        i = e + 1;
    }
    return false;
}

/* verify the host key signature over H */
int verify_hostkey(const char* alg, const uint8_t* blob, uint32_t bl, const uint8_t* sigblob, uint32_t sl, const uint8_t* H)
{
    R kb = {blob, bl, 0, false}; uint32_t tl; (void)r_str(&kb, &tl);       /* key type name: "ssh-rsa" for rsa-sha2-* signatures */
    R sb = {sigblob, sl, 0, false}; uint32_t sal; const uint8_t* sa = r_str(&sb, &sal); uint32_t sigl; const uint8_t* sig = r_str(&sb, &sigl);
    if (sb.bad || sal != (uint32_t)str_len(alg)) return -1;
    for (uint32_t i = 0; i < sal; ++i) if (sa[i] != (uint8_t)alg[i]) return -1;
    if (str_equal(alg, "ssh-ed25519"))
    {
        uint32_t pl; const uint8_t* pk = r_str(&kb, &pl);
        if (kb.bad || pl != 32 || sigl != 64 || tl != 11) return -1;
        return ed25519_verify(pk, H, 32, sig);
    }
    if (str_equal(alg, "rsa-sha2-256") || str_equal(alg, "rsa-sha2-512"))
    {
        uint32_t el, nl2; const uint8_t* e = r_str(&kb, &el); const uint8_t* n = r_str(&kb, &nl2);
        if (kb.bad || tl != 7) return -1;
        int h = alg[9] == '5' ? HASH_SHA512 : HASH_SHA256; uint8_t d[64]; hash_buf(h, H, 32, d);
        return rsa_verify_pkcs1(n, nl2, e, el, h, d, sig, sigl);
    }
    if (str_equal(alg, "ecdsa-sha2-nistp256"))
    {
        uint32_t cl, ql; (void)r_str(&kb, &cl); const uint8_t* q = r_str(&kb, &ql);
        if (kb.bad || ql != 65) return -1;
        R sr = {sig, sigl, 0, false}; uint32_t rl, sl2; const uint8_t* rv = r_str(&sr, &rl); const uint8_t* sv = r_str(&sr, &sl2);
        if (sr.bad) return -1;
        uint8_t der[100]; uint32_t dn = 0; der[dn++] = 0x30; uint32_t lp = dn++; 
        while (rl > 1 && rv[0] == 0 && !(rv[1] & 0x80)) { ++rv; --rl; }
        while (sl2 > 1 && sv[0] == 0 && !(sv[1] & 0x80)) { ++sv; --sl2; }
        if (rl > 33 || sl2 > 33) return -1;
        der[dn++] = 2; der[dn++] = (uint8_t)rl; for (uint32_t i = 0; i < rl; ++i) der[dn++] = rv[i];
        der[dn++] = 2; der[dn++] = (uint8_t)sl2; for (uint32_t i = 0; i < sl2; ++i) der[dn++] = sv[i];
        der[lp] = (uint8_t)(dn - lp - 1);
        uint8_t d[32]; sha256(H, 32, d);
        return ecdsa_verify(1, q, ql, d, 32, der, dn);
    }
    return -1;
}

void derive(uint8_t* out, const uint8_t* K, uint32_t kl, const uint8_t* H, char letter)
{
    static uint8_t b[512]; W w = {b, 0, sizeof(b), false};
    w_mpint(&w, K, kl); w_raw(&w, H, 32); w_u8(&w, (uint8_t)letter); w_raw(&w, g_sid, 32);
    sha256(b, w.n, out);
}

}   // namespace

const char* ssh_error() { return g_err; }

int ssh_connect(int sock, const SshOpts* o)
{
    g_sk = sock; g_err = ""; g_seq_in = g_seq_out = 0; g_enc_in = g_enc_out = 0; g_peer_len = g_peer_pos = 0; g_ch_open = 0; g_exit_status = -1; g_have_sid = 0;
    /* identification strings */
    static const char vc[] = "SSH-2.0-RioOS_1.0";
    uint8_t line[300]; int ll = 0;
    for (int i = 0; vc[i]; ++i) line[ll++] = (uint8_t)vc[i];
    line[ll++] = '\r'; line[ll++] = '\n';
    if (tcp_put(line, (uint32_t)ll) != 0) { g_err = "cannot send identification"; return -1; }
    char vs[260]; int vn = 0;
    for (int lines = 0; lines < 30; ++lines)
    {
        vn = 0; uint8_t c;
        for (;;)
        {
            int r = tcp_get(&c, 1, 10000U); if (r != 0) { g_err = "no SSH banner from the server"; return -1; }
            if (c == '\n') break; if (vn < 255 && c != '\r') vs[vn++] = (char)c;
        }
        vs[vn] = 0;
        if (vn >= 4 && vs[0] == 'S' && vs[1] == 'S' && vs[2] == 'H' && vs[3] == '-') break;
        vn = -1;
    }
    if (vn < 0) { g_err = "server did not send an SSH identification"; return -1; }
    if (!(vs[4] == '2' && vs[5] == '.') && !(vs[4] == '1' && vs[5] == '.' && vs[6] == '9')) { g_err = "server does not speak SSH-2"; return -1; }
    /* KEXINIT */
    static uint8_t ic[1024], is[8192]; uint32_t icl, isl;
    {
        W w = {ic, 0, sizeof(ic), false};
        w_u8(&w, MSG_KEXINIT); uint8_t cookie[16]; tls_random(cookie, 16); w_raw(&w, cookie, 16);
        w_cstr(&w, "curve25519-sha256,curve25519-sha256@libssh.org");
        w_cstr(&w, "ssh-ed25519,rsa-sha2-512,rsa-sha2-256,ecdsa-sha2-nistp256");
        w_cstr(&w, "aes128-ctr"); w_cstr(&w, "aes128-ctr");
        w_cstr(&w, "hmac-sha2-256"); w_cstr(&w, "hmac-sha2-256");
        w_cstr(&w, "none"); w_cstr(&w, "none"); w_cstr(&w, ""); w_cstr(&w, "");
        w_u8(&w, 0); w_u32(&w, 0);
        icl = w.n;
        if (send_packet(ic, icl) != 0) return -1;
    }
    char hostalg[32] = "";
    {
        int r = next_msg(15000U); if (r != 0) { if (!g_err[0]) g_err = "timeout waiting for the server key exchange"; return -1; }
        if (g_payload[0] != MSG_KEXINIT) { g_err = "unexpected message instead of KEXINIT"; return -1; }
        isl = g_payload_len; if (isl > sizeof(is)) { g_err = "server KEXINIT too large"; return -1; }
        for (uint32_t i = 0; i < isl; ++i) is[i] = g_payload[i];
        R rd = {is, isl, 17, false}; uint32_t l[10]; const uint8_t* lst[10];
        for (int i = 0; i < 10; ++i) lst[i] = r_str(&rd, &l[i]);
        if (rd.bad) { g_err = "malformed server KEXINIT"; return -1; }
        if (!name_in_list(lst[0], l[0], "curve25519-sha256") && !name_in_list(lst[0], l[0], "curve25519-sha256@libssh.org")) { g_err = "no common key exchange (server lacks curve25519-sha256)"; return -1; }
        static const char* algs[] = {"ssh-ed25519", "rsa-sha2-512", "rsa-sha2-256", "ecdsa-sha2-nistp256"};
        for (const char* a : algs) if (name_in_list(lst[1], l[1], a)) { str_copy(hostalg, a, sizeof(hostalg)); break; }
        if (!hostalg[0]) { g_err = "no common host key algorithm (need ssh-ed25519, rsa-sha2-256/512 or ecdsa-sha2-nistp256)"; return -1; }
        if (!name_in_list(lst[2], l[2], "aes128-ctr") || !name_in_list(lst[3], l[3], "aes128-ctr")) { g_err = "no common cipher (need aes128-ctr)"; return -1; }
        if (!name_in_list(lst[4], l[4], "hmac-sha2-256") || !name_in_list(lst[5], l[5], "hmac-sha2-256")) { g_err = "no common MAC (need hmac-sha2-256)"; return -1; }
    }
    /* ECDH */
    uint8_t priv[32], qc[32]; tls_random(priv, 32); tls_x25519_base(qc, priv);
    {
        uint8_t b[64]; W w = {b, 0, sizeof(b), false}; w_u8(&w, MSG_ECDH_INIT); w_str(&w, qc, 32);
        if (send_packet(b, w.n) != 0) return -1;
    }
    if (next_msg(15000U) != 0) { if (!g_err[0]) g_err = "timeout waiting for the key exchange reply"; return -1; }
    if (g_payload[0] != MSG_ECDH_REPLY) { g_err = "unexpected message instead of ECDH reply"; return -1; }
    static uint8_t ks[2048], sigb[2048]; uint32_t ksl, sigl; uint8_t qs[32];
    {
        R rd = {g_payload, g_payload_len, 1, false}; uint32_t l; const uint8_t* p = r_str(&rd, &l);
        if (rd.bad || l > sizeof(ks)) { g_err = "bad host key"; return -1; }
        for (uint32_t i = 0; i < l; ++i) ks[i] = p[i]; ksl = l;
        p = r_str(&rd, &l); if (rd.bad || l != 32) { g_err = "bad server ephemeral key"; return -1; }
        for (int i = 0; i < 32; ++i) qs[i] = p[i];
        p = r_str(&rd, &l); if (rd.bad || l > sizeof(sigb)) { g_err = "bad signature"; return -1; }
        for (uint32_t i = 0; i < l; ++i) sigb[i] = p[i]; sigl = l;
    }
    uint8_t K[32]; tls_x25519(K, priv, qs);
    { uint8_t z = 0; for (int i = 0; i < 32; ++i) z |= K[i]; if (!z) { g_err = "invalid key exchange value"; return -1; } }
    uint8_t H[32];
    {
        static uint8_t hb[12000]; W w = {hb, 0, sizeof(hb), false};
        w_str(&w, vc, (uint32_t)str_len(vc)); w_str(&w, vs, (uint32_t)vn); w_str(&w, ic, icl); w_str(&w, is, isl);
        w_str(&w, ks, ksl); w_str(&w, qc, 32); w_str(&w, qs, 32); w_mpint(&w, K, 32);
        if (w.bad) { g_err = "key exchange buffer overflow"; return -1; }
        sha256(hb, w.n, H);
    }
    if (verify_hostkey(hostalg, ks, ksl, sigb, sigl, H) != 0) { g_err = "host key signature is invalid (possible man-in-the-middle)"; return -1; }
    /* known_hosts */
    {
        char key[160], hex[65], fp[64]; known_key(o->host, o->port, key, sizeof(key));
        uint8_t kh[32]; sha256(ks, ksl, kh); hexs(kh, 32, hex); ssh_fingerprint(ks, ksl, fp, sizeof(fp));
        int st = known_lookup(key, hex);
        if (st < 0 && o->strict != 2)
        {
            static char msg[200]; str_copy(msg, "REMOTE HOST IDENTIFICATION HAS CHANGED for ", sizeof(msg)); str_copy(msg + str_len(msg), key, (int)sizeof(msg) - str_len(msg));
            str_copy(msg + str_len(msg), " (new key ", (int)sizeof(msg) - str_len(msg)); str_copy(msg + str_len(msg), fp, (int)sizeof(msg) - str_len(msg));
            str_copy(msg + str_len(msg), "); remove the old key with 'ssh -R host' or use -o StrictHostKeyChecking=no", (int)sizeof(msg) - str_len(msg));
            g_err = msg; return -1;
        }
        if (st == 0 && o->strict == 1) { g_err = "host key is not known and StrictHostKeyChecking=yes"; return -1; }
        if (st != 1)
        {
            if (known_store(key, hex, 0) == 0) { uart_puts("Warning: Permanently added '"); uart_puts(o->host); uart_puts("' ("); uart_puts(hostalg); uart_puts(") to the list of known hosts. "); uart_puts(fp); uart_puts("\r\n"); }
            else { uart_puts("Warning: could not save the host key ("); uart_puts(fp); uart_puts(")\r\n"); }
        }
    }
    /* NEWKEYS */
    { uint8_t b[1] = {MSG_NEWKEYS}; if (send_packet(b, 1) != 0) return -1; }
    for (int i = 0; i < 32; ++i) g_sid[i] = H[i];
    g_have_sid = 1;
    uint8_t iv_c[32], iv_s[32], k_c[32], k_s[32];
    derive(iv_c, K, 32, H, 'A'); derive(iv_s, K, 32, H, 'B'); derive(k_c, K, 32, H, 'C'); derive(k_s, K, 32, H, 'D');
    derive(g_mac_out, K, 32, H, 'E'); derive(g_mac_in, K, 32, H, 'F');
    ctr_init(&g_cout, k_c, iv_c); g_enc_out = 1;
    if (next_msg(15000U) != 0) { if (!g_err[0]) g_err = "timeout waiting for NEWKEYS"; return -1; }
    if (g_payload[0] != MSG_NEWKEYS) { g_err = "expected NEWKEYS"; return -1; }
    ctr_init(&g_cin, k_s, iv_s); g_enc_in = 1;
    /* service + password auth */
    {
        uint8_t b[64]; W w = {b, 0, sizeof(b), false}; w_u8(&w, MSG_SERVICE_REQ); w_cstr(&w, "ssh-userauth");
        if (send_packet(b, w.n) != 0) return -1;
        if (next_msg(15000U) != 0 || g_payload[0] != MSG_SERVICE_ACC) { if (!g_err[0]) g_err = "server refused the ssh-userauth service"; return -1; }
    }
    /* authentication: Ed25519 public key first (when an identity is given), then the password */
    int authed = 0, key_tried = 0;
    if (o->key_seed && o->key_pub)
    {
        static uint8_t b[600]; static uint8_t sd[400];
        uint8_t blob[51]; sshkey_blob(o->key_pub, blob);
        W sw = {sd, 0, sizeof(sd), false};                          /* data to sign (RFC 4252 section 7) */
        w_str(&sw, g_sid, 32); w_u8(&sw, MSG_AUTH_REQ); w_cstr(&sw, o->user); w_cstr(&sw, "ssh-connection"); w_cstr(&sw, "publickey");
        w_u8(&sw, 1); w_cstr(&sw, "ssh-ed25519"); w_str(&sw, blob, 51);
        uint8_t sig[64];
        if (sw.bad || ed25519_sign(o->key_seed, sd, sw.n, sig) != 0) { g_err = "public-key signing failed"; return -1; }
        W w = {b, 0, sizeof(b), false};
        w_u8(&w, MSG_AUTH_REQ); w_cstr(&w, o->user); w_cstr(&w, "ssh-connection"); w_cstr(&w, "publickey"); w_u8(&w, 1); w_cstr(&w, "ssh-ed25519"); w_str(&w, blob, 51);
        uint8_t sb[83]; W sg = {sb, 0, sizeof(sb), false}; w_cstr(&sg, "ssh-ed25519"); w_str(&sg, sig, 64); w_str(&w, sb, sg.n);
        if (w.bad) { g_err = "user name too long"; return -1; }
        if (send_packet(b, w.n) != 0) return -1;
        key_tried = 1;
        for (;;)
        {
            if (next_msg(20000U) != 0) { if (!g_err[0]) g_err = "timeout during authentication"; return -1; }
            uint8_t t = g_payload[0];
            if (t == MSG_AUTH_BANNER) { R rd = {g_payload, g_payload_len, 1, false}; uint32_t l; const uint8_t* s = r_str(&rd, &l); if (!rd.bad) for (uint32_t i = 0; i < l; ++i) { if (s[i] == '\n') uart_putc('\r'); if (s[i] != '\r') uart_putc((char)s[i]); } continue; }
            if (t == MSG_AUTH_OK) { authed = 1; break; }
            if (t == MSG_AUTH_FAIL) break;                                  /* key refused: fall back to the password */
            g_err = "unexpected message during authentication"; return -1;
        }
    }
    if (!authed)
    {
        char pwbuf[128]; const char* pw = o->password;
        if (!pw && o->get_password) { if (o->get_password(pwbuf, sizeof(pwbuf)) < 0) { g_err = key_tried ? "Permission denied (publickey)" : "no password given"; return -1; } pw = pwbuf; }
        if (!pw) { g_err = "Permission denied (publickey)"; return -1; }
        static uint8_t b[600]; W w = {b, 0, sizeof(b), false};
        w_u8(&w, MSG_AUTH_REQ); w_cstr(&w, o->user); w_cstr(&w, "ssh-connection"); w_cstr(&w, "password"); w_u8(&w, 0); w_cstr(&w, pw);
        for (int i = 0; i < (int)sizeof(pwbuf); ++i) pwbuf[i] = 0;
        if (w.bad) { g_err = "user name or password too long"; return -1; }
        if (send_packet(b, w.n) != 0) return -1;
        for (;;)
        {
            if (next_msg(20000U) != 0) { if (!g_err[0]) g_err = "timeout during authentication"; return -1; }
            uint8_t t = g_payload[0];
            if (t == MSG_AUTH_BANNER) { R rd = {g_payload, g_payload_len, 1, false}; uint32_t l; const uint8_t* s = r_str(&rd, &l); if (!rd.bad) for (uint32_t i = 0; i < l; ++i) { if (s[i] == '\n') uart_putc('\r'); if (s[i] != '\r') uart_putc((char)s[i]); } continue; }
            if (t == MSG_AUTH_OK) break;
            if (t == MSG_AUTH_FAIL) { g_err = "Permission denied (password authentication failed)"; return -1; }
            g_err = "unexpected message during authentication"; return -1;
        }
    }
    return 0;
}

namespace {
int wait_channel_reply(int want_ok)
{
    for (;;)
    {
        if (next_msg(15000U) != 0) { if (!g_err[0]) g_err = "timeout waiting for the server"; return -1; }
        uint8_t t = g_payload[0];
        if (t == MSG_CH_SUCCESS) return 0;
        if (t == MSG_CH_FAILURE) { g_err = "server refused the channel request"; return -1; }
        if (t == MSG_CH_WINDOW && g_payload_len >= 9) { g_remote_window += get32(g_payload + 5); continue; }
        if (t == MSG_GLOBAL_REQ) continue;
        (void)want_ok;
    }
}
}   // namespace

int ssh_open_channel(const char* command, int pty)
{
    g_got_eof = g_got_close = 0; g_pend_len = g_pend_pos = 0; g_exit_status = -1; g_local_window = 2097152;
    {
        uint8_t b[64]; W w = {b, 0, sizeof(b), false}; w_u8(&w, MSG_CH_OPEN); w_cstr(&w, "session"); w_u32(&w, 0); w_u32(&w, g_local_window); w_u32(&w, 32768);
        if (send_packet(b, w.n) != 0) return -1;
    }
    for (;;)
    {
        if (next_msg(15000U) != 0) { if (!g_err[0]) g_err = "timeout opening the channel"; return -1; }
        uint8_t t = g_payload[0];
        if (t == MSG_CH_CONFIRM && g_payload_len >= 17) { g_remote_ch = get32(g_payload + 5); g_remote_window = get32(g_payload + 9); g_remote_max = get32(g_payload + 13); break; }
        if (t == MSG_CH_OPEN_FAIL) { g_err = "server refused to open a session channel"; return -1; }
    }
    if (g_remote_max > 32768) g_remote_max = 32768;
    if (g_remote_max < 64) { g_err = "bad remote packet size"; return -1; }
    g_ch_open = 1;
    if (pty)
    {
        uint8_t b[128]; W w = {b, 0, sizeof(b), false}; w_u8(&w, MSG_CH_REQUEST); w_u32(&w, g_remote_ch); w_cstr(&w, "pty-req"); w_u8(&w, 1);
        w_cstr(&w, "vt100"); w_u32(&w, 80); w_u32(&w, 24); w_u32(&w, 0); w_u32(&w, 0); uint8_t modes = 0; w_str(&w, &modes, 1);
        if (send_packet(b, w.n) != 0 || wait_channel_reply(1) != 0) return -1;
    }
    {
        static uint8_t b[700]; W w = {b, 0, sizeof(b), false}; w_u8(&w, MSG_CH_REQUEST); w_u32(&w, g_remote_ch);
        if (command) { w_cstr(&w, "exec"); w_u8(&w, 1); w_cstr(&w, command); } else { w_cstr(&w, "shell"); w_u8(&w, 1); }
        if (w.bad) { g_err = "command too long"; return -1; }
        if (send_packet(b, w.n) != 0 || wait_channel_reply(1) != 0) return -1;
    }
    return 0;
}

namespace {
/* process one incoming message; may fill g_pend */
int handle_msg()
{
    uint8_t t = g_payload[0];
    R rd = {g_payload, g_payload_len, 1, false};
    switch (t)
    {
    case MSG_CH_DATA: case MSG_CH_EXTDATA:
    {
        (void)r_u32(&rd); uint32_t ext = 0; if (t == MSG_CH_EXTDATA) ext = r_u32(&rd);
        uint32_t l; const uint8_t* d = r_str(&rd, &l); if (rd.bad || l > sizeof(g_pend)) { g_err = "bad channel data"; return -2; }
        for (uint32_t i = 0; i < l; ++i) g_pend[i] = d[i];
        g_pend_len = l; g_pend_pos = 0; g_pend_is_err = ext != 0;
        g_local_window = g_local_window > l ? g_local_window - l : 0;
        if (g_local_window < 1048576)
        {
            uint8_t b[16]; W w = {b, 0, sizeof(b), false}; w_u8(&w, MSG_CH_WINDOW); w_u32(&w, g_remote_ch); w_u32(&w, 2097152 - g_local_window);
            if (send_packet(b, w.n) != 0) return -2;
            g_local_window = 2097152;
        }
        return 0;
    }
    case MSG_CH_WINDOW: (void)r_u32(&rd); g_remote_window += r_u32(&rd); return 0;
    case MSG_CH_EOF: g_got_eof = 1; return 0;
    case MSG_CH_CLOSE:
    {
        g_got_close = 1;
        uint8_t b[8]; W w = {b, 0, sizeof(b), false}; w_u8(&w, MSG_CH_CLOSE); w_u32(&w, g_remote_ch); (void)send_packet(b, w.n);
        g_ch_open = 0; return 0;
    }
    case MSG_CH_REQUEST:
    {
        (void)r_u32(&rd); uint32_t l; const uint8_t* nm = r_str(&rd, &l); uint8_t want = r_u8(&rd);
        if (!rd.bad && l == 11 && nm[0] == 'e' && nm[5] == 's' && nm[6] == 't') g_exit_status = (int)r_u32(&rd);       /* exit-status */
        else if (!rd.bad && l == 11 && nm[0] == 'e' && nm[5] == 's' && nm[6] == 'i') g_exit_status = 255;               /* exit-signal */
        if (want) { uint8_t b[8]; W w = {b, 0, sizeof(b), false}; w_u8(&w, MSG_CH_FAILURE); w_u32(&w, g_remote_ch); (void)send_packet(b, w.n); }
        return 0;
    }
    case MSG_GLOBAL_REQ:
    {
        uint32_t l; (void)r_str(&rd, &l); uint8_t want = r_u8(&rd);
        if (want) { uint8_t b[1] = {MSG_REQ_FAIL}; (void)send_packet(b, 1); }
        return 0;
    }
    case MSG_KEXINIT: g_err = "server asked for re-keying (not supported)"; return -2;
    default: return 0;
    }
}
}   // namespace

int ssh_last_was_stderr() { return g_pend_is_err; }
int ssh_exit_status() { return g_exit_status; }

int ssh_recv(uint8_t* buf, uint32_t cap, uint32_t timeout_ms)
{
    uint64_t deadline = timer_millis() + timeout_ms;
    for (;;)
    {
        if (g_pend_pos < g_pend_len)
        {
            uint32_t n = g_pend_len - g_pend_pos; if (n > cap) n = cap;
            for (uint32_t i = 0; i < n; ++i) buf[i] = g_pend[g_pend_pos + i];
            g_pend_pos += n; return (int)n;
        }
        if (g_got_close || (g_got_eof && !g_ch_open)) return 0;
        if (g_got_eof && g_exit_status >= 0) { /* keep waiting briefly for CLOSE */ }
        uint32_t left = tx_time_left(deadline) ? (uint32_t)(deadline - timer_millis()) : 0;
        int r = recv_packet(left > 100 ? 100 : left);
        if (r == -2) { g_ch_open = 0; return g_got_close || g_got_eof ? 0 : -2; }
        if (r == -1) { if (!tx_time_left(deadline)) return -1; continue; }
        if (g_payload_len == 0) continue;
        uint8_t t = g_payload[0];
        if (t == MSG_IGNORE || t == MSG_DEBUG || t == MSG_UNIMPL) continue;
        if (t == MSG_DISCONNECT) { g_ch_open = 0; return g_got_eof ? 0 : -2; }
        if (handle_msg() != 0) return -2;
    }
}

int ssh_send(const uint8_t* data, uint32_t len)
{
    uint32_t off = 0; uint64_t stall = timer_millis();
    while (off < len)
    {
        if (!g_ch_open) { g_err = "channel is closed"; return -1; }
        if (g_remote_window == 0)
        {
            int r = recv_packet(100);
            if (r == -2) return -1;
            if (r == 0 && g_payload_len) { uint8_t t = g_payload[0]; if (t != MSG_IGNORE && t != MSG_DEBUG && t != MSG_UNIMPL) { if (t == MSG_CH_DATA || t == MSG_CH_EXTDATA) { /* stash is single-slot: drop-safe only when empty */ if (g_pend_pos < g_pend_len) { g_err = "receive overrun while sending"; return -1; } } if (handle_msg() != 0) return -1; } }
            if (timer_millis() - stall > 30000) { g_err = "remote window stayed closed"; return -1; }
            continue;
        }
        stall = timer_millis();
        uint32_t n = len - off; if (n > g_remote_max - 16) n = g_remote_max - 16; if (n > g_remote_window) n = g_remote_window;
        static uint8_t b[33000]; W w = {b, 0, sizeof(b), false};
        w_u8(&w, MSG_CH_DATA); w_u32(&w, g_remote_ch); w_str(&w, data + off, n);
        if (send_packet(b, w.n) != 0) return -1;
        g_remote_window -= n; off += n;
    }
    return 0;
}

void ssh_send_eof()
{
    if (!g_ch_open) return;
    uint8_t b[8]; W w = {b, 0, sizeof(b), false}; w_u8(&w, MSG_CH_EOF); w_u32(&w, g_remote_ch); (void)send_packet(b, w.n);
}

void ssh_close()
{
    if (g_sk < 0) return;
    if (g_ch_open)
    {
        uint8_t b[8]; W w = {b, 0, sizeof(b), false}; w_u8(&w, MSG_CH_CLOSE); w_u32(&w, g_remote_ch); (void)send_packet(b, w.n);
        g_ch_open = 0;
    }
    {
        uint8_t b[32]; W w = {b, 0, sizeof(b), false}; w_u8(&w, MSG_DISCONNECT); w_u32(&w, 11); w_cstr(&w, "bye"); w_cstr(&w, ""); (void)send_packet(b, w.n);
    }
    net_tcp_close(0, g_sk); g_sk = -1;
}

int ssh_forget_host(const char* host, uint32_t port)
{
    char key[160]; known_key(host, port, key, sizeof(key));
    int r = known_store(key, "", 1);
    return r == 0 ? 0 : -1;
}
