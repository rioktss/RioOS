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

#include "tls.h"
#include "x509.h"
#include "rtc.h"
#include "netstack.h"
#include "sha256.h"
#include "uart.h"
#include "string.h"
#include "timer.h"

#ifdef HOST_TEST
#define TLS_CNT(v) ((v) = 12345)
#else
#define TLS_CNT(v) asm volatile("mrs %0, cntvct_el0" : "=r"(v))
#endif

/* ============================ X25519 (TweetNaCl-style, 16 x 16-bit limbs) ============== */
typedef long long i64;
typedef i64 gf[16];
static const gf gf121665 = {0xDB41, 1};

static void car25519(gf o)
{
    for (int i = 0; i < 16; ++i)
    {
        o[i] += (1LL << 16);
        i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}
static void sel25519(gf p, gf q, int b)
{
    i64 c = ~((i64)b - 1);
    for (int i = 0; i < 16; ++i) { i64 t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack25519(uint8_t* o, const gf n)
{
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; ++j)
    {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
static void unpack25519(gf o, const uint8_t* n)
{
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void fadd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
static void fsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }
static void fmul(gf o, const gf a, const gf b)
{
    i64 t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i) for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o); car25519(o);
}
static void fsq(gf o, const gf a) { fmul(o, a, a); }
static void inv25519(gf o, const gf i)
{
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) { fsq(c, c); if (a != 2 && a != 4) fmul(c, c, i); }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

void tls_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t z[32];
    i64 x[80];
    gf a, b, c, d, e, f;
    for (int i = 0; i < 32; ++i) z[i] = scalar[i];
    z[31] = (uint8_t)((scalar[31] & 127) | 64);
    z[0] &= 248;
    unpack25519(x, point);
    for (int i = 0; i < 16; ++i) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i)
    {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        fadd(e, a, c); fsub(a, a, c); fadd(c, b, d); fsub(b, b, d);
        fsq(d, e); fsq(f, a); fmul(a, c, a); fmul(c, b, e);
        fadd(e, a, c); fsub(a, a, c); fsq(b, a); fsub(c, d, f);
        fmul(a, c, gf121665); fadd(a, a, d); fmul(c, c, a);
        fmul(a, d, f); fmul(d, b, x); fsq(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    for (int i = 0; i < 16; ++i) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    inv25519(x + 32, x + 32);
    fmul(x + 16, x + 16, x + 32);
    pack25519(out, x + 16);
}

void tls_x25519_base(uint8_t out[32], const uint8_t scalar[32])
{
    uint8_t nine[32]; for (int i = 0; i < 32; ++i) nine[i] = 0; nine[0] = 9;
    tls_x25519(out, scalar, nine);
}

/* ============================ AES-128 + GCM ========================================= */
static uint8_t sbox[256];
static int sbox_ready = 0;
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static void sbox_build()
{
    uint8_t p = 1, q = 1;
    do
    {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
        sbox[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    sbox_ready = 1;
}

struct Aes { uint8_t rk[176]; };
static void aes_key(Aes* a, const uint8_t key[16])
{
    if (!sbox_ready) sbox_build();
    for (int i = 0; i < 16; ++i) a->rk[i] = key[i];
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4)
    {
        uint8_t t[4] = { a->rk[i - 4], a->rk[i - 3], a->rk[i - 2], a->rk[i - 1] };
        if (i % 16 == 0)
        {
            uint8_t k = t[0]; t[0] = (uint8_t)(sbox[t[1]] ^ rcon); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[k];
            rcon = xt(rcon);
        }
        for (int j = 0; j < 4; ++j) a->rk[i + j] = (uint8_t)(a->rk[i - 16 + j] ^ t[j]);
    }
}
static void aes_block(const Aes* a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = (uint8_t)(in[i] ^ a->rk[i]);
    for (int r = 1; r <= 10; ++r)
    {
        uint8_t t[16];
        for (int i = 0; i < 16; ++i) t[i] = sbox[s[i]];
        uint8_t u[16];                                   /* ShiftRows (column-major state) */
        for (int c = 0; c < 4; ++c) for (int rr = 0; rr < 4; ++rr) u[c * 4 + rr] = t[((c + rr) & 3) * 4 + rr];
        if (r < 10)
        {
            for (int c = 0; c < 4; ++c)
            {
                uint8_t* col = u + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (uint8_t)(xt(a0) ^ (xt(a1) ^ a1) ^ a2 ^ a3);
                col[1] = (uint8_t)(a0 ^ xt(a1) ^ (xt(a2) ^ a2) ^ a3);
                col[2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ (xt(a3) ^ a3));
                col[3] = (uint8_t)((xt(a0) ^ a0) ^ a1 ^ a2 ^ xt(a3));
            }
        }
        for (int i = 0; i < 16; ++i) s[i] = (uint8_t)(u[i] ^ a->rk[r * 16 + i]);
    }
    for (int i = 0; i < 16; ++i) out[i] = s[i];
}

struct Gh { uint64_t hi, lo; };
static Gh gh_load(const uint8_t* p)
{
    Gh g = {0, 0};
    for (int i = 0; i < 8; ++i) { g.hi = (g.hi << 8) | p[i]; g.lo = (g.lo << 8) | p[8 + i]; }
    return g;
}
static Gh gh_mul(Gh x, Gh h)
{
    Gh z = {0, 0}, v = h;
    for (int i = 0; i < 128; ++i)
    {
        uint64_t bit = i < 64 ? (x.hi >> (63 - i)) & 1U : (x.lo >> (127 - i)) & 1U;
        if (bit) { z.hi ^= v.hi; z.lo ^= v.lo; }
        uint64_t lsb = v.lo & 1U;
        v.lo = (v.lo >> 1) | (v.hi << 63); v.hi >>= 1;
        if (lsb) v.hi ^= 0xE100000000000000ULL;
    }
    return z;
}
static void ghash_blocks(Gh* y, Gh h, const uint8_t* d, uint32_t len)
{
    for (uint32_t o = 0; o < len; o += 16)
    {
        uint8_t blk[16];
        for (int i = 0; i < 16; ++i) blk[i] = (o + (uint32_t)i < len) ? d[o + (uint32_t)i] : 0;
        Gh b = gh_load(blk);
        y->hi ^= b.hi; y->lo ^= b.lo;
        *y = gh_mul(*y, h);
    }
}
static void gcm_core(const uint8_t key[16], const uint8_t iv[12], const uint8_t* aad, uint32_t aad_len,
                     const uint8_t* in, uint32_t len, uint8_t* out, uint8_t tag[16], int encrypt)
{
    Aes a; aes_key(&a, key);
    uint8_t zero[16] = {0}, hb[16];
    aes_block(&a, zero, hb);
    Gh h = gh_load(hb), y = {0, 0};
    uint8_t j0[16];
    for (int i = 0; i < 12; ++i) j0[i] = iv[i];
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    ghash_blocks(&y, h, aad, aad_len);
    if (!encrypt) ghash_blocks(&y, h, in, len);
    uint8_t ctr[16], ks[16];
    for (int i = 0; i < 16; ++i) ctr[i] = j0[i];
    for (uint32_t o = 0; o < len; o += 16)
    {
        for (int i = 15; i >= 12; --i) if (++ctr[i]) break;
        aes_block(&a, ctr, ks);
        for (uint32_t i = 0; i < 16 && o + i < len; ++i) out[o + i] = (uint8_t)(in[o + i] ^ ks[i]);
    }
    if (encrypt) ghash_blocks(&y, h, out, len);
    uint8_t lens[16];
    uint64_t abits = (uint64_t)aad_len * 8U, cbits = (uint64_t)len * 8U;
    for (int i = 0; i < 8; ++i) { lens[i] = (uint8_t)(abits >> (56 - 8 * i)); lens[8 + i] = (uint8_t)(cbits >> (56 - 8 * i)); }
    Gh lb = gh_load(lens);
    y.hi ^= lb.hi; y.lo ^= lb.lo; y = gh_mul(y, h);
    uint8_t ej[16]; aes_block(&a, j0, ej);
    for (int i = 0; i < 8; ++i) { tag[i] = (uint8_t)((y.hi >> (56 - 8 * i)) ^ ej[i]); tag[8 + i] = (uint8_t)((y.lo >> (56 - 8 * i)) ^ ej[8 + i]); }
}
void tls_aes128_gcm_encrypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t* aad, uint32_t aad_len,
                            const uint8_t* pt, uint32_t len, uint8_t* ct, uint8_t tag[16])
{ gcm_core(key, iv, aad, aad_len, pt, len, ct, tag, 1); }
int tls_aes128_gcm_decrypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t* aad, uint32_t aad_len,
                           const uint8_t* ct, uint32_t len, const uint8_t tag[16], uint8_t* pt)
{
    uint8_t t[16];
    gcm_core(key, iv, aad, aad_len, ct, len, pt, t, 0);
    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= (uint8_t)(t[i] ^ tag[i]);
    return diff ? -1 : 0;
}

/* ============================ HKDF =================================================== */
void tls_hkdf_extract(const uint8_t* salt, uint32_t salt_len, const uint8_t* ikm, uint32_t ikm_len, uint8_t out[32])
{
    uint8_t zeros[32] = {0};
    if (salt_len == 0) { salt = zeros; salt_len = 32; }
    hmac_sha256(salt, salt_len, ikm, ikm_len, out);
}
void tls_hkdf_expand_label(const uint8_t secret[32], const char* label, const uint8_t* ctx, uint32_t ctx_len, uint8_t* out, uint32_t out_len)
{
    uint8_t info[128]; uint32_t n = 0;
    uint32_t ll = 0; while (label[ll]) ++ll;
    info[n++] = (uint8_t)(out_len >> 8); info[n++] = (uint8_t)out_len;
    info[n++] = (uint8_t)(6 + ll);
    const char* pre = "tls13 ";
    for (int i = 0; i < 6; ++i) info[n++] = (uint8_t)pre[i];
    for (uint32_t i = 0; i < ll; ++i) info[n++] = (uint8_t)label[i];
    info[n++] = (uint8_t)ctx_len;
    for (uint32_t i = 0; i < ctx_len; ++i) info[n++] = ctx[i];
    info[n++] = 1;
    uint8_t t[32];
    hmac_sha256(secret, 32, info, n, t);       /* single block: out_len <= 32 */
    for (uint32_t i = 0; i < out_len && i < 32; ++i) out[i] = t[i];
}

/* ============================ TLS 1.3 client ========================================= */
struct TlsSess
{
    int sock, open;
    uint8_t ck[16], civ[12], sk[16], siv[12];
    uint64_t cseq, sseq;
    uint8_t rbuf[16640]; uint32_t rpos, rlen;
};
static TlsSess S;
static int g_verify = 1;
static const char* g_err = "";
void tls_set_verify(int on) { g_verify = on; }
int tls_verify_enabled() { return g_verify; }
const char* tls_last_error() { return g_err; }
static uint8_t recbuf[16640 + 64];
static uint8_t plain[16640 + 64];
static uint8_t hsbuf[20000];

#ifdef HOST_TEST
static void random_bytes(uint8_t* out, uint32_t n)
{
    static uint64_t counter = 0;
    uint8_t seed[64];
    for (uint32_t i = 0; i < n; i += 32)
    {
        uint64_t v, a;
        TLS_CNT(v);
        a = (uint64_t)&seed;
        for (int k = 0; k < 8; ++k) { seed[k] = (uint8_t)(v >> (8 * k)); seed[8 + k] = (uint8_t)(a >> (8 * k)); seed[16 + k] = (uint8_t)(timer_millis() >> (8 * k)); seed[24 + k] = (uint8_t)(++counter >> (8 * k)); }
        for (int k = 32; k < 64; ++k) { uint64_t w; TLS_CNT(w); seed[k] = (uint8_t)(w ^ (w >> 7)); }
        uint8_t h[32]; sha256(seed, sizeof(seed), h);
        for (uint32_t k = 0; k < 32 && i + k < n; ++k) out[i + k] = h[k];
    }
}
#else
#include "random.h"
static void random_bytes(uint8_t* out, uint32_t n) { random_get(out, n); }
#endif
void tls_random(uint8_t* out, uint32_t n) { random_bytes(out, n); }
void tls_aes128_expand(uint8_t rk[176], const uint8_t key[16]) { aes_key((Aes*)rk, key); }
void tls_aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) { aes_block((const Aes*)rk, in, out); }

