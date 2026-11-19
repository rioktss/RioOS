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

#include "task.h"
#include "thread.h"
#include "smp.h"
#include "fpu.h"
#include "tty.h"
#include "vfs.h"
#include "process.h"
#include "scheduler.h"
#include "mmu.h"
#include "elf.h"
#include "fd.h"
#include "fs.h"
#include "uart.h"
#include "timer.h"
#include "netstack.h"
#include "string.h"
#include "security.h"
#include "mapping.h"
#include "futex.h"
#include "memory.h"

#define USER_SPSR_EL0T 0x340ULL   /* EL0t, IRQ unmasked, D/A/F masked */
#define TASK_EXEC_TMP_BASE 0x20000000   /* scratch address-space ids sit above every pid */
#define SIG_UNBLOCKABLE_K ((1U << 9) | (1U << 19))

static int user_task_total = 0;      /* user processes */
static int user_thread_total = 0;    /* user threads (what the scheduler cares about) */

static uint64_t irq_save_local()
{
    uint64_t daif;
    asm volatile("mrs %0, daif\n msr daifset, #0x2\n isb" : "=r"(daif) :: "memory");
    return daif;
}
static int kernel_pid() { int p = process_pid_at(0); return p >= 0 ? p : 0; }

/* Called by the thread layer every time a thread (re)gains the CPU. */
extern "C" void task_context_enter(int pid)
{
    if (pid >= 0 && mmu_user_space_exists(pid))
    {
        if (!mmu_user_space_is_loaded(pid)) (void)mmu_user_space_activate(pid);
        (void)fd_set_process(pid);
        (void)scheduler_set_current_logical(pid);
        fs_set_credentials(process_uid(pid), process_gid(pid));     /* privilege separation */
    }
    else
    {
        mmu_ensure_kernel_space();
        int kp = kernel_pid();
        (void)fd_set_process(kp);
        (void)scheduler_set_current_logical(kp);
        security_apply_fs_credentials();
    }
}

static void task_thread_main(void* arg)
{
    int pid = (int)(uint64_t)arg;
    ProcessCpuContext local;
    if (process_load_context(pid, &local) != 0) task_exit_current(-8, 0);
    user_resume(&local, thread_stack_top_current());
}

/* First run of a thread created by task_thread_create: resumes in user mode with a copy of the creator's registers. */
struct ThreadStart { ProcessCpuContext ctx; };
static void task_thread_entry(void* arg)
{
    ProcessCpuContext local = ((ThreadStart*)arg)->ctx;
    kfree(arg);
    user_resume(&local, thread_stack_top_current());
}

static void ctx_for_entry(ProcessCpuContext* c, uint64_t entry, int pid)
{
    for (int i = 0; i < 31; ++i) c->x[i] = 0;
    c->elr = entry;
    c->spsr = USER_SPSR_EL0T;
    c->user_sp = mmu_user_space_stack_top(pid) - 16ULL;
}

static ThreadParams spawn_pending;
static int spawn_pending_valid = 0;

void task_set_spawn_params(const ThreadParams* params)
{
    if (params) { spawn_pending = *params; spawn_pending_valid = 1; }
    else spawn_pending_valid = 0;
}

static int bind_thread(int pid, const char* name)
{
    ThreadParams p;
    thread_params_default(&p);
    int cur = thread_current();
    if (thread_pid(cur) >= 0)
    {
        int pol = 0, pr = 0, ni = 0;
        if (thread_get_sched(cur, &pol, &pr, &ni) == 0) { p.policy = pol; p.rt_prio = pr; p.nice = ni; }
        p.affinity = thread_get_affinity(cur);
    }
    else if (spawn_pending_valid) p = spawn_pending;
    p.tls = 0;
    if (thread_pid(cur) >= 0) asm volatile("mrs %0, tpidr_el0" : "=r"(p.tls));   /* fork: the child sees the same thread pointer */
    int t = thread_create_ex(name, task_thread_main, (void*)(uint64_t)pid, pid, &p);
    if (t < 0) return -11;
    thread_set_tid(t, (uint32_t)pid);
    (void)process_set_thread(pid, t);
    if (thread_pid(cur) >= 0)
    {
        thread_set_sigmask(t, thread_sigmask_of(cur));              /* fork keeps the caller's mask; a spawn from the shell starts clean */
        uint64_t asp = 0, asz = 0;
        thread_altstack_get(cur, &asp, &asz);
        thread_altstack_set(t, asp, asz);
    }
    (void)process_thread_adjust(pid, 1);
    ++user_task_total;
    ++user_thread_total;
    thread_set_user_tasks(user_thread_total);
    return 0;
}

