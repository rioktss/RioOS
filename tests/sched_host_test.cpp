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

#include "sched_policy.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static void mk(SchedTask* t, int policy, int rt, int nice)
{
    sched_task_init(t, policy, rt, nice);
    t->state = SCHED_READY;
}

/* A tiny machine: `ncpu` CPUs, one ready queue, a scheduler tick exactly like thread.cpp's (account, preempt check, schedule). */
struct Machine
{
    SchedRq rq;
    SchedTask* cur[8];
    SchedTask idle[8];
    SchedRtBudget budget[8];
    int ncpu;
    uint64_t ran[8][64];                 /* ticks each task index ran on each cpu */
    SchedTask* tasks[64];
    int ntasks;
};

static int index_of(Machine* m, SchedTask* t)
{
    for (int i = 0; i < m->ntasks; ++i) if (m->tasks[i] == t) return i;
    return -1;
}

static void machine_init(Machine* m, int ncpu)
{
    memset(m, 0, sizeof *m);
    sched_rq_init(&m->rq);
    m->ncpu = ncpu;
    for (int c = 0; c < ncpu; ++c)
    {
        sched_task_init(&m->idle[c], SCHED_OTHER, 0, 0);
        m->idle[c].is_idle_task = 1;
        m->idle[c].state = SCHED_RUNNING;
        m->cur[c] = &m->idle[c];
        sched_rt_budget_init(&m->budget[c]);
    }
}

static void add(Machine* m, SchedTask* t, int flags)
{
    m->tasks[m->ntasks++] = t;
    t->state = SCHED_READY;
    sched_enqueue(&m->rq, t, flags);
}

static void resched(Machine* m, int cpu, int how)
{
    m->cur[cpu] = sched_schedule(&m->rq, cpu, m->cur[cpu], how, !m->budget[cpu].throttled, &m->idle[cpu]);
}

static void tick(Machine* m)
{
    for (int c = 0; c < m->ncpu; ++c)
    {
        SchedTask* cur = m->cur[c];
        int idx = index_of(m, cur);
        if (idx >= 0) ++m->ran[c][idx];
        int rt = !cur->is_idle_task && sched_eff_rt_prio(cur) > 0;
        sched_rt_budget_tick(&m->budget[c], rt);
        sched_tick_account(&m->rq, cur);
        if (sched_should_preempt(&m->rq, cur, c, !m->budget[c].throttled)) resched(m, c, SCHED_HOW_PREEMPT);
    }
}

static uint64_t total_ran(Machine* m, int idx)
{
    uint64_t s = 0;
    for (int c = 0; c < m->ncpu; ++c) s += m->ran[c][idx];
    return s;
}

