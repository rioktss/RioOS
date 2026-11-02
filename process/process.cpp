#include "process.h"
#include "mmu.h"
#include "uart.h"
#include "string.h"

#define MAX_PROCESSES 48
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
    uint32_t sig_pending;
    uint32_t uid;
    uint32_t gid;
    uint64_t start_ms;
    char args[PROCESS_ARGS_SIZE];
};

static Process processes[MAX_PROCESSES];
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
    p->uid = 0;
    p->gid = 0;
    p->start_ms = 0;
    p->args[0] = '\0';
}

static int valid_state(int state)
{
    return state == PROCESS_READY || state == PROCESS_RUNNING ||
           state == PROCESS_SLEEPING || state == PROCESS_ZOMBIE;
}

static int find_slot_locked(int pid)
{
    for (int i = 0; i < MAX_PROCESSES; ++i)
        if (processes[i].used && processes[i].pid == pid)
            return i;
    return -1;
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
    for (int i = 0; i < MAX_PROCESSES; ++i)
    {
        processes[i].used = 0;
        processes[i].pid = -1;
        processes[i].state = PROCESS_UNUSED;
        processes[i].kind = PROCESS_KERNEL;
        processes[i].exit_code = 0;
        processes[i].user_low = 0;
        processes[i].user_high = 0;
        processes[i].context_valid = 0;
        for (int r = 0; r < 31; ++r) processes[i].context.x[r] = 0;
        processes[i].context.elr = 0;
        processes[i].context.spsr = 0;
        processes[i].context.user_sp = 0;
        processes[i].name[0] = '\0';
        reset_mt_fields(&processes[i]);
    }
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
    if (next_pid < 0 || next_pid >= 0x7FFFFFFF)
    {
        process_irq_restore(flags);
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < MAX_PROCESSES; ++i)
        if (!processes[i].used) { slot = i; break; }

    if (slot < 0)
    {
        process_irq_restore(flags);
        return -1;
    }

    Process* p = &processes[slot];
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
    if (slot < 0 || processes[slot].state == PROCESS_ZOMBIE)
    {
        process_irq_restore(flags);
        return -1;
    }
    processes[slot].kind = PROCESS_USER;
    processes[slot].user_low = low;
    processes[slot].user_high = high;
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
    processes[slot].exit_code = code;
    processes[slot].state = PROCESS_ZOMBIE;
    process_irq_restore(flags);
    return 0;
}

int process_is_user(int pid)
{
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    int result = slot >= 0 && processes[slot].kind == PROCESS_USER;
    process_irq_restore(flags);
    return result;
}

uint64_t process_user_low(int pid)
{
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    uint64_t result = slot >= 0 ? processes[slot].user_low : 0;
    process_irq_restore(flags);
    return result;
}

uint64_t process_user_high(int pid)
{
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    uint64_t result = slot >= 0 ? processes[slot].user_high : 0;
    process_irq_restore(flags);
    return result;
}

int process_exit_code(int pid)
{
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    int result = slot >= 0 ? processes[slot].exit_code : -1;
    process_irq_restore(flags);
    return result;
}

int process_save_context(int pid, const ProcessCpuContext* context)
{
    if (context == 0) return -1;
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    if (slot < 0 || processes[slot].kind != PROCESS_USER ||
        processes[slot].state == PROCESS_ZOMBIE)
    {
        process_irq_restore(flags);
        return -1;
    }
    for (int r = 0; r < 31; ++r) processes[slot].context.x[r] = context->x[r];
    processes[slot].context.elr = context->elr;
    processes[slot].context.spsr = context->spsr;
    processes[slot].context.user_sp = context->user_sp;
    processes[slot].context_valid = 1;
    process_irq_restore(flags);
    return 0;
}