static int read_exact(uint8_t* buf, uint32_t n)
{
    uint32_t got = 0; int tries = 0;
    while (got < n)
    {
        int g = net_tcp_recv(0, S.sock, buf + got, n - got, 1000U);
        if (g == 0) return -1;
        if (g < 0) { if (++tries > 8) return -110; continue; }
        got += (uint32_t)g; tries = 0;
    }
    return 0;
}
static int send_all(const uint8_t* d, uint32_t n)
{
    uint32_t off = 0;
    while (off < n)
    {
        uint32_t c = n - off > 1000U ? 1000U : n - off;
        int r = net_tcp_send(0, S.sock, d + off, c, 4000U);
        if (r <= 0) return -1;
        off += (uint32_t)r;
    }
    return 0;
}

static void make_nonce(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12])
{
    for (int i = 0; i < 12; ++i) nonce[i] = iv[i];
    for (int i = 0; i < 8; ++i) nonce[11 - i] ^= (uint8_t)(seq >> (8 * i));
}

static int send_record(uint8_t type, const uint8_t* data, uint32_t len, int encrypted)
{
    static uint8_t out[16640 + 64];
    if (!encrypted)
    {
        out[0] = type; out[1] = 3; out[2] = 3; out[3] = (uint8_t)(len >> 8); out[4] = (uint8_t)len;
        for (uint32_t i = 0; i < len; ++i) out[5 + i] = data[i];
        return send_all(out, 5 + len);
    }
    uint32_t inner = len + 1U;
    uint32_t total = inner + 16U;
    out[0] = 23; out[1] = 3; out[2] = 3; out[3] = (uint8_t)(total >> 8); out[4] = (uint8_t)total;
    for (uint32_t i = 0; i < len; ++i) plain[i] = data[i];
    plain[len] = type;
    uint8_t nonce[12]; make_nonce(S.civ, S.cseq++, nonce);
    tls_aes128_gcm_encrypt(S.ck, nonce, out, 5, plain, inner, out + 5, out + 5 + inner);
    return send_all(out, 5 + total);
}

