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

#include "user.h"
#include "elf.h"
#include "process.h"
#include "scheduler.h"
#include "mmu.h"
#include "uart.h"
#include "filters.h"
#include "timer.h"
#include "interrupt.h"
#include "fd.h"
#include "netstack.h"
#include "task.h"
#include "thread.h"
#include "jobs.h"
#include "script.h"

/* Foreground program execution. The program runs as a preemptively scheduled
   EL0 task (own kernel thread + address space); the shell waits for it, and
   Ctrl-C / Ctrl-Z reach it as SIGINT / SIGTSTP. */

extern "C" { uint64_t user_return_pc = 0; }
static int last_exit_code = 0;

int user_current_pid() { return thread_current_pid(); }

const char* user_get_args()
{
    int pid = thread_current_pid();
    return pid >= 0 ? process_args(pid) : "";
}

void user_set_exit_code(int code) { last_exit_code = code; }

static void put_u(uint64_t v)
{
    char b[24]; int n = 0;
    do { b[n++] = (char)('0' + v % 10ULL); v /= 10ULL; } while (v && n < 23);
    while (n) uart_putc(b[--n]);
}

static void user_execute_inner(const char* path, const char* args)
{
    if (!path || !path[0]) { uart_puts("exec: missing path\r\n"); return; }
    if (!mmu_enabled()) { uart_puts("exec: MMU is disabled\r\n"); return; }

    int pid = task_spawn(path, args, 1, 0, -1, -1);
    if (pid < 0)
    {
        if (pid == -11) uart_puts("exec: process table full\r\n");
        else if (pid == -24) uart_puts("exec: file descriptor table allocation failed\r\n");
        else { uart_puts("exec: failed to load ELF: "); uart_puts(path); uart_puts("\r\n"); }
        return;
    }

    ProcessCpuContext ctx;
    uint64_t entry = 0;
    if (process_load_context(pid, &ctx) == 0) entry = ctx.elr;
    static const char hex[] = "0123456789ABCDEF";
    int quiet = filters_capturing();            /* program output feeds a pipe / $(...) / redirect: no banner */
    if (!quiet)
    {
        uart_puts("Starting isolated userspace process: "); uart_puts(path); uart_puts("\r\n");
        uart_puts("PID: "); put_u((uint64_t)pid);
        uart_puts("\r\nEntry point: 0x");
        for (int i = 15; i >= 0; --i) uart_putc(hex[(entry >> (i * 4)) & 15]);
        uart_puts("\r\nAddress space: isolated; stack: 64 KiB + guard; preemptive (100 Hz)\r\n");
    }

    int code = task_wait_foreground(pid, 1);
    if (code == TASK_STOPPED)
    {
        uart_puts("\r\n[stopped] pid "); put_u((uint64_t)pid); uart_puts("  (resume with: fg %pid, bg %pid)\r\n");
        jobs_note_stopped(pid, path);
        return;
    }
    last_exit_code = code;
    script_set_status(code);
    if (!quiet)
    {
        uart_puts("Userspace returned to the kernel. Exit code: ");
        if (code < 0) { uart_putc('-'); put_u((uint64_t)(-(int64_t)code)); } else put_u((uint64_t)code);
        uart_puts("\r\n");
    }
}

void user_execute(const char* path, const char* args)
{
    int irq_was_on = interrupt_cpu_irq_enabled();
    user_execute_inner(path, args);
    if (timer_mode() != TIMER_MODE_PERIODIC)
    {
        timer_set_sched_tick(1);
        interrupt_timer_line_enable();
        timer_start_periodic(100U);
    }
    if (irq_was_on) interrupt_enable();
}
