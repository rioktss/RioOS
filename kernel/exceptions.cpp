#include "exceptions.h"
#include "exception_frame.h"
#include "syscall.h"
#include "uart.h"
#include "interrupt.h"
#include "fault.h"

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
    /* Decode and report so a kernel fault is never silent. */
    fault_report(frame->esr, frame->elr, frame->far, frame->spsr);
    interrupt_exception(frame->esr, frame->elr, frame->far, frame->spsr);
    stop_kernel();
}

extern "C" void exception_sync_el0(void* raw)
{
    ExceptionFrame* frame = (ExceptionFrame*)raw;
    if (frame == 0) { stop_kernel(); return; }
    uint64_t ec = (frame->esr >> 26) & 0x3FULL;
    if (ec == 0x15ULL) { syscall_dispatch(frame, 1); return; }

    fault_report(frame->esr, frame->elr, frame->far, frame->spsr);
    uart_puts("[EL0] Fatal user exception; terminating process.\r\n");
    frame->x[0] = (uint64_t)-14;
    frame->spsr = 0x3C5ULL;
    frame->elr = (uint64_t)user_exit_return;
}

extern "C" void exception_irq_unhandled()
{
    uint64_t esr, elr, far, spsr;

    asm volatile("msr daifset, #0xf" ::: "memory");
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, elr_el1" : "=r"(elr));
    asm volatile("mrs %0, far_el1" : "=r"(far));
    asm volatile("mrs %0, spsr_el1" : "=r"(spsr));

    uart_puts("\r\n*** UNHANDLED FIQ/SError/AArch32 vector ***\r\n");
    interrupt_exception(esr, elr, far, spsr);
}
