#ifndef SCHED_POLICY_H
#define SCHED_POLICY_H

#include "types.h"

/* Pure scheduling policy (no hardware access) so it can be unit-tested. */

enum SchedState
{
    SCHED_FREE = 0,
    SCHED_READY = 1,
    SCHED_RUNNING = 2,
    SCHED_SLEEPING = 3,
    SCHED_DEAD = 4
};

#define SCHED_PRIO_MAX 3U          /* priorities 0 (lowest) .. 3 (highest)   */
#define SCHED_AGING_TICKS 10U      /* +1 effective priority per N ticks waited */

struct SchedEntry
{
    int state;               /* SchedState */
    uint32_t priority;       /* 0..SCHED_PRIO_MAX */
    uint32_t waited;         /* ticks spent READY without running */
    uint64_t wake_ms;        /* valid when SLEEPING */
};

/* Moves SLEEPING entries whose wake time has passed to READY.
   Returns how many were woken. */
int sched_wake_sleepers(SchedEntry* entries, int count, uint64_t now_ms);

/* Effective priority = priority + aging bonus (starvation protection). */
uint32_t sched_effective_priority(const SchedEntry* entry);

/* Picks the next entry to run among READY/RUNNING ones: highest effective
   priority wins; ties rotate round-robin starting after `current`.
   Returns -1 when nothing is runnable. */
int sched_pick(const SchedEntry* entries, int count, int current);

/* Accounting after a decision: the chosen entry's wait resets, every other
   READY entry ages by one tick. */
void sched_account(SchedEntry* entries, int count, int chosen);

#endif
