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
#include "fpu.h"
#include "sched_policy.h"
#include "timer.h"
#include "uart.h"
#include "smp.h"
#include "process.h"
#include "memory.h"
#include "mmu.h"

/* Scheduler core. All state below is protected by the big kernel lock (smp_sched_on) and by masking IRQs on the local CPU, exactly as
   before; the policy itself (queues, priorities, fairness, throttling) lives in sched_policy.cpp where it is unit-tested on the host. */

struct Task
{
    SchedTask s;                 /* must stay first: Task* and SchedTask* are converted by cast */
    int id;
    int pid;                     /* process the thread belongs to (thread group), -1 for kernel threads */
    uint32_t tid;
    int on_cpu;                  /* CPU it is running on, -1 otherwise */
    uint64_t saved_sp;           /* kernel SP while switched out */
    uint64_t saved_sp0;          /* user SP (SP_EL0) */
    uint64_t tls;                /* TPIDR_EL0 */
    uint8_t* block;              /* kmalloc block that holds the stack, 0 for the boot and idle contexts */
    uint64_t guard;              /* address of the unmapped guard page */
    uint64_t stack_top;
    uint64_t wake_ms;            /* timeout of a blocked thread, 0 = none */
    int wake_reason;
    Task* snext;                 /* timed-sleep list */
    Task* sprev;
    int in_sleepers;
    int ev_wait;                 /* sleeping in thread_event_wait(): thread_event_notify() wakes it early */
    uint64_t run_ticks;
    uint64_t run_count;
    char name[16];
    FpState fp;
    uint32_t sigmask;
    uint32_t sigpend;
    uint64_t clear_tid;
    uint64_t robust_head;
    int dying;                   /* the process is exiting or this thread was told to go: it ends at the next return to user mode */
    uint64_t alt_sp, alt_size;   /* sigaltstack */
};

static Task** tbl = 0;
static int tbl_cap = 0;
static int tbl_hint = 0;
static Task task0;                               /* the boot context (shell) */
static Task* idle_task[SMP_MAX_CPUS];
static Task* cur_of[SMP_MAX_CPUS];
static Task* last_prev[SMP_MAX_CPUS];
static volatile int need_resched_of[SMP_MAX_CPUS];
static SchedRtBudget budget_of[SMP_MAX_CPUS];
static SchedRq rq;
static Task* sleepers = 0;
static int initialised = 0;
static int preempt_on = 0;
static uint64_t switches = 0;
static uint64_t rt_throttled_ticks = 0;
static int user_task_count = 0;

#define current (cur_of[smp_cpu_id_fast()])
#define need_resched (need_resched_of[smp_cpu_id_fast()])

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

static void print_pad(uint64_t v, int width)
{
    int digits = 1;
    for (uint64_t x = v; x >= 10; x /= 10) ++digits;
    print_u64(v);
    for (; digits < width; ++digits) uart_putc(' ');
}

static void print_pad_str(const char* s, int width)
{
    int n = 0;
    while (s[n]) { uart_putc(s[n]); ++n; }
    for (; n < width; ++n) uart_putc(' ');
}

static Task* task_at(int id)
{
    if (id < 0 || id >= tbl_cap) return 0;
    return tbl[id];
}

static int online_cpus()
{
    int n = (int)smp_online_cpu_count();
    if (n < 1) n = 1;
    if (!smp_sched_on) n = 1;
    return n;
}

static uint64_t online_mask()
{
    int n = (int)smp_online_cpu_count();
    if (n < 1) n = 1;
    return n >= 64 ? ~0ULL : ((1ULL << n) - 1ULL);
}

static void zero_bytes(void* p, uint64_t n)
{
    uint8_t* b = (uint8_t*)p;
    for (uint64_t i = 0; i < n; ++i) b[i] = 0;
}

/* Grows the table (doubling); called with IRQs masked. */
static int table_grow()
{
    int ncap = tbl_cap ? tbl_cap * 2 : 64;
    Task** nt = (Task**)kmalloc((uint64_t)ncap * sizeof(Task*));
    if (!nt) return -1;
    for (int i = 0; i < ncap; ++i) nt[i] = i < tbl_cap ? tbl[i] : 0;
    Task** old = tbl;
    tbl = nt;
    tbl_cap = ncap;
    if (old) kfree(old);
    return 0;
}

static int table_alloc_id()
{
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int n = 0; n < tbl_cap; ++n)
        {
            int i = (tbl_hint + n) % tbl_cap;
            if (i < SMP_MAX_CPUS) continue;          /* 0 = shell, 1..7 = idle contexts */
            if (tbl[i] == 0) { tbl_hint = i + 1; return i; }
        }
        if (pass == 0 && table_grow() != 0) return -1;
    }
    return -1;
}

static void sleepers_add(Task* t)
{
    t->sprev = 0;
    t->snext = sleepers;
    if (sleepers) sleepers->sprev = t;
    sleepers = t;
    t->in_sleepers = 1;
}

