#ifndef THREAD_H
#define THREAD_H

#include "types.h"

/* Kernel threads with context switching (opt-in; boot behaviour unchanged).
   Thread 0 is the boot/shell context. */

#define THREAD_MAX 4
#define THREAD_STACK_SIZE 16384
#define THREAD_SLICE_TICKS 5

typedef void (*ThreadEntry)(void*);

void thread_init();

/* Returns the thread id (1..THREAD_MAX-1) or -1 when no slot is free.
   priority is clamped to 0..3. */
int thread_create(const char* name, ThreadEntry entry, void* arg, uint32_t priority);

void thread_yield();
void thread_sleep_ms(uint64_t ms);
int thread_current();

/* Timer-driven preemption. Needs the periodic timer + CPU IRQs enabled. */
void thread_set_preempt(int enabled);
int thread_preempt_enabled();

/* Hooks called from IRQ context (IRQs masked). */
void thread_timer_tick();
void thread_preempt_check();

int thread_active_count();      /* threads that are not FREE/DEAD */
uint64_t thread_switch_count();
void thread_list();

extern "C" void thread_exit();
extern "C" void thread_entry_hook();
extern "C" void thread_context_switch(uint64_t* old_sp_out, uint64_t new_sp);
extern "C" void thread_trampoline();

#endif