static void drop_process(int pid)
{
    net_udp_close_all_for(pid);
    net_tcp_close_all_for(pid);
    tty_process_exit(pid);
    fd_release_process(pid);
    (void)scheduler_remove(pid);
    (void)mmu_user_space_destroy(pid);
    (void)process_destroy(pid);
}

int task_spawn(const char* path, const char* args, int parent, int background,
               int parent_in_fd, int parent_out_fd)
{
    if (!path || !path[0]) return -2;
    if (!mmu_enabled()) return -12;

    int saved_fd_owner = fd_current_process();
    int active = mmu_user_space_active_pid();

    int pid = process_create(path);
    if (pid >= 0) (void)process_set_cwd(pid, vfs_cwd());
    if (pid < 0) return -11;
    (void)process_set_parent(pid, parent);
    (void)process_set_args(pid, args ? args : "");
    (void)process_set_background(pid, background ? 1 : 0);
    (void)process_set_start(pid, timer_millis());
    (void)process_set_creds(pid, security_uid(), security_gid());

    uint64_t entry = 0;
    int rc = elf_load_for_process(path, &entry, pid);
    if (active >= 0) (void)mmu_user_space_activate(active);
    if (rc != 0 || entry < MMU_USER_BASE || entry >= MMU_USER_LIMIT || (entry & 3ULL) != 0)
    {
        (void)mmu_user_space_destroy(pid);
        (void)process_destroy(pid);
        return -2;
    }
    if (process_configure_user(pid, MMU_USER_BASE, MMU_USER_STACK_TOP) != 0 || scheduler_add(pid) != 0)
    {
        drop_process(pid);
        return -11;
    }
    if (fd_set_process(pid) != 0)
    {
        drop_process(pid);
        return -24;
    }
    fd_close_all();
    /* The shell (pid 1) keeps its descriptors in the permanent kernel table (pid 0). */
    int fd_parent = parent == 1 ? kernel_pid() : parent;
    if (parent_in_fd >= 0 && fd_redirect(pid, 0, fd_parent, parent_in_fd) != 0) { drop_process(pid); return -9; }
    if (parent_out_fd >= 0 && fd_redirect(pid, 1, fd_parent, parent_out_fd) != 0) { drop_process(pid); return -9; }
    if (saved_fd_owner >= 0) (void)fd_set_process(saved_fd_owner);

    ProcessCpuContext ctx;
    ctx_for_entry(&ctx, entry, pid);
    (void)process_save_context(pid, &ctx);
    if (bind_thread(pid, path) != 0)
    {
        drop_process(pid);
        return -11;
    }
    return pid;
}

