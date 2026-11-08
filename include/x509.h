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

#ifndef X509_H
#define X509_H

#include "types.h"

/* Minimal X.509 v3 parser and chain validator for TLS: RSA (PKCS#1 v1.5), ECDSA P-256/P-384 and Ed25519
   keys and signatures, SHA-256/384/512. Checks validity dates, issuer/subject linkage, CA flag (basicConstraints,
   keyUsage keyCertSign, pathLen), signature of every link and the host name (SAN dNSName with one-label
   wildcards, SAN IPv4, CN fallback). Not checked: revocation (CRL/OCSP), name constraints, policies, EKU. */

enum { X509_KEY_NONE = 0, X509_KEY_RSA = 1, X509_KEY_P256 = 2, X509_KEY_P384 = 3, X509_KEY_ED25519 = 4 };

struct X509Key
{
    int type;
    uint8_t rsa_n[512]; uint32_t rsa_nlen;
    uint8_t rsa_e[8]; uint32_t rsa_elen;
    uint8_t point[97]; uint32_t point_len;          /* EC: 0x04|X|Y ; Ed25519: 32-byte key */
};

/* trust store (PEM bundle, "-----BEGIN CERTIFICATE-----" blocks). append=0 replaces the store.
   Returns the number of certificates loaded or a negative error. */
int x509_trust_load_pem(const uint8_t* pem, uint32_t len, int append);
int x509_trust_count();
int x509_trust_name(int index, char* out, uint32_t cap);      /* subject CN (or O) of a trust anchor */
int x509_pem_to_der(const uint8_t* pem, uint32_t len, uint8_t* der, uint32_t cap, uint32_t* consumed);   /* first cert; DER length or <0 */

/* chain[0] = leaf. host may be NULL (no name check). now = Unix time. On success the leaf key is stored in *leaf.
   On failure returns <0 and *err points to a static description. */
int x509_verify_chain(const uint8_t* const* der, const uint32_t* len, int n, const char* host, uint64_t now, X509Key* leaf, const char** err);

/* signature of arbitrary data under a certificate key; scheme = TLS SignatureScheme code. 0 = valid */
int x509_verify_signature(const X509Key* key, uint16_t scheme, const uint8_t* msg, uint32_t mlen, const uint8_t* sig, uint32_t slen);

/* DER helper exposed for tests: parse one certificate and return its subject CN into out */
int x509_subject_cn(const uint8_t* der, uint32_t len, char* out, uint32_t cap);
uint64_t x509_time_to_epoch(const uint8_t* s, uint32_t len, int generalized);

#endif
