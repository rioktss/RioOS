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

#ifndef RIOOS_FD_H
#define RIOOS_FD_H

#include "types.h"

#define FD_MAX_DESCRIPTORS 24
#define FD_IO_MAX 4096ULL
#define FD_FILE_MAX (1024ULL * 1024ULL)
#define FD_PIPE_CAPACITY 4096

#define FD_O_RDONLY 0x000U
#define FD_O_WRONLY 0x001U
#define FD_O_RDWR   0x002U
#define FD_O_CREAT  0x100U
#define FD_O_TRUNC  0x200U
#define FD_O_APPEND 0x400U
#define FD_O_NONBLOCK 0x800U

void fd_init();
void fd_close_all();
int fd_set_process(int pid);
int fd_current_process();
void fd_release_process(int pid);
int fd_open(const char* path, uint32_t flags, int cwd);
int fd_close(int descriptor);
int fd_dup(int descriptor);
int fd_dup2(int old_descriptor, int new_descriptor);
/* Non-blocking bounded pipe. Returns 0 on success and fills descriptors[0/1]. */
int fd_pipe(int descriptors[2]);
int fd_close_all_for(int pid);
int64_t fd_read(int descriptor, void* buffer, uint64_t length);
int64_t fd_write(int descriptor, const void* buffer, uint64_t length);
int64_t fd_seek(int descriptor, int64_t offset, int whence);
void fd_list();

int fd_clone_table(int src_pid, int dst_pid);
int fd_redirect(int dst_pid, int dst_fd, int src_pid, int src_fd);
int fd_close_in(int pid, int descriptor);
int fd_is_pipe(int descriptor);
int fd_file_info(int descriptor, char* path, int cap, int* cwd, uint32_t* flags);
int fd_get_cloexec(int descriptor);
int fd_set_cloexec(int descriptor, int value);
void fd_close_on_exec(int pid);

/* unified descriptors: sockets, eventfd, AF_UNIX, readiness */
int fd_set_nonblock(int descriptor, int value);
int fd_get_flags(int descriptor);
int fd_is_blocking(int descriptor);      /* blocking-capable kind and not O_NONBLOCK */
int fd_is_socket(int descriptor);
int64_t fd_poll_gen(int descriptor);     /* EPOLLET arrival counter: (1<<32)|gen, 0 when the kind has none */
int fd_poll_mask(int descriptor);        /* Linux poll bits: IN 1, OUT 4, ERR 8, HUP 16, NVAL 32 */
int fd_socket(int type_and_flags);       /* inet: 1 stream (TCP) / 2 dgram (UDP) | SOCK_NONBLOCK 0x800 | SOCK_CLOEXEC 0x1000 */
int fd_sock_bind(int descriptor, uint32_t ip, uint16_t port);
int fd_listen_any(int descriptor, int backlog);
int fd_sock_connect(int descriptor, uint32_t ip, uint16_t port);
int fd_accept_any(int descriptor, uint32_t* ip, uint16_t* port, int flags);
int64_t fd_sock_sendto(int descriptor, const void* data, uint64_t len, uint32_t ip, uint16_t port, int has_addr);
int64_t fd_sock_recvfrom(int descriptor, void* data, uint64_t cap, uint32_t* ip, uint16_t* port);
int fd_sock_shutdown(int descriptor, int how);
int fd_sock_name(int descriptor, int peer, uint32_t* ip, uint16_t* port);
int fd_sock_error(int descriptor);
int fd_sock_type(int descriptor);
int fd_sock_is_unix(int descriptor);
int fd_eventfd(uint64_t initial, int flags);
int fd_unix_socket(int type_and_flags);
int fd_socketpair(int type_and_flags, int out[2]);
int fd_unix_bind(int descriptor, const char* path);
int fd_unix_connect(int descriptor, const char* path);
int64_t fd_sendmsg(int descriptor, const void* buf, uint64_t len, const int* fds, int nfds);
int64_t fd_recvmsg(int descriptor, void* buf, uint64_t cap, int* fds, int* nfds, int maxfds);

int fd_signalfd(int descriptor, uint32_t mask, int flags);          /* descriptor -1 creates; returns the descriptor */
int fd_inotify_init(int flags);
int fd_inotify_add_watch(int descriptor, const char* path, uint32_t mask, int cwd);   /* watch id or -errno */
int fd_inotify_rm_watch(int descriptor, int wd);
int fd_timerfd_create(int flags);
int fd_timerfd_settime(int descriptor, uint64_t first_ms, uint64_t interval_ms, uint64_t* old);   /* first_ms 0 disarms */
int fd_timerfd_gettime(int descriptor, uint64_t out[2]);
struct FsStat;
int fd_fstat(int descriptor, FsStat* out);   /* types: 0 file 1 dir 2 link 3 fifo 4 socket 5 tty 6 anonymous */
int fd_ftruncate(int descriptor, uint64_t length);
int fd_file_path(int descriptor, char* out, int cap, int* cwd);   /* regular-file descriptors only: path + cwd it was opened with */
int fd_flock(int descriptor, int op);        /* 1 SH, 2 EX, 8 UN; -11 on conflict */

#endif
