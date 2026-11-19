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

#include "process.h"
#include "mmu.h"
#include "uart.h"
#include "string.h"
#include "fd.h"

#define PROCESS_NAME_SIZE 32

struct Process
{
    int used;
    int pid;
    int state;
    int kind;
    int exit_code;
    uint64_t user_low;
    uint64_t user_high;
    int context_valid;
    ProcessCpuContext context;
    char name[PROCESS_NAME_SIZE];
    /* Multitasking state */
    int ppid;
    int thread;          /* kernel thread bound to this process, -1 if none */
    int term_signal;     /* signal that terminated the process (0 = normal exit) */
    int background;
    int stopped;
    uint32_t sig_pending;                    /* process-directed signals; every thread keeps its own mask and thread-directed set */
    uint64_t sig_handler[32];
    uint32_t sig_amask[32];
    uint32_t sig_flags[32];
    uint64_t sig_restorer;
    uint64_t alarm_at;
    uint64_t alarm_interval;
    int sig_sender[32];
    int sig_code[32];
    uint64_t sig_value[32];
    uint64_t alt_sp;
    uint64_t alt_size;
    uint32_t uid;
    uint32_t gid;
    uint64_t start_ms;
    int cwd;             /* filesystem node of the working directory, -1 = shell's */
    uint32_t umask_bits;
    uint64_t rl_cur[PROC_RLIMIT_N], rl_max[PROC_RLIMIT_N];   /* setrlimit, inherited by fork */
    uint64_t cpu_user_ms, cpu_sys_ms;                        /* charged by the timer tick */
    uint64_t child_user_ms, child_sys_ms;                    /* totals of exited children */
    uint64_t ctx_switches;
    int in_syscall;
    int nthreads;                            /* live threads of the process */
    int exiting;                             /* exit_group / fatal signal in progress */
    int exit_pending_code, exit_pending_sig;
    uint32_t cpu_mask;                                       /* sched_setaffinity: allowed CPUs (0 = all) */
    uint32_t sig_serial;                                     /* counts raised signals (signalfd + EPOLLET) */
    char args[PROCESS_ARGS_SIZE];
};

/* The process table grows on demand: slots are separately allocated records that are recycled, so the only limit is memory. */
#ifdef HOST_TEST
#include <stdlib.h>
static void* proc_alloc(uint64_t n) { return malloc((size_t)n); }
static void proc_free(void* p) { free(p); }
#else
#include "memory.h"
static void* proc_alloc(uint64_t n) { return kmalloc(n); }
static void proc_free(void* p) { kfree(p); }
#endif
#define PROC_PID_LIMIT 0x1FFFFFF0      /* pids stay below the thread-id and scratch address-space ranges */
static Process** proc_tab = 0;
static int proc_n = 0;                 /* allocated records, 0..proc_n-1 */
static int proc_cap = 0;               /* capacity of proc_tab */
static int proc_hint = 0;
#define PROC(i) (*proc_tab[(i)])
static int next_pid;

static uint64_t process_irq_save()
{
#ifdef HOST_TEST
    return 0;
#else
    uint64_t daif;
    asm volatile(
        "mrs %0, daif\n"
        "msr daifset, #0x2\n"
        "isb"
        : "=r"(daif) : : "memory");
    return daif;
#endif
}

static void process_irq_restore(uint64_t flags)
{
#ifndef HOST_TEST
    asm volatile("msr daif, %0\n isb" :: "r"(flags) : "memory");
#else
    (void)flags;
#endif
}

