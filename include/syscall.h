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
    SYS_TIME_MS    = 11   /* milliseconds since boot                   */
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

#endif
