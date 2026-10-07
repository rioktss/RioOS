#include "syscall.h"
#include "uart.h"
#include "timer.h"
#include "memory.h"
#include "scheduler.h"
#include "user.h"
#include "mmu.h"
#include "version.h"

extern "C" void user_exit_return();

void syscall_init()
{
    uart_puts("Syscall interface initialized (EL1 + EL0).\r\n");
}

#ifndef HOST_TEST /* needs AArch64 register variables */
uint64_t syscall_invoke(uint64_t number,uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5)
{
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x4 asm("x4") = a4;
    register uint64_t x5 asm("x5") = a5;
    register uint64_t x8 asm("x8") = number;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x8) :: "memory");
    return x0;
}
#endif

int syscall_user_pointer_ok(const void* pointer, uint64_t length)
{
    return mmu_user_pointer_ok((uint64_t)pointer, length, 0);
}

int syscall_user_pointer_ok_write(const void* pointer, uint64_t length)
{
    return mmu_user_pointer_ok((uint64_t)pointer, length, 1);
}

void syscall_dispatch(ExceptionFrame* frame, int from_user)
{
    if (frame == 0) return;

    if ((frame->esr & 0xFFFFULL) != 0)
    {
        /* ELR_EL1 already holds the address of the instruction after the
           SVC on entry to a synchronous SVC exception (SVC always
           completes and is not re-executed), so it must not be advanced
           here. */
        frame->x[0] = (uint64_t)-22;
        return;
    }

    uint64_t number = frame->x[8];
    uint64_t result = (uint64_t)-38;

    switch (number)
    {
        case SYS_PING:
            result = 0x4D594B56ULL;
            break;

        case SYS_WRITE:
        {
            const char* text = (const char*)frame->x[0];
            if (text == 0 || (from_user && !syscall_user_pointer_ok(text, 1)))
            {
                result = (uint64_t)-14;
                break;
            }
            int written = 0;
            while (written < 512)
            {
                if (from_user && !syscall_user_pointer_ok(text + written, 1))
                {
                    result = (uint64_t)-14;
                    break;
                }
                char c = text[written];
                if (c == '\0')
                {
                    result = (uint64_t)written;
                    break;
                }
                uart_putc(c);
                ++written;
            }
            if (written == 512) result = (uint64_t)-7;
            break;
        }

        case SYS_UPTIME:
            result = timer_seconds();
            break;

        case SYS_GETPID:
        {
            int pid = from_user ? user_current_pid() : scheduler_current_pid();
            result = pid < 0 ? (uint64_t)-3 : (uint64_t)pid;
            break;
        }

        case SYS_SCHED_TICK:
            result = scheduler_ticks();
            break;

        case SYS_MEM_FREE:
            result = heap_free();
            break;

        case SYS_EXIT:
            if (!from_user)
            {
                result = (uint64_t)-1;
                break;
            }
            user_set_exit_code((int)frame->x[0]);
            frame->x[0] = 0;
            frame->spsr = 0x3C5ULL;
            frame->elr = (uint64_t)user_exit_return;
            return;

        case SYS_UNAME:
        {
            const char* out = "MyKernel " MYKERNEL_VERSION_STRING " ARM64";
            char* dst = (char*)frame->x[0];
            if (dst == 0 || (from_user && !syscall_user_pointer_ok_write(dst, 32)))
            {
                result = (uint64_t)-14;
                break;
            }
            int i = 0;
            while (out[i] && i < 31) { dst[i] = out[i]; ++i; }
            dst[i] = '\0';
            result = (uint64_t)i;
            break;
        }

        case SYS_WRITE_BUF:
        {
            const char* buf = (const char*)frame->x[0];
            uint64_t len = frame->x[1];

            if (len == 0)
            {
                result = 0;
                break;
            }
            if (len > SYSCALL_IO_MAX)
            {
                result = (uint64_t)-22;
                break;
            }
            if (buf == 0 || (from_user && !syscall_user_pointer_ok(buf, len)))
            {
                result = (uint64_t)-14;
                break;
            }
            for (uint64_t i = 0; i < len; ++i)
                uart_putc(buf[i]);
            result = len;
            break;
        }

        case SYS_SLEEP_MS:
            if (frame->x[0] > SYSCALL_SLEEP_MAX_MS)
            {
                result = (uint64_t)-22;
                break;
            }
            timer_delay_ms(frame->x[0]);
            result = 0;
            break;

        case SYS_READ:
        {
            char* buf = (char*)frame->x[0];
            uint64_t len = frame->x[1];

            if (len == 0)
            {
                result = 0;
                break;
            }
            if (len > SYSCALL_IO_MAX)
            {
                result = (uint64_t)-22;
                break;
            }
            if (buf == 0 || (from_user && !syscall_user_pointer_ok_write(buf, len)))
            {
                result = (uint64_t)-14;
                break;
            }
            uint64_t got = 0;
            while (got < len)
            {
                int c = uart_try_getc();
                if (c < 0)
                    break;
                buf[got++] = (char)c;
            }
            result = got;
            break;
        }

        case SYS_TIME_MS:
            result = timer_millis();
            break;

        default:
            result = (uint64_t)-38;
            break;
    }

    /* Do not advance ELR_EL1 here: for the SVC exception class it is
       already set to the instruction following the SVC, unlike a fault
       (data/instruction abort) where ELR points at the faulting
       instruction and must be skipped manually. Adding 4 here made the
       kernel eret one instruction past the caller's true return address,
       corrupting control flow on every syscall. */
    frame->x[0] = result;
}