static void reset_mt_fields(Process* p)
{
    p->ppid = -1;
    p->thread = -1;
    p->term_signal = 0;
    p->background = 0;
    p->stopped = 0;
    p->sig_pending = 0;
    p->nthreads = 0; p->exiting = 0; p->exit_pending_code = 0; p->exit_pending_sig = 0;
    for (int i = 0; i < 32; ++i) { p->sig_handler[i] = 0; p->sig_amask[i] = 0; p->sig_flags[i] = 0; }
    p->sig_restorer = 0;
    p->alarm_at = 0;
    p->alarm_interval = 0;
    for (int i = 0; i < 32; ++i) { p->sig_sender[i] = 0; p->sig_code[i] = 0; p->sig_value[i] = 0; }
    p->alt_sp = 0; p->alt_size = 0;
    p->uid = 0;
    p->gid = 0;
    p->start_ms = 0;
    p->cwd = -1;
    p->umask_bits = 022;
    for (int i = 0; i < PROC_RLIMIT_N; ++i) p->rl_cur[i] = p->rl_max[i] = PROC_RLIM_INF;
    p->rl_cur[PROC_RLIMIT_NOFILE] = p->rl_max[PROC_RLIMIT_NOFILE] = FD_DEFAULT_LIMIT;
    p->rl_cur[PROC_RLIMIT_STACK] = p->rl_max[PROC_RLIMIT_STACK] = 65536;
    p->rl_cur[PROC_RLIMIT_CORE] = p->rl_max[PROC_RLIMIT_CORE] = 0;
    p->cpu_user_ms = p->cpu_sys_ms = p->child_user_ms = p->child_sys_ms = p->ctx_switches = 0;
    p->in_syscall = 0; p->cpu_mask = 0; p->sig_serial = 0;
    p->args[0] = '\0';
}

static int valid_state(int state)
{
    return state == PROCESS_READY || state == PROCESS_RUNNING ||
           state == PROCESS_SLEEPING || state == PROCESS_ZOMBIE;
}

static int find_slot_locked(int pid)
{
    if (proc_hint < proc_n && PROC(proc_hint).used && PROC(proc_hint).pid == pid) return proc_hint;
    for (int i = 0; i < proc_n; ++i)
        if (PROC(i).used && PROC(i).pid == pid)
        {
            proc_hint = i;
            return i;
        }
    return -1;
}

/* A free record, allocating a new one when every existing record is in use. -1 only when out of memory. */
static int slot_alloc_locked()
{
    for (int i = 0; i < proc_n; ++i)
        if (!PROC(i).used) return i;
    if (proc_n == proc_cap)
    {
        int ncap = proc_cap ? proc_cap * 2 : 64;
        Process** nt = (Process**)proc_alloc((uint64_t)ncap * sizeof(Process*));
        if (!nt) return -1;
        for (int i = 0; i < proc_n; ++i) nt[i] = proc_tab[i];
        if (proc_tab) proc_free(proc_tab);
        proc_tab = nt;
        proc_cap = ncap;
    }
    Process* np = (Process*)proc_alloc(sizeof(Process));
    if (!np) return -1;
    uint8_t* b = (uint8_t*)np;
    for (uint64_t i = 0; i < sizeof(Process); ++i) b[i] = 0;
    np->pid = -1;
    proc_tab[proc_n] = np;
    return proc_n++;
}

static void print_int(int value)
{
    if (value == 0) { uart_putc('0'); return; }
    if (value < 0) { uart_putc('-'); value = -value; }
    char b[16]; int p = 0;
    while (value > 0 && p < (int)sizeof(b)) { b[p++] = (char)('0' + value % 10); value /= 10; }
    while (p) uart_putc(b[--p]);
}

void process_init()
{
    uint64_t flags = process_irq_save();
    next_pid = 0;
    proc_hint = 0;
    for (int i = 0; i < proc_n; ++i) { PROC(i).used = 0; PROC(i).pid = -1; PROC(i).state = PROCESS_UNUSED; }
    process_irq_restore(flags);

    int kernel_pid = process_create("kernel");
    int shell_pid = process_create("shell");
    if (kernel_pid >= 0)
        (void)process_set_state(kernel_pid, PROCESS_RUNNING);
    (void)shell_pid;
    uart_puts("Process manager initialized.\r\n");
}

int process_create(const char* name)
{
    if (name == 0 || name[0] == '\0')
        return -1;

    uint64_t flags = process_irq_save();
    /* Keep PIDs monotonic and avoid signed overflow. We intentionally stop
       allocating before INT_MAX rather than wrap to a negative/duplicate ID. */
    if (next_pid < 0) next_pid = 0;
    int slot = slot_alloc_locked();
    if (slot < 0)
    {
        process_irq_restore(flags);
        return -1;
    }
    /* Pids are handed out in increasing order; after PROC_PID_LIMIT the counter wraps and skips pids still in use. */
    for (int tries = 0; ; ++tries)
    {
        if (next_pid >= PROC_PID_LIMIT) next_pid = 2;
        if (find_slot_locked(next_pid) < 0) break;
        ++next_pid;
        if (tries > PROC_PID_LIMIT) { process_irq_restore(flags); return -1; }
    }

    Process* p = &PROC(slot);
    p->used = 1;
    p->pid = next_pid++;
    p->state = PROCESS_READY;
    p->kind = PROCESS_KERNEL;
    p->exit_code = 0;
    p->user_low = 0;
    p->user_high = 0;
    p->context_valid = 0;
    for (int r = 0; r < 31; ++r) p->context.x[r] = 0;
    p->context.elr = 0;
    p->context.spsr = 0;
    p->context.user_sp = 0;
    str_copy(p->name, name, PROCESS_NAME_SIZE);
    reset_mt_fields(p);
    int pid = p->pid;
    process_irq_restore(flags);
    return pid;
}

