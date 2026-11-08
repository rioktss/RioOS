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

#include "crypto.h"

/* ================================ SHA-512 / SHA-384 ================================ */
static const uint64_t K512[80] = {
0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};

static inline uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512_block(Sha512Ctx* c, const uint8_t* p)
{
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) { uint64_t v = 0; for (int k = 0; k < 8; ++k) v = (v << 8) | p[i * 8 + k]; w[i] = v; }
    for (int i = 16; i < 80; ++i)
    {
        uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 80; ++i)
    {
        uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
        uint64_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha512_init(Sha512Ctx* c)
{
    static const uint64_t iv[8] = {0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    for (int i = 0; i < 8; ++i) c->h[i] = iv[i];
    c->total = 0; c->fill = 0;
}
void sha384_init(Sha512Ctx* c)
{
    static const uint64_t iv[8] = {0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL};
    for (int i = 0; i < 8; ++i) c->h[i] = iv[i];
    c->total = 0; c->fill = 0;
}
void sha512_update(Sha512Ctx* c, const void* data, uint64_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    c->total += len;
    while (len)
    {
        uint32_t take = 128 - c->fill; if (take > len) take = (uint32_t)len;
        for (uint32_t i = 0; i < take; ++i) c->buf[c->fill + i] = p[i];
        c->fill += take; p += take; len -= take;
        if (c->fill == 128) { sha512_block(c, c->buf); c->fill = 0; }
    }
}
void sha512_final(Sha512Ctx* c, uint8_t* out, int out_len)
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80; sha512_update(c, &pad, 1);
    uint8_t z = 0; while (c->fill != 112) sha512_update(c, &z, 1);
    uint8_t lenb[16] = {0}; for (int i = 0; i < 8; ++i) lenb[8 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha512_update(c, lenb, 16);
    for (int i = 0; i < out_len; ++i) out[i] = (uint8_t)(c->h[i / 8] >> (56 - 8 * (i % 8)));
}
void sha512(const void* d, uint64_t n, uint8_t o[64]) { Sha512Ctx c; sha512_init(&c); sha512_update(&c, d, n); sha512_final(&c, o, 64); }
void sha384(const void* d, uint64_t n, uint8_t o[48]) { Sha512Ctx c; sha384_init(&c); sha512_update(&c, d, n); sha512_final(&c, o, 48); }

void hmac_sha512(const uint8_t* key, uint32_t key_len, const void* msg, uint64_t msg_len, uint8_t out[64])
{
    uint8_t k[128] = {0}, ip[128], op[128], inner[64];
    if (key_len > 128) sha512(key, key_len, k); else for (uint32_t i = 0; i < key_len; ++i) k[i] = key[i];
    for (int i = 0; i < 128; ++i) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    Sha512Ctx c; sha512_init(&c); sha512_update(&c, ip, 128); sha512_update(&c, msg, msg_len); sha512_final(&c, inner, 64);
    sha512_init(&c); sha512_update(&c, op, 128); sha512_update(&c, inner, 64); sha512_final(&c, out, 64);
}

#include "sha256.h"
int hash_len(int alg) { return alg == HASH_SHA256 ? 32 : alg == HASH_SHA384 ? 48 : alg == HASH_SHA512 ? 64 : 0; }
void hash_buf(int alg, const void* d, uint64_t n, uint8_t* out)
{
    if (alg == HASH_SHA256) sha256(d, n, out);
    else if (alg == HASH_SHA384) sha384(d, n, out);
    else sha512(d, n, out);
}

/* ================================ Montgomery big numbers ================================ */
#define MAXL 130          /* limbs: enough for a 4096-bit modulus */
struct Mont { uint32_t n[MAXL]; uint32_t rr[MAXL]; uint32_t one[MAXL]; uint32_t n0; int l; };

static int bn_cmp(const uint32_t* a, const uint32_t* b, int l)
{
    for (int i = l - 1; i >= 0; --i) { if (a[i] > b[i]) return 1; if (a[i] < b[i]) return -1; }
    return 0;
}
static uint32_t bn_sub(uint32_t* r, const uint32_t* a, const uint32_t* b, int l)
{
    uint64_t br = 0;
    for (int i = 0; i < l; ++i) { uint64_t d = (uint64_t)a[i] - b[i] - br; r[i] = (uint32_t)d; br = (d >> 32) & 1; }
    return (uint32_t)br;
}
static uint32_t bn_add(uint32_t* r, const uint32_t* a, const uint32_t* b, int l)
{
    uint64_t c = 0;
    for (int i = 0; i < l; ++i) { uint64_t s = (uint64_t)a[i] + b[i] + c; r[i] = (uint32_t)s; c = s >> 32; }
    return (uint32_t)c;
}
static int bn_zero(const uint32_t* a, int l) { for (int i = 0; i < l; ++i) if (a[i]) return 0; return 1; }
static void bn_copy(uint32_t* r, const uint32_t* a, int l) { for (int i = 0; i < l; ++i) r[i] = a[i]; }
static void bn_set(uint32_t* r, int l, uint32_t v) { for (int i = 0; i < l; ++i) r[i] = 0; r[0] = v; }

static void bn_from_be(uint32_t* a, int l, const uint8_t* p, uint32_t len)
{
    for (int i = 0; i < l; ++i) a[i] = 0;
    for (uint32_t i = 0; i < len; ++i)
    {
        uint32_t pos = len - 1 - i;                      /* byte significance */
        if ((int)(pos / 4) < l) a[pos / 4] |= (uint32_t)p[i] << (8 * (pos % 4));
    }
}
static void bn_to_be(const uint32_t* a, int l, uint8_t* out, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i)
    {
        uint32_t pos = len - 1 - i;
        out[i] = (int)(pos / 4) < l ? (uint8_t)(a[pos / 4] >> (8 * (pos % 4))) : 0;
    }
}

static int mont_init(Mont* m, const uint32_t* n, int l)
{
    if (l < 1 || l >= MAXL || !(n[0] & 1)) return -1;
    m->l = l; bn_copy(m->n, n, l);
    uint32_t x = n[0];                                   /* Newton: x = n^-1 mod 2^32 */
    for (int i = 0; i < 5; ++i) x *= 2 - n[0] * x;
    m->n0 = (uint32_t)(0 - x);
    /* rr = 2^(64*l) mod n by repeated doubling of 1 */
    uint32_t t[MAXL]; bn_set(t, l, 1);
    for (int i = 0; i < 64 * l; ++i)
    {
        uint32_t carry = 0;
        for (int k = 0; k < l; ++k) { uint32_t nc = t[k] >> 31; t[k] = (t[k] << 1) | carry; carry = nc; }
        if (carry || bn_cmp(t, n, l) >= 0) bn_sub(t, t, n, l);
        if (i == 32 * l - 1) bn_copy(m->one, t, l);          /* R mod n = Montgomery form of 1 */
    }
    bn_copy(m->rr, t, l);
    return 0;
}
static void mont_mul(const Mont* m, uint32_t* r, const uint32_t* a, const uint32_t* b)
{
    int l = m->l; uint32_t t[MAXL + 2];
    for (int i = 0; i < l + 2; ++i) t[i] = 0;
    for (int i = 0; i < l; ++i)
    {
        uint64_t c = 0;
        for (int j = 0; j < l; ++j) { uint64_t s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c; t[j] = (uint32_t)s; c = s >> 32; }
        uint64_t s = (uint64_t)t[l] + c; t[l] = (uint32_t)s; t[l + 1] = (uint32_t)(s >> 32);
        uint32_t mm = t[0] * m->n0;
        s = (uint64_t)t[0] + (uint64_t)mm * m->n[0]; c = s >> 32;
        for (int j = 1; j < l; ++j) { s = (uint64_t)t[j] + (uint64_t)mm * m->n[j] + c; t[j - 1] = (uint32_t)s; c = s >> 32; }
        s = (uint64_t)t[l] + c; t[l - 1] = (uint32_t)s; t[l] = t[l + 1] + (uint32_t)(s >> 32);
    }
    if (t[l] || bn_cmp(t, m->n, l) >= 0) bn_sub(t, t, m->n, l);
    for (int i = 0; i < l; ++i) r[i] = t[i];
}
static void mont_add(const Mont* m, uint32_t* r, const uint32_t* a, const uint32_t* b)
{
    uint32_t t[MAXL]; uint32_t c = bn_add(t, a, b, m->l);
    if (c || bn_cmp(t, m->n, m->l) >= 0) bn_sub(t, t, m->n, m->l);
    bn_copy(r, t, m->l);
}
static void mont_sub(const Mont* m, uint32_t* r, const uint32_t* a, const uint32_t* b)
{
    uint32_t t[MAXL]; uint32_t br = bn_sub(t, a, b, m->l);
    if (br) bn_add(t, t, m->n, m->l);
    bn_copy(r, t, m->l);
}
static void mont_to(const Mont* m, uint32_t* r, const uint32_t* a) { mont_mul(m, r, a, m->rr); }
static void mont_from(const Mont* m, uint32_t* r, const uint32_t* a) { uint32_t o[MAXL]; bn_set(o, m->l, 1); mont_mul(m, r, a, o); }
/* r = base^exp in Montgomery form; base in Montgomery form; exp little-endian limbs of el words */
static void mont_pow(const Mont* m, uint32_t* r, const uint32_t* base, const uint32_t* exp, int el)
{
    uint32_t acc[MAXL]; bn_copy(acc, m->one, m->l);
    for (int i = el * 32 - 1; i >= 0; --i)
    {
        mont_mul(m, acc, acc, acc);
        if ((exp[i / 32] >> (i % 32)) & 1) mont_mul(m, acc, acc, base);
    }
    bn_copy(r, acc, m->l);
}
/* exp = n - 2 (for inversion of prime modulus) */
static void mont_inv(const Mont* m, uint32_t* r, const uint32_t* a)
{
    uint32_t e[MAXL]; bn_copy(e, m->n, m->l);
    uint32_t two[MAXL]; bn_set(two, m->l, 2); bn_sub(e, e, two, m->l);
    mont_pow(m, r, a, e, m->l);
}

/* ================================ RSA ================================ */
static int rsa_public(const uint8_t* n, uint32_t nlen, const uint8_t* e, uint32_t elen, const uint8_t* sig, uint32_t slen, uint8_t* em)
{
    while (nlen && n[0] == 0) { ++n; --nlen; }
    while (elen && e[0] == 0) { ++e; --elen; }
    if (nlen < 64 || nlen > 512 || elen == 0 || elen > 8 || slen != nlen) return -1;
    int l = (int)((nlen + 3) / 4);
    uint32_t N[MAXL], S[MAXL], E[8], X[MAXL], R[MAXL];
    bn_from_be(N, l, n, nlen); bn_from_be(S, l, sig, slen); bn_from_be(E, 8, e, elen);
    if (bn_cmp(S, N, l) >= 0) return -1;
    static Mont m;
    if (mont_init(&m, N, l) != 0) return -1;
    mont_to(&m, X, S);
    mont_pow(&m, R, X, E, (int)((elen + 3) / 4));
    mont_from(&m, R, R);
    bn_to_be(R, l, em, nlen);
    return 0;
}
static const uint8_t DI256[19] = {0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
static const uint8_t DI384[19] = {0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30};
static const uint8_t DI512[19] = {0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40};

int rsa_verify_pkcs1(const uint8_t* n, uint32_t nlen, const uint8_t* e, uint32_t elen, int hash, const uint8_t* digest, const uint8_t* sig, uint32_t slen)
{
    static uint8_t em[512];
    while (nlen && n[0] == 0) { ++n; --nlen; }
    if (rsa_public(n, nlen, e, elen, sig, slen, em) != 0) return -1;
    int hl = hash_len(hash); if (!hl) return -1;
    const uint8_t* di = hash == HASH_SHA256 ? DI256 : hash == HASH_SHA384 ? DI384 : DI512;
    uint32_t tl = 19 + (uint32_t)hl;
    if (nlen < tl + 11) return -1;
    uint32_t i = 0;
    if (em[i++] != 0 || em[i++] != 1) return -1;
    uint32_t padend = nlen - tl - 1;
    for (; i < padend; ++i) if (em[i] != 0xff) return -1;
    if (em[i++] != 0) return -1;
    for (int k = 0; k < 19; ++k) if (em[i + k] != di[k]) return -1;
    i += 19;
    uint8_t diff = 0; for (int k = 0; k < hl; ++k) diff |= (uint8_t)(em[i + k] ^ digest[k]);
    return diff ? -1 : 0;
}

int rsa_verify_pss(const uint8_t* n, uint32_t nlen, const uint8_t* e, uint32_t elen, int hash, const uint8_t* digest, const uint8_t* sig, uint32_t slen)
{
    static uint8_t em[512];
    while (nlen && n[0] == 0) { ++n; --nlen; }
    if (rsa_public(n, nlen, e, elen, sig, slen, em) != 0) return -1;
    int hl = hash_len(hash); if (!hl) return -1;
    uint32_t modbits = nlen * 8; { uint8_t t = n[0]; int b = 8; while (b > 0 && !(t & 0x80)) { t <<= 1; --b; } modbits = (nlen - 1) * 8 + (uint32_t)b; }
    uint32_t embits = modbits - 1, emlen = (embits + 7) / 8;
    const uint8_t* E = em + (nlen - emlen);              /* leading byte dropped when modbits-1 is a multiple of 8 */
    if (emlen < (uint32_t)hl * 2 + 2) return -1;
    if (E[emlen - 1] != 0xbc) return -1;
    uint32_t dblen = emlen - (uint32_t)hl - 1;
    static uint8_t db[512];
    const uint8_t* H = E + dblen;
    /* MGF1 */
    for (uint32_t off = 0, ctr = 0; off < dblen; ++ctr)
    {
        uint8_t seed[64 + 4]; for (int k = 0; k < hl; ++k) seed[k] = H[k];
        seed[hl] = (uint8_t)(ctr >> 24); seed[hl + 1] = (uint8_t)(ctr >> 16); seed[hl + 2] = (uint8_t)(ctr >> 8); seed[hl + 3] = (uint8_t)ctr;
        uint8_t blk[64]; hash_buf(hash, seed, (uint64_t)hl + 4, blk);
        for (int k = 0; k < hl && off < dblen; ++k, ++off) db[off] = E[off] ^ blk[k];
    }
    uint32_t topbits = 8 * emlen - embits; if (topbits) db[0] &= (uint8_t)(0xff >> topbits);
    uint32_t i = 0; while (i < dblen - (uint32_t)hl - 1 && db[i] == 0) ++i;
    if (i != dblen - (uint32_t)hl - 1 || db[i] != 1) return -1;       /* salt length = hash length */
    const uint8_t* salt = db + i + 1;
    uint8_t mp[8 + 64 + 64]; for (int k = 0; k < 8; ++k) mp[k] = 0;
    for (int k = 0; k < hl; ++k) { mp[8 + k] = digest[k]; mp[8 + hl + k] = salt[k]; }
    uint8_t h2[64]; hash_buf(hash, mp, 8 + 2 * (uint64_t)hl, h2);
    uint8_t diff = 0; for (int k = 0; k < hl; ++k) diff |= (uint8_t)(h2[k] ^ H[k]);
    return diff ? -1 : 0;
}

/* ================================ ECDSA (P-256 / P-384) ================================ */
struct Curve { int l; const char* p; const char* n; const char* b; const char* gx; const char* gy; };
static const Curve CURVES[2] = {
 {8, "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
     "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b", "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296", "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"},
 {12, "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
      "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
      "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
      "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
      "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f"}};

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10; }
static void bn_from_hex(uint32_t* a, int l, const char* h)
{
    uint8_t b[64]; uint32_t n = 0; while (h[2 * n]) { b[n] = (uint8_t)(hexval(h[2 * n]) * 16 + hexval(h[2 * n + 1])); ++n; }
    bn_from_be(a, l, b, n);
}

struct Pt { uint32_t x[12], y[12], z[12]; };      /* Jacobian, Montgomery form; z==0 -> infinity */

struct Ec { Mont f; Mont o; int l; uint32_t b[12], gx[12], gy[12]; };
static Ec EC;

static void f_mul(uint32_t* r, const uint32_t* a, const uint32_t* b) { mont_mul(&EC.f, r, a, b); }
static void f_add(uint32_t* r, const uint32_t* a, const uint32_t* b) { mont_add(&EC.f, r, a, b); }
static void f_sub(uint32_t* r, const uint32_t* a, const uint32_t* b) { mont_sub(&EC.f, r, a, b); }

static void pt_dbl(Pt* r, const Pt* p)
{
    int l = EC.l;
    if (bn_zero(p->z, l) || bn_zero(p->y, l)) { for (int i = 0; i < l; ++i) { r->x[i] = r->y[i] = r->z[i] = 0; } return; }
    uint32_t delta[12], gamma[12], beta[12], alpha[12], t1[12], t2[12], x3[12], y3[12], z3[12];
    f_mul(delta, p->z, p->z); f_mul(gamma, p->y, p->y); f_mul(beta, p->x, gamma);
    f_sub(t1, p->x, delta); f_add(t2, p->x, delta); f_mul(alpha, t1, t2);
    f_add(t1, alpha, alpha); f_add(alpha, t1, alpha);                 /* 3*(x-d)(x+d) */
    f_mul(x3, alpha, alpha); f_add(t1, beta, beta); f_add(t1, t1, t1); f_add(t2, t1, t1);   /* t1=4beta t2=8beta */
    f_sub(x3, x3, t2);
    f_add(t2, p->y, p->z); f_mul(z3, t2, t2); f_sub(z3, z3, gamma); f_sub(z3, z3, delta);
    f_sub(t1, t1, x3); f_mul(y3, alpha, t1);
    f_mul(t2, gamma, gamma); f_add(t2, t2, t2); f_add(t2, t2, t2); f_add(t2, t2, t2);       /* 8 gamma^2 */
    f_sub(y3, y3, t2);
    bn_copy(r->x, x3, l); bn_copy(r->y, y3, l); bn_copy(r->z, z3, l);
}
static void pt_add(Pt* r, const Pt* p, const Pt* q)
{
    int l = EC.l;
    if (bn_zero(p->z, l)) { *r = *q; return; }
    if (bn_zero(q->z, l)) { *r = *p; return; }
    uint32_t z1z1[12], z2z2[12], u1[12], u2[12], s1[12], s2[12], h[12], i[12], j[12], rr[12], v[12], t[12], x3[12], y3[12], z3[12];
    f_mul(z1z1, p->z, p->z); f_mul(z2z2, q->z, q->z);
    f_mul(u1, p->x, z2z2); f_mul(u2, q->x, z1z1);
    f_mul(t, q->z, z2z2); f_mul(s1, p->y, t);
    f_mul(t, p->z, z1z1); f_mul(s2, q->y, t);
    f_sub(h, u2, u1);
    f_sub(rr, s2, s1);
    if (bn_zero(h, l)) { if (bn_zero(rr, l)) { pt_dbl(r, p); return; } for (int k = 0; k < l; ++k) r->x[k] = r->y[k] = r->z[k] = 0; return; }
    f_add(t, h, h); f_mul(i, t, t); f_mul(j, h, i);
    f_add(rr, rr, rr); f_mul(v, u1, i);
    f_mul(x3, rr, rr); f_sub(x3, x3, j); f_sub(x3, x3, v); f_sub(x3, x3, v);
    f_sub(t, v, x3); f_mul(y3, rr, t); f_mul(t, s1, j); f_add(t, t, t); f_sub(y3, y3, t);
    f_add(t, p->z, q->z); f_mul(z3, t, t); f_sub(z3, z3, z1z1); f_sub(z3, z3, z2z2); f_mul(z3, z3, h);
    bn_copy(r->x, x3, l); bn_copy(r->y, y3, l); bn_copy(r->z, z3, l);
}
static void pt_mul(Pt* r, const Pt* p, const uint32_t* k)
{
    Pt acc; for (int i = 0; i < EC.l; ++i) acc.x[i] = acc.y[i] = acc.z[i] = 0;
    for (int i = EC.l * 32 - 1; i >= 0; --i)
    {
        pt_dbl(&acc, &acc);
        if ((k[i / 32] >> (i % 32)) & 1) pt_add(&acc, &acc, p);
    }
    *r = acc;
}

static int der_int(const uint8_t* p, uint32_t len, uint32_t* pos, const uint8_t** v, uint32_t* vl)
{
    if (*pos + 2 > len || p[*pos] != 0x02) return -1;
    uint32_t l = p[*pos + 1]; if (l & 0x80) return -1;
    *pos += 2; if (*pos + l > len) return -1;
    *v = p + *pos; *vl = l; *pos += l;
    while (*vl > 0 && (*v)[0] == 0) { ++*v; --*vl; }
    return 0;
}

int ecdsa_verify(int curve, const uint8_t* pub, uint32_t publen, const uint8_t* digest, uint32_t dlen, const uint8_t* sig, uint32_t slen)
{
    if (curve < 1 || curve > 2) return -1;
    const Curve* c = &CURVES[curve - 1]; int l = c->l; uint32_t bytes = (uint32_t)l * 4;
    if (publen != 1 + 2 * bytes || pub[0] != 4) return -1;
    /* DER signature */
    if (slen < 8 || sig[0] != 0x30) return -1;
    uint32_t pos = 2; if (sig[1] & 0x80) { if (sig[1] != 0x81) return -1; pos = 3; }
    const uint8_t *rv, *sv; uint32_t rl, sl;
    if (der_int(sig, slen, &pos, &rv, &rl) || der_int(sig, slen, &pos, &sv, &sl)) return -1;
    if (rl == 0 || sl == 0 || rl > bytes || sl > bytes) return -1;
    uint32_t N[12], P[12], R[12], S[12], E[12], w[12], u1[12], u2[12];
    bn_from_hex(P, l, c->p); bn_from_hex(N, l, c->n);
    if (mont_init(&EC.f, P, l) != 0 || mont_init(&EC.o, N, l) != 0) return -1;
    EC.l = l;
    bn_from_be(R, l, rv, rl); bn_from_be(S, l, sv, sl);
    if (bn_zero(R, l) || bn_zero(S, l) || bn_cmp(R, N, l) >= 0 || bn_cmp(S, N, l) >= 0) return -1;
    /* e = leftmost bits of digest */
    uint32_t dl = dlen > bytes ? bytes : dlen;
    bn_from_be(E, l, digest, dl);
    if (bn_cmp(E, N, l) >= 0) bn_sub(E, E, N, l);
    /* w = s^-1 mod n ; u1 = e*w ; u2 = r*w */
    uint32_t sm[12], wm[12], em[12], rm[12];
    mont_to(&EC.o, sm, S); mont_inv(&EC.o, wm, sm);
    mont_to(&EC.o, em, E); mont_to(&EC.o, rm, R);
    mont_mul(&EC.o, em, em, wm); mont_mul(&EC.o, rm, rm, wm);
    mont_from(&EC.o, u1, em); mont_from(&EC.o, u2, rm); (void)w;
    /* points */
    Pt G, Q, a, b, sum;
    uint32_t t[12];
    bn_from_hex(t, l, c->gx); mont_to(&EC.f, G.x, t); bn_from_hex(t, l, c->gy); mont_to(&EC.f, G.y, t); bn_copy(G.z, EC.f.one, l);
    bn_from_be(t, l, pub + 1, bytes); if (bn_cmp(t, P, l) >= 0) return -1; mont_to(&EC.f, Q.x, t);
    bn_from_be(t, l, pub + 1 + bytes, bytes); if (bn_cmp(t, P, l) >= 0) return -1; mont_to(&EC.f, Q.y, t); bn_copy(Q.z, EC.f.one, l);
    /* on-curve check: y^2 = x^3 - 3x + b */
    {
        uint32_t y2[12], x3[12], bm[12], t2[12];
        f_mul(y2, Q.y, Q.y); f_mul(x3, Q.x, Q.x); f_mul(x3, x3, Q.x);
        f_add(t2, Q.x, Q.x); f_add(t2, t2, Q.x); f_sub(x3, x3, t2);
        bn_from_hex(t, l, c->b); mont_to(&EC.f, bm, t); f_add(x3, x3, bm);
        if (bn_cmp(y2, x3, l) != 0) return -1;
    }
    pt_mul(&a, &G, u1); pt_mul(&b, &Q, u2); pt_add(&sum, &a, &b);
    if (bn_zero(sum.z, l)) return -1;
    uint32_t zi[12], zi2[12], x[12];
    mont_inv(&EC.f, zi, sum.z); f_mul(zi2, zi, zi); f_mul(x, sum.x, zi2);
    mont_from(&EC.f, x, x);
    if (bn_cmp(x, N, l) >= 0) bn_sub(x, x, N, l);
    return bn_cmp(x, R, l) == 0 ? 0 : -1;
}

/* ================================ Ed25519 ================================ */
struct Ed { Mont f; uint32_t d2[8], d[8], sqrtm1[8]; };
static Ed ED;
static void e_mul(uint32_t* r, const uint32_t* a, const uint32_t* b) { mont_mul(&ED.f, r, a, b); }
static void e_add(uint32_t* r, const uint32_t* a, const uint32_t* b) { mont_add(&ED.f, r, a, b); }
static void e_sub(uint32_t* r, const uint32_t* a, const uint32_t* b) { mont_sub(&ED.f, r, a, b); }
struct EPt { uint32_t x[8], y[8], z[8], t[8]; };

static void ep_add(EPt* r, const EPt* p, const EPt* q)
{
    uint32_t A[8], B[8], C[8], D[8], E[8], F[8], G[8], H[8], t1[8], t2[8];
    e_sub(t1, p->y, p->x); e_sub(t2, q->y, q->x); e_mul(A, t1, t2);
    e_add(t1, p->y, p->x); e_add(t2, q->y, q->x); e_mul(B, t1, t2);
    e_mul(C, p->t, q->t); e_mul(C, C, ED.d2);
    e_mul(D, p->z, q->z); e_add(D, D, D);
    e_sub(E, B, A); e_sub(F, D, C); e_add(G, D, C); e_add(H, B, A);
    e_mul(r->x, E, F); e_mul(r->y, G, H); e_mul(r->t, E, H); e_mul(r->z, F, G);
}
static void ep_ident(EPt* r) { for (int i = 0; i < 8; ++i) r->x[i] = r->t[i] = 0; bn_copy(r->y, ED.f.one, 8); bn_copy(r->z, ED.f.one, 8); }
static void ep_mul(EPt* r, const EPt* p, const uint8_t* k_le, int nbytes)
{
    EPt acc; ep_ident(&acc);
    for (int i = nbytes * 8 - 1; i >= 0; --i)
    {
        ep_add(&acc, &acc, &acc);
        if ((k_le[i / 8] >> (i % 8)) & 1) ep_add(&acc, &acc, p);
    }
    *r = acc;
}
static int ed_init()
{
    uint32_t p[8]; for (int i = 0; i < 8; ++i) p[i] = 0xffffffffu; p[0] = 0xffffffedu; p[7] = 0x7fffffffu;
    if (mont_init(&ED.f, p, 8) != 0) return -1;
    uint32_t a[8], b[8], c[8], t[8];
    bn_set(t, 8, 121665); mont_to(&ED.f, a, t); bn_set(t, 8, 121666); mont_to(&ED.f, b, t);
    mont_inv(&ED.f, c, b); mont_mul(&ED.f, c, c, a);           /* 121665/121666 */
    uint32_t z[8]; for (int i = 0; i < 8; ++i) z[i] = 0;
    e_sub(ED.d, z, c); e_add(ED.d2, ED.d, ED.d);
    /* sqrt(-1) = 2^((p-1)/4) */
    uint32_t e[8]; bn_copy(e, p, 8); uint32_t one[8]; bn_set(one, 8, 1); bn_sub(e, e, one, 8);
    for (int s = 0; s < 2; ++s) { uint32_t cy = 0; for (int i = 7; i >= 0; --i) { uint32_t nc = e[i] & 1; e[i] = (e[i] >> 1) | (cy << 31); cy = nc; } }
    bn_set(t, 8, 2); mont_to(&ED.f, a, t); mont_pow(&ED.f, ED.sqrtm1, a, e, 8);
    return 0;
}
/* decompress: y (255 bits) + sign bit; returns -1 if not on curve */
static int ep_decode(EPt* r, const uint8_t enc[32])
{
    uint8_t yb[32]; for (int i = 0; i < 32; ++i) yb[31 - i] = enc[i];
    int sign = yb[0] >> 7; yb[0] &= 0x7f;
    uint32_t y[8], P[8], one[8], t[8]; bn_from_be(y, 8, yb, 32);
    for (int i = 0; i < 8; ++i) P[i] = ED.f.n[i];
    if (bn_cmp(y, P, 8) >= 0) return -1;
    uint32_t ym[8], y2[8], u[8], v[8], x[8], x2[8], vx2[8], vi[8];
    mont_to(&ED.f, ym, y); e_mul(y2, ym, ym);
    e_sub(u, y2, ED.f.one);                                  /* y^2 - 1 */
    e_mul(v, y2, ED.d); e_add(v, v, ED.f.one);               /* d y^2 + 1 */
    mont_inv(&ED.f, vi, v); e_mul(x2, u, vi);                /* x^2 = u/v */
    /* x = x2^((p+3)/8) */
    uint32_t e[8]; bn_copy(e, P, 8); bn_set(one, 8, 3); bn_add(e, e, one, 8);
    for (int s = 0; s < 3; ++s) { uint32_t cy = 0; for (int i = 7; i >= 0; --i) { uint32_t nc = e[i] & 1; e[i] = (e[i] >> 1) | (cy << 31); cy = nc; } }
    mont_pow(&ED.f, x, x2, e, 8);
    e_mul(vx2, x, x);
    if (bn_cmp(vx2, x2, 8) != 0) { e_mul(x, x, ED.sqrtm1); e_mul(vx2, x, x); if (bn_cmp(vx2, x2, 8) != 0) return -1; }
    mont_from(&ED.f, t, x);
    if (bn_zero(t, 8) && sign) return -1;
    if ((int)(t[0] & 1) != sign) { uint32_t z[8]; for (int i = 0; i < 8; ++i) z[i] = 0; e_sub(x, z, x); }
    bn_copy(r->x, x, 8); bn_copy(r->y, ym, 8); bn_copy(r->z, ED.f.one, 8); e_mul(r->t, x, ym);
    return 0;
}

int ed25519_verify(const uint8_t pub[32], const uint8_t* msg, uint64_t mlen, const uint8_t sig[64])
{
    static const uint8_t L[32] = {0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};
    if (ed_init() != 0) return -1;
    for (int i = 31; i >= 0; --i) { if (sig[32 + i] < L[i]) break; if (sig[32 + i] > L[i] || i == 0) return -1; }   /* S < L */
    EPt A, R, B, SB, kA, rhs;
    if (ep_decode(&A, pub) != 0 || ep_decode(&R, sig) != 0) return -1;
    uint8_t by[32]; for (int i = 0; i < 32; ++i) by[i] = 0x66; by[0] = 0x58;          /* base point y = 4/5, x even */
    if (ep_decode(&B, by) != 0) return -1;
    Sha512Ctx c; uint8_t h[64];
    sha512_init(&c); sha512_update(&c, sig, 32); sha512_update(&c, pub, 32); sha512_update(&c, msg, mlen); sha512_final(&c, h, 64);
    ep_mul(&SB, &B, sig + 32, 32);
    ep_mul(&kA, &A, h, 64);
    ep_add(&rhs, &R, &kA);
    uint32_t l1[8], l2[8];
    e_mul(l1, SB.x, rhs.z); e_mul(l2, rhs.x, SB.z); if (bn_cmp(l1, l2, 8) != 0) return -1;
    e_mul(l1, SB.y, rhs.z); e_mul(l2, rhs.y, SB.z); if (bn_cmp(l1, l2, 8) != 0) return -1;
    return 0;
}
