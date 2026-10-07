#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "types.h"

/* Reserved for the future full context-switch implementation. */
struct CpuContext
{
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t x29;
    uint64_t x30;
    uint64_t sp;
    uint64_t pc;
};

void scheduler_init();
int scheduler_add(int pid);
int scheduler_remove(int pid);
int scheduler_current_pid();
int scheduler_next_pid();

/* Returns 1 when the logical current PID changed, otherwise 0. */
int scheduler_tick();

/* Set when a timer tick selects a different runnable process. */
int scheduler_preempt_pending();
void scheduler_clear_preempt_pending();

uint64_t scheduler_ticks();
int scheduler_ready_count();
void scheduler_status();

#endif
