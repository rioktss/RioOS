#include "process.h"
#include "mmu.h"
#include "uart.h"
#include "string.h"

#define MAX_PROCESSES 32
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
    char name[PROCESS_NAME_SIZE];
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
        processes[i].name[0] = '\0';
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
    str_copy(p->name, name, PROCESS_NAME_SIZE);
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
    processes[slot].name[0] = '\0';
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