static void sleepers_del(Task* t)
{
    if (!t->in_sleepers) return;
    if (t->sprev) t->sprev->snext = t->snext; else sleepers = t->snext;
    if (t->snext) t->snext->sprev = t->sprev;
    t->snext = t->sprev = 0;
    t->in_sleepers = 0;
}

/* A task just became runnable: ask every CPU whose current task it should preempt (or that is idle) to reschedule. */
static void notify_ready(Task* t)
{
    int me = (int)smp_cpu_id_fast();
    int ncpu = online_cpus();
    uint32_t kick = 0;
    for (int c = 0; c < ncpu; ++c)
    {
        if (!sched_cpu_allowed(&t->s, c)) continue;
        Task* cc = cur_of[c];
        if (!cc || cc == t) continue;
        if (sched_wakeup_preempts(&t->s, &cc->s))
        {
            need_resched_of[c] = 1;
            if (c != me) kick |= 1U << c;
        }
    }
    if (kick) smp_kick_cpus(kick);
}

static void make_ready(Task* t, int flags)
{
    t->s.state = SCHED_READY;
    sched_enqueue(&rq, &t->s, flags);
    notify_ready(t);
}

static int expire_sleepers(uint64_t now)
{
    int woken = 0;
    Task* t = sleepers;
    while (t)
    {
        Task* nx = t->snext;
        if (t->s.state == SCHED_SLEEPING && t->wake_ms != 0 && now >= t->wake_ms)
        {
            sleepers_del(t);
            t->wake_reason = THREAD_WAKE_TIMEOUT;
            make_ready(t, SCHED_Q_WAKEUP);
            ++woken;
        }
        t = nx;
    }
    return woken;
}

/* Freed kernel stacks are kept (guard page still unmapped) for the next thread: mapping a guard page needs a TLB invalidation
   broadcast to every CPU, which is far too slow for a fork/exit loop (very much so on an emulated SMP machine). Touched only with
   IRQs masked under the big kernel lock, like the rest of the thread state. */
#define KSTACK_CACHE 64
static uint8_t* kstack_cache[KSTACK_CACHE];
static int kstack_cached = 0;

static void free_task(Task* t)
{
    if (!t || t == &task0) return;
    if (t->id >= 0 && t->id < tbl_cap && tbl[t->id] == t) tbl[t->id] = 0;
    uint8_t* block = t->block;
    uint64_t guard = t->guard;
    kfree(t);
    if (block)
    {
        if (kstack_cached < KSTACK_CACHE) { kstack_cache[kstack_cached++] = block; return; }
        mmu_kernel_guard_page(guard, 0);
        kfree(block);
    }
}

/* Runs on the stack of the task that was switched in: the one that left the CPU is off-stack now, so a dead one can be freed. */
static void finish_switch()
{
    int cpu = (int)smp_cpu_id_fast();
    Task* d = last_prev[cpu];
    last_prev[cpu] = 0;
    if (d && d->s.state == SCHED_DEAD) free_task(d);
}

void thread_params_default(ThreadParams* p)
{
    p->policy = SCHED_OTHER;
    p->rt_prio = 0;
    p->nice = 0;
    p->affinity = 0;
    p->tls = 0;
}

void thread_init()
{
    uint64_t flags = irq_save();

    tbl = 0; tbl_cap = 0; tbl_hint = SMP_MAX_CPUS;
    if (table_grow() != 0)
    {
        uart_puts("thread_init: out of memory\r\n");
        irq_restore(flags);
        return;
    }
    sched_rq_init(&rq);
    sleepers = 0;
    user_task_count = 0;
    switches = 0;
    rt_throttled_ticks = 0;
    preempt_on = 0;

    /* Adopt the running boot context (the shell) as thread 0. It stays on the boot CPU. */
    zero_bytes(&task0, sizeof task0);
    sched_task_init(&task0.s, SCHED_OTHER, 0, 0);
    task0.s.state = SCHED_RUNNING;
    task0.s.affinity = 1ULL;
    task0.id = 0;
    task0.pid = -1;
    task0.on_cpu = 0;
    copy_name(task0.name, "main");
    task0.run_count = 1;
    tbl[0] = &task0;

    for (int c = 0; c < SMP_MAX_CPUS; ++c)
    {
        need_resched_of[c] = 0;
        last_prev[c] = 0;
        sched_rt_budget_init(&budget_of[c]);
        idle_task[c] = 0;
        cur_of[c] = &task0;
        if (c == 0) continue;
        Task* it = (Task*)kmalloc(sizeof(Task));
        if (!it) continue;
        zero_bytes(it, sizeof(Task));
        sched_task_init(&it->s, SCHED_OTHER, 0, 0);
        it->s.is_idle_task = 1;
        it->s.state = SCHED_SLEEPING;            /* a CPU that has not come up yet */
        it->s.affinity = 1ULL << c;
        it->id = c;
        it->pid = -1;
        it->on_cpu = -1;
        copy_name(it->name, "idle");
        tbl[c] = it;
        idle_task[c] = it;
    }
    initialised = 1;

    irq_restore(flags);
}