int task_fork(ExceptionFrame* frame)
{
    int parent = thread_current_pid();
    if (parent < 0 || !frame) return -1;

    int pid = process_create(process_name(parent));
    if (pid < 0) return -11;
    (void)process_set_parent(pid, parent);
    (void)process_set_args(pid, process_args(parent));
    (void)process_set_background(pid, process_is_background(parent));
    (void)process_set_start(pid, timer_millis());
    (void)process_set_creds(pid, process_uid(parent), process_gid(parent));
    (void)process_sig_inherit(parent, pid);
    (void)process_rlimit_inherit(parent, pid);
    (void)process_set_cwd(pid, process_cwd(parent));
    (void)process_set_umask(pid, process_umask(parent));

    if (mmu_user_space_clone(parent, pid) != 0)
    {
        (void)process_destroy(pid);
        return -12;
    }
    if (process_configure_user(pid, MMU_USER_BASE, MMU_USER_STACK_TOP) != 0 || scheduler_add(pid) != 0)
    {
        drop_process(pid);
        return -11;
    }
    mapping_clone(parent, pid);
    if (fd_set_process(pid) != 0 || fd_clone_table(parent, pid) != 0)
    {
        (void)fd_set_process(parent);
        drop_process(pid);
        return -24;
    }
    (void)fd_set_process(parent);

    ProcessCpuContext ctx;
    for (int i = 0; i < 31; ++i) ctx.x[i] = frame->x[i];
    ctx.x[0] = 0;                              /* fork() returns 0 in the child */
    ctx.elr = frame->elr;
    ctx.spsr = frame->spsr;
    asm volatile("mrs %0, sp_el0" : "=r"(ctx.user_sp));
    (void)process_save_context(pid, &ctx);
    if (bind_thread(pid, process_name(parent)) != 0)
    {
        drop_process(pid);
        return -11;
    }
    thread_fp_copy_live_to(process_thread(pid));       /* child inherits the parent's FP/SIMD registers */
    return pid;
}

static void kill_siblings(int pid);

int task_exec(ExceptionFrame* frame, const char* path, const char* args)
{
    int pid = thread_current_pid();
    if (pid < 0 || !frame || !path || !path[0]) return -22;

    int tmp = pid + TASK_EXEC_TMP_BASE;
    uint64_t entry = 0;
    int rc = elf_load_for_process(path, &entry, tmp);
    /* The loader builds the new image in a separate address space; ours stays active until it is swapped in. */
    if (rc != 0 || entry < MMU_USER_BASE || entry >= MMU_USER_LIMIT || (entry & 3ULL) != 0)
    {
        if (mmu_user_space_exists(tmp)) (void)mmu_user_space_destroy(tmp);
        (void)mmu_user_space_activate(pid);
        return -2;
    }
    /* Point of no return: every other thread of the process ends, and this one becomes the process's only (main) thread. */
    {
        int me = thread_current();
        if (process_thread_count(pid) > 1)
        {
            kill_siblings(pid);
            while (process_thread_count(pid) > 1) thread_wait_ms(1);
        }
        thread_set_tid(me, (uint32_t)pid);
        (void)process_set_thread(pid, me);
        thread_set_clear_tid(me, 0);
        thread_set_robust(me, 0);
        thread_altstack_set(me, 0, 0);
    }
    if (mmu_user_space_replace(pid, tmp) != 0)
    {
        (void)mmu_user_space_destroy(tmp);
        (void)mmu_user_space_activate(pid);
        return -12;
    }
    (void)process_set_args(pid, args ? args : "");
    (void)process_set_name(pid, path);
    mapping_release_pid(pid);
    mapping_gc();
    (void)process_sig_reset_on_exec(pid);
    (void)fd_close_on_exec(pid);
    (void)mmu_user_space_activate(pid);

    for (int i = 0; i < 31; ++i) frame->x[i] = 0;
    frame->elr = entry;
    frame->spsr = USER_SPSR_EL0T;
    uint64_t sp = mmu_user_space_stack_top(pid) - 16ULL;
    asm volatile("msr sp_el0, %0" :: "r"(sp));
    thread_fp_reset_live();
    thread_set_tls_live(0);                    /* the new image sets up its own thread pointer */
    return 0;
}

/* ---- thread-aware signal state: every thread has its own mask and thread-directed pending set; the process holds the
        process-directed pending set and the handlers ---- */
