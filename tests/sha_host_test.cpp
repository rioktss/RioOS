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
#include <cstdio>
#include <cstring>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static void hex(const uint8_t* d, int n, char* o) { for (int i = 0; i < n; ++i) std::sprintf(o + i*2, "%02x", d[i]); }
int main()
{
    uint8_t d[32]; char h[65];
    sha256("abc", 3, d); hex(d, 32, h);
    CHECK(!std::strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    sha256("", 0, d); hex(d, 32, h);
    CHECK(!std::strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two, std::strlen(two), d); hex(d, 32, h);
    CHECK(!std::strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    hmac_sha256((const uint8_t*)"key", 3, "The quick brown fox jumps over the lazy dog", 43, d); hex(d, 32, h);
    CHECK(!std::strcmp(h, "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"));
    pbkdf2_sha256("password", (const uint8_t*)"salt", 4, 4096, d); hex(d, 32, h);
    CHECK(!std::strcmp(h, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
    if (!fails) std::puts("sha_host_test: all passed");
    return fails ? 1 : 0;
}
