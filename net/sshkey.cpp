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
#include "string.h"

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int base64_encode(const uint8_t* in, uint32_t n, char* out, int cap)
{
    int o = 0;
    for (uint32_t i = 0; i < n; i += 3)
    {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        if (o + 4 >= cap) return -1;
        out[o++] = B64[(v >> 18) & 63]; out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '='; out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    if (o >= cap) return -1;
    out[o] = 0; return o;
}
static int b64v(int c) { if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26; if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '+') return 62; if (c == '/') return 63; return -1; }
int base64_decode(const char* in, uint32_t n, uint8_t* out, int cap)
{
    uint32_t acc = 0; int bits = 0, o = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        int c = (uint8_t)in[i];
        if (c == '=') break;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        int v = b64v(c); if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; if (o >= cap) return -1; out[o++] = (uint8_t)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    return o;
}

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int sshkey_blob(const uint8_t pub[32], uint8_t* out)
{
    put32(out, 11); for (int i = 0; i < 11; ++i) out[4 + i] = (uint8_t)"ssh-ed25519"[i];
    put32(out + 15, 32); for (int i = 0; i < 32; ++i) out[19 + i] = pub[i];
    return 51;
}

int sshkey_format_public(const uint8_t pub[32], const char* comment, char* out, int cap)
{
    uint8_t blob[51]; sshkey_blob(pub, blob);
    char b[80]; if (base64_encode(blob, 51, b, sizeof(b)) < 0) return -1;
    int n = 0; const char* pre = "ssh-ed25519 ";
    for (int i = 0; pre[i]; ++i) { if (n >= cap - 1) return -1; out[n++] = pre[i]; }
    for (int i = 0; b[i]; ++i) { if (n >= cap - 1) return -1; out[n++] = b[i]; }
    if (comment && comment[0]) { if (n >= cap - 1) return -1; out[n++] = ' '; for (int i = 0; comment[i]; ++i) { if (n >= cap - 2) return -1; out[n++] = comment[i]; } }
    out[n++] = '\n'; out[n] = 0; return n;
}

int sshkey_parse_public(const char* line, uint8_t pub[32])
{
    while (*line == ' ' || *line == '\t') ++line;
    const char* pre = "ssh-ed25519 ";
    for (int i = 0; pre[i]; ++i) if (line[i] != pre[i]) return -1;
    const char* b = line + 12; uint32_t n = 0; while (b[n] && b[n] != ' ' && b[n] != '\n' && b[n] != '\r') ++n;
    uint8_t blob[64]; int l = base64_decode(b, n, blob, sizeof(blob));
    if (l != 51 || get32(blob) != 11) return -1;
    for (int i = 0; i < 11; ++i) if (blob[4 + i] != (uint8_t)"ssh-ed25519"[i]) return -1;
    if (get32(blob + 15) != 32) return -1;
    for (int i = 0; i < 32; ++i) pub[i] = blob[19 + i];
    return 0;
}

#define MAGIC "openssh-key-v1"
int sshkey_format_private(const uint8_t seed[32], const uint8_t pub[32], const char* comment, uint32_t check, char* out, int cap)
{
    uint8_t buf[512]; uint32_t n = 0;
    for (int i = 0; i < 15; ++i) buf[n++] = (uint8_t)MAGIC[i];                     /* includes the trailing NUL */
    put32(buf + n, 4); n += 4; for (int i = 0; i < 4; ++i) buf[n++] = (uint8_t)"none"[i];          /* cipher */
    put32(buf + n, 4); n += 4; for (int i = 0; i < 4; ++i) buf[n++] = (uint8_t)"none"[i];          /* kdf */
    put32(buf + n, 0); n += 4;                                                                    /* kdf options */
    put32(buf + n, 1); n += 4;                                                                    /* one key */
    put32(buf + n, 51); n += 4; sshkey_blob(pub, buf + n); n += 51;
    uint32_t clen = 0; if (comment) while (comment[clen] && clen < 100) ++clen;
    uint8_t priv[256]; uint32_t p = 0;
    put32(priv + p, check); p += 4; put32(priv + p, check); p += 4;
    put32(priv + p, 11); p += 4; for (int i = 0; i < 11; ++i) priv[p++] = (uint8_t)"ssh-ed25519"[i];
    put32(priv + p, 32); p += 4; for (int i = 0; i < 32; ++i) priv[p++] = pub[i];
    put32(priv + p, 64); p += 4; for (int i = 0; i < 32; ++i) priv[p++] = seed[i]; for (int i = 0; i < 32; ++i) priv[p++] = pub[i];
    put32(priv + p, clen); p += 4; for (uint32_t i = 0; i < clen; ++i) priv[p++] = (uint8_t)comment[i];
    uint8_t pad = 1; while (p % 8) priv[p++] = pad++;
    put32(buf + n, p); n += 4; for (uint32_t i = 0; i < p; ++i) buf[n++] = priv[i];
    char b64[800]; int bl = base64_encode(buf, n, b64, sizeof(b64)); if (bl < 0) return -1;
    int o = 0; const char* head = "-----BEGIN OPENSSH PRIVATE KEY-----\n"; const char* tail = "-----END OPENSSH PRIVATE KEY-----\n";
    for (int i = 0; head[i]; ++i) { if (o >= cap - 1) return -1; out[o++] = head[i]; }
    for (int i = 0; i < bl; ++i) { if (o >= cap - 2) return -1; out[o++] = b64[i]; if (i % 70 == 69 || i == bl - 1) out[o++] = '\n'; }
    for (int i = 0; tail[i]; ++i) { if (o >= cap - 1) return -1; out[o++] = tail[i]; }
    out[o] = 0; return o;
}

