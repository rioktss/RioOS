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
int task_reap(int pid);                 /* destroy a zombie's record */
int task_reap_orphans(int shell_pid, void (*notify)(int pid, int code, int sig));
int task_running_count();

extern "C" void task_deliver_signals();
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
