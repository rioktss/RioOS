#include "task.h"
#include "thread.h"
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

#define USER_SPSR_EL0T 0x340ULL   /* EL0t, IRQ unmasked, D/A/F masked */
#define TASK_EXEC_TMP_BASE 0x10000

static int user_task_total = 0;

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
        if (mmu_user_space_active_pid() != pid) (void)mmu_user_space_activate(pid);
        (void)fd_set_process(pid);
        (void)scheduler_set_current_logical(pid);
        fs_set_credentials(process_uid(pid), process_gid(pid));     /* privilege separation */
    }
    else
    {
        if (mmu_user_space_active_pid() >= 0) (void)mmu_user_space_deactivate();
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

static void ctx_for_entry(ProcessCpuContext* c, uint64_t entry)
{
    for (int i = 0; i < 31; ++i) c->x[i] = 0;
    c->elr = entry;
    c->spsr = USER_SPSR_EL0T;
    c->user_sp = MMU_USER_STACK_TOP - 16ULL;
}

static int bind_thread(int pid, const char* name)
{
    int t = thread_create_for_pid(name, task_thread_main, (void*)(uint64_t)pid, 1, pid);
    if (t < 0) return -11;
    (void)process_set_thread(pid, t);
    ++user_task_total;
    thread_set_user_tasks(user_task_total);
    return 0;
}

static void drop_process(int pid)
{
    net_udp_close_all_for(pid);
    net_tcp_close_all_for(pid);
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
    ctx_for_entry(&ctx, entry);
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
    return pid;
}

int task_exec(ExceptionFrame* frame, const char* path, const char* args)
{
    int pid = thread_current_pid();
    if (pid < 0 || !frame || !path || !path[0]) return -22;

    int tmp = pid + TASK_EXEC_TMP_BASE;
    uint64_t entry = 0;
    int rc = elf_load_for_process(path, &entry, tmp);
    /* The loader leaves the kernel address space active; restore ours on failure. */
    if (rc != 0 || entry < MMU_USER_BASE || entry >= MMU_USER_LIMIT || (entry & 3ULL) != 0)
    {
        if (mmu_user_space_exists(tmp)) (void)mmu_user_space_destroy(tmp);
        (void)mmu_user_space_activate(pid);
        return -2;
    }
    if (mmu_user_space_replace(pid, tmp) != 0)
    {
        (void)mmu_user_space_destroy(tmp);
        (void)mmu_user_space_activate(pid);
        return -12;
    }
    (void)process_set_args(pid, args ? args : "");
    (void)process_set_name(pid, path);
    (void)process_sig_take(pid);
    (void)mmu_user_space_activate(pid);

    for (int i = 0; i < 31; ++i) frame->x[i] = 0;
    frame->elr = entry;
    frame->spsr = USER_SPSR_EL0T;
    uint64_t sp = MMU_USER_STACK_TOP - 16ULL;
    asm volatile("msr sp_el0, %0" :: "r"(sp));
    return 0;
}

extern "C" void task_exit_current(int code, int signal)
{
    int pid = thread_current_pid();
    if (pid < 0) return;

    (void)irq_save_local();                    /* stays masked until the thread is gone */
    int parent = process_parent(pid);

    net_udp_close_all_for(pid);
    net_tcp_close_all_for(pid);
    (void)fd_set_process(pid);
    fd_close_all();
    fd_release_process(pid);
    (void)scheduler_remove(pid);
    (void)mmu_user_space_destroy(pid);         /* also returns TTBR0 to the kernel space */

    process_reparent_children(pid, 1);
    (void)process_set_term_signal(pid, signal);
    (void)process_mark_exit(pid, code);
    (void)process_set_thread(pid, -1);
    if (parent >= 0) (void)process_sig_raise(parent, SIG_CHLD);

    --user_task_total;
    thread_set_user_tasks(user_task_total);
    thread_exit();
}

int task_kill(int pid, int sig)
{
    if (pid <= 1) return -1;                   /* kernel and shell are not killable */
    if (!process_is_user(pid)) return -3;
    if (process_get_state(pid) == PROCESS_ZOMBIE) return -3;
    if (sig == 0) return 0;                    /* existence check */
    if (sig < 0 || sig >= 32) return -22;
    if (sig == SIG_CONT) (void)process_set_stopped(pid, 0);
    return process_sig_raise(pid, sig);
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
        if (process_sig_pending(parent) & ~((1U << SIG_CHLD) | (1U << SIG_CONT))) return -4;   /* EINTR */
        thread_wait_ms(2);
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

extern "C" void task_deliver_signals()
{
    int pid = thread_current_pid();
    if (pid < 0) return;
    int sig;
    while ((sig = process_sig_take(pid)) != 0)
    {
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
                    if (process_sig_pending(pid) & ((1U << SIG_KILL) | (1U << SIG_TERM) | (1U << SIG_CONT)))
                    {
                        if (process_sig_pending(pid) & (1U << SIG_CONT)) { (void)process_sig_take(pid); (void)process_set_stopped(pid, 0); break; }
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
    }
    fg_job_pid = -1;
    return result;
}