/* Must be called with IRQs masked (and the kernel lock held when SMP scheduling is on). Returns 1 when it switched. */
static int schedule(int how)
{
    int cpu = (int)smp_cpu_id_fast();
    Task* prev = cur_of[cpu];

    if (sleepers) (void)expire_sleepers(timer_millis());

    Task* idle = (cpu != 0 && smp_sched_on) ? idle_task[cpu] : 0;
    SchedTask* ns = sched_schedule(&rq, cpu, &prev->s, how, !budget_of[cpu].throttled, idle ? &idle->s : 0);
    need_resched_of[cpu] = 0;

    if (ns == 0)
    {
        /* Only reachable if the shell were blocked, which it never is. */
        uart_puts("scheduler: nothing runnable\r\n");
        while (1) asm volatile("wfe");
    }
    Task* next = (Task*)ns;

    if (prev->s.queued) notify_ready(prev);          /* preempted but still runnable: another CPU may take it */
    if (next == prev) return 0;

    prev->on_cpu = -1;
    next->on_cpu = cpu;
    cur_of[cpu] = next;
    ++switches;
    ++next->run_count;

    asm volatile("mrs %0, sp_el0" : "=r"(prev->saved_sp0));
    asm volatile("mrs %0, tpidr_el0" : "=r"(prev->tls));
    if (prev->pid >= 0) fp_save(&prev->fp);              /* the live V0-V31 belong to the thread that is leaving */
    last_prev[cpu] = prev;
    thread_context_switch(&prev->saved_sp, next->saved_sp);

    /* Resumed, possibly on another CPU: `current` now names this thread on the CPU we are running on. */
    Task* me = current;
    asm volatile("msr sp_el0, %0" :: "r"(me->saved_sp0));
    asm volatile("msr tpidr_el0, %0" :: "r"(me->tls));
    if (me->pid >= 0) fp_restore(&me->fp);
    finish_switch();

    /* We are the resumed thread again: re-establish our address space. */
    task_context_enter(me->pid);
    return 1;
}

int thread_create_ex(const char* name, ThreadEntry entry, void* arg, int pid, const ThreadParams* params)
{
    if (!initialised || entry == 0)
        return -1;

    ThreadParams dp;
    if (!params) { thread_params_default(&dp); params = &dp; }
    if (!sched_params_valid(params->policy, params->rt_prio, params->nice)) return -1;

    Task* t = (Task*)kmalloc(sizeof(Task));
    if (!t) return -1;
    zero_bytes(t, sizeof(Task));

    /* kernel stack: THREAD_STACK_SIZE usable bytes above one unmapped guard page */
    uint8_t* block = 0;
    int guarded = 0;                       /* a cached stack already has its guard page unmapped */
    {
        uint64_t cf = irq_save();
        if (kstack_cached > 0) { block = kstack_cache[--kstack_cached]; guarded = 1; }
        irq_restore(cf);
    }
    if (!block) block = (uint8_t*)kmalloc((uint64_t)THREAD_STACK_SIZE + 2ULL * 4096ULL);
    if (!block) { kfree(t); return -1; }
    uint64_t guard = ((uint64_t)block + 4095ULL) & ~4095ULL;
    uint64_t lo = guard + 4096ULL;
    uint64_t top = lo + THREAD_STACK_SIZE;

    uint64_t flags = irq_save();
    int id = table_alloc_id();
    if (id < 0)
    {
        if (guarded && kstack_cached < KSTACK_CACHE) kstack_cache[kstack_cached++] = block;
        else { if (guarded) mmu_kernel_guard_page(guard, 0); kfree(block); }
        irq_restore(flags);
        kfree(t);
        return -1;
    }

    /* Initial frame consumed by thread_context_switch: x19..x30 (12 regs). */
    uint64_t* frame = (uint64_t*)(top - 96ULL);
    for (int i = 0; i < 12; ++i) frame[i] = 0;
    frame[0] = (uint64_t)entry;                 /* x19 */
    frame[1] = (uint64_t)arg;                   /* x20 */
    frame[11] = (uint64_t)thread_trampoline;    /* x30 */

    sched_task_init(&t->s, params->policy, params->rt_prio, params->nice);
    t->s.affinity = params->affinity & online_mask();
    if (params->affinity != 0 && t->s.affinity == 0) t->s.affinity = 0;     /* nothing usable asked for: no restriction */
    if (t->s.affinity == online_mask()) t->s.affinity = 0;
    t->id = id;
    t->pid = pid;
    t->tid = 0;
    t->on_cpu = -1;
    t->saved_sp = (uint64_t)frame;
    t->tls = params->tls;
    t->block = block;
    t->guard = guard;
    t->stack_top = top;
    copy_name(t->name, name);
    fp_state_clear(&t->fp);
    tbl[id] = t;
    if (!guarded) mmu_kernel_guard_page(guard, 1);

    make_ready(t, SCHED_Q_NEW);
    irq_restore(flags);

    return id;
}

static int priority_to_nice(uint32_t priority)
{
    static const int map[4] = { 10, 0, -5, -10 };
    return map[priority > 3U ? 3U : priority];
}

