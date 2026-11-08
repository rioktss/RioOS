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
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
static int fails = 0, checks = 0;
static std::vector<uint8_t> unhex(const std::string& h)
{
    std::vector<uint8_t> o; if (h == "-") return o;
    for (size_t i = 0; i + 1 < h.size(); i += 2) o.push_back((uint8_t)strtoul(h.substr(i, 2).c_str(), 0, 16));
    return o;
}
static void expect(const char* what, int got_ok, int want_ok, int line)
{
    ++checks; if (got_ok != want_ok) { ++fails; std::printf("FAIL line %d %s (got %d want %d)\n", line, what, got_ok, want_ok); }
}
int main(int argc, char** argv)
{
    FILE* f = std::fopen(argc > 1 ? argv[1] : "tests/crypto_vectors.txt", "r");
    if (!f) { std::printf("no vectors\n"); return 2; }
    char line[20000]; int ln = 0;
    while (std::fgets(line, sizeof(line), f))
    {
        ++ln; std::vector<std::string> w; char* sp = nullptr; char* t = strtok_r(line, " \n", &sp); while (t) { w.push_back(t); t = strtok_r(nullptr, " \n", &sp); }
        if (w.empty()) continue;
        if (w[0] == "SHA")
        {
            auto m = unhex(w[2]); uint8_t o[64]; if (w[1] == "3") sha512(m.data(), m.size(), o); else sha384(m.data(), m.size(), o);
            auto e = unhex(w[3]); expect("sha", memcmp(o, e.data(), e.size()) == 0, 1, ln);
        }
        else if (w[0] == "RSA1" || w[0] == "RSAPSS")
        {
            int h = atoi(w[1].c_str()); auto n = unhex(w[2]), e = unhex(w[3]), d = unhex(w[4]), s = unhex(w[5]);
            int rc = w[0] == "RSA1" ? rsa_verify_pkcs1(n.data(), n.size(), e.data(), e.size(), h, d.data(), s.data(), s.size())
                                    : rsa_verify_pss(n.data(), n.size(), e.data(), e.size(), h, d.data(), s.data(), s.size());
            expect(w[0].c_str(), rc == 0, atoi(w[6].c_str()), ln);
        }
        else if (w[0] == "ECDSA")
        {
            auto p = unhex(w[2]), d = unhex(w[3]), s = unhex(w[4]);
            expect("ecdsa", ecdsa_verify(atoi(w[1].c_str()), p.data(), p.size(), d.data(), d.size(), s.data(), s.size()) == 0, atoi(w[5].c_str()), ln);
        }
        else if (w[0] == "ED25519")
        {
            auto p = unhex(w[1]), m = unhex(w[2]), s = unhex(w[3]);
            expect("ed25519", ed25519_verify(p.data(), m.data(), m.size(), s.data()) == 0, atoi(w[4].c_str()), ln);
        }
    }
    if (fails == 0) std::printf("CRYPTO HOST TEST PASS (%d checks)\n", checks);
    return fails ? 1 : 0;
}
