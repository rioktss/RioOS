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

#include "x509.h"
#include "crypto.h"

namespace {

struct Tlv { uint8_t tag; const uint8_t* v; uint32_t vl; const uint8_t* end; const uint8_t* start; };

bool tlv(const uint8_t* p, const uint8_t* end, Tlv* t)
{
    if (p + 2 > end) return false;
    t->start = p; t->tag = p[0];
    uint32_t l = p[1]; const uint8_t* q = p + 2;
    if (l & 0x80)
    {
        int nb = (int)(l & 0x7f); if (nb < 1 || nb > 3 || nb > (int)(end - q)) return false;
        l = 0; for (int i = 0; i < nb; ++i) l = (l << 8) | *q++;
    }
    if (l > (uint32_t)(end - q)) return false;   /* length compared against what is left: no pointer overflow */
    t->v = q; t->vl = l; t->end = q + l;
    return true;
}

bool same(const uint8_t* a, uint32_t al, const uint8_t* b, uint32_t bl)
{
    if (al != bl) return false;
    for (uint32_t i = 0; i < al; ++i) if (a[i] != b[i]) return false;
    return true;
}

enum { SIG_NONE = 0, SIG_RSA256, SIG_RSA384, SIG_RSA512, SIG_EC256, SIG_EC384, SIG_EC512, SIG_ED25519 };

struct Cert
{
    const uint8_t* der; uint32_t derlen;
    const uint8_t *tbs; uint32_t tbslen;
    const uint8_t *issuer, *subject; uint32_t issuerlen, subjectlen;
    uint64_t nb, na;
    int sigalg; const uint8_t* sig; uint32_t siglen;
    X509Key key;
    const uint8_t* san; uint32_t sanlen;
    int is_ca, has_bc, pathlen;           /* pathlen -1 = unlimited */
    int has_ku; uint32_t ku;
};

const uint8_t OID_RSA[]      = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
const uint8_t OID_EC[]       = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
const uint8_t OID_P256[]     = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
const uint8_t OID_P384[]     = {0x2b,0x81,0x04,0x00,0x22};
const uint8_t OID_ED25519[]  = {0x2b,0x65,0x70};
const uint8_t OID_SHA256RSA[]= {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
const uint8_t OID_SHA384RSA[]= {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c};
const uint8_t OID_SHA512RSA[]= {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0d};
const uint8_t OID_ECDSA256[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};
const uint8_t OID_ECDSA384[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03};
const uint8_t OID_ECDSA512[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x04};
const uint8_t OID_CN[]       = {0x55,0x04,0x03};
const uint8_t OID_O[]        = {0x55,0x04,0x0a};
const uint8_t OID_SAN[]      = {0x55,0x1d,0x11};
const uint8_t OID_BC[]       = {0x55,0x1d,0x13};
const uint8_t OID_KU[]       = {0x55,0x1d,0x0f};

#define OIDEQ(t, o) same((t).v, (t).vl, o, sizeof(o))

int days_from_civil(int y, int m, int d)
{
    y -= m <= 2; int era = (y >= 0 ? y : y - 399) / 400; int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int parse_sigalg(const uint8_t* p, const uint8_t* end)
{
    Tlv s, o; if (!tlv(p, end, &s) || s.tag != 0x30 || !tlv(s.v, s.end, &o) || o.tag != 0x06) return SIG_NONE;
    if (OIDEQ(o, OID_SHA256RSA)) return SIG_RSA256;
    if (OIDEQ(o, OID_SHA384RSA)) return SIG_RSA384;
    if (OIDEQ(o, OID_SHA512RSA)) return SIG_RSA512;
    if (OIDEQ(o, OID_ECDSA256)) return SIG_EC256;
    if (OIDEQ(o, OID_ECDSA384)) return SIG_EC384;
    if (OIDEQ(o, OID_ECDSA512)) return SIG_EC512;
    if (OIDEQ(o, OID_ED25519)) return SIG_ED25519;
    return SIG_NONE;
}

bool parse_spki(const Tlv& spki, X509Key* k)
{
    k->type = X509_KEY_NONE;
    Tlv alg, oid, bits;
    if (!tlv(spki.v, spki.end, &alg) || alg.tag != 0x30) return false;
    if (!tlv(alg.v, alg.end, &oid) || oid.tag != 0x06) return false;
    if (!tlv(alg.end, spki.end, &bits) || bits.tag != 0x03 || bits.vl < 2 || bits.v[0] != 0) return false;
    const uint8_t* kb = bits.v + 1; uint32_t kl = bits.vl - 1;
    if (OIDEQ(oid, OID_RSA))
    {
        Tlv seq, n, e;
        if (!tlv(kb, kb + kl, &seq) || seq.tag != 0x30 || !tlv(seq.v, seq.end, &n) || n.tag != 2 || !tlv(n.end, seq.end, &e) || e.tag != 2) return false;
        const uint8_t* nv = n.v; uint32_t nl = n.vl; while (nl && nv[0] == 0) { ++nv; --nl; }
        const uint8_t* ev = e.v; uint32_t el = e.vl; while (el && ev[0] == 0) { ++ev; --el; }
        if (nl < 64 || nl > 512 || el == 0 || el > 8) return false;
        for (uint32_t i = 0; i < nl; ++i) k->rsa_n[i] = nv[i];
        for (uint32_t i = 0; i < el; ++i) k->rsa_e[i] = ev[i];
        k->rsa_nlen = nl; k->rsa_elen = el; k->type = X509_KEY_RSA; return true;
    }
    if (OIDEQ(oid, OID_EC))
    {
        Tlv cv; if (!tlv(oid.end, alg.end, &cv) || cv.tag != 0x06) return false;
        int t = OIDEQ(cv, OID_P256) ? X509_KEY_P256 : OIDEQ(cv, OID_P384) ? X509_KEY_P384 : 0;
        uint32_t want = t == X509_KEY_P256 ? 65 : 97;
        if (!t || kl != want || kb[0] != 4) return false;
        for (uint32_t i = 0; i < kl; ++i) k->point[i] = kb[i];
        k->point_len = kl; k->type = t; return true;
    }
    if (OIDEQ(oid, OID_ED25519))
    {
        if (kl != 32) return false;
        for (uint32_t i = 0; i < 32; ++i) k->point[i] = kb[i];
        k->point_len = 32; k->type = X509_KEY_ED25519; return true;
    }
    return false;
}

uint64_t parse_time(const Tlv& t)
{
    return x509_time_to_epoch(t.v, t.vl, t.tag == 0x18);
}

bool parse_cert(const uint8_t* der, uint32_t len, Cert* c)
{
    Tlv cert, tbs, t;
    if (!tlv(der, der + len, &cert) || cert.tag != 0x30) return false;
    c->der = der; c->derlen = len;
    if (!tlv(cert.v, cert.end, &tbs) || tbs.tag != 0x30) return false;
    c->tbs = tbs.start; c->tbslen = (uint32_t)(tbs.end - tbs.start);
    Tlv sa, sv;
    if (!tlv(tbs.end, cert.end, &sa) || sa.tag != 0x30) return false;
    c->sigalg = parse_sigalg(sa.start, sa.end);
    if (!tlv(sa.end, cert.end, &sv) || sv.tag != 0x03 || sv.vl < 2 || sv.v[0] != 0) return false;
    c->sig = sv.v + 1; c->siglen = sv.vl - 1;
    const uint8_t* p = tbs.v;
    if (!tlv(p, tbs.end, &t)) return false;
    if (t.tag == 0xa0) { p = t.end; if (!tlv(p, tbs.end, &t)) return false; }       /* version */
    if (t.tag != 0x02) return false;
    p = t.end;                                     /* serial */
    if (!tlv(p, tbs.end, &t) || t.tag != 0x30) return false;
    p = t.end;             /* sig alg */
    if (!tlv(p, tbs.end, &t) || t.tag != 0x30) return false;                        /* issuer */
    c->issuer = t.start; c->issuerlen = (uint32_t)(t.end - t.start); p = t.end;
    Tlv val, nb, na;
    if (!tlv(p, tbs.end, &val) || val.tag != 0x30) return false;
    p = val.end;
    if (!tlv(val.v, val.end, &nb) || (nb.tag != 0x17 && nb.tag != 0x18) || !tlv(nb.end, val.end, &na) || (na.tag != 0x17 && na.tag != 0x18)) return false;
    c->nb = parse_time(nb); c->na = parse_time(na);
    if (!tlv(p, tbs.end, &t) || t.tag != 0x30) return false;                        /* subject */
    c->subject = t.start; c->subjectlen = (uint32_t)(t.end - t.start); p = t.end;
    Tlv spki; if (!tlv(p, tbs.end, &spki) || spki.tag != 0x30) return false; p = spki.end;
    if (!parse_spki(spki, &c->key)) return false;
    c->san = 0; c->sanlen = 0; c->is_ca = 0; c->has_bc = 0; c->pathlen = -1; c->has_ku = 0; c->ku = 0;
    while (p < tbs.end)
    {
        if (!tlv(p, tbs.end, &t)) return false;
        p = t.end;
        if (t.tag != 0xa3) continue;                                                // [3] extensions
        Tlv exts; if (!tlv(t.v, t.end, &exts) || exts.tag != 0x30) return false;
        const uint8_t* q = exts.v;
        while (q < exts.end)
        {
            Tlv ext, oid, nxt; if (!tlv(q, exts.end, &ext) || ext.tag != 0x30) return false; q = ext.end;
            if (!tlv(ext.v, ext.end, &oid) || oid.tag != 0x06) return false;
            const uint8_t* r = oid.end;
            if (!tlv(r, ext.end, &nxt)) return false;
            if (nxt.tag == 0x01) { if (!tlv(nxt.end, ext.end, &nxt)) return false; }  // critical flag
            if (nxt.tag != 0x04) return false;
            Tlv body;
            if (OIDEQ(oid, OID_SAN)) { c->san = nxt.v; c->sanlen = nxt.vl; }
            else if (OIDEQ(oid, OID_BC))
            {
                c->has_bc = 1;
                if (!tlv(nxt.v, nxt.end, &body) || body.tag != 0x30) return false;
                const uint8_t* b = body.v; Tlv f;
                if (b < body.end && tlv(b, body.end, &f) && f.tag == 0x01) { c->is_ca = f.vl && f.v[0] != 0; b = f.end; }
                if (b < body.end && tlv(b, body.end, &f) && f.tag == 0x02 && f.vl >= 1 && f.vl <= 2) { int v = 0; for (uint32_t i = 0; i < f.vl; ++i) v = (v << 8) | f.v[i]; c->pathlen = v; }
            }
            else if (OIDEQ(oid, OID_KU))
            {
                if (!tlv(nxt.v, nxt.end, &body) || body.tag != 0x03 || body.vl < 2) return false;
                c->has_ku = 1; c->ku = body.v[1]; if (body.vl > 2) c->ku |= (uint32_t)body.v[2] << 8;
            }
        }
    }
    return true;
}

/* verify cert c's signature with the issuer key */
int check_sig(const Cert* c, const X509Key* ik)
{
    uint8_t d[64]; int hl;
    switch (c->sigalg)
    {
    case SIG_RSA256: case SIG_EC256: hash_buf(HASH_SHA256, c->tbs, c->tbslen, d); hl = HASH_SHA256; break;
    case SIG_RSA384: case SIG_EC384: hash_buf(HASH_SHA384, c->tbs, c->tbslen, d); hl = HASH_SHA384; break;
    case SIG_RSA512: case SIG_EC512: hash_buf(HASH_SHA512, c->tbs, c->tbslen, d); hl = HASH_SHA512; break;
    case SIG_ED25519:
        if (ik->type != X509_KEY_ED25519 || c->siglen != 64) return -1;
        return ed25519_verify(ik->point, c->tbs, c->tbslen, c->sig);
    default: return -1;
    }
    if (c->sigalg <= SIG_RSA512)
    {
        if (ik->type != X509_KEY_RSA) return -1;
        return rsa_verify_pkcs1(ik->rsa_n, ik->rsa_nlen, ik->rsa_e, ik->rsa_elen, hl, d, c->sig, c->siglen);
    }
    if (ik->type != X509_KEY_P256 && ik->type != X509_KEY_P384) return -1;
    return ecdsa_verify(ik->type == X509_KEY_P256 ? 1 : 2, ik->point, ik->point_len, d, (uint32_t)hash_len(hl), c->sig, c->siglen);
}

bool ieq(char a, char b) { if (a >= 'A' && a <= 'Z') a = (char)(a + 32); if (b >= 'A' && b <= 'Z') b = (char)(b + 32); return a == b; }

bool name_match(const char* pat, uint32_t pl, const char* host)
{
    uint32_t hl = 0; while (host[hl]) ++hl;
    if (pl > 2 && pat[0] == '*' && pat[1] == '.')
    {
        uint32_t dot = 0; while (dot < hl && host[dot] != '.') ++dot;
        if (dot == 0 || dot >= hl) return false;
        const char* rest = host + dot; uint32_t rl = hl - dot;
        if (rl != pl - 1) return false;
        for (uint32_t i = 0; i < rl; ++i) if (!ieq(rest[i], pat[1 + i])) return false;
        return true;
    }
    if (pl != hl) return false;
    for (uint32_t i = 0; i < pl; ++i) if (!ieq(pat[i], host[i])) return false;
    return true;
}

bool parse_ipv4(const char* s, uint8_t out[4])
{
    int part = 0, val = 0, digits = 0;
    for (;; ++s)
    {
        if (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); if (++digits > 3 || val > 255) return false; }
        else if (*s == '.' || *s == 0)
        {
            if (!digits || part > 3) return false;
            out[part++] = (uint8_t)val; val = 0; digits = 0;
            if (*s == 0) break;
        }
        else return false;
    }
    return part == 4;
}

bool host_ok(const Cert* c, const char* host)
{
    uint8_t ip[4]; bool is_ip = parse_ipv4(host, ip);
    bool any_dns = false;
    if (c->san)
    {
        Tlv seq; if (tlv(c->san, c->san + c->sanlen, &seq) && seq.tag == 0x30)
        {
            const uint8_t* p = seq.v; Tlv g;
            while (p < seq.end && tlv(p, seq.end, &g))
            {
                p = g.end;
                if (g.tag == 0x82) { any_dns = true; if (!is_ip && name_match((const char*)g.v, g.vl, host)) return true; }
                else if (g.tag == 0x87 && is_ip && g.vl == 4 && same(g.v, 4, ip, 4)) return true;
            }
        }
    }
    if (any_dns || is_ip) return false;
    /* no SAN dNSName: fall back to the subject CN */
    char cn[128]; if (x509_subject_cn(c->der, c->derlen, cn, sizeof(cn)) > 0) { uint32_t l = 0; while (cn[l]) ++l; return name_match(cn, l, host); }
    return false;
}

/* ---- trust store ---- */
#define TRUST_BYTES (512 * 1024)
#define TRUST_MAX 512
uint8_t g_trust[TRUST_BYTES];
uint32_t g_toff[TRUST_MAX], g_tlen[TRUST_MAX];
int g_tcount = 0; uint32_t g_tused = 0;

int b64v(uint8_t c) { if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26; if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '+') return 62; if (c == '/') return 63; return -1; }

bool starts(const uint8_t* p, const uint8_t* end, const char* s) { while (*s) { if (p >= end || *p != (uint8_t)*s) return false; ++p; ++s; } return true; }

}   // namespace