int process_configure_user(int pid, uint64_t low, uint64_t high)
{
    /* This kernel currently supports one fixed EL0 virtual window. Do not let
       metadata claim arbitrary physical/kernel memory is a valid user range. */
    if (low < MMU_USER_BASE || high > MMU_USER_STACK_TOP || low >= high ||
        (low & (MMU_PAGE_SIZE - 1ULL)) != 0 ||
        (high & (MMU_PAGE_SIZE - 1ULL)) != 0)
        return -1;

    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    if (slot < 0 || PROC(slot).state == PROCESS_ZOMBIE)
    {
        process_irq_restore(flags);
        return -1;
    }
    PROC(slot).kind = PROCESS_USER;
    PROC(slot).user_low = low;
    PROC(slot).user_high = high;
    process_irq_restore(flags);
    return 0;
}

int process_mark_exit(int pid, int code)
{
    if (pid == 0 || pid == 1)
        return -2; /* Built-in kernel and shell records are permanent. */

    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    if (slot < 0)
    {
        process_irq_restore(flags);
        return -1;
    }
    PROC(slot).exit_code = code;
    PROC(slot).state = PROCESS_ZOMBIE;
    {   /* RUSAGE_CHILDREN: the parent accumulates what this process and its own waited children used */
        int ps = PROC(slot).ppid >= 0 ? find_slot_locked(PROC(slot).ppid) : -1;
        if (ps >= 0)
        {
            PROC(ps).child_user_ms += PROC(slot).cpu_user_ms + PROC(slot).child_user_ms;
            PROC(ps).child_sys_ms += PROC(slot).cpu_sys_ms + PROC(slot).child_sys_ms;
        }
    }
    process_irq_restore(flags);
    return 0;
}

int process_is_user(int pid)
{
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    int result = slot >= 0 && PROC(slot).kind == PROCESS_USER;
    process_irq_restore(flags);
    return result;
}

uint64_t process_user_low(int pid)
{
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    uint64_t result = slot >= 0 ? PROC(slot).user_low : 0;
    process_irq_restore(flags);
    return result;
}

uint64_t process_user_high(int pid)
{
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    uint64_t result = slot >= 0 ? PROC(slot).user_high : 0;
    process_irq_restore(flags);
    return result;
}

int process_exit_code(int pid)
{
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    int result = slot >= 0 ? PROC(slot).exit_code : -1;
    process_irq_restore(flags);
    return result;
}

int process_save_context(int pid, const ProcessCpuContext* context)
{
    if (context == 0) return -1;
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    if (slot < 0 || PROC(slot).kind != PROCESS_USER ||
        PROC(slot).state == PROCESS_ZOMBIE)
    {
        process_irq_restore(flags);
        return -1;
    }
    for (int r = 0; r < 31; ++r) PROC(slot).context.x[r] = context->x[r];
    PROC(slot).context.elr = context->elr;
    PROC(slot).context.spsr = context->spsr;
    PROC(slot).context.user_sp = context->user_sp;
    PROC(slot).context_valid = 1;
    process_irq_restore(flags);
    return 0;
}

int process_load_context(int pid, ProcessCpuContext* context)
{
    if (context == 0) return -1;
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    if (slot < 0 || !PROC(slot).context_valid)
    {
        process_irq_restore(flags);
        return -1;
    }
    for (int r = 0; r < 31; ++r) context->x[r] = PROC(slot).context.x[r];
    context->elr = PROC(slot).context.elr;
    context->spsr = PROC(slot).context.spsr;
    context->user_sp = PROC(slot).context.user_sp;
    process_irq_restore(flags);
    return 0;
}