int thread_create(const char* name, ThreadEntry entry, void* arg, uint32_t priority)
{
    ThreadParams p; thread_params_default(&p);
    p.nice = priority_to_nice(priority);
    return thread_create_ex(name, entry, arg, -1, &p);
}

int thread_create_for_pid(const char* name, ThreadEntry entry, void* arg, uint32_t priority, int pid)
{
    ThreadParams p; thread_params_default(&p);
    p.nice = priority_to_nice(priority);
    return thread_create_ex(name, entry, arg, pid, &p);
}

uint64_t thread_stack_top_current() { return current->stack_top; }
uint64_t thread_stack_top_of(int t) { Task* x = task_at(t); return x ? x->stack_top : 0; }

int thread_pid(int t) { Task* x = task_at(t); return x ? x->pid : -1; }
int thread_current_pid() { return current->pid; }
int thread_state_of(int t) { Task* x = task_at(t); return x ? x->s.state : SCHED_FREE; }

int thread_kill(int t)
{
    if (t <= 0) return -1;
    uint64_t flags = irq_save();
    Task* x = task_at(t);
    if (!x || x->s.is_idle_task || x == current || x->on_cpu >= 0)
    {
        irq_restore(flags);
        return -1;
    }
    sleepers_del(x);
    sched_dequeue(&rq, &x->s);
    x->s.state = SCHED_DEAD;
    for (int c = 0; c < SMP_MAX_CPUS; ++c) if (last_prev[c] == x) last_prev[c] = 0;
    free_task(x);
    irq_restore(flags);
    return 0;
}

void thread_set_user_tasks(int count) { user_task_count = count < 0 ? 0 : count; }
int thread_user_tasks() { return user_task_count; }

void thread_wait_ms(uint64_t ms)
{
    thread_sleep_ms(ms);
}

extern "C" void thread_user_preempt()
{
    if (!initialised || !need_resched) return;
    schedule(SCHED_HOW_PREEMPT);
}

void thread_yield()
{
    if (!initialised)
        return;

    uint64_t flags = irq_save();
    int shell = (current == &task0);
    int switched = schedule(shell ? SCHED_HOW_IDLE_YIELD : SCHED_HOW_YIELD);
    if (!switched && smp_sched_on && shell) smp_window();   /* nothing else to run here: let the other CPUs into the kernel */
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

    if (current == &task0)
    {
        /* The boot/shell thread is the idle thread of last resort and must
           stay runnable: wait by yielding instead of sleeping. */
        while (timer_millis() < wake)
            thread_yield();

        return;
    }

    uint64_t flags = irq_save();
    Task* t = current;

    t->wake_ms = wake ? wake : 1;
    t->wake_reason = 0;
    t->s.state = SCHED_SLEEPING;
    sleepers_add(t);
    schedule(SCHED_HOW_PREEMPT);

    irq_restore(flags);
}

/* Event wait: a poll loop sleeps here instead of in thread_wait_ms(); whatever changes the state it polls (a pipe was written,
   a child exited) calls thread_event_notify() and the sleepers retry at once instead of at their next timer tick. The timeout
   still bounds the wait, so an event source that forgets to notify only costs latency. The generation counter closes the window
   between the caller's last check and going to sleep. */
static volatile uint32_t ev_generation = 0;

uint32_t thread_event_gen() { return ev_generation; }

void thread_event_wait(uint32_t gen, uint64_t ms)
{
    if (!initialised || current == &task0) { thread_sleep_ms(ms); return; }
    uint64_t flags = irq_save();
    if (ev_generation != gen) { irq_restore(flags); return; }
    Task* t = current;
    uint64_t wake = timer_millis() + ms;
    t->wake_ms = wake ? wake : 1;
    t->wake_reason = 0;
    t->ev_wait = 1;
    t->s.state = SCHED_SLEEPING;
    sleepers_add(t);
    schedule(SCHED_HOW_PREEMPT);
    t->ev_wait = 0;
    irq_restore(flags);
}

void thread_event_notify()
{
    if (!initialised) return;
    uint64_t flags = irq_save();
    ++ev_generation;
    for (Task* t = sleepers; t;)
    {
        Task* next = t->snext;
        if (t->ev_wait && t->s.state == SCHED_SLEEPING && !t->s.is_idle_task)
        {
            sleepers_del(t);
            t->wake_reason = THREAD_WAKE_NORMAL;
            make_ready(t, SCHED_Q_WAKEUP);
        }
        t = next;
    }
    irq_restore(flags);
}

int thread_block(uint64_t timeout_ms)
{
    if (!initialised) return THREAD_WAKE_TIMEOUT;
    uint64_t flags = irq_save();
    Task* t = current;

    if (t == &task0)
    {
        /* The shell never sleeps: poll (yield) until woken by thread_wake()/timeout. Only reached by kernel-internal waits. */
        uint64_t end = timeout_ms ? timer_millis() + timeout_ms : 0;
        t->wake_reason = 0;
        irq_restore(flags);
        while (t->wake_reason == 0)
        {
            if (end && timer_millis() >= end) return THREAD_WAKE_TIMEOUT;
            thread_yield();
        }
        return t->wake_reason;
    }

    t->wake_reason = 0;
    t->wake_ms = timeout_ms ? timer_millis() + timeout_ms : 0;
    if (timeout_ms && t->wake_ms == 0) t->wake_ms = 1;
    t->s.state = SCHED_SLEEPING;
    if (timeout_ms) sleepers_add(t);
    schedule(SCHED_HOW_PREEMPT);
    int r = t->wake_reason ? t->wake_reason : THREAD_WAKE_NORMAL;
    irq_restore(flags);
    return r;
}

