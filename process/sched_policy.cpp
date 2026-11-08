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

int sched_wake_sleepers(SchedEntry* entries, int count, uint64_t now_ms)
{
    int woken = 0;

    for (int i = 0; i < count; ++i)
    {
        if (entries[i].state == SCHED_SLEEPING && now_ms >= entries[i].wake_ms)
        {
            entries[i].state = SCHED_READY;
            entries[i].waited = 0;
            ++woken;
        }
    }

    return woken;
}

uint32_t sched_effective_priority(const SchedEntry* entry)
{
    uint32_t bonus = entry->waited / SCHED_AGING_TICKS;

    /* Cap the bonus so aging can lift a thread above every static level. */
    if (bonus > SCHED_PRIO_MAX + 1U)
        bonus = SCHED_PRIO_MAX + 1U;

    return entry->priority + bonus;
}

int sched_pick(const SchedEntry* entries, int count, int current)
{
    int best = -1;
    uint32_t best_prio = 0;

    if (count <= 0)
        return -1;

    int start = (current >= 0 && current < count) ? current : count - 1;

    /* Walk starting after `current`, so equal priorities rotate and the
       current entry is considered last. */
    for (int offset = 1; offset <= count; ++offset)
    {
        int i = (start + offset) % count;

        if (entries[i].state != SCHED_READY && entries[i].state != SCHED_RUNNING)
            continue;

        uint32_t prio = sched_effective_priority(&entries[i]);

        if (best < 0 || prio > best_prio)
        {
            best = i;
            best_prio = prio;
        }
    }

    return best;
}

void sched_account(SchedEntry* entries, int count, int chosen)
{
    for (int i = 0; i < count; ++i)
    {
        if (i == chosen)
            entries[i].waited = 0;
        else if (entries[i].state == SCHED_READY)
            ++entries[i].waited;
    }
}