int sshkey_parse_private(const uint8_t* text, uint32_t len, uint8_t seed[32], uint8_t pub[32], char* comment, int comment_cap)
{
    const char* head = "-----BEGIN OPENSSH PRIVATE KEY-----"; const char* tail = "-----END OPENSSH PRIVATE KEY-----";
    uint32_t hl = 35, tl = 33; uint32_t i = 0;
    while (i + hl <= len) { uint32_t k = 0; while (k < hl && text[i + k] == (uint8_t)head[k]) ++k; if (k == hl) break; ++i; }
    if (i + hl > len) return SSHKEY_NOT_KEY;
    uint32_t s = i + hl, e = s;
    while (e + tl <= len) { uint32_t k = 0; while (k < tl && text[e + k] == (uint8_t)tail[k]) ++k; if (k == tl) break; ++e; }
    if (e + tl > len) return SSHKEY_CORRUPT;
    uint8_t buf[700]; int n = base64_decode((const char*)text + s, e - s, buf, sizeof(buf));
    if (n < 0) return SSHKEY_CORRUPT;
    uint32_t p = 0, un = (uint32_t)n;
    if (un < 15 + 4) return SSHKEY_CORRUPT;
    for (int k = 0; k < 15; ++k) if (buf[k] != (uint8_t)MAGIC[k]) return SSHKEY_NOT_KEY;
    p = 15;
    #define NEED(x) do { if (p + (x) > un) return SSHKEY_CORRUPT; } while (0)
    NEED(4); uint32_t cl = get32(buf + p); p += 4; NEED(cl);
    int enc = !(cl == 4 && buf[p] == 'n' && buf[p + 1] == 'o' && buf[p + 2] == 'n' && buf[p + 3] == 'e'); p += cl;
    NEED(4); uint32_t kl = get32(buf + p); p += 4; NEED(kl); p += kl;
    NEED(4); uint32_t ol = get32(buf + p); p += 4; NEED(ol); p += ol;
    if (enc) return SSHKEY_ENCRYPTED;
    NEED(4); uint32_t nk = get32(buf + p); p += 4; if (nk != 1) return SSHKEY_UNSUPPORTED;
    NEED(4); uint32_t bl = get32(buf + p); p += 4; NEED(bl); p += bl;
    NEED(4); uint32_t pl = get32(buf + p); p += 4; NEED(pl);
    uint32_t q = p, qend = p + pl;
    #define NEEDQ(x) do { if (q + (x) > qend) return SSHKEY_CORRUPT; } while (0)
    NEEDQ(8); if (get32(buf + q) != get32(buf + q + 4)) return SSHKEY_CORRUPT; q += 8;
    NEEDQ(4); uint32_t tl2 = get32(buf + q); q += 4; NEEDQ(tl2);
    if (!(tl2 == 11 && buf[q] == 's' && buf[q + 1] == 's' && buf[q + 2] == 'h' && buf[q + 3] == '-' && buf[q + 4] == 'e' && buf[q + 5] == 'd')) return SSHKEY_UNSUPPORTED;
    q += tl2;
    NEEDQ(4); if (get32(buf + q) != 32) return SSHKEY_CORRUPT; q += 4; NEEDQ(32); for (int k = 0; k < 32; ++k) pub[k] = buf[q + k]; q += 32;
    NEEDQ(4); if (get32(buf + q) != 64) return SSHKEY_CORRUPT; q += 4; NEEDQ(64);
    for (int k = 0; k < 32; ++k) seed[k] = buf[q + k];
    for (int k = 0; k < 32; ++k) if (buf[q + 32 + k] != pub[k]) return SSHKEY_CORRUPT;
    q += 64;
    NEEDQ(4); uint32_t cm = get32(buf + q); q += 4; NEEDQ(cm);
    if (comment && comment_cap > 0) { uint32_t c = cm < (uint32_t)comment_cap - 1 ? cm : (uint32_t)comment_cap - 1; for (uint32_t k = 0; k < c; ++k) comment[k] = (char)buf[q + k]; comment[c] = 0; }
    return SSHKEY_OK;
}