int thread_wake(int id, int reason)
{
    uint64_t flags = irq_save();
    Task* t = task_at(id);
    int woke = 0;
    if (t == &task0)
    {
        if (t->wake_reason == 0) { t->wake_reason = reason; woke = 1; }
    }
    else if (t && t->s.state == SCHED_SLEEPING && !t->s.is_idle_task)
    {
        sleepers_del(t);
        t->wake_reason = reason;
        make_ready(t, SCHED_Q_WAKEUP);
        woke = 1;
    }
    irq_restore(flags);
    return woke;
}

int thread_is_blocked(int id)
{
    Task* t = task_at(id);
    return t && t->s.state == SCHED_SLEEPING && !t->s.is_idle_task;
}

int thread_current()
{
    return current->id;
}

void thread_set_preempt(int enabled)
{
    uint64_t flags = irq_save();

    preempt_on = enabled ? 1 : 0;
    need_resched = 0;

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

    int cpu = (int)smp_cpu_id_fast();
    Task* cur = cur_of[cpu];

    ++cur->run_ticks;
    if (cur->pid >= 0) process_account_tick(cur->pid, 1000U / 100U);   /* 100 Hz tick (timer_start_periodic(100U)) */

    int rt = !cur->s.is_idle_task && sched_eff_rt_prio(&cur->s) > 0;
    int was = budget_of[cpu].throttled;
    int throttled = sched_rt_budget_tick(&budget_of[cpu], rt);
    if (throttled) ++rt_throttled_ticks;
    sched_tick_account(&rq, &cur->s);

    int woke = (sleepers != 0) ? expire_sleepers(timer_millis()) : 0;
    if (woke > 0 && cur->s.is_idle_task) need_resched_of[cpu] = 1;

    if (preempt_on || user_task_count > 0 || cur->s.is_idle_task)
    {
        if (was != throttled || sched_should_preempt(&rq, &cur->s, cpu, !throttled)) need_resched_of[cpu] = 1;
    }
}

void thread_preempt_check()
{
    /* Called with IRQs masked, after the interrupt was acknowledged and
       ended at the GIC, so switching away here cannot block the IRQ line. */
    if (!initialised || !preempt_on || !need_resched)
        return;

    schedule(SCHED_HOW_PREEMPT);
}

int thread_active_count()
{
    int count = 0;

    for (int i = 0; i < tbl_cap; ++i)
    {
        Task* t = tbl[i];
        if (t && !t->s.is_idle_task && t->s.state != SCHED_FREE && t->s.state != SCHED_DEAD)
            ++count;
    }

    return count;
}

uint64_t thread_switch_count()
{
    return switches;
}

uint64_t thread_rt_throttled_ticks() { return rt_throttled_ticks; }

static const char* policy_name(int p)
{
    switch (p)
    {
        case SCHED_FIFO: return "FIFO";
        case SCHED_RR: return "RR";
        case SCHED_BATCH: return "BATCH";
        case SCHED_IDLE: return "IDLE";
        default: return "OTHER";
    }
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
    uart_puts(", ");
    print_u64((uint64_t)thread_active_count());
    uart_puts(" active, RT throttled ticks ");
    print_u64(rt_throttled_ticks);
    uart_puts(")\r\n");
    print_pad_str("ID", 6); print_pad_str("TID", 11); print_pad_str("PID", 6); print_pad_str("NAME", 17); print_pad_str("STATE", 10);
    print_pad_str("POL", 7); print_pad_str("PRI", 5); print_pad_str("NI", 5); print_pad_str("CPU", 5); print_pad_str("AFF", 6);
    print_pad_str("RUNS", 8); uart_puts("TICKS\r\n");

    for (int i = 0; i < tbl_cap; ++i)
    {
        Task* t = tbl[i];
        if (!t || t->s.state == SCHED_FREE || (t->s.is_idle_task && t->s.state == SCHED_SLEEPING))
            continue;

        print_pad((uint64_t)t->id, 6);
        print_pad((uint64_t)t->tid, 11);
        if (t->pid >= 0) print_pad((uint64_t)t->pid, 6); else print_pad_str("-", 6);
        print_pad_str(t->name, 17);
        print_pad_str(state_names[t->s.state], 10);
        print_pad_str(policy_name(t->s.policy), 7);
        print_pad((uint64_t)sched_eff_rt_prio(&t->s), 5);
        if (t->s.nice < 0) { uart_putc('-'); print_pad((uint64_t)(-t->s.nice), 4); } else print_pad((uint64_t)t->s.nice, 5);
        if (t->on_cpu >= 0) print_pad((uint64_t)t->on_cpu, 5); else print_pad_str("-", 5);
        if (t->s.affinity) { uart_puts("0x"); static const char hx[] = "0123456789abcdef"; uint64_t a = t->s.affinity; int started = 0;
            int printed = 2;
            for (int sh = 60; sh >= 0; sh -= 4) { int d = (int)((a >> sh) & 15); if (d || started || sh == 0) { started = 1; uart_putc(hx[d]); ++printed; } }
            for (; printed < 6; ++printed) uart_putc(' '); }
        else print_pad_str("all", 6);
        print_pad(t->run_count, 8);
        print_u64(t->run_ticks);
        uart_puts("\r\n");
    }

    irq_restore(flags);
}

