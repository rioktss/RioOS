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
    USER_SYS_UNAME = 7
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

#endif
