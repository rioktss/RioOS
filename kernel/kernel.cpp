#include "uart.h"
#include "memory.h"
#include "fs.h"
#include "vfs.h"
#include "process.h"
#include "timer.h"
#include "keyboard.h"
#include "shell.h"
#include "scheduler.h"
#include "exceptions.h"
#include "syscall.h"
#include "mmu.h"
#include "storage.h"
#include "netstack.h"
#include "interrupt.h"
#include "page_alloc.h"
#include "version.h"

extern "C" void kernel_main()
{
    uart_puts(
        "\r\n"
        "============================\r\n"
        "        MyKernel v" MYKERNEL_VERSION_STRING "\r\n"
        "============================\r\n"
        "ARM64 Bare-Metal Kernel\r\n"
        "QEMU virt machine\r\n"
        "\r\n"
    );

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

    uart_puts("[11] Initializing keyboard...\r\n");
    keyboard_init();

    uart_puts("[12] Initializing scheduler...\r\n");
    scheduler_init();

    uart_puts("[13] Initializing interrupt controller...\r\n");
    interrupt_init();

    uart_puts("[14] Initializing syscall...\r\n");
    syscall_init();

    uart_puts("[15] Timer IRQ prepared (off by default; use 'irq' to test stages B-D).\r\n");

uart_puts(
    "\r\n"
    "MyKernel initialization complete.\r\n"
    "Welcome to MyKernel!\r\n"
    "Type 'help' to get started.\r\n"
    "\r\n"
);

shell();

    shell();

    while (1)
        asm volatile("wfe");
}
