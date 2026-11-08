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

#ifndef SSH_H
#define SSH_H

#include "types.h"

/* SSH-2 client (RFC 4251-4254): curve25519-sha256 key exchange, host keys ssh-ed25519 / rsa-sha2-256 / rsa-sha2-512 /
   ecdsa-sha2-nistp256, aes128-ctr + hmac-sha2-256, password authentication, one "session" channel (exec, shell or
   scp). Host keys are remembered in ~/.ssh_known_hosts (/etc/ssh_known_hosts for root). Not supported: public-key
   authentication, port forwarding, compression, rekeying (sessions that transfer more than ~1 GiB). */
struct SshOpts
{
    const char* host;      /* name used for the known_hosts entry */
    uint32_t port;
    const char* user;
    const char* password;
    int strict;            /* 0 accept-new (default), 1 yes (refuse unknown hosts), 2 no (accept changed keys) */
};

int ssh_connect(int sock, const SshOpts* o);          /* transport + host key check + authentication; 0 = ok */
const char* ssh_error();
int ssh_open_channel(const char* command, int pty);   /* command==0 -> interactive shell; 0 = ok */
int ssh_send(const uint8_t* data, uint32_t len);      /* data to the remote stdin; blocks while the window is closed */
/* >0 bytes read (stdout and stderr), 0 = channel finished (EOF/close), -1 = nothing yet (timeout), -2 = error */
int ssh_recv(uint8_t* buf, uint32_t cap, uint32_t timeout_ms);
int ssh_last_was_stderr();
int ssh_exit_status();                                /* -1 when the server sent none */
void ssh_send_eof();
void ssh_close();
int ssh_forget_host(const char* host, uint32_t port); /* 0 removed, -1 not found */
void ssh_known_hosts_path(char* out, int cap);
void ssh_fingerprint(const uint8_t* blob, uint32_t len, char* out, int cap);   /* "SHA256:..." */

#endif