int process_context_valid(int pid)
{
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    int valid = slot >= 0 && PROC(slot).context_valid;
    process_irq_restore(flags);
    return valid;
}

int process_destroy(int pid)
{
    if (pid == 0 || pid == 1)
        return -2;

    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    if (slot < 0)
    {
        process_irq_restore(flags);
        return -1;
    }
    if (PROC(slot).state == PROCESS_RUNNING)
    {
        process_irq_restore(flags);
        return -3; /* A running record must be stopped before reclamation. */
    }

    PROC(slot).used = 0;
    PROC(slot).pid = -1;
    PROC(slot).state = PROCESS_UNUSED;
    PROC(slot).kind = PROCESS_KERNEL;
    PROC(slot).exit_code = 0;
    PROC(slot).user_low = 0;
    PROC(slot).user_high = 0;
    PROC(slot).context_valid = 0;
    for (int r = 0; r < 31; ++r) PROC(slot).context.x[r] = 0;
    PROC(slot).context.elr = 0;
    PROC(slot).context.spsr = 0;
    PROC(slot).context.user_sp = 0;
    PROC(slot).name[0] = '\0';
    reset_mt_fields(&PROC(slot));
    process_irq_restore(flags);
    return 0;
}

int process_count()
{
    uint64_t flags = process_irq_save();
    int count = 0;
    for (int i = 0; i < proc_n; ++i)
        if (PROC(i).used) ++count;
    process_irq_restore(flags);
    return count;
}

int process_slot_count() { return proc_n; }

int process_pid_at(int slot)
{
    if (slot < 0 || slot >= proc_n)
        return -1;
    uint64_t flags = process_irq_save();
    int result = PROC(slot).used ? PROC(slot).pid : -1;
    process_irq_restore(flags);
    return result;
}

int process_set_state(int pid, int state)
{
    if (!valid_state(state))
        return -1;

    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    if (slot < 0 || PROC(slot).state == PROCESS_ZOMBIE)
    {
        process_irq_restore(flags);
        return -1;
    }

    if (state == PROCESS_RUNNING)
    {
        /* There is one CPU and no process context switch yet: never expose
           multiple RUNNING records in the process table. This changes metadata
           only; it does NOT claim to switch CPU context. */
        for (int i = 0; i < proc_n; ++i)
            if (i != slot && PROC(i).used && PROC(i).state == PROCESS_RUNNING)
                PROC(i).state = PROCESS_READY;
    }
    PROC(slot).state = state;
    process_irq_restore(flags);
    return 0;
}

int process_get_state(int pid)
{
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    int result = slot >= 0 ? PROC(slot).state : -1;
    process_irq_restore(flags);
    return result;
}

void process_list()
{
    uint64_t flags = process_irq_save();
    uart_puts("PID   STATE      TYPE   NAME\r\n-----------------------------\r\n");
    for (int i = 0; i < proc_n; ++i)
    {
        if (!PROC(i).used) continue;
        print_int(PROC(i).pid); uart_puts("     ");
        switch (PROC(i).state)
        {
            case PROCESS_READY: uart_puts("READY      "); break;
            case PROCESS_RUNNING: uart_puts("RUNNING    "); break;
            case PROCESS_SLEEPING: uart_puts("SLEEPING   "); break;
            case PROCESS_ZOMBIE: uart_puts("ZOMBIE     "); break;
            default: uart_puts("INVALID    "); break;
        }
        uart_puts(PROC(i).kind == PROCESS_USER ? "USER   " : "KERNEL ");
        uart_puts(PROC(i).name);
        uart_puts("\r\n");
    }
    process_irq_restore(flags);
}


/* ---- multitasking metadata ------------------------------------------------ */

#define PROC_GET(expr, fallback) \
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid); \
    __typeof__(fallback) r = slot >= 0 ? (expr) : (fallback); process_irq_restore(f); return r;

