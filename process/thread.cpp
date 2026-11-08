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

#include "thread.h"
#include "sched_policy.h"
#include "timer.h"
#include "uart.h"

static SchedEntry sched[THREAD_MAX];
static uint64_t saved_sp[THREAD_MAX];
static char names[THREAD_MAX][16];
static uint64_t run_ticks[THREAD_MAX];
static uint64_t run_count[THREAD_MAX];

static uint8_t stacks[THREAD_MAX][THREAD_STACK_SIZE]
    __attribute__((aligned(16)));

static int current = 0;
static int initialised = 0;
static int preempt_on = 0;
static volatile int need_resched = 0;
static int slice_left = THREAD_SLICE_TICKS;
static uint64_t switches = 0;
static int tpid[THREAD_MAX];
static uint64_t saved_sp0[THREAD_MAX];   /* user stack pointer: SP_EL0 is not part of the kernel context */
static int user_task_count = 0;

extern "C" void task_context_enter(int pid);

static uint64_t irq_save()
{
    uint64_t daif;

    asm volatile(
        "mrs %0, daif\n"
        "msr daifset, #0x2\n"
        "isb"
        : "=r"(daif) : : "memory");

    return daif;
}

static void irq_restore(uint64_t daif)
{
    asm volatile(
        "msr daif, %0\n"
        "isb"
        :: "r"(daif) : "memory");
}

