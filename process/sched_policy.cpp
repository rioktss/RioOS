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

static const uint32_t prio_to_weight[40] =
{
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */  9548,  7620,  6100,  4904,  3906,
    /*  -5 */  3121,  2501,  1991,  1586,  1277,
    /*   0 */  1024,   820,   655,   526,   423,
    /*   5 */   335,   272,   215,   172,   137,
    /*  10 */   110,    87,    70,    56,    45,
    /*  15 */    36,    29,    23,    18,    15
};

int sched_policy_is_rt(int policy) { return policy == SCHED_FIFO || policy == SCHED_RR; }

int sched_params_valid(int policy, int rt_prio, int nice)
{
    if (sched_policy_is_rt(policy)) return rt_prio >= SCHED_RT_MIN && rt_prio <= SCHED_RT_MAX;
    if (policy == SCHED_OTHER || policy == SCHED_BATCH || policy == SCHED_IDLE)
        return rt_prio == 0 && nice >= SCHED_NICE_MIN && nice <= SCHED_NICE_MAX;
    return 0;
}

uint32_t sched_nice_to_weight(int nice)
{
    if (nice < SCHED_NICE_MIN) nice = SCHED_NICE_MIN;
    if (nice > SCHED_NICE_MAX) nice = SCHED_NICE_MAX;
    return prio_to_weight[nice + 20];
}

int sched_eff_rt_prio(const SchedTask* t)
{
    int p = sched_policy_is_rt(t->policy) ? t->rt_prio : 0;
    return t->boost > p ? t->boost : p;
}

int sched_cpu_allowed(const SchedTask* t, int cpu)
{
    if (t->affinity == 0) return 1;
    if (cpu < 0 || cpu >= 64) return 0;
    return (int)((t->affinity >> cpu) & 1ULL);
}

static uint32_t weight_for(int policy, int nice)
{
    return policy == SCHED_IDLE ? SCHED_IDLE_WEIGHT : sched_nice_to_weight(nice);
}

void sched_task_init(SchedTask* t, int policy, int rt_prio, int nice)
{
    t->state = SCHED_FREE;
    t->policy = policy;
    t->nice = nice;
    t->rt_prio = sched_policy_is_rt(policy) ? rt_prio : 0;
    t->boost = 0;
    t->weight = weight_for(policy, nice);
    t->vruntime = 0;
    t->affinity = 0;
    t->rr_left = 0;
    t->queued = 0;
    t->qkind = 0;
    t->qlevel = 0;
    t->is_idle_task = 0;
    t->next = 0;
    t->prev = 0;
}

int sched_task_set(SchedTask* t, int policy, int rt_prio, int nice)
{
    if (!sched_params_valid(policy, rt_prio, nice)) return -22;
    t->policy = policy;
    if (sched_policy_is_rt(policy)) t->rt_prio = rt_prio;
    else { t->rt_prio = 0; t->nice = nice; }
    t->weight = weight_for(policy, t->nice);
    t->rr_left = 0;
    return 0;
}

void sched_rq_init(SchedRq* rq)
{
    for (int i = 0; i < SCHED_RT_LEVELS; ++i) { rq->rt_head[i] = 0; rq->rt_tail[i] = 0; }
    rq->rt_bitmap[0] = rq->rt_bitmap[1] = 0;
    rq->fair_head = rq->fair_tail = 0;
    rq->idle_head = rq->idle_tail = 0;
    rq->min_vruntime = 0;
    rq->nr_queued = 0;
}

static void push_tail(SchedTask** head, SchedTask** tail, SchedTask* t)
{
    t->next = 0; t->prev = *tail;
    if (*tail) (*tail)->next = t; else *head = t;
    *tail = t;
}

static void push_head(SchedTask** head, SchedTask** tail, SchedTask* t)
{
    t->prev = 0; t->next = *head;
    if (*head) (*head)->prev = t; else *tail = t;
    *head = t;
}

static void unlink_task(SchedTask** head, SchedTask** tail, SchedTask* t)
{
    if (t->prev) t->prev->next = t->next; else *head = t->next;
    if (t->next) t->next->prev = t->prev; else *tail = t->prev;
    t->next = t->prev = 0;
}

static uint64_t tick_vruntime(const SchedTask* t)
{
    return SCHED_TICK_NS * 1024ULL / (uint64_t)t->weight;
}

