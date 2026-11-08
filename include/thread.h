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

/* Kernel threads with context switching (opt-in; boot behaviour unchanged).
   Thread 0 is the boot/shell context. */

#define THREAD_MAX 40
#define THREAD_STACK_SIZE 32768
#define THREAD_SLICE_TICKS 5

typedef void (*ThreadEntry)(void*);

void thread_init();

/* Returns the thread id (1..THREAD_MAX-1) or -1 when no slot is free.
   priority is clamped to 0..3. */
int thread_create(const char* name, ThreadEntry entry, void* arg, uint32_t priority);

/* Like thread_create but binds the thread to a user process: on every switch
   to it the process address space and descriptor table are activated. */
int thread_create_for_pid(const char* name, ThreadEntry entry, void* arg, uint32_t priority, int pid);
int thread_pid(int thread);        /* bound pid or -1 */
int thread_current_pid();          /* pid bound to the running thread, -1 for the shell/kernel thread */
int thread_kill(int thread);       /* terminate another thread (never the running one) */
int thread_state_of(int thread);   /* SchedState value */
extern "C" void thread_user_preempt();        /* IRQ-from-EL0 hook: switch if the slice expired */
void thread_set_user_tasks(int count);
int thread_user_tasks();
void thread_wait_ms(uint64_t ms);  /* sleep (user threads) or yield-wait (shell) */
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