/* top: sample every thread's run ticks, wait, and show who used the CPUs meanwhile (busiest first). */
void thread_top(uint64_t interval_ms)
{
    if (interval_ms < 100) interval_ms = 100;
    if (interval_ms > 60000) interval_ms = 60000;
    int cap = tbl_cap;
    Task** st = (Task**)kmalloc((uint64_t)cap * sizeof(Task*));
    uint64_t* before = (uint64_t*)kmalloc((uint64_t)cap * sizeof(uint64_t));
    uint64_t* delta = (uint64_t*)kmalloc((uint64_t)cap * sizeof(uint64_t));
    if (!st || !before || !delta) { uart_puts("top: out of memory\r\n"); if (st) kfree(st); if (before) kfree(before); if (delta) kfree(delta); return; }
    uint64_t flags = irq_save();
    for (int i = 0; i < cap; ++i) { st[i] = tbl[i]; before[i] = tbl[i] ? tbl[i]->run_ticks : 0; delta[i] = 0; }
    irq_restore(flags);
    uint64_t t0 = timer_millis();
    thread_wait_ms(interval_ms);
    uint64_t elapsed = timer_millis() - t0;
    if (elapsed == 0) elapsed = 1;

    flags = irq_save();
    uint64_t busy = 0;
    for (int i = 0; i < cap && i < tbl_cap; ++i)
    {
        Task* t = tbl[i];
        if (t && t == st[i] && t->run_ticks >= before[i]) delta[i] = t->run_ticks - before[i];
        else if (t && t != st[i]) delta[i] = t->run_ticks;          /* started during the interval */
        if (t && !t->s.is_idle_task && t->id != 0) busy += delta[i];
    }
    uint64_t ncpu = (uint64_t)online_cpus();
    uint64_t total10 = busy * 10ULL * 1000ULL / (elapsed * ncpu);     /* tenths of a percent of the whole machine */
    uart_puts("top: interval "); print_u64(elapsed); uart_puts(" ms, "); print_u64(ncpu); uart_puts(" CPU(s), busy ");
    print_u64(total10 / 10); uart_putc('.'); print_u64(total10 % 10); uart_puts("%\r\n");
    print_pad_str("TID", 11); print_pad_str("PID", 6); print_pad_str("NAME", 17); print_pad_str("POL", 7); print_pad_str("PRI", 5); print_pad_str("NI", 5);
    print_pad_str("CPU", 5); print_pad_str("%CPU", 8); uart_puts("TICKS\r\n");
    for (int shown = 0; shown < 25; ++shown)
    {
        int best = -1;
        for (int i = 0; i < cap && i < tbl_cap; ++i)
        {
            Task* t = tbl[i];
            if (!t || t->s.state == SCHED_FREE || t->s.state == SCHED_DEAD || t->s.is_idle_task || t->id == 0 || delta[i] == (uint64_t)-1) continue;
            if (best < 0 || delta[i] > delta[best]) best = i;
        }
        if (best < 0) break;
        Task* t = tbl[best];
        uint64_t p10 = delta[best] * 10ULL * 1000ULL / elapsed;       /* tenths of a percent of one CPU */
        print_pad((uint64_t)t->tid, 11);
        if (t->pid >= 0) print_pad((uint64_t)t->pid, 6); else print_pad_str("-", 6);
        print_pad_str(t->name, 17); print_pad_str(policy_name(t->s.policy), 7);
        print_pad((uint64_t)sched_eff_rt_prio(&t->s), 5);
        if (t->s.nice < 0) { uart_putc('-'); print_pad((uint64_t)(-t->s.nice), 4); } else print_pad((uint64_t)t->s.nice, 5);
        if (t->on_cpu >= 0) print_pad((uint64_t)t->on_cpu, 5); else print_pad_str("-", 5);
        print_u64(p10 / 10); uart_putc('.'); print_u64(p10 % 10); uart_puts("%   ");
        print_u64(t->run_ticks);
        uart_puts("\r\n");
        delta[best] = (uint64_t)-1;
    }
    irq_restore(flags);
    kfree(st); kfree(before); kfree(delta);
}

void fp_state_clear(FpState* state)
{
    uint64_t* w = (uint64_t*)state;
    for (unsigned i = 0; i < sizeof(FpState) / 8U; ++i) w[i] = 0;
}