int process_set_parent(int pid, int ppid)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) PROC(slot).ppid = ppid;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_parent(int pid) { PROC_GET(PROC(slot).ppid, -1) }
int process_set_thread(int pid, int thread)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) PROC(slot).thread = thread;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_thread(int pid) { PROC_GET(PROC(slot).thread, -1) }
int process_set_args(int pid, const char* args)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) str_copy(PROC(slot).args, args ? args : "", PROCESS_ARGS_SIZE);
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
const char* process_args(int pid)
{
    int slot = find_slot_locked(pid);
    return slot >= 0 ? PROC(slot).args : "";
}
const char* process_name(int pid)
{
    int slot = find_slot_locked(pid);
    return slot >= 0 ? PROC(slot).name : "";
}
int process_set_name(int pid, const char* name)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) str_copy(PROC(slot).name, name, PROCESS_NAME_SIZE);
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_set_term_signal(int pid, int sig)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) PROC(slot).term_signal = sig;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_term_signal(int pid) { PROC_GET(PROC(slot).term_signal, 0) }
int process_set_background(int pid, int bg)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) PROC(slot).background = bg;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_is_background(int pid) { PROC_GET(PROC(slot).background, 0) }
int process_set_stopped(int pid, int v)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) PROC(slot).stopped = v;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_is_stopped(int pid) { PROC_GET(PROC(slot).stopped, 0) }
int process_sig_raise_info(int pid, int sig, int sender, int code, uint64_t value)
{
    if (sig <= 0 || sig >= 32) return -22;
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0 && PROC(slot).state != PROCESS_ZOMBIE
        && !(PROC(slot).sig_handler[sig] == 1 && sig != 9 && sig != 19))   /* ignored signals are discarded */
    {
        PROC(slot).sig_pending |= (1U << sig); ++PROC(slot).sig_serial;
        PROC(slot).sig_sender[sig] = sender; PROC(slot).sig_code[sig] = code; PROC(slot).sig_value[sig] = value;
    }
    process_irq_restore(f);
    return slot >= 0 ? 0 : -3;
}
int process_sig_raise(int pid, int sig) { return process_sig_raise_info(pid, sig, 0, 0, 0); }
/* Record siginfo for a thread-directed signal (the pending bit lives with the thread). */
int process_sig_note(int pid, int sig, int sender, int code, uint64_t value)
{
    if (sig <= 0 || sig >= 32) return -22;
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) { ++PROC(slot).sig_serial; PROC(slot).sig_sender[sig] = sender; PROC(slot).sig_code[sig] = code; PROC(slot).sig_value[sig] = value; }
    process_irq_restore(f);
    return slot >= 0 ? 0 : -3;
}
int process_sig_info_get(int pid, int sig, int* sender, int* code, uint64_t* value)
{
    if (sig <= 0 || sig >= 32) return -22;
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) { if (sender) *sender = PROC(slot).sig_sender[sig]; if (code) *code = PROC(slot).sig_code[sig]; if (value) *value = PROC(slot).sig_value[sig]; }
    process_irq_restore(f); return slot >= 0 ? 0 : -3;
}
int process_sig_take_set(int pid, uint32_t set)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    int sig = 0;
    if (slot >= 0)
    {
        uint32_t ready = PROC(slot).sig_pending & set;
        for (int i = 1; ready && i < 32; ++i)
            if (ready & (1U << i)) { sig = i; PROC(slot).sig_pending &= ~(1U << i); break; }
    }
    process_irq_restore(f); return sig;
}
int process_altstack_set(int pid, uint64_t sp, uint64_t size)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) { PROC(slot).alt_sp = sp; PROC(slot).alt_size = size; }
    process_irq_restore(f); return slot >= 0 ? 0 : -3;
}
int process_altstack_get(int pid, uint64_t* sp, uint64_t* size)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) { *sp = PROC(slot).alt_sp; *size = PROC(slot).alt_size; }
    process_irq_restore(f); return slot >= 0 ? 0 : -3;
}
uint64_t process_itimer_get(int pid, uint64_t now, uint64_t* interval_ms)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    uint64_t rem = 0;
    if (slot >= 0) { if (PROC(slot).alarm_at > now) rem = PROC(slot).alarm_at - now; if (interval_ms) *interval_ms = PROC(slot).alarm_interval; }
    process_irq_restore(f); return rem;
}
uint64_t process_itimer_set(int pid, uint64_t value_ms, uint64_t interval_ms, uint64_t now)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    uint64_t rem = 0;
    if (slot >= 0)
    {
        if (PROC(slot).alarm_at > now) rem = PROC(slot).alarm_at - now;
        PROC(slot).alarm_at = value_ms ? now + value_ms : 0;
        PROC(slot).alarm_interval = interval_ms;
    }
    process_irq_restore(f); return rem;
}
#define SIG_UNBLOCKABLE ((1U << 9) | (1U << 19))
/* Process-directed signals not blocked by `mask` (the calling thread's mask): the lowest one is returned and cleared, or 0. */
int process_sig_take_mask(int pid, uint32_t mask)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    int sig = 0;
    if (slot >= 0)
    {
        uint32_t ready = PROC(slot).sig_pending & (~mask | SIG_UNBLOCKABLE);
        for (int i = 1; ready && i < 32; ++i)
            if (ready & (1U << i)) { sig = i; PROC(slot).sig_pending &= ~(1U << i); break; }
    }
    process_irq_restore(f); return sig;
}
uint32_t process_sig_pending_mask(int pid, uint32_t mask)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    uint32_t v = slot >= 0 ? (PROC(slot).sig_pending & (~mask | SIG_UNBLOCKABLE)) : 0U;
    process_irq_restore(f); return v;
}
uint32_t process_sig_pending_raw(int pid) { PROC_GET(PROC(slot).sig_pending, 0U) }
int process_sig_set_action(int pid, int sig, uint64_t handler, uint32_t amask, uint32_t flags, uint64_t restorer, uint64_t* old)
{
    if (sig <= 0 || sig >= 32 || sig == 9 || sig == 19) return -22;
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0)
    {
        if (old) *old = PROC(slot).sig_handler[sig];
        PROC(slot).sig_handler[sig] = handler;
        PROC(slot).sig_amask[sig] = amask & ~SIG_UNBLOCKABLE;
        PROC(slot).sig_flags[sig] = flags;
        if (restorer) PROC(slot).sig_restorer = restorer;
        if (handler == 1) PROC(slot).sig_pending &= ~(1U << sig);   /* ignoring discards pending */
    }
    process_irq_restore(f); return slot >= 0 ? 0 : -3;
}
uint64_t process_sig_handler(int pid, int sig, uint32_t* amask, uint32_t* flags)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    uint64_t h = 0;
    if (slot >= 0 && sig > 0 && sig < 32)
    {
        h = PROC(slot).sig_handler[sig];
        if (amask) *amask = PROC(slot).sig_amask[sig];
        if (flags) *flags = PROC(slot).sig_flags[sig];
    }
    process_irq_restore(f); return h;
}
uint64_t process_sig_restorer(int pid) { PROC_GET(PROC(slot).sig_restorer, 0ULL) }
/* Threads of a process: a live count, and the exit_group state so that the last thread out finishes the teardown. */
int process_thread_adjust(int pid, int delta)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    int n = -1;
    if (slot >= 0) { PROC(slot).nthreads += delta; if (PROC(slot).nthreads < 0) PROC(slot).nthreads = 0; n = PROC(slot).nthreads; }
    process_irq_restore(f); return n;
}
int process_thread_count(int pid) { PROC_GET(PROC(slot).nthreads, 0) }
int process_begin_exit(int pid, int code, int sig)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    int first = 0;
    if (slot >= 0 && !PROC(slot).exiting) { PROC(slot).exiting = 1; PROC(slot).exit_pending_code = code; PROC(slot).exit_pending_sig = sig; first = 1; }
    process_irq_restore(f); return first;
}
int process_is_exiting(int pid) { PROC_GET(PROC(slot).exiting, 0) }
int process_exit_pending(int pid, int* code, int* sig)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) { if (code) *code = PROC(slot).exit_pending_code; if (sig) *sig = PROC(slot).exit_pending_sig; }
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
/* ---- resource limits, CPU accounting, CPU affinity ---- */
int process_rlimit_get(int pid, int res, uint64_t* cur, uint64_t* max)
{
    if (res < 0 || res >= PROC_RLIMIT_N) return -22;
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) { if (cur) *cur = PROC(slot).rl_cur[res]; if (max) *max = PROC(slot).rl_max[res]; }
    process_irq_restore(f); return slot >= 0 ? 0 : -3;
}
uint64_t process_rlimit_cur(int pid, int res)
{
    uint64_t c = PROC_RLIM_INF;
    if (res >= 0 && res < PROC_RLIMIT_N && process_rlimit_get(pid, res, &c, 0) != 0) c = PROC_RLIM_INF;
    return c;
}
/* raising the hard limit needs root; the soft limit may not exceed the hard limit */
int process_rlimit_set(int pid, int res, uint64_t cur, uint64_t max, int privileged)
{
    if (res < 0 || res >= PROC_RLIMIT_N || cur > max) return -22;
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid); int rc = 0;
    if (slot < 0) rc = -3;
    else if (max > PROC(slot).rl_max[res] && !privileged) rc = -1;
    else if (res == PROC_RLIMIT_NOFILE && max > FD_MAX_DESCRIPTORS && max != PROC_RLIM_INF) rc = -22;
    else { PROC(slot).rl_cur[res] = cur; PROC(slot).rl_max[res] = max; }
    process_irq_restore(f); return rc;
}
int process_rlimit_inherit(int parent, int child)
{
    uint64_t f = process_irq_save(); int ps = find_slot_locked(parent); int cs = find_slot_locked(child);
    if (ps >= 0 && cs >= 0)
    {
        for (int i = 0; i < PROC_RLIMIT_N; ++i) { PROC(cs).rl_cur[i] = PROC(ps).rl_cur[i]; PROC(cs).rl_max[i] = PROC(ps).rl_max[i]; }
        PROC(cs).cpu_mask = PROC(ps).cpu_mask;
    }
    process_irq_restore(f); return (ps >= 0 && cs >= 0) ? 0 : -1;
}
void process_set_in_syscall(int pid, int v)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) PROC(slot).in_syscall = v;
    process_irq_restore(f);
}
/* One timer tick of `ms` milliseconds on the CPU the process is running on.  Enforces RLIMIT_CPU: SIGXCPU (24) once the soft
   limit (seconds) is reached and again every second, SIGKILL at the hard limit. */