uint32_t task_sig_mask() { return thread_sigmask_of(thread_current()); }
void task_sig_set_mask(uint32_t mask) { thread_set_sigmask(thread_current(), mask & ~SIG_UNBLOCKABLE_K); }
static int lowest_signal(uint32_t bits)
{
    for (int i = 1; i < 32; ++i) if (bits & (1U << i)) return i;
    return 0;
}
uint32_t task_sig_pending(int pid)
{
    int th = thread_current();
    uint32_t m = thread_sigmask_of(th);
    return (thread_sigpending_of(th) & (~m | SIG_UNBLOCKABLE_K)) | process_sig_pending_mask(pid, m);
}
uint32_t task_sig_pending_raw(int pid) { return thread_sigpending_of(thread_current()) | process_sig_pending_raw(pid); }
int task_sig_take(int pid)
{
    int th = thread_current();
    uint32_t m = thread_sigmask_of(th);
    int sig = lowest_signal(thread_sigpending_of(th) & (~m | SIG_UNBLOCKABLE_K));
    if (sig) { thread_sigpending_clear(th, 1U << sig); return sig; }
    return process_sig_take_mask(pid, m);
}
/* sigwait / sigtimedwait / signalfd: the lowest pending signal in `set`, blocked or not; thread-directed ones first */
int task_sig_take_set(int pid, uint32_t set)
{
    int th = thread_current();
    int sig = lowest_signal(thread_sigpending_of(th) & set);
    if (sig) { thread_sigpending_clear(th, 1U << sig); return sig; }
    return process_sig_take_set(pid, set);
}
/* A blocking system call must return -EINTR: a deliverable signal (other than SIGCHLD/SIGCONT, which default to ignored),
   or the thread has been told to go (exit_group, exec in another thread). */
int task_sig_interrupts(int pid)
{
    if (thread_is_dying(thread_current())) return 1;
    return (task_sig_pending(pid) & ~((1U << SIG_CHLD) | (1U << SIG_CONT))) != 0;
}

/* An alarm was (re)armed: threads of the process that sleep re-evaluate how long they may sleep. */
void task_alarm_poke(int pid)
{
    int me = thread_current();
    for (int th = thread_group_next(pid, -1); th >= 0; th = thread_group_next(pid, th))
        if (th != me && thread_is_blocked(th)) (void)thread_wake(th, THREAD_WAKE_SIGNAL);
}

/* Wake one thread (or `target`) that sleeps in a blocking call and could take `sig`, so it returns -EINTR. */
static void signal_wakeup(int pid, int sig, int target)
{
    for (int th = thread_group_next(pid, -1); th >= 0; th = thread_group_next(pid, th))
    {
        if (target >= 0 && th != target) continue;
        if (!thread_is_blocked(th)) continue;
        if (!(SIG_UNBLOCKABLE_K & (1U << sig)) && (thread_sigmask_of(th) & (1U << sig))) continue;
        (void)thread_wake(th, THREAD_WAKE_SIGNAL);
        if (target < 0) break;
    }
}

/* ---- ending threads and processes ---- */
static void process_teardown(int pid, int code, int signal)
{
    int parent = process_parent(pid);

    net_udp_close_all_for(pid);
    net_tcp_close_all_for(pid);
    (void)fd_set_process(pid);
    mapping_release_pid(pid);                  /* shared file pages are written back before the space goes */
    fd_close_all();
    tty_process_exit(pid);
    fd_release_process(pid);
    (void)scheduler_remove(pid);
    (void)mmu_user_space_destroy(pid);         /* also returns TTBR0 to the kernel space */
    mapping_gc();

    process_reparent_children(pid, 1);
    (void)process_set_term_signal(pid, signal);
    (void)process_mark_exit(pid, code);
    (void)process_set_thread(pid, -1);
    if (parent >= 0) (void)process_sig_raise(parent, SIG_CHLD);
    thread_event_notify();                     /* a parent waiting in waitpid() retries now */
    smp_kick_cpus(1U);                         /* a boot-CPU shell waiting in wfi notices the exit at once */
}

