#ifndef MYKERNEL_USER_API_H
#define MYKERNEL_USER_API_H

#include "types.h"

#define USER_FD_MAX_DESCRIPTORS 24
#define USER_FD_O_RDONLY 0x000U
#define USER_FD_O_WRONLY 0x001U
#define USER_FD_O_RDWR   0x002U
#define USER_FD_O_CREAT  0x100U
#define USER_FD_O_TRUNC  0x200U
#define USER_FD_O_APPEND 0x400U
#define USER_FS_FILE 0U
#define USER_FS_DIR  1U

struct UserFsStat
{
    uint32_t inode;
    uint32_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t sector_count;
    uint64_t size;
    uint64_t mtime;
};
typedef struct UserFsStat UserFsStat;

enum UserApiSyscall
{
    USER_SYS_PING = 0,
    USER_SYS_WRITE = 1,
    USER_SYS_UPTIME = 2,
    USER_SYS_GETPID = 3,
    USER_SYS_SCHED_TICK = 4,
    USER_SYS_MEM_FREE = 5,
    USER_SYS_EXIT = 6,
    USER_SYS_UNAME = 7,
    USER_SYS_WRITE_BUF = 8,
    USER_SYS_SLEEP_MS = 9,
    USER_SYS_READ = 10,
    USER_SYS_TIME_MS = 11,
    USER_SYS_FILE_READ = 12,
    USER_SYS_FILE_WRITE = 13,
    USER_SYS_GETARG = 14,
    USER_SYS_FD_OPEN = 15,
    USER_SYS_FD_CLOSE = 16,
    USER_SYS_FD_READ = 17,
    USER_SYS_FD_WRITE = 18,
    USER_SYS_FD_SEEK = 19,
    USER_SYS_FD_DUP = 20,
    USER_SYS_FD_DUP2 = 21,
    USER_SYS_FD_PIPE = 22,
    USER_SYS_FS_STAT = 23,
    USER_SYS_FS_TRUNCATE = 24,
    USER_SYS_FS_CHMOD = 25,
    USER_SYS_NET_UDP_SOCKET = 26,
    USER_SYS_NET_UDP_BIND = 27,
    USER_SYS_NET_UDP_SENDTO = 28,
    USER_SYS_NET_UDP_RECVFROM = 29,
    USER_SYS_NET_UDP_CLOSE = 30,
    USER_SYS_NET_DNS_LOOKUP = 31
};

static inline uint64_t user_syscall0(uint64_t nr)
{
    register uint64_t x0 asm("x0") = 0;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x8) :: "memory");
    return x0;
}

static inline uint64_t user_syscall1(uint64_t nr, uint64_t a0)
{
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x8) :: "memory");
    return x0;
}

static inline uint64_t user_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x8) :: "memory");
    return x0;
}

static inline uint64_t user_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x8) :: "memory");
    return x0;
}

static inline uint64_t user_syscall5(uint64_t nr, uint64_t a0, uint64_t a1,
                                     uint64_t a2, uint64_t a3, uint64_t a4)
{
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x4 asm("x4") = a4;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x8) :: "memory");
    return x0;
}

static inline uint64_t user_syscall6(uint64_t nr, uint64_t a0, uint64_t a1,
                                     uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5)
{
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x4 asm("x4") = a4;
    register uint64_t x5 asm("x5") = a5;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x8) :: "memory");
    return x0;
}

/* Userspace runtime helpers (return the raw kernel result; negative = error). */
static inline int64_t user_write(const void* buf, uint64_t len) { return (int64_t)user_syscall2(USER_SYS_WRITE_BUF, (uint64_t)buf, len); }
static inline int64_t user_read(void* buf, uint64_t len)       { return (int64_t)user_syscall2(USER_SYS_READ, (uint64_t)buf, len); }
static inline int64_t user_sleep_ms(uint64_t ms)               { return (int64_t)user_syscall1(USER_SYS_SLEEP_MS, ms); }
static inline uint64_t user_time_ms()                          { return user_syscall0(USER_SYS_TIME_MS); }
static inline int64_t user_fd_open(const char* path, uint64_t flags) { return (int64_t)user_syscall2(USER_SYS_FD_OPEN, (uint64_t)path, flags); }
static inline int64_t user_fd_close(int64_t fd) { return (int64_t)user_syscall1(USER_SYS_FD_CLOSE, (uint64_t)fd); }
static inline int64_t user_fd_read(int64_t fd, void* buf, uint64_t len) { return (int64_t)user_syscall3(USER_SYS_FD_READ, (uint64_t)fd, (uint64_t)buf, len); }
static inline int64_t user_fd_write(int64_t fd, const void* buf, uint64_t len) { return (int64_t)user_syscall3(USER_SYS_FD_WRITE, (uint64_t)fd, (uint64_t)buf, len); }
static inline int64_t user_fd_seek(int64_t fd, int64_t offset, int whence) { return (int64_t)user_syscall3(USER_SYS_FD_SEEK, (uint64_t)fd, (uint64_t)offset, (uint64_t)whence); }
static inline int64_t user_fd_dup(int64_t fd) { return (int64_t)user_syscall1(USER_SYS_FD_DUP, (uint64_t)fd); }
static inline int64_t user_fd_dup2(int64_t old_fd, int64_t new_fd) { return (int64_t)user_syscall2(USER_SYS_FD_DUP2, (uint64_t)old_fd, (uint64_t)new_fd); }
static inline int64_t user_fd_pipe(int descriptors[2]) { return (int64_t)user_syscall1(USER_SYS_FD_PIPE, (uint64_t)descriptors); }
static inline int64_t user_fs_stat(const char* path, UserFsStat* out) { return (int64_t)user_syscall2(USER_SYS_FS_STAT, (uint64_t)path, (uint64_t)out); }
static inline int64_t user_fs_truncate(const char* path, uint64_t size) { return (int64_t)user_syscall2(USER_SYS_FS_TRUNCATE, (uint64_t)path, size); }
static inline int64_t user_fs_chmod(const char* path, uint32_t mode) { return (int64_t)user_syscall2(USER_SYS_FS_CHMOD, (uint64_t)path, mode); }
static inline int64_t user_udp_socket(void) { return (int64_t)user_syscall0(USER_SYS_NET_UDP_SOCKET); }
static inline int64_t user_udp_bind(int64_t socket, uint16_t local_port) { return (int64_t)user_syscall2(USER_SYS_NET_UDP_BIND, (uint64_t)socket, local_port); }
static inline int64_t user_udp_sendto(int64_t socket, uint32_t ip, uint16_t port, const void* data, uint32_t length)
{ return (int64_t)user_syscall5(USER_SYS_NET_UDP_SENDTO, (uint64_t)socket, ip, port, (uint64_t)data, length); }
static inline int64_t user_udp_recvfrom(int64_t socket, uint32_t* source_ip, uint16_t* source_port,
                                        void* data, uint32_t capacity, uint32_t timeout_ms)
{ return (int64_t)user_syscall6(USER_SYS_NET_UDP_RECVFROM, (uint64_t)socket, (uint64_t)source_ip,
                                (uint64_t)source_port, (uint64_t)data, capacity, timeout_ms); }
static inline int64_t user_udp_close(int64_t socket) { return (int64_t)user_syscall1(USER_SYS_NET_UDP_CLOSE, (uint64_t)socket); }
static inline int64_t user_dns_lookup(const char* hostname, uint32_t* out_ip, uint32_t timeout_ms)
{ return (int64_t)user_syscall3(USER_SYS_NET_DNS_LOOKUP, (uint64_t)hostname, (uint64_t)out_ip, timeout_ms); }

#endif
