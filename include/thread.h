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

#ifndef THREAD_H
#define THREAD_H

#include "types.h"
#include "sched_policy.h"

/* Kernel threads with context switching. Every EL0 task (and every thread of a multi-threaded process) is one kernel thread with its
   own kernel stack. Thread 0 is the boot/shell context; threads 1..SMP_MAX_CPUS-1 are the idle contexts of the secondary CPUs.
   Tasks live in a table that grows on demand: there is no fixed thread limit, only memory. Each kernel stack is THREAD_STACK_SIZE
   bytes with an unmapped guard page below it. */

#define THREAD_STACK_SIZE 32768

typedef void (*ThreadEntry)(void*);

/* Wake-up reasons returned by thread_block(). */
#define THREAD_WAKE_NORMAL  1
#define THREAD_WAKE_TIMEOUT 2
#define THREAD_WAKE_SIGNAL  3

/* Scheduling parameters a thread starts with. */
struct ThreadParams
{
    int policy;          /* SCHED_OTHER ... */
    int rt_prio;         /* 1..99 for FIFO/RR */
    int nice;            /* -20..19 */
    uint64_t affinity;   /* 0 = every CPU */
    uint64_t tls;        /* initial TPIDR_EL0 */
};

void thread_params_default(ThreadParams* p);
void thread_init();

/* Returns the thread id or -1 when out of memory. priority 0..3 maps to nice +10, 0, -5, -10. */
int thread_create(const char* name, ThreadEntry entry, void* arg, uint32_t priority);

/* Like thread_create but binds the thread to a user process: on every switch
   to it the process address space and descriptor table are activated. */
int thread_create_for_pid(const char* name, ThreadEntry entry, void* arg, uint32_t priority, int pid);
int thread_create_ex(const char* name, ThreadEntry entry, void* arg, int pid, const ThreadParams* params);
int thread_pid(int thread);        /* bound pid or -1 */
int thread_current_pid();          /* pid bound to the running thread, -1 for the shell/kernel thread */
int thread_kill(int thread);       /* terminate another thread that is not running (kernel demo threads) */
int thread_state_of(int thread);   /* SchedState value */
extern "C" void thread_user_preempt();        /* exception-from-EL0 hook: switch if a better task is waiting */
void thread_set_user_tasks(int count);
int thread_user_tasks();
void thread_fp_copy_live_to(int thread);   /* fork: snapshot the live FP/SIMD registers into a new thread */
void thread_fp_reset_live();               /* exec: clear the live FP/SIMD registers */
void thread_wait_ms(uint64_t ms);  /* sleep (user threads) or yield-wait (shell) */
void thread_yield();
void thread_sleep_ms(uint64_t ms);
uint32_t thread_event_gen();                        /* read BEFORE checking the condition a poll loop waits for */
void thread_event_wait(uint32_t gen, uint64_t ms);  /* sleep up to ms, or until thread_event_notify() (returns at once if one happened since gen) */
void thread_event_notify();                         /* some pollable state changed: wake event waiters */
int thread_current();

/* Timer-driven preemption. Needs the periodic timer + CPU IRQs enabled. */
void thread_set_preempt(int enabled);
int thread_preempt_enabled();

/* Hooks called from IRQ context (IRQs masked). */
void thread_timer_tick();
void thread_preempt_check();

int thread_active_count();      /* threads that are not FREE/DEAD (idle contexts excluded) */
uint64_t thread_switch_count();
void thread_list();
void thread_top(uint64_t interval_ms);           /* per-thread CPU share over an interval, busiest first */

/* ---- scheduling policy, priority, affinity ---- */
int thread_set_sched(int thread, int policy, int rt_prio, int nice);   /* 0, -22 invalid, -3 no such thread, -1 not allowed */
int thread_get_sched(int thread, int* policy, int* rt_prio, int* nice);
int thread_set_affinity(int thread, uint64_t mask);                    /* 0, -22 when no online CPU is in the mask */
uint64_t thread_get_affinity(int thread);                              /* 0 = every CPU */
uint64_t thread_online_mask();                                         /* bit n = CPU n is online */
int thread_set_boost(int thread, int rt_prio);                         /* priority inheritance: 0 clears */
int thread_boost_of(int thread);
int thread_cpu_of(int thread);                                         /* CPU it runs on right now, -1 when not running */
int thread_effective_prio(int thread);                                 /* real-time priority in force, 0 = none */
uint64_t thread_run_ticks_of(int thread);
uint64_t thread_run_count_of(int thread);
const char* thread_name_of(int thread);
void thread_set_name(int thread, const char* name);
uint64_t thread_rt_throttled_ticks();                                  /* ticks during which a CPU hid the real-time class */

/* ---- blocking and waking (futex, join, ...). Caller holds the kernel lock; wake-before-block cannot race. ---- */
int thread_block(uint64_t timeout_ms);              /* THREAD_WAKE_*; timeout 0 = no timeout */
int thread_wake(int thread, int reason);            /* 1 when the thread was blocked and is runnable now */
int thread_is_blocked(int thread);

/* ---- per-thread user state ---- */
void thread_set_tid(int thread, uint32_t tid);
uint32_t thread_tid_of(int thread);
int thread_find_tid(uint32_t tid);                  /* thread id or -1 */
uint32_t thread_current_tid();
uint64_t thread_tls_of(int thread);
void thread_set_tls(int thread, uint64_t tls);      /* takes effect on the next switch to it (live register for the running thread) */
void thread_set_tls_live(uint64_t tls);
uint32_t thread_sigmask_of(int thread);
void thread_set_sigmask(int thread, uint32_t mask);
uint32_t thread_sigpending_of(int thread);
void thread_sigpending_or(int thread, uint32_t bits);
void thread_sigpending_clear(int thread, uint32_t bits);
uint64_t thread_clear_tid_of(int thread);
void thread_set_clear_tid(int thread, uint64_t addr);
uint64_t thread_robust_of(int thread);
void thread_set_robust(int thread, uint64_t addr);
void thread_set_dying(int thread, int v);           /* a dying thread exits at its next return to user mode (exit_group, exec) */
int thread_is_dying(int thread);
void thread_altstack_set(int thread, uint64_t sp, uint64_t size);
void thread_altstack_get(int thread, uint64_t* sp, uint64_t* size);
uint32_t thread_alloc_tid();                        /* a fresh thread id for non-main threads (above the pid range) */
int thread_group_next(int pid, int after);          /* iterate the threads of a process: first with after = -1, then the previous result; -1 at the end */
int thread_group_count(int pid);
uint64_t thread_stack_top_of(int thread);
uint64_t thread_stack_top_current();

extern "C" void thread_exit();
extern "C" void thread_entry_hook();
extern "C" void thread_context_switch(uint64_t* old_sp_out, uint64_t new_sp);
extern "C" void thread_trampoline();

#endif
