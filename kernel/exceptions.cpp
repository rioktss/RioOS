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

#include "exceptions.h"
#include "exception_frame.h"
#include "syscall.h"
#include "uart.h"
#include "interrupt.h"
#include "fault.h"
#include "user.h"
#include "process.h"
#include "mmu.h"
#include "task.h"

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

    /* Capture the complete EL0 register frame at each boundary. P5 retains it
       in the PCB so a later scheduler can resume this process without relying
       on the current shell's C stack frame. The live scheduler remains
       cooperative until process-frame switching has passed QEMU IRQ tests. */
    int pid = user_current_pid();
    if (pid >= 0 && process_is_user(pid))
    {
        ProcessCpuContext context;
        for (int r = 0; r < 31; ++r) context.x[r] = frame->x[r];
        context.elr = frame->elr;
        context.spsr = frame->spsr;
#ifndef HOST_TEST
        asm volatile("mrs %0, sp_el0" : "=r"(context.user_sp));
#else
        context.user_sp = 0;
#endif
        (void)process_save_context(pid, &context);
    }

    uint64_t ec = (frame->esr >> 26) & 0x3FULL;
    if (ec == 0x15ULL) { syscall_dispatch(frame, 1); return; }

    /* Demand paging / copy-on-write: translation (0b0001xx) and permission
       (0b0011xx) faults on a user page are resolved and the instruction retried. */
    if (ec == 0x24ULL || ec == 0x20ULL)
    {
        uint64_t fsc = frame->esr & 0x3FULL;
        int is_write = (ec == 0x24ULL) && ((frame->esr >> 6) & 1ULL);
        if (((fsc >> 2) == 1ULL || (fsc >> 2) == 3ULL) && mmu_user_fault(frame->far, is_write))
            return;
    }

#ifndef HOST_TEST
    /* A program that installed a SIGSEGV / SIGBUS / SIGILL handler gets to run it (siglongjmp out of it to recover). */
    {
        int fsig = SIG_SEGV;
        uint64_t fsc2 = frame->esr & 0x3FULL;
        if (ec == 0x00ULL || ec == 0x0EULL) fsig = SIG_ILL;
        else if (ec == 0x22ULL || ec == 0x26ULL || ((ec == 0x24ULL || ec == 0x20ULL) && fsc2 == 0x21ULL)) fsig = SIG_BUS;
        if (pid >= 0 && task_deliver_fault(frame, fsig, frame->far) == 0) return;
    }
#endif
    fault_report(frame->esr, frame->elr, frame->far, frame->spsr);
    uart_puts("[EL0] Fatal user exception; terminating process.\r\n");
    user_set_exit_code(-14);
    task_exit_current(-14, SIG_SEGV);       /* never returns */
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
