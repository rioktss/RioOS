#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "exception_frame.h"

enum SyscallNumber
{
    SYS_PING       = 0,
    SYS_WRITE      = 1,
    SYS_UPTIME     = 2,
    SYS_GETPID     = 3,
    SYS_SCHED_TICK = 4,
    SYS_MEM_FREE   = 5,
    SYS_EXIT       = 6,
    SYS_UNAME      = 7,
    SYS_WRITE_BUF  = 8,   /* write(buf, len) -> bytes written          */
    SYS_SLEEP_MS   = 9,   /* sleep(ms), ms <= 10000                    */
    SYS_READ       = 10,  /* read(buf, len) non-blocking -> bytes read */
    SYS_TIME_MS    = 11,  /* milliseconds since boot                   */
    SYS_FILE_READ  = 12,  /* file_read(path, buf, cap) -> bytes read   */
    SYS_FILE_WRITE = 13,  /* file_write(path, buf, len) -> bytes written */
    SYS_GETARG     = 14,  /* getarg(buf, cap) -> length of argument string */
    SYS_FD_OPEN    = 15,  /* fd_open(path, flags) -> descriptor */
    SYS_FD_CLOSE   = 16,  /* fd_close(fd) */
    SYS_FD_READ    = 17,  /* fd_read(fd, buffer, length) */
    SYS_FD_WRITE   = 18,  /* fd_write(fd, buffer, length) */
    SYS_FD_SEEK    = 19,  /* fd_seek(fd, offset, whence) */
    SYS_FD_DUP     = 20,  /* fd_dup(oldfd) */
    SYS_FD_DUP2    = 21,  /* fd_dup2(oldfd, newfd) */
    SYS_FD_PIPE    = 22,  /* fd_pipe(int descriptors[2]), non-blocking */
    SYS_FS_STAT    = 23,  /* stat(path, FsStat*) */
    SYS_FS_TRUNCATE= 24,  /* truncate(path, length) */
    SYS_FS_CHMOD   = 25,  /* chmod(path, mode), owner or root */
    SYS_NET_UDP_SOCKET   = 26, /* udp_socket() -> owner-scoped handle */
    SYS_NET_UDP_BIND     = 27, /* udp_bind(handle, local_port) */
    SYS_NET_UDP_SENDTO   = 28, /* udp_sendto(handle, ip, port, data, length) */
    SYS_NET_UDP_RECVFROM = 29, /* udp_recvfrom(handle, &ip, &port, data, cap, timeout_ms) */
    SYS_NET_UDP_CLOSE    = 30, /* udp_close(handle) */
    SYS_NET_DNS_LOOKUP   = 31, /* dns_lookup(name, &ip, timeout_ms) */
    SYS_NET_TCP_SOCKET   = 32, /* tcp_socket() -> owner-scoped handle */
    SYS_NET_TCP_CONNECT  = 33, /* tcp_connect(handle, ip, port, timeout_ms) */
    SYS_NET_TCP_SEND     = 34, /* tcp_send(handle, data, length, timeout_ms) */
    SYS_NET_TCP_RECV     = 35, /* tcp_recv(handle, data, capacity, timeout_ms) */
    SYS_NET_TCP_CLOSE    = 36  /* tcp_close(handle) */
};

void syscall_init();

uint64_t syscall_invoke(
    uint64_t number,
    uint64_t a0 = 0,
    uint64_t a1 = 0,
    uint64_t a2 = 0,
    uint64_t a3 = 0,
    uint64_t a4 = 0,
    uint64_t a5 = 0
);

void syscall_dispatch(ExceptionFrame* frame, int from_user);
int syscall_user_pointer_ok(const void* pointer, uint64_t length);
int syscall_user_pointer_ok_write(const void* pointer, uint64_t length);

#define SYSCALL_IO_MAX 4096ULL
#define SYSCALL_SLEEP_MAX_MS 10000ULL
#define SYSCALL_PATH_MAX 128ULL
#define SYSCALL_FILE_MAX 4096ULL

#endif
