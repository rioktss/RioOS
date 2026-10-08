#include "version.h"

#include "fb.h"
#include "uart.h"
#include "fs.h"
#include "memory.h"
#include "storage.h"
#include "process.h"
#include "timer.h"
#include "virtio_net.h"
#include "netstack.h"
#include "page_alloc.h"
#include "scheduler.h"
#include "security.h"

static void putc_both(char c)
{
    uart_putc(c);
    fb_putc(c, 0xFFFFFFU);
}

static void puts_both(const char* text)
{
    if (text == 0)
        return;

    uart_puts(text);
    fb_print(text, 0xFFFFFFU);
}

static void print_u64_both(uint64_t value)
{
    char buffer[32];
    int pos = 0;

    if (value == 0)
    {
        putc_both('0');
        return;
    }

    while (value > 0 && pos < (int)sizeof(buffer) - 1)
    {
        buffer[pos++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }

    while (pos > 0)
        putc_both(buffer[--pos]);
}

static uint64_t current_el()
{
    uint64_t value = 0;

    asm volatile("mrs %0, CurrentEL" : "=r"(value));
    return (value >> 2) & 3ULL;
}

static const char MYKERNEL_LOGO[] =
    "                               __\r\n"
    "                            .d$$b\r\n"
    "                          .' TO$;\\\r\n"
    "                         /  : TP._;\r\n"
    "                        / _.;  :Tb|\r\n"
    "                       /   /   ;j$j\r\n"
    "                   _.-\"       d$$$$\r\n"
    "                 .' ..       d$$$$;\r\n"
    "                /  /P'      d$$$$P. |\\\r\n"
    "               /   \"      .d$$$P' |\\^\"l\r\n"
    "             .'           `T$P^\"\"\"\"\"  :\r\n"
    "         ._.'      _.'                ;\r\n"
    "      `-.-\".-'-' ._.       _.-\"    .-\"\r\n"
    "    `.-\" _____  ._              .-\"\r\n"
    "   -(.g$$$$$$$b.              .'\r\n"
    "     \"\"^^T$$$P^)            .(:\r\n"
    "       _/  -\"  /.'         /:/;\r\n"
    "    ._.'-'`-'  \"/         /;/;\r\n"
    " `-.-\"..--\"\"   \" /         /  ;\r\n"
    ".-\" ..--\"\"        -'          :\r\n"
    "..--\"\"--.-\"         (\\      .-(\\\r\n"
    "  ..--\"\"              `-\\(\\/;`\r\n"
    "    _.                      :\r\n"
    "                            ;`-\r\n"
    "                           :\\\r\n"
    "                           ; \r\n";

static void print_info()
{
    puts_both("MyKernel v");
    print_u64_both(MYKERNEL_VERSION_MAJOR);
    putc_both('.');
    print_u64_both(MYKERNEL_VERSION_MINOR);
    putc_both('.');
    print_u64_both(MYKERNEL_VERSION_PATCH);
    puts_both("\r\n");

    puts_both("OS            : RioOS\r\n");
    puts_both("Kernel        : Mykernel v");
    puts_both(MYKERNEL_VERSION_STRING);
    puts_both("\r\n");
    puts_both("Architecture  : AArch64\r\n");
    puts_both("Machine       : QEMU virt\r\n");
    puts_both("CPU           : Cortex-A72\r\n");
    puts_both("RAM           : ");
    print_u64_both(memory_total() / (1024ULL * 1024ULL));
    puts_both(" MiB\r\n");
    puts_both("Heap          : ");
    print_u64_both(heap_used() / 1024ULL);
    puts_both(" KiB used / ");
    print_u64_both(heap_total() / 1024ULL);
    puts_both(" KiB total\r\n");
    puts_both("Storage       : ");
    puts_both(storage_ready() ? "VirtIO Block (ready)" : "VirtIO Block (offline)");
    puts_both("\r\n");
    puts_both("Filesystem    : MyFS (");
    puts_both(fs_persistent() ? "persistent" : "RAM only");
    puts_both(")\r\n");
    puts_both("Network       : ");
    if (net_ready())
        puts_both("ready");
    else if (virtio_net_present())
        puts_both("VirtIO-Net detected");
    else
        puts_both("offline");
    puts_both("\r\n");
    puts_both("Processes     : ");
    print_u64_both((uint64_t)process_count());
    puts_both("\r\n");
    puts_both("Runnable      : ");
    print_u64_both((uint64_t)scheduler_ready_count());
    puts_both("\r\n");
    puts_both("Pages         : ");
    PageStats pages;
    page_alloc_get_stats(&pages);
    print_u64_both(pages.free_pages);
    puts_both(" free / ");
    print_u64_both(pages.total_pages);
    puts_both(" total\r\n");
    puts_both("Uptime        : ");
    print_u64_both(timer_seconds());
    puts_both(" s\r\n");
    puts_both("Current EL    : EL");
    print_u64_both(current_el());
    puts_both("\r\n");
    puts_both("User          : ");
    puts_both(security_username());
    puts_both("\r\n");
}

void command_version()
{
    /* UART gets the full logo first, matching the requested terminal output. */
    uart_puts(MYKERNEL_LOGO);

    /* Framebuffer gets a fastfetch-like two-column layout. */
    fb_set_cursor(0, 0);
    fb_print(MYKERNEL_LOGO, 0x00FF00U);
    fb_set_cursor(320, 0);
    print_info();

    /* Leave the framebuffer cursor below the longest column for the shell. */
    fb_set_cursor(0, 512);

    uart_puts("\r\n");
}
