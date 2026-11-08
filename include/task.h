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

int task_fork(ExceptionFrame* frame);
int task_exec(ExceptionFrame* frame, const char* path, const char* args);
extern "C" void task_exit_current(int code, int signal);
int task_wait(int parent, int pid, int* status, int options);
int task_kill(int pid, int signal);
int task_kill_info(int pid, int signal, int sender, int code, uint64_t value);   /* sender pid, si_code, si_value */
int task_deliver_fault(ExceptionFrame* frame, int sig, uint64_t addr);       /* 0 = a user handler was entered */
int task_reap(int pid);                 /* destroy a zombie's record */
int task_reap_orphans(int shell_pid, void (*notify)(int pid, int code, int sig));
int task_running_count();

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
