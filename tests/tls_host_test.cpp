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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("TLS HOST TEST FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static void unhex(const char* h, uint8_t* o) { for (size_t i = 0; h[2*i]; ++i) { unsigned v; std::sscanf(h + 2*i, "%2x", &v); o[i] = (uint8_t)v; } }
uint64_t timer_millis() { return 1; }
uint64_t rtc_get_epoch() { return 1800000000ULL; }
int tls_trust_reload() { return 0; }
int net_tcp_recv(int, int, void*, uint32_t, uint32_t) { return -1; }
int net_tcp_send(int, int, const void*, uint32_t, uint32_t) { return -1; }
int main()
{
    /* RFC 7748 5.2 vector 1 */
    uint8_t s[32], p[32], e[32], o[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", s);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", p);
    unhex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", e);
    tls_x25519(o, s, p); CHECK(memcmp(o, e, 32) == 0);
    /* RFC 7748 6.1: Alice public key */
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", s);
    unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", e);
    tls_x25519_base(o, s); CHECK(memcmp(o, e, 32) == 0);
    /* NIST GCM test case 2: key 0, iv 0, pt 16 zero bytes */
    uint8_t key[16] = {0}, iv[12] = {0}, pt[16] = {0}, ct[16], tag[16], exp_ct[16], exp_tag[16], back[16];
    unhex("0388dace60b6a392f328c2b971b2fe78", exp_ct); unhex("ab6e47d42cec13bdf53a67b21257bddf", exp_tag);
    tls_aes128_gcm_encrypt(key, iv, 0, 0, pt, 16, ct, tag);
    CHECK(memcmp(ct, exp_ct, 16) == 0); CHECK(memcmp(tag, exp_tag, 16) == 0);
    CHECK(tls_aes128_gcm_decrypt(key, iv, 0, 0, ct, 16, tag, back) == 0 && memcmp(back, pt, 16) == 0);
    tag[0] ^= 1; CHECK(tls_aes128_gcm_decrypt(key, iv, 0, 0, ct, 16, tag, back) != 0);
    /* NIST GCM test case 4 (with AAD, 60-byte pt) */
    uint8_t k4[16], iv4[12], pt4[60], aad4[20], ct4[60], tg4[16], e_ct4[60], e_tg4[16];
    unhex("feffe9928665731c6d6a8f9467308308", k4); unhex("cafebabefacedbaddecaf888", iv4);
    unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", pt4);
    unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad4);
    unhex("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091", e_ct4);
    unhex("5bc94fbc3221a5db94fae95ae7121a47", e_tg4);
    tls_aes128_gcm_encrypt(k4, iv4, aad4, 20, pt4, 60, ct4, tg4);
    CHECK(memcmp(ct4, e_ct4, 60) == 0); CHECK(memcmp(tg4, e_tg4, 16) == 0);
    /* AES-128 FIPS-197 via GCM keystream is covered above; HKDF test: RFC 5869 case 1 extract */
    uint8_t ikm[22], salt[13], prk[32], eprk[32];
    memset(ikm, 0x0b, 22); unhex("000102030405060708090a0b0c", salt);
    unhex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", eprk);
    tls_hkdf_extract(salt, 13, ikm, 22, prk); CHECK(memcmp(prk, eprk, 32) == 0);
    if (fails == 0) std::printf("TLS HOST TEST PASS\n");
    return fails ? 1 : 0;
}