/* read one record; decrypt if keys active (outer type 23). Returns inner type (>0) or negative */
static int read_record(int use_keys, uint32_t* out_len)
{
    for (;;)
    {
        uint8_t hdr[5];
        int r = read_exact(hdr, 5);
        if (r != 0) return r;
        uint32_t len = ((uint32_t)hdr[3] << 8) | hdr[4];
        if (len > 16640U) return -2;
        r = read_exact(recbuf, len);
        if (r != 0) return r;
        if (hdr[0] == 20) continue;                       /* ChangeCipherSpec (compat) */
        if (hdr[0] == 21 && !use_keys) { *out_len = len; for (uint32_t i = 0; i < len; ++i) plain[i] = recbuf[i]; return 21; }
        if (hdr[0] == 22 && !use_keys) { *out_len = len; for (uint32_t i = 0; i < len; ++i) plain[i] = recbuf[i]; return 22; }
        if (hdr[0] != 23 || !use_keys || len < 17U) return -3;
        uint32_t clen = len - 16U;
        uint8_t nonce[12]; make_nonce(S.siv, S.sseq++, nonce);
        if (tls_aes128_gcm_decrypt(S.sk, nonce, hdr, 5, recbuf, clen, recbuf + clen, plain) != 0) return -4;
        uint32_t n = clen;
        while (n > 0 && plain[n - 1] == 0) --n;
        if (n == 0) return -5;
        *out_len = n - 1U;
        return plain[n - 1];
    }
}

