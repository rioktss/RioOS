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

#include "sha256.h"

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

static inline uint32_t ror(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void block(Sha256Ctx* c, const uint8_t* p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; ++i)
    {
        uint32_t s0 = ror(w[i-15], 7) ^ ror(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror(w[i-2], 17) ^ ror(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; ++i)
    {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(Sha256Ctx* c)
{
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    for (int i = 0; i < 8; ++i) c->h[i] = iv[i];
    c->total = 0; c->fill = 0;
}

void sha256_update(Sha256Ctx* c, const void* data, uint64_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    c->total += len;
    while (len)
    {
        uint32_t n = 64 - c->fill;
        if (n > len) n = (uint32_t)len;
        for (uint32_t i = 0; i < n; ++i) c->buf[c->fill + i] = p[i];
        c->fill += n; p += n; len -= n;
        if (c->fill == 64) { block(c, c->buf); c->fill = 0; }
    }
}

void sha256_final(Sha256Ctx* c, uint8_t out[32])
{
    uint64_t bits = c->total * 8ULL;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    pad = 0;
    while (c->fill != 56) sha256_update(c, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, len, 8);
    for (int i = 0; i < 8; ++i)
    {
        out[i*4] = (uint8_t)(c->h[i] >> 24); out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8); out[i*4+3] = (uint8_t)c->h[i];
    }
}

void sha256(const void* data, uint64_t len, uint8_t out[32])
{
    Sha256Ctx c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

void hmac_sha256(const uint8_t* key, uint32_t key_len, const void* msg, uint64_t msg_len, uint8_t out[32])
{
    uint8_t k[64]; uint8_t tmp[32];
    for (int i = 0; i < 64; ++i) k[i] = 0;
    if (key_len > 64) sha256(key, key_len, k); else for (uint32_t i = 0; i < key_len; ++i) k[i] = key[i];
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    Sha256Ctx c;
    sha256_init(&c); sha256_update(&c, ipad, 64); sha256_update(&c, msg, msg_len); sha256_final(&c, tmp);
    sha256_init(&c); sha256_update(&c, opad, 64); sha256_update(&c, tmp, 32); sha256_final(&c, out);
}

void pbkdf2_sha256(const char* password, const uint8_t* salt, uint32_t salt_len, uint32_t iterations, uint8_t out[32])
{
    uint32_t plen = 0; while (password[plen]) ++plen;
    uint8_t msg[64]; uint8_t u[32];
    if (salt_len > 60) salt_len = 60;
    for (uint32_t i = 0; i < salt_len; ++i) msg[i] = salt[i];
    msg[salt_len] = 0; msg[salt_len+1] = 0; msg[salt_len+2] = 0; msg[salt_len+3] = 1;
    hmac_sha256((const uint8_t*)password, plen, msg, salt_len + 4, u);
    for (int i = 0; i < 32; ++i) out[i] = u[i];
    for (uint32_t it = 1; it < iterations; ++it)
    {
        hmac_sha256((const uint8_t*)password, plen, u, 32, u);
        for (int i = 0; i < 32; ++i) out[i] ^= u[i];
    }
}