void process_account_tick(int pid, uint64_t ms)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0 && PROC(slot).state != PROCESS_ZOMBIE)
    {
        Process* p = &PROC(slot);
        uint64_t before = (p->cpu_user_ms + p->cpu_sys_ms) / 1000U;
        if (p->in_syscall) p->cpu_sys_ms += ms; else p->cpu_user_ms += ms;
        uint64_t total_s = (p->cpu_user_ms + p->cpu_sys_ms) / 1000U;
        if (total_s != before)
        {
            if (p->rl_max[PROC_RLIMIT_CPU] != PROC_RLIM_INF && total_s >= p->rl_max[PROC_RLIMIT_CPU]) p->sig_pending |= (1U << 9);
            else if (p->rl_cur[PROC_RLIMIT_CPU] != PROC_RLIM_INF && total_s >= p->rl_cur[PROC_RLIMIT_CPU] && !(p->sig_handler[24] == 1)) p->sig_pending |= (1U << 24);
        }
    }
    process_irq_restore(f);
}
int process_rusage(int pid, int children, uint64_t out[4])
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0)
    {
        out[0] = children ? PROC(slot).child_user_ms : PROC(slot).cpu_user_ms;
        out[1] = children ? PROC(slot).child_sys_ms : PROC(slot).cpu_sys_ms;
        out[2] = 0; out[3] = PROC(slot).ctx_switches;
    }
    process_irq_restore(f); return slot >= 0 ? 0 : -3;
}
void process_count_switch(int pid)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) ++PROC(slot).ctx_switches;
    process_irq_restore(f);
}
uint32_t process_sig_serial(int pid) { PROC_GET(PROC(slot).sig_serial, 0U) }
uint32_t process_cpu_mask(int pid) { PROC_GET(PROC(slot).cpu_mask, 0U) }
int process_set_cpu_mask(int pid, uint32_t mask)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) PROC(slot).cpu_mask = mask;
    process_irq_restore(f); return slot >= 0 ? 0 : -3;
}