void sched_enqueue(SchedRq* rq, SchedTask* t, int flags)
{
    if (t->queued) return;
    int eff = sched_eff_rt_prio(t);
    if (eff > 0)
    {
        if (eff > SCHED_RT_MAX) eff = SCHED_RT_MAX;
        t->qkind = 1; t->qlevel = eff;
        if (flags & SCHED_Q_HEAD) push_head(&rq->rt_head[eff], &rq->rt_tail[eff], t);
        else push_tail(&rq->rt_head[eff], &rq->rt_tail[eff], t);
        rq->rt_bitmap[eff >> 6] |= 1ULL << (eff & 63);
    }
    else
    {
        if (flags & SCHED_Q_NEW) t->vruntime = rq->min_vruntime + tick_vruntime(t);
        else if (flags & SCHED_Q_WAKEUP)
        {
            uint64_t floor = rq->min_vruntime > SCHED_SLEEPER_CREDIT_NS ? rq->min_vruntime - SCHED_SLEEPER_CREDIT_NS : 0;
            if (t->vruntime < floor) t->vruntime = floor;
        }
        else if (flags & SCHED_Q_YIELD)
        {
            if (t->vruntime < rq->min_vruntime) t->vruntime = rq->min_vruntime;
        }
        if (t->policy == SCHED_IDLE) { t->qkind = 3; push_tail(&rq->idle_head, &rq->idle_tail, t); }
        else { t->qkind = 2; push_tail(&rq->fair_head, &rq->fair_tail, t); }
    }
    t->queued = 1;
    ++rq->nr_queued;
}

void sched_dequeue(SchedRq* rq, SchedTask* t)
{
    if (!t->queued) return;
    if (t->qkind == 1)
    {
        int lvl = t->qlevel;
        unlink_task(&rq->rt_head[lvl], &rq->rt_tail[lvl], t);
        if (rq->rt_head[lvl] == 0) rq->rt_bitmap[lvl >> 6] &= ~(1ULL << (lvl & 63));
    }
    else if (t->qkind == 2) unlink_task(&rq->fair_head, &rq->fair_tail, t);
    else unlink_task(&rq->idle_head, &rq->idle_tail, t);
    t->queued = 0;
    t->qkind = 0;
    --rq->nr_queued;
}

/* Highest real-time level that holds a task allowed on `cpu`; 0 when none. `skip` is ignored when it is not the only candidate. */
static int rt_level_for(const SchedRq* rq, int cpu, SchedTask** first, const SchedTask* skip, int* only_skip)
{
    *first = 0;
    if (only_skip) *only_skip = 0;
    for (int lvl = SCHED_RT_MAX; lvl >= SCHED_RT_MIN; --lvl)
    {
        if (!((rq->rt_bitmap[lvl >> 6] >> (lvl & 63)) & 1ULL)) continue;
        SchedTask* skipped = 0;
        for (SchedTask* t = rq->rt_head[lvl]; t; t = t->next)
        {
            if (!sched_cpu_allowed(t, cpu)) continue;
            if (t == skip) { skipped = t; continue; }
            *first = t;
            return lvl;
        }
        if (skipped) { *first = skipped; if (only_skip) *only_skip = 1; return lvl; }
    }
    return 0;
}

static SchedTask* leftmost_fair(const SchedRq* rq, int cpu, const SchedTask* skip, SchedTask** skipped_out)
{
    SchedTask* best = 0;
    if (skipped_out) *skipped_out = 0;
    for (SchedTask* t = rq->fair_head; t; t = t->next)
    {
        if (!sched_cpu_allowed(t, cpu)) continue;
        if (t == skip) { if (skipped_out) *skipped_out = t; continue; }
        if (!best || t->vruntime < best->vruntime) best = t;
    }
    return best;
}

SchedTask* sched_pick(SchedRq* rq, int cpu, int rt_allowed, const SchedTask* skip, int skip_strict)
{
    SchedTask* chosen = 0;
    SchedTask* strict_fallback = 0;
    const SchedTask* class_skip = skip_strict ? 0 : skip;
    const SchedTask* hard_skip = skip_strict ? skip : 0;

    if (rt_allowed)
    {
        SchedTask* first = 0; int only = 0;
        int lvl = rt_level_for(rq, cpu, &first, hard_skip ? hard_skip : class_skip, &only);
        if (lvl > 0)
        {
            if (hard_skip && first == hard_skip) strict_fallback = first;
            else chosen = first;
        }
    }
    if (!chosen)
    {
        SchedTask* skipped = 0;
        SchedTask* best = leftmost_fair(rq, cpu, skip, &skipped);
        if (best) chosen = best;
        else if (skipped && !skip_strict) chosen = skipped;
        else if (skipped) strict_fallback = skipped;
    }
    if (!chosen)
    {
        for (SchedTask* t = rq->idle_head; t; t = t->next)
            if (sched_cpu_allowed(t, cpu)) { chosen = t; break; }
    }
    if (!chosen) chosen = strict_fallback;
    if (chosen)
    {
        sched_dequeue(rq, chosen);
        if (chosen->policy == SCHED_RR && chosen->rr_left <= 0) chosen->rr_left = SCHED_RR_QUANTUM_TICKS;
    }
    return chosen;
}