/* The calling thread ends. The last thread of a process finishes the process (zombie, descriptors, address space). */
extern "C" void task_thread_die_code(int code)
{
    int pid = thread_current_pid();
    int th = thread_current();
    (void)irq_save_local();                    /* stays masked until the thread is gone */
    if (pid >= 0)
    {
        futex_thread_exit(pid, th);            /* clear_child_tid + robust mutexes: joiners and lock waiters wake up */
        int left = process_thread_adjust(pid, -1);
        --user_thread_total;
        thread_set_user_tasks(user_thread_total);
        if (left <= 0)
        {
            int c = code, sg = 0;
            if (process_is_exiting(pid)) (void)process_exit_pending(pid, &c, &sg);
            process_teardown(pid, c, sg);
            --user_task_total;
        }
    }
    thread_exit();
}
extern "C" void task_thread_die() { task_thread_die_code(0); }

/* Mark every other thread of `pid` as dying and wake the ones that sleep. */
static void kill_siblings(int pid)
{
    int me = thread_current();
    for (int th = thread_group_next(pid, -1); th >= 0; th = thread_group_next(pid, th))
        if (th != me) { thread_set_dying(th, 1); (void)thread_wake(th, THREAD_WAKE_SIGNAL); }
    smp_kick_others();
}

/* exit_group / fatal signal: the whole process goes, whichever thread asks. */
extern "C" void task_exit_current(int code, int signal)
{
    int pid = thread_current_pid();
    if (pid < 0) return;

    (void)irq_save_local();
    if (process_begin_exit(pid, code, signal)) kill_siblings(pid);
    thread_set_dying(thread_current(), 1);
    task_thread_die();
}

int task_thread_create(ExceptionFrame* frame, uint64_t stack_top, uint64_t tls, uint64_t clear_tid)
{
    int pid = thread_current_pid();
    if (pid < 0 || !frame) return -1;
    if (process_is_exiting(pid)) return -11;
    if ((stack_top & 15ULL) != 0 || stack_top < MMU_USER_BASE + 4096ULL || stack_top > MMU_USER_LIMIT) return -22;
    if (clear_tid && ((clear_tid & 3ULL) != 0 || !mmu_user_pointer_ok(clear_tid, 4, 1))) return -14;
    ThreadStart* ts = (ThreadStart*)kmalloc(sizeof(ThreadStart));
    if (!ts) return -11;
    for (int i = 0; i < 31; ++i) ts->ctx.x[i] = frame->x[i];
    ts->ctx.x[0] = 0;                          /* the new thread sees thread_create() return 0 */
    ts->ctx.elr = frame->elr;
    ts->ctx.spsr = frame->spsr;
    ts->ctx.user_sp = stack_top;

    ThreadParams p;
    thread_params_default(&p);
    int cur = thread_current();
    int pol = 0, pr = 0, ni = 0;
    if (thread_get_sched(cur, &pol, &pr, &ni) == 0) { p.policy = pol; p.rt_prio = pr; p.nice = ni; }
    p.affinity = thread_get_affinity(cur);
    p.tls = tls;
    int t = thread_create_ex(process_name(pid), task_thread_entry, ts, pid, &p);
    if (t < 0) { kfree(ts); return -11; }
    uint32_t tid = thread_alloc_tid();
    while (thread_find_tid(tid) >= 0) tid = thread_alloc_tid();
    thread_set_tid(t, tid);
    thread_set_sigmask(t, thread_sigmask_of(cur));
    thread_set_clear_tid(t, clear_tid);
    (void)process_thread_adjust(pid, 1);
    ++user_thread_total;
    thread_set_user_tasks(user_thread_total);
    return (int)tid;
}