int process_sig_inherit(int parent, int child)
{
    uint64_t f = process_irq_save(); int ps = find_slot_locked(parent); int cs = find_slot_locked(child);
    if (ps >= 0 && cs >= 0)
    {
        for (int i = 0; i < 32; ++i) { PROC(cs).sig_handler[i] = PROC(ps).sig_handler[i]; PROC(cs).sig_amask[i] = PROC(ps).sig_amask[i]; PROC(cs).sig_flags[i] = PROC(ps).sig_flags[i]; }
        PROC(cs).sig_restorer = PROC(ps).sig_restorer;
        PROC(cs).alarm_at = 0; PROC(cs).alarm_interval = 0;   /* alarms are not inherited */
        PROC(cs).alt_sp = PROC(ps).alt_sp; PROC(cs).alt_size = PROC(ps).alt_size;
        PROC(cs).sig_pending = 0;
    }
    process_irq_restore(f); return (ps >= 0 && cs >= 0) ? 0 : -1;
}
int process_sig_reset_on_exec(int pid)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0)
        for (int i = 0; i < 32; ++i)
            if (PROC(slot).sig_handler[i] > 1) { PROC(slot).sig_handler[i] = 0; PROC(slot).sig_flags[i] = 0; PROC(slot).sig_amask[i] = 0; }
    if (slot >= 0) { PROC(slot).alt_sp = 0; PROC(slot).alt_size = 0; }
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_sig_clear_all(int pid)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) PROC(slot).sig_pending = 0;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
uint64_t process_alarm_set(int pid, uint64_t ms, uint64_t now)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    uint64_t rem = 0;
    if (slot >= 0)
    {
        if (PROC(slot).alarm_at > now) rem = PROC(slot).alarm_at - now;
        PROC(slot).alarm_at = ms ? now + ms : 0;
        PROC(slot).alarm_interval = 0;
    }
    process_irq_restore(f); return rem;
}
uint64_t process_alarm_deadline(int pid)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    uint64_t at = slot >= 0 ? PROC(slot).alarm_at : 0;
    process_irq_restore(f); return at;
}
void process_alarm_check(int pid, uint64_t now)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0 && PROC(slot).alarm_at && now >= PROC(slot).alarm_at)
    {
        PROC(slot).alarm_at = PROC(slot).alarm_interval ? now + PROC(slot).alarm_interval : 0;
        PROC(slot).sig_pending |= (1U << 14);
        PROC(slot).sig_sender[14] = 0; PROC(slot).sig_code[14] = 0x80; PROC(slot).sig_value[14] = 0;   /* SI_TIMER-ish */
    }
    process_irq_restore(f);
}
int process_cwd(int pid) { PROC_GET(PROC(slot).cwd, -1) }
int process_set_cwd(int pid, int node)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) PROC(slot).cwd = node;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
uint32_t process_umask(int pid) { PROC_GET(PROC(slot).umask_bits, 022U) }
int process_set_umask(int pid, uint32_t m)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) PROC(slot).umask_bits = m & 0777U;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}