static void be16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

int tls_active() { return S.open; }

int tls_connect(int tcp_socket, const char* hostname)
{
    S.sock = tcp_socket; S.open = 0; S.cseq = S.sseq = 0; S.rpos = S.rlen = 0;
    g_err = "";
    X509Key leaf_key; int have_cert = 0, have_cv = 0;
    if (g_verify)
    {
        int tc = tls_trust_reload();
        if (tc <= 0) { g_err = "no trusted CA certificates (/etc/ssl/certs.pem missing; use 'certs add' or -k)"; return -40; }
    }
    uint8_t priv[32], pub[32], rnd[32], sid[32];
    random_bytes(priv, 32); random_bytes(rnd, 32); random_bytes(sid, 32);
    tls_x25519_base(pub, priv);
    uint32_t hl = 0; while (hostname[hl] && hl < 200) ++hl;

    /* ---- ClientHello ---- */
    uint8_t* h = hsbuf; uint32_t n = 0;
    h[n++] = 1; n += 3;                                    /* type + length placeholder */
    h[n++] = 3; h[n++] = 3;
    for (int i = 0; i < 32; ++i) h[n++] = rnd[i];
    h[n++] = 32; for (int i = 0; i < 32; ++i) h[n++] = sid[i];
    be16(h + n, 2); n += 2; h[n++] = 0x13; h[n++] = 0x01;
    h[n++] = 1; h[n++] = 0;
    uint32_t ext_len_pos = n; n += 2;
    uint32_t ext_start = n;
    be16(h + n, 0); n += 2; be16(h + n, hl + 5); n += 2; be16(h + n, hl + 3); n += 2; h[n++] = 0; be16(h + n, hl); n += 2;
    for (uint32_t i = 0; i < hl; ++i) h[n++] = (uint8_t)hostname[i];
    be16(h + n, 10); n += 2; be16(h + n, 4); n += 2; be16(h + n, 2); n += 2; be16(h + n, 0x001d); n += 2;
    be16(h + n, 13); n += 2; be16(h + n, 16); n += 2; be16(h + n, 14); n += 2;
    { static const uint16_t sa[7] = {0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0807, 0x0401}; for (int i = 0; i < 7; ++i) { be16(h + n, sa[i]); n += 2; } }
    be16(h + n, 43); n += 2; be16(h + n, 3); n += 2; h[n++] = 2; h[n++] = 3; h[n++] = 4;
    be16(h + n, 51); n += 2; be16(h + n, 38); n += 2; be16(h + n, 36); n += 2; be16(h + n, 0x001d); n += 2; be16(h + n, 32); n += 2;
    for (int i = 0; i < 32; ++i) h[n++] = pub[i];
    be16(h + ext_len_pos, n - ext_start);
    uint32_t body = n - 4U; h[1] = (uint8_t)(body >> 16); h[2] = (uint8_t)(body >> 8); h[3] = (uint8_t)body;

    Sha256Ctx th; sha256_init(&th);
    sha256_update(&th, h, n);
    if (send_record(22, h, n, 0) != 0) return -10;

    /* ---- ServerHello ---- */
    uint32_t len = 0;
    int t = read_record(0, &len);
    if (t == 21) return -11;
    if (t != 22) return t < 0 ? t : -12;
    if (len < 42 || plain[0] != 2) return -13;
    uint32_t sh_len = 4U + (((uint32_t)plain[1] << 16) | ((uint32_t)plain[2] << 8) | plain[3]);
    if (sh_len > len) return -14;
    {
        static const uint8_t hrr[32] = {0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C};
        int same = 1; for (int i = 0; i < 32; ++i) if (plain[6 + i] != hrr[i]) same = 0;
        if (same) return -15;                              /* HelloRetryRequest unsupported */
    }
    sha256_update(&th, plain, sh_len);
    uint32_t p = 4 + 2 + 32; uint32_t sl = plain[p]; p += 1 + sl;
    if (p + 3 > sh_len) return -16;
    if (plain[p] != 0x13 || plain[p + 1] != 0x01) return -17;   /* only TLS_AES_128_GCM_SHA256 */
    p += 3;
    uint32_t el = ((uint32_t)plain[p] << 8) | plain[p + 1]; p += 2;
    uint32_t end = p + el;
    const uint8_t* server_pub = 0;
    while (p + 4 <= end && end <= sh_len)
    {
        uint32_t et = ((uint32_t)plain[p] << 8) | plain[p + 1], ell = ((uint32_t)plain[p + 2] << 8) | plain[p + 3];
        if (et == 51 && ell >= 36 && plain[p + 4] == 0x00 && plain[p + 5] == 0x1d) server_pub = plain + p + 8;
        p += 4 + ell;
    }
    if (!server_pub) return -18;
    uint8_t shared[32]; tls_x25519(shared, priv, server_pub);

    /* ---- key schedule ---- */
    uint8_t zeros[32] = {0}, early[32], derived[32], hs[32], empty_hash[32], thash[32];
    sha256("", 0, empty_hash);
    tls_hkdf_extract(zeros, 0, zeros, 32, early);
    tls_hkdf_expand_label(early, "derived", empty_hash, 32, derived, 32);
    tls_hkdf_extract(derived, 32, shared, 32, hs);
    { Sha256Ctx c = th; sha256_final(&c, thash); }
    uint8_t c_hs[32], s_hs[32];
    tls_hkdf_expand_label(hs, "c hs traffic", thash, 32, c_hs, 32);
    tls_hkdf_expand_label(hs, "s hs traffic", thash, 32, s_hs, 32);
    tls_hkdf_expand_label(s_hs, "key", 0, 0, S.sk, 16); tls_hkdf_expand_label(s_hs, "iv", 0, 0, S.siv, 12);
    tls_hkdf_expand_label(c_hs, "key", 0, 0, S.ck, 16); tls_hkdf_expand_label(c_hs, "iv", 0, 0, S.civ, 12);
    S.cseq = S.sseq = 0;

    /* ---- server handshake flight (EncryptedExtensions ... Finished) ---- */
    uint32_t have = 0; int got_finished = 0;
    uint8_t server_fin_hash[32];
    while (!got_finished)
    {
        t = read_record(1, &len);
        if (t < 0) return t - 100;
        if (t == 21) return -19;
        if (t != 22) return -20;
        if (have + len > sizeof(hsbuf)) return -21;
        for (uint32_t i = 0; i < len; ++i) hsbuf[have + i] = plain[i];
        have += len;
        uint32_t off = 0;
        while (have - off >= 4)
        {
            uint32_t ml = ((uint32_t)hsbuf[off + 1] << 16) | ((uint32_t)hsbuf[off + 2] << 8) | hsbuf[off + 3];
            if (have - off < 4 + ml) break;
            uint8_t mt = hsbuf[off];
            if (mt == 11 && g_verify)
            {
                /* Certificate: ctx_len(1) ctx list_len(3) { cert_len(3) cert ext_len(2) ext }* */
                const uint8_t* b = hsbuf + off + 4; uint32_t bl = ml;
                if (bl < 4 || b[0] + 4U > bl) { g_err = "malformed Certificate message"; return -41; }
                uint32_t q = 1U + b[0];
                uint32_t ll = ((uint32_t)b[q] << 16) | ((uint32_t)b[q + 1] << 8) | b[q + 2]; q += 3;
                if (q + ll > bl) { g_err = "malformed Certificate message"; return -41; }
                const uint8_t* cp[8]; uint32_t cl[8]; int cn = 0; uint32_t end2 = q + ll;
                while (q + 3 <= end2 && cn < 8)
                {
                    uint32_t l1 = ((uint32_t)b[q] << 16) | ((uint32_t)b[q + 1] << 8) | b[q + 2]; q += 3;
                    if (q + l1 + 2 > end2) { g_err = "malformed Certificate message"; return -41; }
                    cp[cn] = b + q; cl[cn] = l1; ++cn; q += l1;
                    uint32_t e1 = ((uint32_t)b[q] << 8) | b[q + 1]; q += 2 + e1;
                }
                if (cn == 0) { g_err = "server sent no certificate"; return -42; }
                int vr = x509_verify_chain(cp, cl, cn, hostname, rtc_get_epoch(), &leaf_key, &g_err);
                if (vr != 0) return -43;
                have_cert = 1;
            }
            else if (mt == 15 && g_verify)
            {
                if (!have_cert || ml < 4) { g_err = "CertificateVerify without a verified certificate"; return -44; }
                uint16_t scheme = (uint16_t)(((uint16_t)hsbuf[off + 4] << 8) | hsbuf[off + 5]);
                uint32_t sl2 = ((uint32_t)hsbuf[off + 6] << 8) | hsbuf[off + 7];
                if (sl2 + 4 > ml) { g_err = "malformed CertificateVerify"; return -44; }
                uint8_t content[64 + 34 + 32];
                for (int i = 0; i < 64; ++i) content[i] = 0x20;
                static const char lbl[] = "TLS 1.3, server CertificateVerify";
                for (int i = 0; i < 33; ++i) content[64 + i] = (uint8_t)lbl[i];
                content[97] = 0;
                { Sha256Ctx c = th; sha256_final(&c, content + 98); }
                if (x509_verify_signature(&leaf_key, scheme, content, 130, hsbuf + off + 8, sl2) != 0) { g_err = "server handshake signature is invalid"; return -45; }
                have_cv = 1;
            }
            if (mt == 20)
            {
                if (g_verify && !have_cv) { g_err = "server did not authenticate itself (no certificate/signature)"; return -46; }
                { Sha256Ctx c = th; sha256_final(&c, thash); }
                uint8_t fk[32], mac[32];
                tls_hkdf_expand_label(s_hs, "finished", 0, 0, fk, 32);
                hmac_sha256(fk, 32, thash, 32, mac);
                if (ml != 32) return -22;
                uint8_t diff = 0; for (int i = 0; i < 32; ++i) diff |= (uint8_t)(mac[i] ^ hsbuf[off + 4 + i]);
                if (diff) return -23;
                sha256_update(&th, hsbuf + off, 4 + ml);
                { Sha256Ctx c = th; sha256_final(&c, server_fin_hash); }
                got_finished = 1;
                off += 4 + ml;
                break;
            }
            sha256_update(&th, hsbuf + off, 4 + ml);
            off += 4 + ml;
        }
        if (off < have) { for (uint32_t i = off; i < have; ++i) hsbuf[i - off] = hsbuf[i]; have -= off; } else have = 0;
    }

    /* ---- client Finished ---- */
    uint8_t cfk[32], cmac[32];
    tls_hkdf_expand_label(c_hs, "finished", 0, 0, cfk, 32);
    hmac_sha256(cfk, 32, server_fin_hash, 32, cmac);
    uint8_t fin[36]; fin[0] = 20; fin[1] = 0; fin[2] = 0; fin[3] = 32;
    for (int i = 0; i < 32; ++i) fin[4 + i] = cmac[i];
    { uint8_t ccs[6] = {20, 3, 3, 0, 1, 1}; if (send_all(ccs, 6) != 0) return -24; }
    if (send_record(22, fin, 36, 1) != 0) return -25;

    /* ---- application keys ---- */
    uint8_t derived2[32], master[32], c_ap[32], s_ap[32];
    tls_hkdf_expand_label(hs, "derived", empty_hash, 32, derived2, 32);
    tls_hkdf_extract(derived2, 32, zeros, 32, master);
    tls_hkdf_expand_label(master, "c ap traffic", server_fin_hash, 32, c_ap, 32);
    tls_hkdf_expand_label(master, "s ap traffic", server_fin_hash, 32, s_ap, 32);
    tls_hkdf_expand_label(c_ap, "key", 0, 0, S.ck, 16); tls_hkdf_expand_label(c_ap, "iv", 0, 0, S.civ, 12);
    tls_hkdf_expand_label(s_ap, "key", 0, 0, S.sk, 16); tls_hkdf_expand_label(s_ap, "iv", 0, 0, S.siv, 12);
    S.cseq = S.sseq = 0;
    S.open = 1;
    return 0;
}

