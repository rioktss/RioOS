#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"

#define PROCESS_ARGS_SIZE 128

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
              SIG_USR2 = 12, SIG_PIPE = 13, SIG_TERM = 15, SIG_CHLD = 17, SIG_CONT = 18,
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
int process_sig_take(int pid);
uint32_t process_sig_pending(int pid);
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