int process_set_creds(int pid, uint32_t uid, uint32_t gid)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) { PROC(slot).uid = uid; PROC(slot).gid = gid; }
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
uint32_t process_uid(int pid) { PROC_GET(PROC(slot).uid, 0U) }
uint32_t process_gid(int pid) { PROC_GET(PROC(slot).gid, 0U) }
void process_set_start(int pid, uint64_t ms)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) PROC(slot).start_ms = ms;
    process_irq_restore(f);
}
uint64_t process_start_ms(int pid) { PROC_GET(PROC(slot).start_ms, 0ULL) }

int process_has_children(int parent)
{
    uint64_t f = process_irq_save(); int n = 0;
    for (int i = 0; i < proc_n; ++i)
        if (PROC(i).used && PROC(i).ppid == parent && PROC(i).pid != parent) ++n;
    process_irq_restore(f); return n;
}
/* wanted < 0: any child. Returns child pid or -1. */
int process_find_zombie_child(int parent, int wanted)
{
    uint64_t f = process_irq_save(); int found = -1;
    for (int i = 0; i < proc_n; ++i)
        if (PROC(i).used && PROC(i).ppid == parent && PROC(i).state == PROCESS_ZOMBIE &&
            (wanted < 0 || PROC(i).pid == wanted)) { found = PROC(i).pid; break; }
    process_irq_restore(f); return found;
}
int process_is_child_of(int parent, int pid)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    int r = slot >= 0 && PROC(slot).ppid == parent;
    process_irq_restore(f); return r;
}
void process_reparent_children(int from, int to)
{
    uint64_t f = process_irq_save();
    for (int i = 0; i < proc_n; ++i)
        if (PROC(i).used && PROC(i).ppid == from && PROC(i).pid != from) PROC(i).ppid = to;
    process_irq_restore(f);
}
/* Re-reads the n-th used slot's pid starting from an index, for iteration. */
int process_pid_at_index(int index) { return process_pid_at(index); }
