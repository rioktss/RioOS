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

#ifndef SSHKEY_H
#define SSHKEY_H
#include "types.h"

/* OpenSSH key files (Ed25519 only, unencrypted private keys, "openssh-key-v1"): compatible with OpenSSH's ssh-keygen. */
enum { SSHKEY_OK = 0, SSHKEY_NOT_KEY = -1, SSHKEY_ENCRYPTED = -2, SSHKEY_UNSUPPORTED = -3, SSHKEY_CORRUPT = -4, SSHKEY_TOO_BIG = -5 };

int sshkey_parse_private(const uint8_t* text, uint32_t len, uint8_t seed[32], uint8_t pub[32], char* comment, int comment_cap);
int sshkey_format_private(const uint8_t seed[32], const uint8_t pub[32], const char* comment, uint32_t check, char* out, int cap);   /* text length or <0 */
int sshkey_format_public(const uint8_t pub[32], const char* comment, char* out, int cap);                                            /* "ssh-ed25519 AAAA... comment\n" */
int sshkey_parse_public(const char* line, uint8_t pub[32]);                                                                          /* one authorized_keys / .pub line; 0 = ok */
int sshkey_blob(const uint8_t pub[32], uint8_t* out);                                                                                /* wire blob, 51 bytes */
int base64_encode(const uint8_t* in, uint32_t n, char* out, int cap);                                                                /* length or -1 */
int base64_decode(const char* in, uint32_t n, uint8_t* out, int cap);                                                                /* length or -1 (whitespace skipped) */
#endif