int process_load_context(int pid, ProcessCpuContext* context)
{
    if (context == 0) return -1;
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    if (slot < 0 || !processes[slot].context_valid)
    {
        process_irq_restore(flags);
        return -1;
    }
    for (int r = 0; r < 31; ++r) context->x[r] = processes[slot].context.x[r];
    context->elr = processes[slot].context.elr;
    context->spsr = processes[slot].context.spsr;
    context->user_sp = processes[slot].context.user_sp;
    process_irq_restore(flags);
    return 0;
}

int process_context_valid(int pid)
{
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    int valid = slot >= 0 && processes[slot].context_valid;
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
    if (processes[slot].state == PROCESS_RUNNING)
    {
        process_irq_restore(flags);
        return -3; /* A running record must be stopped before reclamation. */
    }

    processes[slot].used = 0;
    processes[slot].pid = -1;
    processes[slot].state = PROCESS_UNUSED;
    processes[slot].kind = PROCESS_KERNEL;
    processes[slot].exit_code = 0;
    processes[slot].user_low = 0;
    processes[slot].user_high = 0;
    processes[slot].context_valid = 0;
    for (int r = 0; r < 31; ++r) processes[slot].context.x[r] = 0;
    processes[slot].context.elr = 0;
    processes[slot].context.spsr = 0;
    processes[slot].context.user_sp = 0;
    processes[slot].name[0] = '\0';
    reset_mt_fields(&processes[slot]);
    process_irq_restore(flags);
    return 0;
}

int process_count()
{
    uint64_t flags = process_irq_save();
    int count = 0;
    for (int i = 0; i < MAX_PROCESSES; ++i)
        if (processes[i].used) ++count;
    process_irq_restore(flags);
    return count;
}

int process_pid_at(int slot)
{
    if (slot < 0 || slot >= MAX_PROCESSES)
        return -1;
    uint64_t flags = process_irq_save();
    int result = processes[slot].used ? processes[slot].pid : -1;
    process_irq_restore(flags);
    return result;
}

int process_set_state(int pid, int state)
{
    if (!valid_state(state))
        return -1;

    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    if (slot < 0 || processes[slot].state == PROCESS_ZOMBIE)
    {
        process_irq_restore(flags);
        return -1;
    }

    if (state == PROCESS_RUNNING)
    {
        /* There is one CPU and no process context switch yet: never expose
           multiple RUNNING records in the process table. This changes metadata
           only; it does NOT claim to switch CPU context. */
        for (int i = 0; i < MAX_PROCESSES; ++i)
            if (i != slot && processes[i].used && processes[i].state == PROCESS_RUNNING)
                processes[i].state = PROCESS_READY;
    }
    processes[slot].state = state;
    process_irq_restore(flags);
    return 0;
}

int process_get_state(int pid)
{
    uint64_t flags = process_irq_save();
    int slot = find_slot_locked(pid);
    int result = slot >= 0 ? processes[slot].state : -1;
    process_irq_restore(flags);
    return result;
}

