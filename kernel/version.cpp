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

#include "version.h"

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
    uart_putc(c); /* mirrored to the QEMU window console automatically */
}

static void puts_both(const char* text)
{
    if (text == 0)
        return;

    uart_puts(text);
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
    uart_puts(MYKERNEL_LOGO);
    uart_puts("\r\n");
    print_info();
    uart_puts("\r\n");
}
