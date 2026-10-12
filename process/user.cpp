#include "user.h"
#include "elf.h"
#include "process.h"
#include "scheduler.h"
#include "mmu.h"
#include "uart.h"
#include "timer.h"
#include "interrupt.h"
#include "fd.h"
#include "netstack.h"

extern "C" { uint64_t user_return_pc = 0; }
static int current_user_pid = -1;
static int last_exit_code = 0;

static char user_args[USER_ARGS_MAX];

int user_current_pid() { return current_user_pid; }
const char* user_get_args() { return user_args; }
void user_set_exit_code(int code) { last_exit_code = code; }

void user_execute(const char* path, const char* args)
{
    if (!path || !path[0]) { uart_puts("exec: missing path\r\n"); return; }
    if (!mmu_enabled()) { uart_puts("exec: MMU is disabled\r\n"); return; }

    /* Userspace is deliberately run with asynchronous timer IRQs quiesced.
       Shell input is polling-based, and user syscalls do not require the
       scheduler tick, so this avoids timer-driven re-entrancy entirely. */
    timer_stop();
    interrupt_timer_line_disable();
    interrupt_disable();
    timer_set_sched_tick(0);

    /* Always start from a clean, non-userspace MMU state. */
    mmu_user_prepare();

    user_args[0] = '\0';
    if (args)
    {
        uint64_t n = 0;
        while (args[n] && n < USER_ARGS_MAX - 1) { user_args[n] = args[n]; ++n; }
        user_args[n] = '\0';
    }

    uint64_t entry=0;
    if (elf_load(path,&entry)!=0)
    {
        mmu_user_prepare();
        uart_puts("exec: failed to load ELF: ");
        uart_puts(path);
        uart_puts("\r\n");
        return;
    }

    if (entry < MMU_USER_BASE || entry >= MMU_USER_LIMIT ||
        (entry & 3ULL) != 0)
    {
        uart_puts("exec: ELF entry is outside the user address space\r\n");
        mmu_user_prepare();
        return;
    }

    int pid=process_create(path);
    if(pid<0){uart_puts("exec: process table full\r\n");mmu_user_prepare();return;}
    if(process_configure_user(pid,MMU_USER_BASE,MMU_USER_STACK_TOP)!=0){process_destroy(pid);mmu_user_prepare();uart_puts("exec: process isolation setup failed\r\n");return;}
    if(scheduler_add(pid)!=0){(void)process_destroy(pid);mmu_user_prepare();uart_puts("exec: scheduler full\r\n");return;}
    if(scheduler_set_current_logical(pid)!=0){(void)scheduler_remove(pid);(void)process_destroy(pid);mmu_user_prepare();uart_puts("exec: scheduler selection failed\r\n");return;}

    if (fd_set_process(pid) != 0)
    {
        (void)scheduler_remove(pid);
        (void)process_destroy(pid);
        int kernel_pid = process_pid_at(0);
        if (kernel_pid >= 0) (void)scheduler_set_current_logical(kernel_pid);
        (void)fd_set_process(kernel_pid >= 0 ? kernel_pid : 0);
        mmu_user_prepare();
        uart_puts("exec: file descriptor table allocation failed\r\n");
        return;
    }
    fd_close_all(); /* Fresh process-local descriptor table; standard streams remain 0/1/2. */
    current_user_pid=pid; last_exit_code=0;
    uart_puts("Starting userspace process: "); uart_puts(path); uart_puts("\r\n");
    uart_puts("EL0 entry: 0x"); static const char h[]="0123456789ABCDEF";for(int i=15;i>=0;--i)uart_putc(h[(entry>>(i*4))&15]);uart_puts("\r\n");

    user_enter(entry,mmu_user_stack_top());
    net_udp_close_all_for(pid);
    fd_close_all();
    fd_release_process(pid);

    /* Remove the active logical PID before marking it terminal; this lets the
       scheduler select a surviving record without compacting away its current
       entry prematurely. This remains metadata scheduling, not CPU switching. */
    (void)scheduler_remove(pid);
    (void)process_mark_exit(pid, last_exit_code);
    (void)process_destroy(pid);

    /* EL0 execution is synchronous and returns to the existing kernel/shell
       stack. Restore both the process-table state and the logical scheduler PID. */
    int kernel_pid = process_pid_at(0);
    if (kernel_pid >= 0)
        (void)scheduler_set_current_logical(kernel_pid);
    (void)fd_set_process(kernel_pid >= 0 ? kernel_pid : 0);

    uart_puts("Userspace returned to EL1. Exit code: ");
    if(last_exit_code==0)uart_puts("0"); else {int v=last_exit_code<0?-last_exit_code:last_exit_code;char b[12];int n=0;while(v&&n<11){b[n++]=(char)('0'+v%10);v/=10;}if(last_exit_code<0)uart_putc('-');while(n)uart_putc(b[--n]);}
    uart_puts("\r\n");
    current_user_pid=-1;
    user_args[0] = '\0';
    mmu_user_prepare();
}