void process_list()
{
    uint64_t flags = process_irq_save();
    uart_puts("PID   STATE      TYPE   NAME\r\n-----------------------------\r\n");
    for (int i = 0; i < MAX_PROCESSES; ++i)
    {
        if (!processes[i].used) continue;
        print_int(processes[i].pid); uart_puts("     ");
        switch (processes[i].state)
        {
            case PROCESS_READY: uart_puts("READY      "); break;
            case PROCESS_RUNNING: uart_puts("RUNNING    "); break;
            case PROCESS_SLEEPING: uart_puts("SLEEPING   "); break;
            case PROCESS_ZOMBIE: uart_puts("ZOMBIE     "); break;
            default: uart_puts("INVALID    "); break;
        }
        uart_puts(processes[i].kind == PROCESS_USER ? "USER   " : "KERNEL ");
        uart_puts(processes[i].name);
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
    if (slot >= 0) processes[slot].ppid = ppid;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_parent(int pid) { PROC_GET(processes[slot].ppid, -1) }
int process_set_thread(int pid, int thread)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) processes[slot].thread = thread;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_thread(int pid) { PROC_GET(processes[slot].thread, -1) }
int process_set_args(int pid, const char* args)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) str_copy(processes[slot].args, args ? args : "", PROCESS_ARGS_SIZE);
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
const char* process_args(int pid)
{
    int slot = find_slot_locked(pid);
    return slot >= 0 ? processes[slot].args : "";
}
const char* process_name(int pid)
{
    int slot = find_slot_locked(pid);
    return slot >= 0 ? processes[slot].name : "";
}
int process_set_name(int pid, const char* name)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) str_copy(processes[slot].name, name, PROCESS_NAME_SIZE);
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_set_term_signal(int pid, int sig)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) processes[slot].term_signal = sig;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_term_signal(int pid) { PROC_GET(processes[slot].term_signal, 0) }
int process_set_background(int pid, int bg)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) processes[slot].background = bg;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_is_background(int pid) { PROC_GET(processes[slot].background, 0) }
int process_set_stopped(int pid, int v)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) processes[slot].stopped = v;
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
int process_is_stopped(int pid) { PROC_GET(processes[slot].stopped, 0) }
int process_sig_raise(int pid, int sig)
{
    if (sig <= 0 || sig >= 32) return -22;
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0 && processes[slot].state != PROCESS_ZOMBIE) processes[slot].sig_pending |= (1U << sig);
    process_irq_restore(f);
    return slot >= 0 ? 0 : -3;
}
/* Returns the lowest pending signal and clears it, or 0 when none. */
int process_sig_take(int pid)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    int sig = 0;
    if (slot >= 0 && processes[slot].sig_pending)
    {
        for (int i = 1; i < 32; ++i)
            if (processes[slot].sig_pending & (1U << i)) { sig = i; processes[slot].sig_pending &= ~(1U << i); break; }
    }
    process_irq_restore(f); return sig;
}
uint32_t process_sig_pending(int pid) { PROC_GET(processes[slot].sig_pending, 0U) }
int process_set_creds(int pid, uint32_t uid, uint32_t gid)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) { processes[slot].uid = uid; processes[slot].gid = gid; }
    process_irq_restore(f); return slot >= 0 ? 0 : -1;
}
uint32_t process_uid(int pid) { PROC_GET(processes[slot].uid, 0U) }
uint32_t process_gid(int pid) { PROC_GET(processes[slot].gid, 0U) }
void process_set_start(int pid, uint64_t ms)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    if (slot >= 0) processes[slot].start_ms = ms;
    process_irq_restore(f);
}
uint64_t process_start_ms(int pid) { PROC_GET(processes[slot].start_ms, 0ULL) }

int process_has_children(int parent)
{
    uint64_t f = process_irq_save(); int n = 0;
    for (int i = 0; i < MAX_PROCESSES; ++i)
        if (processes[i].used && processes[i].ppid == parent && processes[i].pid != parent) ++n;
    process_irq_restore(f); return n;
}
/* wanted < 0: any child. Returns child pid or -1. */
int process_find_zombie_child(int parent, int wanted)
{
    uint64_t f = process_irq_save(); int found = -1;
    for (int i = 0; i < MAX_PROCESSES; ++i)
        if (processes[i].used && processes[i].ppid == parent && processes[i].state == PROCESS_ZOMBIE &&
            (wanted < 0 || processes[i].pid == wanted)) { found = processes[i].pid; break; }
    process_irq_restore(f); return found;
}
int process_is_child_of(int parent, int pid)
{
    uint64_t f = process_irq_save(); int slot = find_slot_locked(pid);
    int r = slot >= 0 && processes[slot].ppid == parent;
    process_irq_restore(f); return r;
}
void process_reparent_children(int from, int to)
{
    uint64_t f = process_irq_save();
    for (int i = 0; i < MAX_PROCESSES; ++i)
        if (processes[i].used && processes[i].ppid == from && processes[i].pid != from) processes[i].ppid = to;
    process_irq_restore(f);
}
/* Re-reads the n-th used slot's pid starting from an index, for iteration. */
int process_pid_at_index(int index) { return process_pid_at(index); }