uint64_t x509_time_to_epoch(const uint8_t* s, uint32_t len, int generalized)
{
    uint32_t i = 0; int y;
    auto two = [&](void) -> int { int v = (s[i] - '0') * 10 + (s[i + 1] - '0'); i += 2; return v; };
    if (generalized) { if (len < 15) return 0; y = two() * 100; y += two(); }
    else { if (len < 13) return 0; y = two(); y += y >= 50 ? 1900 : 2000; }
    int mo = two(), d = two(), h = two(), mi = two(), se = two();
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    int64_t days = days_from_civil(y, mo, d);
    return (uint64_t)(days * 86400 + h * 3600 + mi * 60 + se);
}

int x509_pem_to_der(const uint8_t* pem, uint32_t len, uint8_t* der, uint32_t cap, uint32_t* consumed)
{
    const uint8_t* p = pem; const uint8_t* end = pem + len;
    while (p < end && !starts(p, end, "-----BEGIN CERTIFICATE-----")) ++p;
    if (p >= end) return -1;
    p += 27;
    uint32_t n = 0; uint32_t acc = 0; int bits = 0;
    while (p < end && *p != '-')
    {
        int v = b64v(*p++);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; if (n >= cap) return -2; der[n++] = (uint8_t)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    while (p < end && !starts(p, end, "-----END CERTIFICATE-----")) ++p;
    if (p >= end) return -1;
    p += 25;
    if (consumed) *consumed = (uint32_t)(p - pem);
    return (int)n;
}

int x509_trust_load_pem(const uint8_t* pem, uint32_t len, int append)
{
    if (!append) { g_tcount = 0; g_tused = 0; }
    int added = 0; uint32_t off = 0;
    while (off < len)
    {
        uint32_t used = 0;
        int n = x509_pem_to_der(pem + off, len - off, g_trust + g_tused, TRUST_BYTES - g_tused, &used);
        if (n == -2) return added ? added : -2;
        if (n < 0) break;
        off += used;
        Cert c;
        if (g_tcount < TRUST_MAX && n > 0 && parse_cert(g_trust + g_tused, (uint32_t)n, &c))
        {
            g_toff[g_tcount] = g_tused; g_tlen[g_tcount] = (uint32_t)n; ++g_tcount; g_tused += (uint32_t)n; ++added;
        }
    }
    return added;
}
int x509_trust_count() { return g_tcount; }

int x509_subject_cn(const uint8_t* der, uint32_t len, char* out, uint32_t cap)
{
    Cert c; if (!parse_cert(der, len, &c)) return -1;
    Tlv name; if (!tlv(c.subject, c.subject + c.subjectlen, &name)) return -1;
    const uint8_t* p = name.v; const uint8_t* o_val = 0; uint32_t o_len = 0;
    while (p < name.end)
    {
        Tlv set, atv, oid, val;
        if (!tlv(p, name.end, &set) || set.tag != 0x31) return -1;
        p = set.end;
        if (!tlv(set.v, set.end, &atv) || atv.tag != 0x30 || !tlv(atv.v, atv.end, &oid) || oid.tag != 0x06 || !tlv(oid.end, atv.end, &val)) return -1;
        if (OIDEQ(oid, OID_CN) && val.vl < cap) { for (uint32_t i = 0; i < val.vl; ++i) out[i] = (char)val.v[i]; out[val.vl] = 0; return (int)val.vl; }
        if (OIDEQ(oid, OID_O)) { o_val = val.v; o_len = val.vl; }
    }
    if (o_val && o_len < cap) { for (uint32_t i = 0; i < o_len; ++i) out[i] = (char)o_val[i]; out[o_len] = 0; return (int)o_len; }
    return -1;
}
int x509_trust_name(int index, char* out, uint32_t cap)
{
    if (index < 0 || index >= g_tcount) return -1;
    int r = x509_subject_cn(g_trust + g_toff[index], g_tlen[index], out, cap);
    if (r < 0 && cap) { out[0] = 0; return 0; }
    return r;
}

int x509_verify_chain(const uint8_t* const* der, const uint32_t* len, int n, const char* host, uint64_t now, X509Key* leaf, const char** err)
{
    static const char* dummy; if (!err) err = &dummy;
    static Cert certs[12];
    if (n < 1 || n > 12) { *err = "bad certificate chain length"; return -1; }
    for (int i = 0; i < n; ++i) if (!parse_cert(der[i], len[i], &certs[i])) { *err = "unparsable certificate (unsupported key or format)"; return -2; }
    if (now < 1700000000ULL) { *err = "system clock is not set (run ntpdate)"; return -3; }
    *leaf = certs[0].key;
    if (host && !host_ok(&certs[0], host)) { *err = "certificate does not match the host name"; return -4; }
    int cur = 0; unsigned used = 1;
    for (int depth = 0; depth < 12; ++depth)
    {
        Cert* c = &certs[cur];
        if (now < c->nb) { *err = "certificate is not yet valid"; return -5; }
        if (now > c->na) { *err = "certificate has expired"; return -6; }
        if (c->sigalg == SIG_NONE) { *err = "unsupported signature algorithm"; return -7; }
        /* trust anchor? */
        for (int t = 0; t < g_tcount; ++t)
        {
            Cert r;
            if (!parse_cert(g_trust + g_toff[t], g_tlen[t], &r)) continue;
            if (!same(r.subject, r.subjectlen, c->issuer, c->issuerlen)) continue;
            if (now < r.nb || now > r.na) continue;
            if (r.has_bc && !r.is_ca) continue;
            if (r.has_ku && !(r.ku & 0x04)) continue;
            if (depth > 0 && r.pathlen >= 0 && depth - 1 > r.pathlen) continue;
            if (check_sig(c, &r.key) == 0) return 0;
        }
        /* intermediate in the provided chain */
        int next = -1;
        for (int i = 1; i < n; ++i)
        {
            if (used & (1u << i)) continue;
            if (!same(certs[i].subject, certs[i].subjectlen, c->issuer, c->issuerlen)) continue;
            if (check_sig(c, &certs[i].key) != 0) continue;
            next = i; break;
        }
        if (next < 0)
        {
            *err = depth == 0 && n == 1 ? "unknown issuer (self-signed or CA not in trust store)" : "unknown issuer (CA not in trust store or bad signature)";
            return -8;
        }
        Cert* ic = &certs[next];
        if (!ic->has_bc || !ic->is_ca) { *err = "issuer is not a CA"; return -9; }
        if (ic->has_ku && !(ic->ku & 0x04)) { *err = "issuer key usage forbids certificate signing"; return -9; }
        if (ic->pathlen >= 0 && depth > ic->pathlen) { *err = "CA path length exceeded"; return -9; }
        used |= 1u << next; cur = next;
    }
    *err = "certificate chain too long"; return -10;
}

int x509_verify_signature(const X509Key* k, uint16_t scheme, const uint8_t* msg, uint32_t mlen, const uint8_t* sig, uint32_t slen)
{
    uint8_t d[64];
    switch (scheme)
    {
    case 0x0804: case 0x0805: case 0x0806:
    {
        if (k->type != X509_KEY_RSA) return -1;
        int h = scheme == 0x0804 ? HASH_SHA256 : scheme == 0x0805 ? HASH_SHA384 : HASH_SHA512;
        hash_buf(h, msg, mlen, d);
        return rsa_verify_pss(k->rsa_n, k->rsa_nlen, k->rsa_e, k->rsa_elen, h, d, sig, slen);
    }
    case 0x0401: case 0x0501: case 0x0601:
    {
        if (k->type != X509_KEY_RSA) return -1;
        int h = scheme == 0x0401 ? HASH_SHA256 : scheme == 0x0501 ? HASH_SHA384 : HASH_SHA512;
        hash_buf(h, msg, mlen, d);
        return rsa_verify_pkcs1(k->rsa_n, k->rsa_nlen, k->rsa_e, k->rsa_elen, h, d, sig, slen);
    }
    case 0x0403: case 0x0503:
    {
        int want = scheme == 0x0403 ? X509_KEY_P256 : X509_KEY_P384;
        if (k->type != want) return -1;
        int h = scheme == 0x0403 ? HASH_SHA256 : HASH_SHA384;
        hash_buf(h, msg, mlen, d);
        return ecdsa_verify(k->type == X509_KEY_P256 ? 1 : 2, k->point, k->point_len, d, (uint32_t)hash_len(h), sig, slen);
    }
    case 0x0807:
        if (k->type != X509_KEY_ED25519 || slen != 64) return -1;
        return ed25519_verify(k->point, msg, mlen, sig);
    default: return -1;
    }
}