SchedTask* sched_schedule(SchedRq* rq, int cpu, SchedTask* cur, int how, int rt_allowed, SchedTask* idle_task)
{
    const SchedTask* skip = 0;
    int strict = 0;
    if (cur && cur->state == SCHED_RUNNING && !cur->is_idle_task)
    {
        int flags = SCHED_Q_TAIL;
        int eff = sched_eff_rt_prio(cur);
        if (how == SCHED_HOW_PREEMPT)
        {
            /* a real-time task pushed off by a higher priority one resumes first; a round-robin task that used its quantum goes behind its peers */
            if (eff > 0 && !(cur->policy == SCHED_RR && cur->rr_left <= 0)) flags = SCHED_Q_HEAD;
        }
        else
        {
            flags |= SCHED_Q_YIELD;
            skip = cur;
            strict = (how == SCHED_HOW_IDLE_YIELD);
        }
        cur->state = SCHED_READY;
        sched_enqueue(rq, cur, flags);
    }
    SchedTask* next = sched_pick(rq, cpu, rt_allowed, skip, strict);
    if (!next) next = idle_task;
    if (next) next->state = SCHED_RUNNING;
    return next;
}

void sched_tick_account(SchedRq* rq, SchedTask* cur)
{
    if (cur && !cur->is_idle_task)
    {
        if (sched_eff_rt_prio(cur) > 0)
        {
            if (cur->policy == SCHED_RR && cur->rr_left > 0) --cur->rr_left;
        }
        else cur->vruntime += tick_vruntime(cur);
    }
    /* min_vruntime follows the slowest runnable fair task and never moves back. */
    uint64_t low = ~0ULL;
    if (cur && !cur->is_idle_task && sched_eff_rt_prio(cur) == 0 && cur->policy != SCHED_IDLE) low = cur->vruntime;
    for (SchedTask* t = rq->fair_head; t; t = t->next) if (t->vruntime < low) low = t->vruntime;
    if (low != ~0ULL && low > rq->min_vruntime) rq->min_vruntime = low;
}

int sched_should_preempt(SchedRq* rq, SchedTask* cur, int cpu, int rt_allowed)
{
    SchedTask* first = 0;
    if (cur->is_idle_task)
    {
        if (rt_allowed && rt_level_for(rq, cpu, &first, 0, 0) > 0) return 1;
        if (leftmost_fair(rq, cpu, 0, 0)) return 1;
        for (SchedTask* t = rq->idle_head; t; t = t->next) if (sched_cpu_allowed(t, cpu)) return 1;
        return 0;
    }
    int eff = sched_eff_rt_prio(cur);
    if (eff > 0)
    {
        if (!rt_allowed)
        {
            if (leftmost_fair(rq, cpu, 0, 0)) return 1;
            for (SchedTask* t = rq->idle_head; t; t = t->next) if (sched_cpu_allowed(t, cpu)) return 1;
            return 0;
        }
        int lvl = rt_level_for(rq, cpu, &first, 0, 0);
        if (lvl > eff) return 1;
        if (cur->policy == SCHED_RR && cur->rr_left <= 0)
        {
            if (lvl == eff) return 1;                       /* a peer is waiting: rotate */
            cur->rr_left = SCHED_RR_QUANTUM_TICKS;          /* nobody to hand over to */
        }
        return 0;
    }
    if (rt_allowed && rt_level_for(rq, cpu, &first, 0, 0) > 0) return 1;
    SchedTask* best = leftmost_fair(rq, cpu, 0, 0);
    if (cur->policy == SCHED_IDLE) return best != 0;
    if (best && cur->vruntime > best->vruntime + SCHED_GRAN_NS) return 1;
    return 0;
}

int sched_wakeup_preempts(const SchedTask* woken, const SchedTask* cur)
{
    if (cur->is_idle_task) return 1;
    int we = sched_eff_rt_prio(woken);
    int ce = sched_eff_rt_prio(cur);
    if (we > 0 || ce > 0) return we > ce;
    if (cur->policy == SCHED_IDLE) return woken->policy != SCHED_IDLE;
    if (woken->policy == SCHED_IDLE || woken->policy == SCHED_BATCH) return 0;
    return cur->vruntime > woken->vruntime + SCHED_WAKE_GRAN_NS;
}

void sched_rt_budget_init(SchedRtBudget* b)
{
    b->period_left = SCHED_RT_PERIOD_TICKS;
    b->used = 0;
    b->throttled = 0;
}

int sched_rt_budget_tick(SchedRtBudget* b, int cur_is_rt)
{
    if (cur_is_rt && b->used < SCHED_RT_PERIOD_TICKS) ++b->used;
    if (--b->period_left <= 0)
    {
        b->period_left = SCHED_RT_PERIOD_TICKS;
        b->used = 0;
        b->throttled = 0;
    }
    else if (b->used >= SCHED_RT_RUNTIME_TICKS) b->throttled = 1;
    return b->throttled;
}