int task_kill_info(int pid, int sig, int sender, int code, uint64_t value)
{
    if (pid <= 1) return -1;                   /* kernel and shell are not killable */
    if (!process_is_user(pid)) return -3;
    if (process_get_state(pid) == PROCESS_ZOMBIE) return -3;
    if (sig == 0) return 0;                    /* existence check */
    if (sig < 0 || sig >= 32) return -22;
    if (sig == SIG_CONT) (void)process_set_stopped(pid, 0);
    int rc = process_sig_raise_info(pid, sig, sender, code, value);
    if (rc == 0) signal_wakeup(pid, sig, -1);
    return rc;
}
int task_kill(int pid, int sig) { return task_kill_info(pid, sig, 0, 0, 0); }

/* tgkill: a signal for one particular thread of `tgid`. Stop/continue/kill act on the whole process regardless. */
int task_tgkill(int tgid, uint32_t tid, int sig, int sender)
{
    int th = thread_find_tid(tid);
    if (th < 0 || thread_pid(th) != tgid || tgid <= 1 || !process_is_user(tgid)) return -3;
    if (sig == 0) return 0;
    if (sig < 0 || sig >= 32) return -22;
    if (sig == SIG_KILL || sig == SIG_STOP || sig == SIG_TSTP || sig == SIG_CONT) return task_kill_info(tgid, sig, sender, 0, 0);
    uint64_t h = process_sig_handler(tgid, sig, 0, 0);
    if (h == 1) return 0;                      /* ignored signals are discarded */
    process_sig_note(tgid, sig, sender, 0, 0);
    thread_sigpending_or(th, 1U << sig);
    signal_wakeup(tgid, sig, th);
    return 0;
}

int task_reap(int pid)
{
    if (process_get_state(pid) != PROCESS_ZOMBIE) return -1;
    (void)process_destroy(pid);
    return 0;
}

int task_wait(int parent, int pid, int* status, int options)
{
    for (;;)
    {
        uint32_t evg = thread_event_gen();
        int z = process_find_zombie_child(parent, pid > 0 ? pid : -1);
        if (z >= 0)
        {
            int code = process_exit_code(z);
            int sig = process_term_signal(z);
            if (status) *status = sig ? sig : ((code & 0xFF) << 8);
            (void)task_reap(z);
            return z;
        }
        if (pid > 0 ? !process_is_child_of(parent, pid) : !process_has_children(parent))
            return -10;                         /* ECHILD */
        if (options & TASK_WNOHANG) return 0;
        process_alarm_check(parent, timer_millis());
        if (task_sig_interrupts(parent)) return -4;   /* EINTR */
        thread_event_wait(evg, 2);
    }
}

int task_reap_orphans(int shell_pid, void (*notify)(int pid, int code, int sig))
{
    int n = 0;
    for (;;)
    {
        int z = process_find_zombie_child(shell_pid, -1);
        if (z < 0) break;
        int code = process_exit_code(z), sig = process_term_signal(z);
        if (notify) notify(z, code, sig);
        (void)task_reap(z);
        ++n;
    }
    return n;
}

int task_running_count() { return user_task_total; }
int task_thread_total() { return user_thread_total; }

/* Frame pushed on the user stack when a caught signal is delivered. The handler is called as
   handler(sig, &info, &uc) so SA_SIGINFO handlers work; plain handlers just ignore x1/x2. */
struct SigInfoK { int32_t si_signo, si_errno, si_code, si_pid; uint64_t si_value; uint64_t si_addr; };
struct SigFrame
{
    uint64_t magic;
    uint32_t old_mask;
    uint32_t sig;
    SigInfoK info;
    uint64_t x[31];
    uint64_t elr, spsr, sp;
    uint64_t pad;
    FpState fp;                  /* FP/SIMD registers at the time of the signal (restored by sigreturn) */
};
#define SIGFRAME_MAGIC 0x52494F5349474652ULL   /* "RIOSIGFR" */
#define SA_ONSTACK_FLAG 0x08000000U