static void copy_name(char* dst, const char* src)
{
    int i = 0;

    while (src != 0 && src[i] != 0 && i < 15)
    {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = 0;
}

static void print_u64(uint64_t value)
{
    char buf[24];
    int n = 0;

    if (value == 0)
        buf[n++] = '0';

    while (value != 0 && n < 23)
    {
        buf[n++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }

    while (n > 0)
        uart_putc(buf[--n]);
}

void thread_init()
{
    uint64_t flags = irq_save();

    for (int i = 0; i < THREAD_MAX; ++i)
    {
        sched[i].state = SCHED_FREE;
        sched[i].priority = 1;
        sched[i].waited = 0;
        sched[i].wake_ms = 0;
        saved_sp[i] = 0;
        names[i][0] = 0;
        run_ticks[i] = 0;
        run_count[i] = 0;
        tpid[i] = -1;
    }
    user_task_count = 0;

    /* Adopt the running boot context (the shell) as thread 0. */
    sched[0].state = SCHED_RUNNING;
    copy_name(names[0], "main");
    run_count[0] = 1;

    current = 0;
    preempt_on = 0;
    need_resched = 0;
    slice_left = THREAD_SLICE_TICKS;
    switches = 0;
    initialised = 1;

    irq_restore(flags);
}

/* Must be called with IRQs masked. */
static void schedule()
{
    (void)sched_wake_sleepers(sched, THREAD_MAX, timer_millis());

    int next = sched_pick(sched, THREAD_MAX, current);

    need_resched = 0;
    slice_left = THREAD_SLICE_TICKS;

    if (next < 0 || next == current)
        return;

    sched_account(sched, THREAD_MAX, next);

    if (sched[current].state == SCHED_RUNNING)
        sched[current].state = SCHED_READY;

    sched[next].state = SCHED_RUNNING;

    int prev = current;
    current = next;
    ++switches;
    ++run_count[next];

    asm volatile("mrs %0, sp_el0" : "=r"(saved_sp0[prev]));
    thread_context_switch(&saved_sp[prev], saved_sp[next]);
    asm volatile("msr sp_el0, %0" :: "r"(saved_sp0[current]));

    /* We are the resumed thread again: re-establish our address space. */
    task_context_enter(tpid[current]);
}

static int thread_create_inner(const char* name, ThreadEntry entry, void* arg, uint32_t priority, int pid)
{
    if (!initialised || entry == 0)
        return -1;

    if (priority > SCHED_PRIO_MAX)
        priority = SCHED_PRIO_MAX;

    uint64_t flags = irq_save();
    int slot = -1;

    for (int i = 1; i < THREAD_MAX; ++i)
    {
        if (sched[i].state == SCHED_FREE || sched[i].state == SCHED_DEAD)
        {
            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        irq_restore(flags);
        return -1;
    }

    /* Initial frame consumed by thread_context_switch: x19..x30 (12 regs). */
    uint64_t top = (uint64_t)&stacks[slot][THREAD_STACK_SIZE];
    uint64_t* frame = (uint64_t*)(top - 96ULL);

    for (int i = 0; i < 12; ++i)
        frame[i] = 0;

    frame[0] = (uint64_t)entry;           /* x19 */
    frame[1] = (uint64_t)arg;             /* x20 */
    frame[11] = (uint64_t)thread_trampoline; /* x30 */

    saved_sp[slot] = (uint64_t)frame;
    copy_name(names[slot], name);
    run_ticks[slot] = 0;
    run_count[slot] = 0;

    sched[slot].priority = priority;
    sched[slot].waited = 0;
    sched[slot].wake_ms = 0;
    tpid[slot] = pid;
    sched[slot].state = SCHED_READY;

    irq_restore(flags);

    return slot;
}

int thread_create(const char* name, ThreadEntry entry, void* arg, uint32_t priority)
{
    return thread_create_inner(name, entry, arg, priority, -1);
}

int thread_create_for_pid(const char* name, ThreadEntry entry, void* arg, uint32_t priority, int pid)
{
    return thread_create_inner(name, entry, arg, priority, pid);
}

uint64_t thread_stack_top_current() { return (uint64_t)&stacks[current][THREAD_STACK_SIZE]; }

int thread_pid(int t) { return (t >= 0 && t < THREAD_MAX) ? tpid[t] : -1; }
int thread_current_pid() { return tpid[current]; }
int thread_state_of(int t) { return (t >= 0 && t < THREAD_MAX) ? sched[t].state : SCHED_FREE; }

int thread_kill(int t)
{
    if (t <= 0 || t >= THREAD_MAX || t == current) return -1;
    uint64_t flags = irq_save();
    sched[t].state = SCHED_DEAD;
    tpid[t] = -1;
    irq_restore(flags);
    return 0;
}

void thread_set_user_tasks(int count) { user_task_count = count < 0 ? 0 : count; }
int thread_user_tasks() { return user_task_count; }

void thread_wait_ms(uint64_t ms)
{
    if (current == 0) { thread_sleep_ms(ms); return; }
    thread_sleep_ms(ms);
}

extern "C" void thread_user_preempt()
{
    if (!initialised || !need_resched) return;
    schedule();
}

void thread_yield()
{
    if (!initialised)
        return;

    uint64_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void thread_sleep_ms(uint64_t ms)
{
    if (!initialised)
    {
        timer_delay_ms(ms);
        return;
    }

    uint64_t wake = timer_millis() + ms;

    if (current == 0)
    {
        /* The boot/shell thread is the idle thread of last resort and must
           stay runnable: wait by yielding instead of sleeping. */
        while (timer_millis() < wake)
            thread_yield();

        return;
    }

    uint64_t flags = irq_save();

    sched[current].wake_ms = wake;
    sched[current].state = SCHED_SLEEPING;
    schedule();

    irq_restore(flags);
}

int thread_current()
{
    return current;
}

void thread_set_preempt(int enabled)
{
    uint64_t flags = irq_save();

    preempt_on = enabled ? 1 : 0;
    need_resched = 0;
    slice_left = THREAD_SLICE_TICKS;

    irq_restore(flags);
}

int thread_preempt_enabled()
{
    return preempt_on;
}

void thread_timer_tick()
{
    if (!initialised)
        return;

    ++run_ticks[current];

    /* Age every waiting thread; the running one resets. */
    sched_account(sched, THREAD_MAX, current);

    if (sched_wake_sleepers(sched, THREAD_MAX, timer_millis()) > 0)
        need_resched = 1;

    if ((preempt_on || user_task_count > 0) && --slice_left <= 0)
        need_resched = 1;
}

void thread_preempt_check()
{
    /* Called with IRQs masked, after the interrupt was acknowledged and
       ended at the GIC, so switching away here cannot block the IRQ line. */
    if (!initialised || !preempt_on || !need_resched)
        return;

    schedule();
}

int thread_active_count()
{
    int count = 0;

    for (int i = 0; i < THREAD_MAX; ++i)
    {
        if (sched[i].state != SCHED_FREE && sched[i].state != SCHED_DEAD)
            ++count;
    }

    return count;
}

uint64_t thread_switch_count()
{
    return switches;
}

void thread_list()
{
    static const char* const state_names[] =
        { "free", "ready", "running", "sleeping", "dead" };

    uint64_t flags = irq_save();

    uart_puts("Threads (preempt ");
    uart_puts(preempt_on ? "on" : "off");
    uart_puts(", switches ");
    print_u64(switches);
    uart_puts(")\r\nID  NAME             STATE     PRIO  RUNS  TICKS\r\n");

    for (int i = 0; i < THREAD_MAX; ++i)
    {
        if (sched[i].state == SCHED_FREE)
            continue;

        print_u64((uint64_t)i);
        uart_puts("   ");
        uart_puts(names[i]);

        int len = 0;

        while (names[i][len] != 0)
            ++len;

        for (int pad = len; pad < 17; ++pad)
            uart_putc(' ');

        uart_puts(state_names[sched[i].state]);
        uart_puts("   ");
        print_u64(sched[i].priority);
        uart_puts("     ");
        print_u64(run_count[i]);
        uart_puts("     ");
        print_u64(run_ticks[i]);
        uart_puts("\r\n");
    }

    irq_restore(flags);
}

extern "C" void thread_entry_hook()
{
    /* Cooperative threads do not need asynchronous IRQs. Keep the IRQ mask
       inherited from schedule(); in particular, never unmask the CPU IRQ line
       as a side effect of creating/running a demo thread. */
    asm volatile("isb" ::: "memory");
    task_context_enter(tpid[current]);
}

extern "C" void thread_exit()
{
    (void)irq_save();

    sched[current].state = SCHED_DEAD;
    tpid[current] = -1;
    schedule();

    /* A dead thread is never picked again. */
    while (1)
        asm volatile("wfe");
}
