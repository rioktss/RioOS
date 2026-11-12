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

#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"

#define PROCESS_ARGS_SIZE 1024   /* argument line, optionally followed by \x01 and NAME=value environment entries */

enum ProcessState
{
    PROCESS_UNUSED = 0,
    PROCESS_READY = 1,
    PROCESS_RUNNING = 2,
    PROCESS_SLEEPING = 3,
    PROCESS_ZOMBIE = 4
};

/* EL0 architectural state retained at an exception boundary. This is metadata
   for a resumable process context; it is distinct from kernel-thread SP state. */
struct ProcessCpuContext
{
    uint64_t x[31];
    uint64_t elr;
    uint64_t spsr;
    uint64_t user_sp;
};

enum ProcessKind
{
    PROCESS_KERNEL = 0,
    PROCESS_USER = 1
};

void process_init();
int process_create(const char* name);
int process_cwd(int pid);                     /* working-directory node, -1 = inherit the shell's */
int process_set_cwd(int pid, int node);
uint32_t process_umask(int pid);
int process_set_umask(int pid, uint32_t mask);
/* process_destroy returns -2 for built-ins, -3 for a RUNNING record, -1 if missing. */
int process_destroy(int pid);
int process_count();
void process_list();
int process_set_state(int pid, int state);
int process_get_state(int pid);
int process_configure_user(int pid, uint64_t low, uint64_t high);
int process_mark_exit(int pid, int code);
int process_is_user(int pid);
uint64_t process_user_low(int pid);
uint64_t process_user_high(int pid);
int process_exit_code(int pid);
int process_save_context(int pid, const ProcessCpuContext* context);
int process_load_context(int pid, ProcessCpuContext* context);
int process_context_valid(int pid);
int process_pid_at(int slot);


/* ---- multitasking metadata (fork/exec/wait, signals, job control) ---- */
enum Signal { SIG_HUP = 1, SIG_INT = 2, SIG_QUIT = 3, SIG_KILL = 9, SIG_USR1 = 10, SIG_SEGV = 11,
              SIG_USR2 = 12, SIG_PIPE = 13, SIG_ALRM = 14, SIG_ILL = 4, SIG_BUS = 7, SIG_FPE = 8, SIG_TERM = 15, SIG_CHLD = 17, SIG_CONT = 18,
              SIG_STOP = 19, SIG_TSTP = 20 };

int process_set_parent(int pid, int ppid);
int process_parent(int pid);
int process_set_thread(int pid, int thread);
int process_thread(int pid);
int process_set_args(int pid, const char* args);
const char* process_args(int pid);
const char* process_name(int pid);
int process_set_name(int pid, const char* name);
int process_set_term_signal(int pid, int sig);
int process_term_signal(int pid);
int process_set_background(int pid, int bg);
int process_is_background(int pid);
int process_set_stopped(int pid, int v);
int process_is_stopped(int pid);
int process_sig_raise(int pid, int sig);
int process_sig_raise_info(int pid, int sig, int sender, int code, uint64_t value);   /* siginfo: sender pid, si_code, si_value */
int process_sig_info_get(int pid, int sig, int* sender, int* code, uint64_t* value);
int process_sig_take_set(int pid, uint32_t set);   /* sigwait: lowest pending signal in `set`, blocked or not */
int process_altstack_set(int pid, uint64_t sp, uint64_t size);
int process_altstack_get(int pid, uint64_t* sp, uint64_t* size);
uint64_t process_itimer_get(int pid, uint64_t now, uint64_t* interval_ms);
uint64_t process_itimer_set(int pid, uint64_t value_ms, uint64_t interval_ms, uint64_t now);   /* returns previous remaining ms */
int process_sig_take(int pid);               /* lowest pending, unblocked signal (KILL/STOP are never blocked) */
uint32_t process_sig_pending(int pid);       /* pending and deliverable (not blocked) */
uint32_t process_sig_pending_raw(int pid);   /* pending, including blocked */
/* user signal handlers: handler 0 = default, 1 = ignore, else a user address */
int process_sig_set_action(int pid, int sig, uint64_t handler, uint32_t amask, uint32_t flags, uint64_t restorer, uint64_t* old);
uint64_t process_sig_handler(int pid, int sig, uint32_t* amask, uint32_t* flags);
uint64_t process_sig_restorer(int pid);
uint32_t process_sig_mask(int pid);
int process_sig_set_mask(int pid, uint32_t mask);
int process_sig_inherit(int parent, int child);   /* fork: handlers, mask */
int process_sig_reset_on_exec(int pid);           /* exec: caught handlers -> default (ignored stay ignored) */
int process_sig_clear_all(int pid);
/* alarm(): returns the previous remaining milliseconds; ms == 0 cancels */
uint64_t process_alarm_set(int pid, uint64_t ms, uint64_t now);
void process_alarm_check(int pid, uint64_t now);
int process_set_creds(int pid, uint32_t uid, uint32_t gid);
uint32_t process_uid(int pid);
uint32_t process_gid(int pid);
void process_set_start(int pid, uint64_t ms);
uint64_t process_start_ms(int pid);
int process_has_children(int parent);
int process_find_zombie_child(int parent, int wanted);
int process_is_child_of(int parent, int pid);
void process_reparent_children(int from, int to);

#endif
