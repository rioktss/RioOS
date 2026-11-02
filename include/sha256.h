#ifndef SHA256_H
#define SHA256_H

#include "types.h"

struct Sha256Ctx
{
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t total;
    uint32_t fill;
};

void sha256_init(Sha256Ctx* c);
void sha256_update(Sha256Ctx* c, const void* data, uint64_t len);
void sha256_final(Sha256Ctx* c, uint8_t out[32]);
void sha256(const void* data, uint64_t len, uint8_t out[32]);
void hmac_sha256(const uint8_t* key, uint32_t key_len, const void* msg, uint64_t msg_len, uint8_t out[32]);
/* PBKDF2-HMAC-SHA256 (single 32-byte block) used for password hashing. */
void pbkdf2_sha256(const char* password, const uint8_t* salt, uint32_t salt_len, uint32_t iterations, uint8_t out[32]);

#endif
