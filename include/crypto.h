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

#ifndef CRYPTO_H
#define CRYPTO_H

#include "types.h"

/* SHA-384 / SHA-512 */
struct Sha512Ctx { uint64_t h[8]; uint8_t buf[128]; uint64_t total; uint32_t fill; };
void sha512_init(Sha512Ctx* c);
void sha384_init(Sha512Ctx* c);
void sha512_update(Sha512Ctx* c, const void* data, uint64_t len);
void sha512_final(Sha512Ctx* c, uint8_t* out, int out_len);      /* out_len 64 (SHA-512) or 48 (SHA-384) */
void sha512(const void* data, uint64_t len, uint8_t out[64]);
void sha384(const void* data, uint64_t len, uint8_t out[48]);
void hmac_sha512(const uint8_t* key, uint32_t key_len, const void* msg, uint64_t msg_len, uint8_t out[64]);

/* hash selector for signature checks */
enum { HASH_SHA256 = 1, HASH_SHA384 = 2, HASH_SHA512 = 3 };
int hash_len(int alg);
void hash_buf(int alg, const void* data, uint64_t len, uint8_t* out);

/* RSA public-key signature verification (modulus/exponent are big-endian, up to 4096-bit modulus).
   Return 0 when the signature is valid. */
int rsa_verify_pkcs1(const uint8_t* n, uint32_t nlen, const uint8_t* e, uint32_t elen, int hash, const uint8_t* digest, const uint8_t* sig, uint32_t slen);
int rsa_verify_pss(const uint8_t* n, uint32_t nlen, const uint8_t* e, uint32_t elen, int hash, const uint8_t* digest, const uint8_t* sig, uint32_t slen);

/* ECDSA (P-256 = curve 1, P-384 = curve 2). pub = uncompressed point 0x04|X|Y; sig = DER SEQUENCE{r,s}. 0 = valid */
int ecdsa_verify(int curve, const uint8_t* pub, uint32_t publen, const uint8_t* digest, uint32_t dlen, const uint8_t* sig, uint32_t slen);

/* Ed25519 (RFC 8032) verification, 0 = valid */
int ed25519_verify(const uint8_t pub[32], const uint8_t* msg, uint64_t mlen, const uint8_t sig[64]);

#endif
