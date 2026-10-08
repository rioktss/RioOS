#ifndef MYKERNEL_USER_API_H
#define MYKERNEL_USER_API_H

#include "types.h"

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
    USER_SYS_GETARG = 14
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

/* Userspace runtime helpers (return the raw kernel result; negative = error). */
static inline int64_t user_write(const void* buf, uint64_t len) { return (int64_t)user_syscall2(USER_SYS_WRITE_BUF, (uint64_t)buf, len); }
static inline int64_t user_read(void* buf, uint64_t len)       { return (int64_t)user_syscall2(USER_SYS_READ, (uint64_t)buf, len); }
static inline int64_t user_sleep_ms(uint64_t ms)               { return (int64_t)user_syscall1(USER_SYS_SLEEP_MS, ms); }
static inline uint64_t user_time_ms()                          { return user_syscall0(USER_SYS_TIME_MS); }

#endif
