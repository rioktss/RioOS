#ifndef TLS_H
#define TLS_H

#include "types.h"

/* TLS 1.3 client (TLS_AES_128_GCM_SHA256, X25519). Handshake integrity is verified through the
   server Finished MAC, but the server certificate chain is NOT validated (no trust store):
   traffic is encrypted but the server is not authenticated. One session at a time. */
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
