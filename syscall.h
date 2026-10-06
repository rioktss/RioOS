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
    SYS_UNAME      = 7
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

#endif