/* fork(): the child starts with a copy of the parent's live FP/SIMD registers. */
void thread_fp_copy_live_to(int thread)
{
    Task* t = task_at(thread);
    if (t && thread > 0) fp_save(&t->fp);
}

/* exec(): a new program image starts with cleared FP/SIMD registers. */
void thread_fp_reset_live()
{
    FpState zero;
    fp_state_clear(&zero);
    fp_restore(&zero);
}

void thread_secondary_adopt(uint32_t cpu)
{
    Task* it = idle_task[cpu];
    if (!it) return;
    uint64_t flags = irq_save();
    it->s.state = SCHED_RUNNING;
    it->on_cpu = (int)cpu;
    cur_of[cpu] = it;
    need_resched_of[cpu] = 0;
    sched_rt_budget_init(&budget_of[cpu]);
    irq_restore(flags);
}
void thread_idle_schedule() { (void)schedule(SCHED_HOW_PREEMPT); }

/* Earliest wake-up time of a sleeping thread (0 when none): an idle CPU programs its timer for it, so a 1 ms block_step sleep is
   not stretched to the next 10 ms tick. */
uint64_t thread_next_wake_ms()
{
    uint64_t best = 0;
    for (Task* t = sleepers; t; t = t->snext)
        if (t->s.state == SCHED_SLEEPING && t->wake_ms != 0 && (best == 0 || t->wake_ms < best)) best = t->wake_ms;
    return best;
}
void thread_reload_context() { task_context_enter(current->pid); }

extern "C" void thread_entry_hook()
{
    /* Cooperative threads do not need asynchronous IRQs. Keep the IRQ mask
       inherited from schedule(); in particular, never unmask the CPU IRQ line
       as a side effect of creating/running a demo thread. */
    asm volatile("isb" ::: "memory");
    Task* me = current;
    asm volatile("msr tpidr_el0, %0" :: "r"(me->tls));
    finish_switch();
    task_context_enter(me->pid);
    if (me->pid >= 0) fp_restore(&me->fp);   /* new thread: cleared state, or the parent's copy after fork */
}

extern "C" void thread_exit()
{
    (void)irq_save();

    Task* t = current;
    sleepers_del(t);
    t->s.state = SCHED_DEAD;
    t->pid = -1;
    schedule(SCHED_HOW_PREEMPT);

    /* A dead thread is never picked again. */
    while (1)
        asm volatile("wfe");
}

/* ---- scheduling parameters ---- */

static void kick_cpu_of(Task* t)
{
    if (t->on_cpu >= 0)
    {
        need_resched_of[t->on_cpu] = 1;
        if (t->on_cpu != (int)smp_cpu_id_fast()) smp_kick_cpus(1U << t->on_cpu);
    }
}

static void requeue_after_change(Task* t, int was_queued)
{
    if (was_queued) { sched_enqueue(&rq, &t->s, SCHED_Q_WAKEUP); notify_ready(t); }
    kick_cpu_of(t);
}

int thread_set_sched(int id, int policy, int rt_prio, int nice)
{
    if (!sched_params_valid(policy, rt_prio, nice)) return -22;
    uint64_t flags = irq_save();
    Task* t = task_at(id);
    int rc = 0;
    if (!t || t->s.is_idle_task || t->s.state == SCHED_DEAD) rc = -3;
    else if (t == &task0) rc = -1;
    else
    {
        int was = t->s.queued;
        if (was) sched_dequeue(&rq, &t->s);
        (void)sched_task_set(&t->s, policy, rt_prio, nice);
        requeue_after_change(t, was);
    }
    irq_restore(flags);
    return rc;
}

int thread_get_sched(int id, int* policy, int* rt_prio, int* nice)
{
    Task* t = task_at(id);
    if (!t || t->s.is_idle_task) return -3;
    if (policy) *policy = t->s.policy;
    if (rt_prio) *rt_prio = t->s.rt_prio;
    if (nice) *nice = t->s.nice;
    return 0;
}

int thread_set_affinity(int id, uint64_t mask)
{
    uint64_t flags = irq_save();
    Task* t = task_at(id);
    int rc = 0;
    if (!t || t->s.is_idle_task || t->s.state == SCHED_DEAD) rc = -3;
    else if (t == &task0) rc = -1;
    else
    {
        uint64_t online = online_mask();
        if ((mask & online) == 0) rc = -22;
        else
        {
            int was = t->s.queued;
            if (was) sched_dequeue(&rq, &t->s);
            t->s.affinity = ((mask & online) == online) ? 0 : (mask & online);
            if (was) { sched_enqueue(&rq, &t->s, SCHED_Q_TAIL); notify_ready(t); }
            if (t->on_cpu >= 0 && !sched_cpu_allowed(&t->s, t->on_cpu)) kick_cpu_of(t);   /* it leaves this CPU at the next preemption point */
        }
    }
    irq_restore(flags);
    return rc;
}

uint64_t thread_get_affinity(int id) { Task* t = task_at(id); return t ? t->s.affinity : 0; }
uint64_t thread_online_mask() { return online_mask(); }

