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
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
static int fails = 0, checks = 0;
static std::vector<uint8_t> slurp(const std::string& p)
{
    std::vector<uint8_t> v; FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return v;
    uint8_t b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof(b), f)) > 0) v.insert(v.end(), b, b + n);
    std::fclose(f); return v;
}
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int verify(const std::string& dir, const std::string& chainfile, const char* host, uint64_t now, const char** err)
{
    auto pem = slurp(dir + "/" + chainfile);
    static uint8_t der[12][4096]; const uint8_t* p[12]; uint32_t l[12]; int n = 0; uint32_t off = 0;
    while (n < 12)
    {
        uint32_t used = 0; int r = x509_pem_to_der(pem.data() + off, (uint32_t)pem.size() - off, der[n], 4096, &used);
        if (r < 0) break;
        p[n] = der[n]; l[n] = (uint32_t)r; ++n; off += used;
    }
    X509Key k;
    return x509_verify_chain(p, l, n, host, now, &k, err);
}
static void trust(const std::string& dir, const char* rootfile)
{
    auto pem = slurp(dir + "/" + rootfile); CHECK(x509_trust_load_pem(pem.data(), (uint32_t)pem.size(), 0) == 1);
}
int main(int argc, char** argv)
{
    std::string dir = argc > 1 ? argv[1] : "/tmp/pki"; uint64_t now = (uint64_t)std::time(0); const char* err = "";
    const char* tags[] = {"rsa", "rsa4k", "p256", "p384", "mixed", "ed"};
    for (const char* t : tags)
    {
        trust(dir, (std::string(t) + "_root.pem").c_str());
        std::string c = std::string(t) + "_chain.pem";
        int r = verify(dir, c, "test.rio.local", now, &err); if (r) std::printf("  %s: %s\n", t, err); CHECK(r == 0);
        CHECK(verify(dir, c, "10.0.2.2", now, &err) == 0);
        CHECK(verify(dir, c, "a.wild.rio.local", now, &err) == 0);
        CHECK(verify(dir, c, "a.b.wild.rio.local", now, &err) != 0);
        CHECK(verify(dir, c, "wild.rio.local", now, &err) != 0);
        CHECK(verify(dir, c, "evil.example.com", now, &err) != 0);
        CHECK(verify(dir, c, "10.0.2.3", now, &err) != 0);
        CHECK(verify(dir, c, 0, now, &err) == 0);
        CHECK(verify(dir, c, "test.rio.local", now + 86400ULL * 400, &err) != 0);        /* leaf expired */
        CHECK(verify(dir, c, "test.rio.local", 1000, &err) != 0);                         /* clock unset */
        CHECK(verify(dir, std::string(t) + "_leaf.pem", "test.rio.local", now, &err) != 0);   /* missing intermediate */
        CHECK(verify(dir, std::string(t) + "_full.pem", "test.rio.local", now, &err) == 0);   /* root included */
    }
    /* wrong trust anchor */
    { auto pem = slurp(dir + "/other_root.pem"); x509_trust_load_pem(pem.data(), (uint32_t)pem.size(), 0);
      CHECK(verify(dir, "rsa_chain.pem", "test.rio.local", now, &err) != 0); std::printf("  wrong CA: %s\n", err); }
    /* all roots loaded together */
    { x509_trust_load_pem((const uint8_t*)"", 0, 0); for (const char* t : tags) { auto pem = slurp(dir + "/" + t + "_root.pem"); x509_trust_load_pem(pem.data(), (uint32_t)pem.size(), 1); }
      CHECK(x509_trust_count() == 6); char nm[64]; CHECK(x509_trust_name(0, nm, sizeof(nm)) > 0 && std::strstr(nm, "Root CA"));
      CHECK(verify(dir, "p384_chain.pem", "test.rio.local", now, &err) == 0);
      CHECK(verify(dir, "expired_chain.pem", "test.rio.local", now, &err) != 0);
      CHECK(verify(dir, "self_leaf.pem", "test.rio.local", now, &err) != 0); std::printf("  self-signed: %s\n", err); }
    /* tampered leaf (flip a byte inside the cert) must fail */
    { auto pem = slurp(dir + "/rsa_chain.pem"); uint8_t der[4096]; uint32_t used; int r = x509_pem_to_der(pem.data(), (uint32_t)pem.size(), der, sizeof(der), &used);
      der[r / 2] ^= 1; const uint8_t* p[1] = {der}; uint32_t l[1] = {(uint32_t)r}; X509Key k; CHECK(x509_verify_chain(p, l, 1, 0, now, &k, &err) != 0); }
    if (!fails) std::printf("X509 HOST TEST PASS (%d checks)\n", checks);
    return fails ? 1 : 0;
}
