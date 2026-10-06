#include "exceptions.h"
#include "exception_frame.h"
#include "syscall.h"
#include "uart.h"

extern "C" char exception_vectors_el1[];
extern "C" void user_exit_return();

static uint64_t current_el()
{
    uint64_t value;
    asm volatile("mrs %0, CurrentEL" : "=r"(value));
    return (value >> 2) & 3ULL;
}

extern "C" void exceptions_init()
{
    if (current_el() != 1)
    {
        uart_puts("Exception setup: unexpected EL.\r\n");
        while (1) asm volatile("wfe");
    }

    asm volatile(
        "msr vbar_el1, %0\n"
        "dsb sy\n"
        "isb\n" :: "r"(exception_vectors_el1) : "memory");
    asm volatile("msr daifset, #0xf\n dsb sy\n isb" ::: "memory");
    uart_puts("Exception vectors initialized (EL1/EL0).\r\n");
}

static void stop_kernel()
{
    uart_puts("Execution stopped safely.\r\n");
    while (1) asm volatile("wfe");
}

extern "C" void exception_sync_el1(void* raw)
{
    ExceptionFrame* frame = (ExceptionFrame*)raw;
    if (frame == 0) { stop_kernel(); return; }
    uint64_t ec = (frame->esr >> 26) & 0x3FULL;
    if (ec == 0x15ULL) { syscall_dispatch(frame, 0); return; }
    uart_puts("\r\n*** KERNEL EXCEPTION ***\r\n");
    stop_kernel();
}

extern "C" void exception_sync_el0(void* raw)
{
    ExceptionFrame* frame = (ExceptionFrame*)raw;
    if (frame == 0) { stop_kernel(); return; }
    uint64_t ec = (frame->esr >> 26) & 0x3FULL;
    if (ec == 0x15ULL) { syscall_dispatch(frame, 1); return; }

    uart_puts("\r\n[EL0] Fatal user exception; terminating process.\r\n");
    frame->x[0] = (uint64_t)-14;
    frame->spsr = 0x3C5ULL;
    frame->elr = (uint64_t)user_exit_return;
}

extern "C" void exception_irq_unhandled()
{
    uart_puts("\r\n*** IRQ ***\r\n");
    uart_puts("IRQ is disabled in this release.\r\n");
    asm volatile("msr daifset, #0xf" ::: "memory");
    while (1) asm volatile("wfe");
}