static int push_signal_frame(ExceptionFrame* frame, int pid, int sig, uint64_t handler, uint64_t fault_addr)
{
    uint32_t amask = 0, flags = 0;
    (void)process_sig_handler(pid, sig, &amask, &flags);
    uint64_t sp;
    asm volatile("mrs %0, sp_el0" : "=r"(sp));
    uint64_t base = sp;
    uint64_t asp = 0, asz = 0;
    thread_altstack_get(thread_current(), &asp, &asz);
    if ((flags & SA_ONSTACK_FLAG) && asp && asz)
        if (!(sp >= asp && sp < asp + asz)) base = asp + asz;        /* switch to the alternate stack unless already on it */
    uint64_t nsp = (base - (base == sp ? 128ULL : 0ULL) - sizeof(SigFrame)) & ~15ULL;
    if (!mmu_user_pointer_ok(nsp, sizeof(SigFrame), 1)) return -1;
    SigFrame* sf = (SigFrame*)nsp;
    sf->magic = SIGFRAME_MAGIC;
    sf->old_mask = task_sig_mask();
    sf->sig = (uint32_t)sig;
    int sender = 0, code = 0; uint64_t value = 0;
    (void)process_sig_info_get(pid, sig, &sender, &code, &value);
    sf->info.si_signo = sig; sf->info.si_errno = 0; sf->info.si_code = code; sf->info.si_pid = sender; sf->info.si_value = value; sf->info.si_addr = fault_addr;
    for (int i = 0; i < 31; ++i) sf->x[i] = frame->x[i];
    sf->elr = frame->elr; sf->spsr = frame->spsr; sf->sp = sp; sf->pad = 0;
    fp_save(&sf->fp);
    uint32_t nm = sf->old_mask | amask;
    if (!(flags & 0x40000000U)) nm |= (1U << sig);          /* SA_NODEFER not set */
    task_sig_set_mask(nm);
    if (flags & 0x80000000U) { uint64_t o; (void)process_sig_set_action(pid, sig, 0, 0, 0, 0, &o); }   /* SA_RESETHAND */
    uint64_t rest = process_sig_restorer(pid);
    for (int i = 0; i < 31; ++i) frame->x[i] = 0;
    frame->x[0] = (uint64_t)sig;
    frame->x[1] = (uint64_t)&sf->info;
    frame->x[2] = (uint64_t)&sf->x[0];
    frame->x[30] = rest;
    frame->elr = handler;
    asm volatile("msr sp_el0, %0" :: "r"(nsp));
    return 0;
}

/* A fatal user fault (SIG_SEGV / SIG_BUS / SIG_ILL): run the program's handler when it has one and the
   signal is not already being handled. Returns 0 when a handler frame was pushed. */
int task_deliver_fault(ExceptionFrame* frame, int sig, uint64_t addr)
{
    int pid = thread_current_pid();
    if (pid < 0 || !frame) return -1;
    uint64_t h = process_sig_handler(pid, sig, 0, 0);
    if (h <= 1) return -1;
    if (task_sig_mask() & (1U << sig)) return -1;     /* fault inside its own handler: terminate */
    (void)process_sig_info_get(pid, sig, 0, 0, 0);
    (void)process_sig_raise_info(pid, sig, 0, 1 /* SEGV_MAPERR */, 0);
    (void)process_sig_take_set(pid, 1U << sig);            /* consume the pending bit we just set */
    return push_signal_frame(frame, pid, sig, h, addr);
}

int task_sigreturn(ExceptionFrame* frame)
{
    int pid = thread_current_pid();
    uint64_t sp;
    asm volatile("mrs %0, sp_el0" : "=r"(sp));
    if (pid < 0 || !mmu_user_pointer_ok(sp, sizeof(SigFrame), 0)) return -14;
    const SigFrame* sf = (const SigFrame*)sp;
    if (sf->magic != SIGFRAME_MAGIC) return -22;
    for (int i = 0; i < 31; ++i) frame->x[i] = sf->x[i];
    frame->elr = sf->elr;
    frame->spsr = (frame->spsr & ~0xF0000000ULL) | (sf->spsr & 0xF0000000ULL);   /* only the condition flags come back */
    fp_restore(&sf->fp);
    uint32_t om = sf->old_mask; uint64_t nsp = sf->sp;
    task_sig_set_mask(om);
    asm volatile("msr sp_el0, %0" :: "r"(nsp));
    return 0;
}

