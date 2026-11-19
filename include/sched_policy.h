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

#ifndef SCHED_POLICY_H
#define SCHED_POLICY_H

#include "types.h"

/* Pure scheduling policy (no hardware access) so it can be unit-tested on the host.

   Classes, highest first:
     SCHED_FIFO / SCHED_RR   real time, priority 1..99, strict priority, bitmap + per-level FIFO lists
     SCHED_OTHER / SCHED_BATCH  fair share by weighted virtual runtime, nice -20..19 (Linux prio_to_weight)
     SCHED_IDLE              runs only when nothing else is runnable
   Real-time tasks are throttled (95 % of every 1 s period per CPU) so a spinning RT task cannot lock the machine up. */

enum SchedState
{
    SCHED_FREE = 0,
    SCHED_READY = 1,
    SCHED_RUNNING = 2,
    SCHED_SLEEPING = 3,
    SCHED_DEAD = 4
};

/* Policy numbers are the Linux ones (sched_setscheduler). */
#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
#define SCHED_BATCH 3
#define SCHED_IDLE  5

#define SCHED_RT_MIN 1
#define SCHED_RT_MAX 99
#define SCHED_RT_LEVELS 100
#define SCHED_NICE_MIN (-20)
#define SCHED_NICE_MAX 19

#define SCHED_TICK_NS        10000000ULL     /* the 100 Hz scheduler tick */
#define SCHED_GRAN_NS        15000000ULL     /* a running fair task is preempted by one that is this much behind */
#define SCHED_WAKE_GRAN_NS    5000000ULL     /* wake-up preemption threshold */
#define SCHED_SLEEPER_CREDIT_NS 10000000ULL  /* a waking task may be this far behind min_vruntime */
#define SCHED_RR_QUANTUM_TICKS 10            /* 100 ms */
#define SCHED_RT_PERIOD_TICKS 100            /* 1 s */
#define SCHED_RT_RUNTIME_TICKS 95            /* 95 % */
#define SCHED_IDLE_WEIGHT 3U

/* enqueue flags */
#define SCHED_Q_TAIL   0
#define SCHED_Q_HEAD   1     /* a real-time task preempted by a higher priority one keeps its place at the front */
#define SCHED_Q_WAKEUP 2     /* fair task: clamp vruntime so a long sleeper does not monopolise the CPU */
#define SCHED_Q_NEW    4     /* fair task: start at min_vruntime plus one slice (a fork bomb gains nothing) */
#define SCHED_Q_YIELD  8     /* fair task: never stays behind min_vruntime (an always-yielding task must not pin it) */

struct SchedTask
{
    int state;                 /* SchedState */
    int policy;                /* SCHED_* */
    int nice;                  /* -20..19 */
    int rt_prio;               /* 1..99 for FIFO/RR, 0 otherwise */
    int boost;                 /* priority inherited through a PI mutex (0 = none), acts as a real-time priority */
    uint32_t weight;           /* from nice */
    uint64_t vruntime;         /* weighted run time, fair class */
    uint64_t affinity;         /* allowed CPUs, 0 = every CPU */
    int rr_left;               /* ticks left in the round-robin quantum */
    int queued;                /* 1 while linked into a SchedRq */
    int qkind;                 /* which list: 1 real time, 2 fair, 3 idle class */
    int qlevel;                /* real-time level while queued */
    int is_idle_task;          /* per-CPU idle context: never queued */
    SchedTask* next;
    SchedTask* prev;
};

struct SchedRq
{
    SchedTask* rt_head[SCHED_RT_LEVELS];
    SchedTask* rt_tail[SCHED_RT_LEVELS];
    uint64_t rt_bitmap[2];
    SchedTask* fair_head;
    SchedTask* fair_tail;
    SchedTask* idle_head;
    SchedTask* idle_tail;
    uint64_t min_vruntime;
    int nr_queued;
};

/* Per-CPU real-time budget. */
struct SchedRtBudget
{
    int period_left;
    int used;
    int throttled;
};

int sched_params_valid(int policy, int rt_prio, int nice);   /* 1 when the combination is legal */
uint32_t sched_nice_to_weight(int nice);
int sched_policy_is_rt(int policy);
int sched_eff_rt_prio(const SchedTask* t);                   /* real-time priority in force (policy or PI boost), 0 for none */
int sched_cpu_allowed(const SchedTask* t, int cpu);

void sched_task_init(SchedTask* t, int policy, int rt_prio, int nice);
int sched_task_set(SchedTask* t, int policy, int rt_prio, int nice);   /* 0 or -22; the caller re-queues */
void sched_rq_init(SchedRq* rq);
void sched_enqueue(SchedRq* rq, SchedTask* t, int flags);
void sched_dequeue(SchedRq* rq, SchedTask* t);

/* Chooses (and removes from the queue) the next task for `cpu`. `skip` is passed over unless nothing else is eligible (sched_yield: within its own class; skip_strict = in every class, the shell's
   idle loop).
   rt_allowed = 0 hides the real-time class (throttled). NULL when nothing is runnable. */
SchedTask* sched_pick(SchedRq* rq, int cpu, int rt_allowed, const SchedTask* skip, int skip_strict);

/* How a scheduling decision was triggered. */
#define SCHED_HOW_PREEMPT    0   /* tick / wake-up / the task blocked */
#define SCHED_HOW_YIELD      1   /* sched_yield(): stays behind its own class peers */
#define SCHED_HOW_IDLE_YIELD 2   /* the shell's idle loop: behind every other task, whatever the class */

/* The decision itself: puts `cur` back on the queue when it is still runnable, picks the next task for `cpu` and marks it RUNNING.
   Returns `idle_task` (may be NULL) when nothing is runnable. The caller switches contexts when the result differs from `cur`. */
SchedTask* sched_schedule(SchedRq* rq, int cpu, SchedTask* cur, int how, int rt_allowed, SchedTask* idle_task);

/* One scheduler tick for the task that ran: updates vruntime / round-robin quantum. */
void sched_tick_account(SchedRq* rq, SchedTask* cur);
/* True when a queued task deserves the CPU more than `cur` (checked on every tick and after a wake-up). */
int sched_should_preempt(SchedRq* rq, SchedTask* cur, int cpu, int rt_allowed);
/* A task just became runnable: does it preempt `cur` right away? */
int sched_wakeup_preempts(const SchedTask* woken, const SchedTask* cur);

void sched_rt_budget_init(SchedRtBudget* b);
/* Called once per tick on a CPU; returns 1 when the real-time class must be hidden for the rest of the period. */
int sched_rt_budget_tick(SchedRtBudget* b, int cur_is_rt);

#endif