int main()
{
    /* ---- parameters ---- */
    CHECK(sched_params_valid(SCHED_OTHER, 0, 0) && sched_params_valid(SCHED_OTHER, 0, -20) && sched_params_valid(SCHED_OTHER, 0, 19));
    CHECK(!sched_params_valid(SCHED_OTHER, 0, 20) && !sched_params_valid(SCHED_OTHER, 0, -21));
    CHECK(!sched_params_valid(SCHED_OTHER, 5, 0));
    CHECK(sched_params_valid(SCHED_FIFO, 1, 0) && sched_params_valid(SCHED_RR, 99, 0));
    CHECK(!sched_params_valid(SCHED_FIFO, 0, 0) && !sched_params_valid(SCHED_RR, 100, 0));
    CHECK(sched_params_valid(SCHED_BATCH, 0, 5) && sched_params_valid(SCHED_IDLE, 0, 0));
    CHECK(!sched_params_valid(4, 0, 0) && !sched_params_valid(7, 0, 0) && !sched_params_valid(-1, 0, 0));
    CHECK(sched_nice_to_weight(0) == 1024 && sched_nice_to_weight(-20) == 88761 && sched_nice_to_weight(19) == 15);
    CHECK(sched_nice_to_weight(-100) == 88761 && sched_nice_to_weight(100) == 15);
    for (int n = -20; n < 19; ++n) CHECK(sched_nice_to_weight(n) > sched_nice_to_weight(n + 1));

    /* ---- real-time class ---- */
    {
        SchedRq rq; sched_rq_init(&rq);
        SchedTask a, b, c, d, f;
        mk(&a, SCHED_FIFO, 10, 0); mk(&b, SCHED_FIFO, 50, 0); mk(&c, SCHED_FIFO, 50, 0); mk(&d, SCHED_RR, 99, 0); mk(&f, SCHED_OTHER, 0, 0);
        CHECK(sched_pick(&rq, 0, 1, 0, 0) == 0);
        sched_enqueue(&rq, &f, 0); sched_enqueue(&rq, &a, 0); sched_enqueue(&rq, &b, 0); sched_enqueue(&rq, &c, 0);
        CHECK(rq.nr_queued == 4);
        CHECK(sched_pick(&rq, 0, 1, 0, 0) == &b);                 /* highest level, FIFO order within it */
        CHECK(sched_pick(&rq, 0, 1, 0, 0) == &c);
        sched_enqueue(&rq, &d, 0);
        CHECK(sched_pick(&rq, 0, 1, 0, 0) == &d);                 /* 99 beats everything */
        CHECK(d.rr_left == SCHED_RR_QUANTUM_TICKS);
        CHECK(sched_pick(&rq, 0, 0, 0, 0) == &f);                 /* throttled: the real-time class is hidden */
        CHECK(sched_pick(&rq, 0, 1, 0, 0) == &a);
        CHECK(rq.nr_queued == 0 && rq.rt_bitmap[0] == 0 && rq.rt_bitmap[1] == 0);
        /* head insertion keeps a preempted FIFO task in front of its peers */
        sched_enqueue(&rq, &b, 0); sched_enqueue(&rq, &c, SCHED_Q_HEAD);
        CHECK(sched_pick(&rq, 0, 1, 0, 0) == &c);
        CHECK(sched_pick(&rq, 0, 1, 0, 0) == &b);
        /* dequeue from the middle keeps the list and the bitmap consistent */
        sched_enqueue(&rq, &a, 0); sched_enqueue(&rq, &b, 0); sched_enqueue(&rq, &c, 0);
        sched_dequeue(&rq, &c); sched_dequeue(&rq, &c);
        CHECK(sched_pick(&rq, 0, 1, 0, 0) == &b && sched_pick(&rq, 0, 1, 0, 0) == &a && sched_pick(&rq, 0, 1, 0, 0) == 0);
        /* PI boost lifts a normal task into the real-time class */
        f.boost = 60; sched_enqueue(&rq, &f, 0); sched_enqueue(&rq, &a, 0);
        CHECK(sched_eff_rt_prio(&f) == 60 && sched_pick(&rq, 0, 1, 0, 0) == &f);
        f.boost = 0;
    }

    /* ---- affinity ---- */
    {
        SchedRq rq; sched_rq_init(&rq);
        SchedTask a, b;
        mk(&a, SCHED_OTHER, 0, 0); mk(&b, SCHED_FIFO, 20, 0);
        a.affinity = 1ULL << 2; b.affinity = (1ULL << 1) | (1ULL << 3);
        sched_enqueue(&rq, &a, 0); sched_enqueue(&rq, &b, 0);
        CHECK(sched_pick(&rq, 0, 1, 0, 0) == 0);
        CHECK(sched_pick(&rq, 2, 1, 0, 0) == &a);
        CHECK(sched_pick(&rq, 2, 1, 0, 0) == 0);
        CHECK(sched_pick(&rq, 3, 1, 0, 0) == &b);
        CHECK(sched_cpu_allowed(&a, 2) && !sched_cpu_allowed(&a, 1) && !sched_cpu_allowed(&a, 64));
        a.affinity = 0; CHECK(sched_cpu_allowed(&a, 7));
    }

    /* ---- yield ---- */
    {
        SchedRq rq; sched_rq_init(&rq);
        SchedTask a, b, i, hi;
        mk(&a, SCHED_FIFO, 10, 0); mk(&b, SCHED_FIFO, 10, 0); mk(&i, SCHED_IDLE, 0, 0); mk(&hi, SCHED_OTHER, 0, 0);
        sched_enqueue(&rq, &a, 0); sched_enqueue(&rq, &b, 0);
        CHECK(sched_pick(&rq, 0, 1, &a, 0) == &b);                /* yield: behind its peer */
        CHECK(sched_pick(&rq, 0, 1, &a, 0) == &a);                /* nobody else at its level: it continues */
        /* a fair task that yields does not let an idle-class task through, the shell's idle loop does */
        sched_enqueue(&rq, &hi, 0); sched_enqueue(&rq, &i, 0);
        CHECK(sched_pick(&rq, 0, 1, &hi, 0) == &hi);
        sched_enqueue(&rq, &hi, 0);
        CHECK(sched_pick(&rq, 0, 1, &hi, 1) == &i);
        CHECK(sched_pick(&rq, 0, 1, &hi, 1) == &hi);              /* last resort */
    }

    /* ---- preemption decisions ---- */
    {
        SchedRq rq; sched_rq_init(&rq);
        SchedTask f1, f2, r1, r2, idle, batch, id;
        mk(&f1, SCHED_OTHER, 0, 0); mk(&f2, SCHED_OTHER, 0, 0); mk(&r1, SCHED_FIFO, 5, 0); mk(&r2, SCHED_FIFO, 6, 0); mk(&batch, SCHED_BATCH, 0, 0); mk(&id, SCHED_IDLE, 0, 0);
        sched_task_init(&idle, SCHED_OTHER, 0, 0); idle.is_idle_task = 1;
        CHECK(sched_wakeup_preempts(&f1, &idle) && sched_wakeup_preempts(&r1, &idle));
        CHECK(sched_wakeup_preempts(&r2, &r1) && !sched_wakeup_preempts(&r1, &r2) && !sched_wakeup_preempts(&r1, &r1));
        CHECK(sched_wakeup_preempts(&r1, &f1) && !sched_wakeup_preempts(&f1, &r1));
        f1.vruntime = 100000000ULL; f2.vruntime = 0;
        CHECK(sched_wakeup_preempts(&f2, &f1) && !sched_wakeup_preempts(&f1, &f2));
        batch.vruntime = 0; CHECK(!sched_wakeup_preempts(&batch, &f1));         /* batch never preempts on wake-up */
        CHECK(sched_wakeup_preempts(&f1, &id) && !sched_wakeup_preempts(&id, &f1));
        /* tick check */
        f1.vruntime = 100000000ULL; f2.vruntime = 99000000ULL;
        sched_enqueue(&rq, &f2, 0);
        CHECK(!sched_should_preempt(&rq, &f1, 0, 1));                           /* only 1 ms behind: below the granularity */
        f2.vruntime = 50000000ULL; CHECK(sched_should_preempt(&rq, &f1, 0, 1));
        sched_dequeue(&rq, &f2);
        sched_enqueue(&rq, &r1, 0);
        CHECK(sched_should_preempt(&rq, &f1, 0, 1) && !sched_should_preempt(&rq, &f1, 0, 0));
        CHECK(!sched_should_preempt(&rq, &r2, 0, 1));
        sched_dequeue(&rq, &r1);
        CHECK(sched_should_preempt(&rq, &r2, 0, 0) == 0);                       /* throttled with nothing else to run: it continues */
        sched_enqueue(&rq, &f1, 0); CHECK(sched_should_preempt(&rq, &r2, 0, 0)); sched_dequeue(&rq, &f1);
        CHECK(sched_should_preempt(&rq, &idle, 0, 1) == 0);
        sched_enqueue(&rq, &id, 0); CHECK(sched_should_preempt(&rq, &idle, 0, 1));
    }

    /* ---- fair share by nice ---- */
    {
        Machine m; machine_init(&m, 1);
        SchedTask a, b, c;
        mk(&a, SCHED_OTHER, 0, 0); mk(&b, SCHED_OTHER, 0, 5); mk(&c, SCHED_OTHER, 0, -5);
        add(&m, &a, SCHED_Q_NEW); add(&m, &b, SCHED_Q_NEW); add(&m, &c, SCHED_Q_NEW);
        resched(&m, 0, SCHED_HOW_PREEMPT);
        for (int i = 0; i < 20000; ++i) tick(&m);
        double ta = (double)total_ran(&m, 0), tb = (double)total_ran(&m, 1), tc = (double)total_ran(&m, 2);
        double sum = ta + tb + tc;
        CHECK(sum >= 19999);
        double wa = 1024.0, wb = 335.0, wc = 3121.0, ws = wa + wb + wc;
        CHECK(ta / sum > wa / ws * 0.93 && ta / sum < wa / ws * 1.07);
        CHECK(tb / sum > wb / ws * 0.9 && tb / sum < wb / ws * 1.1);
        CHECK(tc / sum > wc / ws * 0.93 && tc / sum < wc / ws * 1.07);
    }
    /* equal tasks get equal time, and nobody starves */
    {
        Machine m; machine_init(&m, 1);
        SchedTask t[5];
        for (int i = 0; i < 5; ++i) { mk(&t[i], SCHED_OTHER, 0, 0); add(&m, &t[i], SCHED_Q_NEW); }
        resched(&m, 0, SCHED_HOW_PREEMPT);
        for (int i = 0; i < 10000; ++i) tick(&m);
        for (int i = 0; i < 5; ++i) { uint64_t r = total_ran(&m, i); CHECK(r > 1900 && r < 2100); }
    }
    /* idle class only gets the leftovers; a long sleeper does not monopolise on wake-up */
    {
        Machine m; machine_init(&m, 1);
        SchedTask a, i, s;
        mk(&a, SCHED_OTHER, 0, 0); mk(&i, SCHED_IDLE, 0, 0); mk(&s, SCHED_OTHER, 0, 0);
        add(&m, &a, SCHED_Q_NEW); add(&m, &i, SCHED_Q_NEW);
        resched(&m, 0, SCHED_HOW_PREEMPT);
        for (int k = 0; k < 5000; ++k) tick(&m);
        CHECK(total_ran(&m, 1) == 0);                                  /* a is runnable all the time */
        add(&m, &s, SCHED_Q_WAKEUP);                                   /* appears after 50 s */
        for (int k = 0; k < 4000; ++k) tick(&m);
        uint64_t ra = total_ran(&m, 0), rs = total_ran(&m, 2);
        CHECK(rs > 1800 && rs < 2200 && ra > 5000 + 1800);              /* shares the CPU with a from now on instead of owning it */
        CHECK(total_ran(&m, 1) == 0);
    }

    /* ---- round robin and FIFO behaviour on a machine ---- */
    {
        Machine m; machine_init(&m, 1);
        SchedTask r1, r2, r3;
        mk(&r1, SCHED_RR, 10, 0); mk(&r2, SCHED_RR, 10, 0); mk(&r3, SCHED_RR, 9, 0);
        add(&m, &r1, 0); add(&m, &r2, 0); add(&m, &r3, 0);
        resched(&m, 0, SCHED_HOW_PREEMPT);
        for (int i = 0; i < 1000; ++i) tick(&m);
        uint64_t a = total_ran(&m, 0), b = total_ran(&m, 1), c = total_ran(&m, 2);
        CHECK(a + b + c == 1000);
        CHECK(a > 400 && b > 400 && a + b >= 940);                      /* equal priority RR peers share (throttle takes 5 %) */
        CHECK(c <= 60);                                                 /* the lower level only runs while throttled */
    }
    {
        Machine m; machine_init(&m, 1);
        SchedTask f1, f2, o;
        mk(&f1, SCHED_FIFO, 10, 0); mk(&f2, SCHED_FIFO, 10, 0); mk(&o, SCHED_OTHER, 0, 0);
        add(&m, &f1, 0); add(&m, &f2, 0); add(&m, &o, SCHED_Q_NEW);
        resched(&m, 0, SCHED_HOW_PREEMPT);
        for (int i = 0; i < 1000; ++i) tick(&m);
        /* FIFO never rotates by itself: the first one keeps the CPU (except while throttled); the normal task gets the 5 % budget gap */
        CHECK(total_ran(&m, 0) >= 940 && total_ran(&m, 1) == 0);
        CHECK(total_ran(&m, 2) >= 40 && total_ran(&m, 2) <= 60);
    }
    /* real-time throttle: the normal task keeps at least 5 % of every period */
    {
        Machine m; machine_init(&m, 1);
        SchedTask hog, o;
        mk(&hog, SCHED_FIFO, 99, 0); mk(&o, SCHED_OTHER, 0, 0);
        add(&m, &hog, 0); add(&m, &o, SCHED_Q_NEW);
        resched(&m, 0, SCHED_HOW_PREEMPT);
        for (int i = 0; i < 3000; ++i) tick(&m);
        uint64_t rh = total_ran(&m, 0), ro = total_ran(&m, 1);
        CHECK(rh + ro == 3000);
        CHECK(ro >= 140 && ro <= 160);
    }
    /* a higher real-time task preempts at once and the preempted FIFO task resumes first */
    {
        Machine m; machine_init(&m, 1);
        SchedTask lo1, lo2, hi;
        mk(&lo1, SCHED_FIFO, 10, 0); mk(&lo2, SCHED_FIFO, 10, 0); mk(&hi, SCHED_FIFO, 20, 0);
        add(&m, &lo1, 0); add(&m, &lo2, 0);
        resched(&m, 0, SCHED_HOW_PREEMPT);
        CHECK(m.cur[0] == &lo1);
        add(&m, &hi, 0);
        CHECK(sched_wakeup_preempts(&hi, m.cur[0]));
        resched(&m, 0, SCHED_HOW_PREEMPT);
        CHECK(m.cur[0] == &hi);
        hi.state = SCHED_SLEEPING;                                       /* it blocks */
        resched(&m, 0, SCHED_HOW_PREEMPT);
        CHECK(m.cur[0] == &lo1);                                         /* lo1 was pushed off first: it is in front of lo2 */
        lo1.state = SCHED_SLEEPING;
        resched(&m, 0, SCHED_HOW_PREEMPT);
        CHECK(m.cur[0] == &lo2);
        lo2.state = SCHED_DEAD;
        resched(&m, 0, SCHED_HOW_PREEMPT);
        CHECK(m.cur[0] == &m.idle[0]);
    }

    /* ---- SMP: affinity is respected, a task never runs on two CPUs, work spreads ---- */
    {
        Machine m; machine_init(&m, 4);
        SchedTask t[8];
        for (int i = 0; i < 8; ++i) { mk(&t[i], SCHED_OTHER, 0, 0); }
        t[0].affinity = 1ULL << 0;             /* the shell */
        t[1].affinity = (1ULL << 2) | (1ULL << 3);
        t[2].affinity = 1ULL << 3;
        for (int i = 0; i < 8; ++i) add(&m, &t[i], SCHED_Q_NEW);
        for (int c = 0; c < 4; ++c) resched(&m, c, SCHED_HOW_PREEMPT);
        for (int i = 0; i < 4000; ++i)
        {
            tick(&m);
            for (int a = 0; a < 4; ++a) for (int b = a + 1; b < 4; ++b) CHECK(m.cur[a] == &m.idle[a] || m.cur[a] != m.cur[b]);
            for (int c = 0; c < 4; ++c) CHECK(m.cur[c]->is_idle_task || sched_cpu_allowed(m.cur[c], c));
        }
        for (int c = 0; c < 4; ++c)
        {
            if (c != 0) CHECK(m.ran[c][0] == 0);
            if (c != 2 && c != 3) CHECK(m.ran[c][1] == 0);
            if (c != 3) CHECK(m.ran[c][2] == 0);
        }
        uint64_t sum = 0;
        for (int i = 0; i < 8; ++i) sum += total_ran(&m, i);
        CHECK(sum >= 4000 * 4 - 8);                                      /* every CPU stayed busy */
        for (int i = 0; i < 8; ++i) CHECK(total_ran(&m, i) > 1000);
    }

    /* ---- parameter changes ---- */
    {
        SchedTask t;
        sched_task_init(&t, SCHED_OTHER, 0, 3);
        CHECK(t.weight == sched_nice_to_weight(3));
        CHECK(sched_task_set(&t, SCHED_RR, 40, 0) == 0 && t.policy == SCHED_RR && t.rt_prio == 40 && t.nice == 3);
        CHECK(sched_task_set(&t, SCHED_FIFO, 0, 0) == -22 && t.policy == SCHED_RR);
        CHECK(sched_task_set(&t, SCHED_BATCH, 0, -7) == 0 && t.rt_prio == 0 && t.nice == -7 && t.weight == sched_nice_to_weight(-7));
        CHECK(sched_task_set(&t, SCHED_IDLE, 0, 0) == 0 && t.weight == SCHED_IDLE_WEIGHT);
        CHECK(sched_task_set(&t, SCHED_OTHER, 0, 20) == -22);
    }

    printf("sched host test ok\n");
    return 0;
}
