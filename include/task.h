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

#ifndef TASK_H
#define TASK_H

#include "types.h"
#include "exception_frame.h"

/* Preemptive EL0 tasks: every user process owns a kernel thread (with its own
   kernel stack) and a private address space. */

#define TASK_WNOHANG 1

/* Spawn a user program as a new process. background != 0 detaches it from the
   shell's foreground. in/out fds redirect the child's stdin/stdout to a
   descriptor of `parent` (use -1 to inherit the console). Returns the pid or
   a negative error code (-2 not found / bad ELF, -12 no memory, -11 no slot). */
int task_spawn(const char* path, const char* args, int parent, int background,
               int parent_in_fd, int parent_out_fd);

/* Scheduling parameters for the next program the shell starts (nice / chrt / taskset prefix commands). Programs started by a
   user process, and forked children, inherit the starter's policy, priority, nice value and CPU mask instead. */
struct ThreadParams;
void task_set_spawn_params(const ThreadParams* params);   /* 0 clears */

int task_fork(ExceptionFrame* frame);
int task_exec(ExceptionFrame* frame, const char* path, const char* args);
extern "C" void task_exit_current(int code, int signal);   /* exit_group: the whole process ends */
extern "C" void task_thread_die_code(int code);            /* the calling thread ends; the last one finishes the process */
extern "C" void task_thread_die();
int task_thread_create(ExceptionFrame* frame, uint64_t stack_top, uint64_t tls, uint64_t clear_tid);   /* new thread id or -errno */
int task_tgkill(int tgid, uint32_t tid, int signal, int sender);
int task_wait(int parent, int pid, int* status, int options);
int task_kill(int pid, int signal);
int task_kill_info(int pid, int signal, int sender, int code, uint64_t value);   /* sender pid, si_code, si_value */
int task_deliver_fault(ExceptionFrame* frame, int sig, uint64_t addr);       /* 0 = a user handler was entered */
int task_reap(int pid);                 /* destroy a zombie's record */
int task_reap_orphans(int shell_pid, void (*notify)(int pid, int code, int sig));
int task_running_count();
int task_thread_total();
/* signal state of the calling thread (mask and thread-directed signals) combined with the process-directed ones */
uint32_t task_sig_mask();
void task_sig_set_mask(uint32_t mask);
uint32_t task_sig_pending(int pid);          /* deliverable now */
uint32_t task_sig_pending_raw(int pid);      /* including blocked */
int task_sig_take(int pid);
int task_sig_take_set(int pid, uint32_t set);
void task_alarm_poke(int pid);           /* sleeping threads of the process re-check their alarm deadline */
int task_sig_interrupts(int pid);            /* a blocking call must return -EINTR */

extern "C" void task_deliver_signals(ExceptionFrame* frame);
int task_sigreturn(ExceptionFrame* frame);          /* restores the context saved by a signal frame */
extern "C" void task_context_enter(int pid);

/* Console job control used by SYS_READ: returns 1 if the byte was consumed
   as a job-control key (Ctrl-C / Ctrl-Z) for the calling foreground task. */
int task_console_key(int pid, char c);

/* Shell side: block until `pid` exits or stops. Returns the exit code, or
   TASK_STOPPED when the job was suspended. */
#define TASK_STOPPED (-1000)
int task_wait_foreground(int pid, int shell_pid);

extern "C" void user_resume(const void* context, uint64_t kernel_stack_top);
uint64_t thread_stack_top_current();

#endif