int thread_set_boost(int id, int rt_prio)
{
    if (rt_prio < 0 || rt_prio > SCHED_RT_MAX) return -22;
    uint64_t flags = irq_save();
    Task* t = task_at(id);
    int rc = 0;
    if (!t || t->s.is_idle_task || t->s.state == SCHED_DEAD || t == &task0) rc = -3;
    else if (t->s.boost != rt_prio)
    {
        int was = t->s.queued;
        if (was) sched_dequeue(&rq, &t->s);
        t->s.boost = rt_prio;
        requeue_after_change(t, was);
    }
    irq_restore(flags);
    return rc;
}

int thread_boost_of(int id) { Task* t = task_at(id); return t ? t->s.boost : 0; }
int thread_cpu_of(int id) { Task* t = task_at(id); return t ? t->on_cpu : -1; }
int thread_effective_prio(int id) { Task* t = task_at(id); return t ? sched_eff_rt_prio(&t->s) : 0; }
uint64_t thread_run_ticks_of(int id) { Task* t = task_at(id); return t ? t->run_ticks : 0; }
uint64_t thread_run_count_of(int id) { Task* t = task_at(id); return t ? t->run_count : 0; }
const char* thread_name_of(int id) { Task* t = task_at(id); return t ? t->name : ""; }
void thread_set_name(int id, const char* name) { Task* t = task_at(id); if (t) copy_name(t->name, name); }

/* ---- per-thread user state ---- */

void thread_set_tid(int id, uint32_t tid) { Task* t = task_at(id); if (t) t->tid = tid; }
uint32_t thread_tid_of(int id) { Task* t = task_at(id); return t ? t->tid : 0; }
uint32_t thread_current_tid() { return current->tid; }
int thread_find_tid(uint32_t tid)
{
    if (tid == 0) return -1;
    for (int i = 0; i < tbl_cap; ++i)
        if (tbl[i] && tbl[i]->tid == tid && tbl[i]->s.state != SCHED_DEAD) return i;
    return -1;
}
uint64_t thread_tls_of(int id) { Task* t = task_at(id); return t ? t->tls : 0; }
void thread_set_tls(int id, uint64_t tls) { Task* t = task_at(id); if (t) t->tls = tls; }
void thread_set_tls_live(uint64_t tls) { current->tls = tls; asm volatile("msr tpidr_el0, %0" :: "r"(tls)); }
uint32_t thread_sigmask_of(int id) { Task* t = task_at(id); return t ? t->sigmask : 0; }
void thread_set_sigmask(int id, uint32_t mask) { Task* t = task_at(id); if (t) t->sigmask = mask; }
uint32_t thread_sigpending_of(int id) { Task* t = task_at(id); return t ? t->sigpend : 0; }
void thread_sigpending_or(int id, uint32_t bits) { Task* t = task_at(id); if (t) t->sigpend |= bits; }
void thread_sigpending_clear(int id, uint32_t bits) { Task* t = task_at(id); if (t) t->sigpend &= ~bits; }
uint64_t thread_clear_tid_of(int id) { Task* t = task_at(id); return t ? t->clear_tid : 0; }
void thread_set_clear_tid(int id, uint64_t addr) { Task* t = task_at(id); if (t) t->clear_tid = addr; }
void thread_set_dying(int id, int v) { Task* t = task_at(id); if (t) t->dying = v; }
int thread_is_dying(int id) { Task* t = task_at(id); return t ? t->dying : 0; }
void thread_altstack_set(int id, uint64_t sp, uint64_t size) { Task* t = task_at(id); if (t) { t->alt_sp = sp; t->alt_size = size; } }
void thread_altstack_get(int id, uint64_t* sp, uint64_t* size) { Task* t = task_at(id); if (sp) *sp = t ? t->alt_sp : 0; if (size) *size = t ? t->alt_size : 0; }
static uint32_t tid_serial = 0;
uint32_t thread_alloc_tid()   /* thread ids above the pid range (pids stay below 0x1FFFFFF0); 30 bits so the PI futex word keeps its two flag bits */
{
    for (int tries = 0; tries < 64; ++tries)
    {
        uint32_t t = 0x20000000U + (++tid_serial & 0x1FFFFFFFU);
        if (thread_find_tid(t) < 0) return t;
    }
    return 0x20000000U + (++tid_serial & 0x1FFFFFFFU);
}
uint64_t thread_robust_of(int id) { Task* t = task_at(id); return t ? t->robust_head : 0; }
void thread_set_robust(int id, uint64_t addr) { Task* t = task_at(id); if (t) t->robust_head = addr; }

int thread_group_next(int pid, int after)
{
    for (int i = after + 1; i < tbl_cap; ++i)
        if (tbl[i] && tbl[i]->pid == pid && tbl[i]->s.state != SCHED_DEAD) return i;
    return -1;
}

int thread_group_count(int pid)
{
    int n = 0;
    for (int i = 0; i < tbl_cap; ++i)
        if (tbl[i] && tbl[i]->pid == pid && tbl[i]->s.state != SCHED_DEAD) ++n;
    return n;
}
