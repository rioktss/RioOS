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

#ifndef TLS_H
#define TLS_H

#include "types.h"

/* TLS 1.3 client (TLS_AES_128_GCM_SHA256, X25519). The server certificate chain is validated against the
   trust store (/etc/ssl/certs.pem plus an optional extra CA file; see x509.h), the host name is checked and
   the CertificateVerify signature is verified. tls_set_verify(0) (wget/curl/httpsget -k) turns this off and
   leaves an encrypted but unauthenticated session. One session at a time. */
void tls_set_verify(int on);                                    /* default 1 */
int tls_verify_enabled();
const char* tls_last_error();                                   /* reason of the last failed tls_connect */
int tls_trust_reload();                                         /* (re)load the trust store; certificates loaded or <0 */
void tls_set_extra_ca(const char* path);
/* primitives re-used by the SSH client */
void tls_random(uint8_t* out, uint32_t n);
void tls_aes128_expand(uint8_t rk[176], const uint8_t key[16]);
void tls_aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]);                        /* additional CA file for the next connections ("" = none) */
int tls_connect(int tcp_socket, const char* hostname);          /* 0 ok, negative on error */
int tls_send(const void* data, uint32_t len);                  /* bytes sent or negative */
int tls_recv(void* buf, uint32_t cap, uint32_t timeout_ms);    /* bytes, 0 = closed, -110 timeout, other negative = error */
void tls_close();
int tls_active();

/* crypto primitives (exposed for host tests) */
void tls_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void tls_x25519_base(uint8_t out[32], const uint8_t scalar[32]);
void tls_aes128_gcm_encrypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t* aad, uint32_t aad_len,
                            const uint8_t* pt, uint32_t len, uint8_t* ct, uint8_t tag[16]);
int tls_aes128_gcm_decrypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t* aad, uint32_t aad_len,
                           const uint8_t* ct, uint32_t len, const uint8_t tag[16], uint8_t* pt);   /* 0 ok, -1 bad tag */
void tls_hkdf_extract(const uint8_t* salt, uint32_t salt_len, const uint8_t* ikm, uint32_t ikm_len, uint8_t out[32]);
void tls_hkdf_expand_label(const uint8_t secret[32], const char* label, const uint8_t* ctx, uint32_t ctx_len, uint8_t* out, uint32_t out_len);

#endif
