#include "uart.h"
#include "memory.h"
#include "fs.h"
#include "vfs.h"
#include "process.h"
#include "timer.h"
#include "keyboard.h"
#include "shell.h"
#include "scheduler.h"
#include "thread.h"
#include "exceptions.h"
#include "syscall.h"
#include "mmu.h"
#include "storage.h"
#include "netstack.h"
#include "interrupt.h"
#include "page_alloc.h"
#include "security.h"
#include "smp.h"
#include "version.h"
#include "fb.h"
#include "console.h"

extern "C" void kernel_main()
{
    uart_init();
    uart_puts(
        "\r\n"
        "============================\r\n"
        "        RioOS v" MYKERNEL_VERSION_STRING "\r\n"
        "============================\r\n"
        "ARM64 Bare-Metal Kernel\r\n"
        "QEMU virt machine\r\n"
        "\r\n"
    );

    /* Read this core's MPIDR affinity. Secondary-core startup remains disabled. */
    smp_init();
    uart_puts("[0] CPU topology: boot CPU affinity-0 = ");
    {
        char cpu_id_text[4];
        uint32_t cpu_id = smp_cpu_id();
        int pos = 0;
        if (cpu_id == 0U) cpu_id_text[pos++] = '0';
        while (cpu_id > 0U && pos < 3)
        {
            cpu_id_text[pos++] = (char)('0' + (cpu_id % 10U));
            cpu_id /= 10U;
        }
        while (pos > 0) uart_putc(cpu_id_text[--pos]);
    }
    uart_puts("; online CPUs = 1; SMP disabled by design.\r\n");

    uart_puts("[1] Initializing memory...\r\n");
    memory_init();
    uart_puts("[2] Memory initialized.\r\n");

    /* Physical page pool (4 MiB + alignment slack) carved from the heap. */
    {
        void* pool = kmalloc(4ULL * 1024ULL * 1024ULL + PAGE_SIZE_BYTES);

        if (pool == 0 || page_alloc_init(pool, 4ULL * 1024ULL * 1024ULL + PAGE_SIZE_BYTES) != 0)
            uart_puts("Page allocator unavailable.\r\n");
        else
            uart_puts("Page allocator ready.\r\n");
    }

    /* Install a real EL1 vector before enabling MMU. If MMU setup faults,
       the exception path can report it instead of silently hanging. */
    uart_puts("[3] Initializing exception vectors...\r\n");
    exceptions_init();

    /* The counter is needed by every timeout path, so bring it up early. */
    uart_puts("[4] Initializing timer...\r\n");
    timer_init();
    /* Timer is armed after the interrupt controller is up (step 15). */

    uart_puts("[5] Initializing MMU...\r\n");
    mmu_init();

    uart_puts("[6] Initializing storage / VirtIO block...\r\n");
    storage_init();

    uart_puts("[7] Initializing network...\r\n");
    net_init();

    uart_puts("[8] Initializing filesystem...\r\n");
    fs_init();

    uart_puts("[9] Initializing VFS...\r\n");
    vfs_init();

    uart_puts("[10] Initializing process manager...\r\n");
    process_init();
    security_init();

    uart_puts("[11] Initializing keyboard...\r\n");
    keyboard_init();

    uart_puts("[12] Initializing scheduler...\r\n");
    scheduler_init();
    thread_init();
    uart_puts("Cooperative thread subsystem initialized (no preemption; timer tick only).\r\n");

    uart_puts("[13] Initializing interrupt controller...\r\n");
    interrupt_init();

    uart_puts("[14] Initializing syscall...\r\n");
    syscall_init();

    /* System tick: 100 Hz periodic timer IRQ, live for the kernel shell and
       while EL0 programs run. Line is unmasked at the GIC first, then the
       timer is armed, then the CPU I-bit is cleared. */
    uart_puts("[15] System tick (100 Hz) starts before the shell...\r\n");

uart_puts(
    "\r\n"
    "RioOS initialization complete.\r\n"
    "\r\n"
);

/* If QEMU provides a display (run-terminal.sh), take over the framebuffer
       with the text terminal. Everything printed from here on is mirrored to
       the window; without a display the serial console works unchanged. */
fb_init();
if (fb_has_display() && console_start() == 0)
    uart_puts_serial_only("Console: framebuffer terminal active (mouse wheel / Shift+PageUp scroll).\r\n");
timer_set_sched_tick(1);
interrupt_timer_line_enable();
timer_start_periodic(100U);
interrupt_enable();
uart_puts("Timer IRQ active: CPU IRQ unmasked, tick = 100 Hz.\r\n");
uart_puts(
    "Welcome to RioOS!\r\n"
    "Type 'help' to get started.\r\n"
    "\r\n"
);

shell();

    while (1)
        asm volatile("wfe");
}