int tls_send(const void* data, uint32_t len)
{
    if (!S.open) return -1;
    const uint8_t* d = (const uint8_t*)data; uint32_t off = 0;
    while (off < len)
    {
        uint32_t c = len - off > 8192U ? 8192U : len - off;
        if (send_record(23, d + off, c, 1) != 0) return -2;
        off += c;
    }
    return (int)len;
}

int tls_recv(void* buf, uint32_t cap, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!S.open) return 0;
    while (S.rpos >= S.rlen)
    {
        uint32_t len = 0;
        int t = read_record(1, &len);
        if (t == -110) return -110;
        if (t < 0) { S.open = 0; return t == -1 ? 0 : t; }
        if (t == 23) { for (uint32_t i = 0; i < len; ++i) S.rbuf[i] = plain[i]; S.rpos = 0; S.rlen = len; }
        else if (t == 21) { S.open = 0; return 0; }          /* alert (close_notify or fatal) */
        /* t == 22: NewSessionTicket / KeyUpdate: ignored */
    }
    uint32_t n = S.rlen - S.rpos; if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; ++i) ((uint8_t*)buf)[i] = S.rbuf[S.rpos + i];
    S.rpos += n;
    return (int)n;
}

void tls_close()
{
    if (S.open) { uint8_t a[2] = {1, 0}; (void)send_record(21, a, 2, 1); }
    S.open = 0;
}