extern "C" void task_deliver_signals(ExceptionFrame* frame)
{
    int pid = thread_current_pid();
    if (pid < 0) return;
    int me = thread_current();
    if (thread_is_dying(me)) task_thread_die();                 /* exit_group / exec asked this thread to go */
    process_alarm_check(pid, timer_millis());
    /* A stopped process stops every thread: they wait here, at their next return to user mode. */
    while (process_is_stopped(pid))
    {
        if (thread_is_dying(me)) task_thread_die();
        uint32_t wake = (1U << SIG_KILL) | (1U << SIG_TERM) | (1U << SIG_CONT);
        if (task_sig_pending(pid) & wake) break;
        thread_wait_ms(5);
    }
    int sig;
    while ((sig = task_sig_take(pid)) != 0)
    {
        uint64_t handler = process_sig_handler(pid, sig, 0, 0);
        if (handler == 1) continue;                      /* ignored */
        if (handler > 1)
        {
            if (frame && push_signal_frame(frame, pid, sig, handler, 0) == 0) return;   /* one handler per return; the rest stay pending */
            task_exit_current(128 + SIG_SEGV, SIG_SEGV);  /* no room for the signal frame */
            return;
        }
        switch (sig)
        {
            case SIG_CHLD:
            case SIG_CONT:
                break;                          /* ignored by default */
            case SIG_STOP:
            case SIG_TSTP:
                (void)process_set_stopped(pid, 1);
                while (process_is_stopped(pid))
                {
                    if (thread_is_dying(me)) task_thread_die();
                    if (task_sig_pending(pid) & ((1U << SIG_KILL) | (1U << SIG_TERM) | (1U << SIG_CONT)))
                    {
                        if (task_sig_pending(pid) & (1U << SIG_CONT)) { (void)task_sig_take_set(pid, 1U << SIG_CONT); (void)process_set_stopped(pid, 0); break; }
                        break;
                    }
                    thread_wait_ms(5);
                }
                (void)process_set_stopped(pid, 0);
                break;
            default:
                task_exit_current(128 + sig, sig);
                return;
        }
    }
}

int task_console_key(int pid, char c)
{
    if (pid < 0 || process_is_background(pid)) return 0;
    if (c == 3) { (void)process_sig_raise(pid, SIG_INT); return 1; }
    if (c == 26) { (void)process_sig_raise(pid, SIG_TSTP); return 1; }
    return 0;
}

static int fg_job_pid = -1;
static int job_key_filter(int c)
{
    if (fg_job_pid < 0 || process_is_background(fg_job_pid)) return 0;
    if (c == 3) { (void)process_sig_raise(fg_job_pid, SIG_INT); return 1; }
    if (c == 26) { (void)process_sig_raise(fg_job_pid, SIG_TSTP); return 1; }
    return 0;
}

int task_wait_foreground(int pid, int shell_pid)
{
    fg_job_pid = pid;
    int result = -1;
    for (;;)
    {
        uart_poll_job_keys(job_key_filter);
        if (process_get_state(pid) == PROCESS_ZOMBIE)
        {
            int code = process_exit_code(pid);
            int sig = process_term_signal(pid);
            (void)task_reap(pid);
            result = sig ? (128 + sig) : code;
            break;
        }
        if (process_is_stopped(pid)) { result = TASK_STOPPED; break; }
        if (!process_is_child_of(shell_pid, pid) && process_get_state(pid) < 0) { result = -1; break; }
        thread_yield();
        smp_idle_wait();
    }
    fg_job_pid = -1;
    return result;
}
