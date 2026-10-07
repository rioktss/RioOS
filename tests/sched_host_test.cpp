#include "sched_policy.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static void mk(SchedEntry* e, int state, uint32_t prio) { e->state = state; e->priority = prio; e->waited = 0; e->wake_ms = 0; }

int main()
{
    SchedEntry t[4]; memset(t, 0, sizeof t);

    /* nothing runnable */
    CHECK(sched_pick(t, 4, 0) == -1);
    CHECK(sched_pick(t, 0, 0) == -1);

    /* equal priority: round robin, current considered last */
    mk(&t[0], SCHED_RUNNING, 1); mk(&t[1], SCHED_READY, 1); mk(&t[2], SCHED_READY, 1);
    CHECK(sched_pick(t, 4, 0) == 1);
    CHECK(sched_pick(t, 4, 1) == 2);
    CHECK(sched_pick(t, 4, 2) == 0);

    /* higher priority wins */
    mk(&t[3], SCHED_READY, 3);
    CHECK(sched_pick(t, 4, 0) == 3);

    /* sleepers and dead entries are never picked */
    t[3].state = SCHED_SLEEPING; t[3].wake_ms = 100;
    t[1].state = SCHED_DEAD;
    CHECK(sched_pick(t, 4, 0) == 2);
    CHECK(sched_wake_sleepers(t, 4, 99) == 0 && t[3].state == SCHED_SLEEPING);
    CHECK(sched_wake_sleepers(t, 4, 100) == 1 && t[3].state == SCHED_READY);
    CHECK(sched_pick(t, 4, 0) == 3);

    /* starvation protection: a prio-0 thread eventually beats a prio-3 hog */
    SchedEntry s[2]; memset(s, 0, sizeof s);
    mk(&s[0], SCHED_RUNNING, 3); mk(&s[1], SCHED_READY, 0);
    int picked_low = -1;
    for (int tick = 1; tick <= 200; ++tick)
    {
        int c = sched_pick(s, 2, 0);
        sched_account(s, 2, c);
        if (c == 1) { picked_low = tick; break; }
    }
    CHECK(picked_low > 0 && picked_low <= 60);

    /* after it ran, its wait resets and the hog gets the CPU back */
    s[1].state = SCHED_RUNNING; s[0].state = SCHED_READY;
    CHECK(s[1].waited == 0);
    CHECK(sched_pick(s, 2, 1) == 0);

    /* effective priority is capped */
    SchedEntry e; mk(&e, SCHED_READY, 0); e.waited = 100000;
    CHECK(sched_effective_priority(&e) == SCHED_PRIO_MAX + 1U);

    printf("SCHED HOST TEST: PASS\n");
    return 0;
}
