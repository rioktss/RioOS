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
#include "console.h"
#include "fb.h"
#include "smp.h"

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


/* ---------------------------------------------------------------------------------------------------------------------------
 * fastfetch: the MYKERNEL_LOGO on the left, system information on the right (also available as `neofetch`).
 *   fastfetch [--no-logo] [--plain] [--help]
 * Every value is read from the running kernel; nothing is hard-coded except the board name the kernel is written for.
 * ------------------------------------------------------------------------------------------------------------------------- */

namespace
{
struct FfLine
{
    char text[200];
    int len;
};

static int ff_color = 1;

static void ff_reset(FfLine& l) { l.len = 0; l.text[0] = 0; }

static void ff_str(FfLine& l, const char* s)
{
    while (s && *s && l.len < (int)sizeof(l.text) - 1) l.text[l.len++] = *s++;
    l.text[l.len] = 0;
}

static void ff_u64(FfLine& l, uint64_t v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10ULL)); v /= 10ULL; }
    while (n > 0 && l.len < (int)sizeof(l.text) - 1) l.text[l.len++] = tmp[--n];
    l.text[l.len] = 0;
}

static void ff_hex(FfLine& l, uint64_t v)
{
    char tmp[20];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = "0123456789abcdef"[v & 15ULL]; v >>= 4; }
    ff_str(l, "0x");
    while (n > 0 && l.len < (int)sizeof(l.text) - 1) l.text[l.len++] = tmp[--n];
    l.text[l.len] = 0;
}

static void ff_esc(FfLine& l, const char* code)      /* an SGR sequence such as "1;36", ignored with --plain */
{
    if (!ff_color) return;
    ff_str(l, "\x1B[");
    ff_str(l, code);
    ff_str(l, "m");
}

/* 1536 -> "1.5 KiB" style size with one decimal for MiB and above */
static void ff_size(FfLine& l, uint64_t bytes)
{
    const uint64_t KiB = 1024ULL, MiB = KiB * 1024ULL, GiB = MiB * 1024ULL;
    if (bytes >= GiB)
    {
        ff_u64(l, bytes / GiB); ff_str(l, "."); ff_u64(l, (bytes % GiB) * 10ULL / GiB); ff_str(l, " GiB");
    }
    else if (bytes >= MiB)
    {
        ff_u64(l, bytes / MiB); ff_str(l, "."); ff_u64(l, (bytes % MiB) * 10ULL / MiB); ff_str(l, " MiB");
    }
    else if (bytes >= KiB)
    {
        ff_u64(l, bytes / KiB); ff_str(l, " KiB");
    }
    else
    {
        ff_u64(l, bytes); ff_str(l, " B");
    }
}

static void ff_percent(FfLine& l, uint64_t used, uint64_t total)
{
    ff_str(l, " (");
    ff_u64(l, total ? used * 100ULL / total : 0ULL);
    ff_str(l, "%)");
}

static void ff_plural(FfLine& l, uint64_t n, const char* one, const char* many)
{
    ff_u64(l, n); ff_str(l, " "); ff_str(l, n == 1 ? one : many);
}

static void ff_uptime(FfLine& l, uint64_t secs)
{
    uint64_t d = secs / 86400ULL, h = (secs / 3600ULL) % 24ULL, m = (secs / 60ULL) % 60ULL, s = secs % 60ULL;
    int any = 0;
    if (d) { ff_plural(l, d, "day", "days"); any = 1; }
    if (h) { if (any) ff_str(l, ", "); ff_plural(l, h, "hour", "hours"); any = 1; }
    if (m) { if (any) ff_str(l, ", "); ff_plural(l, m, "min", "mins"); any = 1; }
    if (s || !any) { if (any) ff_str(l, ", "); ff_plural(l, s, "sec", "secs"); }
}

static void ff_ipv4(FfLine& l, uint32_t ip)
{
    for (int n = 0; n < 4; ++n)
    {
        if (n) ff_str(l, ".");
        ff_u64(l, (ip >> (24 - 8 * n)) & 0xFFU);
    }
}

static void ff_cpu_name(FfLine& l)
{
    uint64_t midr = 0;
    asm volatile("mrs %0, midr_el1" : "=r"(midr));
    uint32_t implementer = (uint32_t)((midr >> 24) & 0xFFU);
    uint32_t part = (uint32_t)((midr >> 4) & 0xFFFU);
    const char* name = 0;
    if (implementer == 0x41U)
    {
        switch (part)
        {
            case 0xD03: name = "Cortex-A53"; break;
            case 0xD04: name = "Cortex-A35"; break;
            case 0xD05: name = "Cortex-A55"; break;
            case 0xD07: name = "Cortex-A57"; break;
            case 0xD08: name = "Cortex-A72"; break;
            case 0xD09: name = "Cortex-A73"; break;
            case 0xD0A: name = "Cortex-A75"; break;
            case 0xD0B: name = "Cortex-A76"; break;
            case 0xD0C: name = "Neoverse-N1"; break;
            case 0xD0D: name = "Cortex-A77"; break;
            case 0xD40: name = "Neoverse-V1"; break;
            case 0xD41: name = "Cortex-A78"; break;
            case 0xD44: name = "Cortex-X1"; break;
            case 0xD46: name = "Cortex-A510"; break;
            case 0xD47: name = "Cortex-A710"; break;
            case 0xD48: name = "Cortex-X2"; break;
            case 0xD49: name = "Neoverse-N2"; break;
            case 0xD4F: name = "Neoverse-V2"; break;
            default: break;
        }
    }
    else if (implementer == 0x61U)
        name = "Apple Silicon";
    if (name)
    {
        ff_str(l, name);
        return;
    }
    ff_str(l, "ARMv8 (implementer ");
    ff_hex(l, implementer);
    ff_str(l, ", part ");
    ff_hex(l, part);
    ff_str(l, ")");
}

static int ff_package_count()
{
    int dir = fs_resolve("/var/lib/pkg", 0);
    if (dir < 0) return 0;
    int count = 0;
    int children = fs_get_child_count(dir);
    for (int i = 0; i < children; ++i)
    {
        int child = fs_get_child(dir, i);
        if (child < 0) continue;
        const char* name = fs_get_name(child);
        int n = 0;
        while (name && name[n]) ++n;
        if (n > 5 && name[n - 5] == '.' && name[n - 4] == 'i' && name[n - 3] == 'n' && name[n - 2] == 'f' && name[n - 1] == 'o') ++count;
    }
    return count;
}

static void ff_key(FfLine& l, const char* key)
{
    ff_esc(l, "1;36");
    ff_str(l, key);
    ff_esc(l, "0");
    ff_str(l, ": ");
}

static int ff_build_info(FfLine* out, int max)
{
    int n = 0;
    FfLine* l;

#define FF_NEXT() do { if (n >= max) return n; l = &out[n++]; ff_reset(*l); } while (0)

    FF_NEXT();                                           /* title: user@host */
    ff_esc(*l, "1;32"); ff_str(*l, security_username()); ff_esc(*l, "0");
    ff_str(*l, "@");
    ff_esc(*l, "1;36"); ff_str(*l, "RioOS"); ff_esc(*l, "0");
    int title_len = 0;
    {
        const char* u = security_username();
        while (u && u[title_len]) ++title_len;
        title_len += 6;                                   /* "@RioOS" */
    }

    FF_NEXT();
    for (int i = 0; i < title_len; ++i) ff_str(*l, "-");

    FF_NEXT(); ff_key(*l, "OS");        ff_str(*l, "RioOS aarch64");
    FF_NEXT(); ff_key(*l, "Host");      ff_str(*l, "QEMU virt");
    FF_NEXT(); ff_key(*l, "Kernel");    ff_str(*l, "Mykernel "); ff_str(*l, MYKERNEL_VERSION_STRING);
    FF_NEXT(); ff_key(*l, "Uptime");    ff_uptime(*l, timer_seconds());
    FF_NEXT(); ff_key(*l, "Packages");  ff_u64(*l, (uint64_t)ff_package_count()); ff_str(*l, " (pkg)");
    FF_NEXT(); ff_key(*l, "Shell");     ff_str(*l, "RioOS shell");

    FF_NEXT(); ff_key(*l, "Display");
    if (fb_has_display())
    {
        ff_u64(*l, (uint64_t)fb_get_width()); ff_str(*l, "x"); ff_u64(*l, (uint64_t)fb_get_height());
        ff_str(*l, " ("); ff_str(*l, fb_backend_name()); ff_str(*l, ")");
    }
    else
        ff_str(*l, "none (serial only)");

    FF_NEXT(); ff_key(*l, "Terminal");
    if (console_active())
    {
        ff_str(*l, "RioOS console ");
        ff_u64(*l, (uint64_t)console_columns()); ff_str(*l, "x"); ff_u64(*l, (uint64_t)console_rows());
    }
    else
        ff_str(*l, "UART serial");

    FF_NEXT(); ff_key(*l, "CPU");       ff_cpu_name(*l); ff_str(*l, " ("); ff_u64(*l, smp_online_cpu_count()); ff_str(*l, ")");
    FF_NEXT(); ff_key(*l, "Scheduler"); ff_str(*l, "preemptive, 100 Hz, "); ff_u64(*l, (uint64_t)scheduler_ready_count()); ff_str(*l, " runnable");

    PageStats pages;
    page_alloc_get_stats(&pages);
    uint64_t ram = memory_total();
    uint64_t free_bytes = pages.free_pages * PAGE_SIZE_BYTES + heap_free();
    uint64_t used_bytes = ram > free_bytes ? ram - free_bytes : 0;
    FF_NEXT(); ff_key(*l, "Memory");    ff_size(*l, used_bytes); ff_str(*l, " / "); ff_size(*l, ram); ff_percent(*l, used_bytes, ram);
    {   /* the page pool is carved out of the heap: show the heap proper, as `free` does */
        uint64_t pool = pages.total_pages * PAGE_SIZE_BYTES;
        uint64_t heap_proper = heap_used() > pool ? heap_used() - pool : 0;
        FF_NEXT(); ff_key(*l, "Heap");  ff_size(*l, heap_proper); ff_str(*l, " / "); ff_size(*l, heap_proper + heap_free());
    }

    FsMountInfo mi;
    FF_NEXT(); ff_key(*l, "Disk (/)");
    if (fs_mount_info(0, &mi) == 0)
    {
        uint64_t total = mi.total_sectors * 512ULL, used = mi.used_sectors * 512ULL;
        ff_size(*l, used); ff_str(*l, " / "); ff_size(*l, total); ff_percent(*l, used, total);
        ff_str(*l, fs_persistent() ? " - MyFS" : " - MyFS (RAM only)");
    }
    else
        ff_str(*l, "n/a");

    FF_NEXT(); ff_key(*l, "Local IP");
    if (net_ready()) ff_ipv4(*l, net_local_ip());
    else ff_str(*l, virtio_net_present() ? "no lease yet" : "offline");

    FF_NEXT(); ff_key(*l, "Processes"); ff_u64(*l, (uint64_t)process_count());

    if (ff_color)                                         /* blank line, then two rows of colour blocks */
    {
        FF_NEXT();
        FF_NEXT();
        for (int c = 0; c < 8; ++c) { char code[8]; code[0] = '4'; code[1] = (char)('0' + c); code[2] = 0; ff_esc(*l, code); ff_str(*l, "   "); }
        ff_esc(*l, "0");
        FF_NEXT();
        for (int c = 0; c < 8; ++c) { char code[8]; code[0] = '1'; code[1] = '0'; code[2] = (char)('0' + c); code[3] = 0; ff_esc(*l, code); ff_str(*l, "   "); }
        ff_esc(*l, "0");
    }
#undef FF_NEXT
    return n;
}
}

void command_fastfetch(const char* argument)
{
    int show_logo = 1;
    ff_color = 1;

    /* options */
    const char* p = argument ? argument : "";
    while (*p)
    {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        int len = (int)(p - start);
        auto is = [&](const char* word) { int i = 0; while (word[i]) { if (i >= len || start[i] != word[i]) return false; ++i; } return i == len; };
        if (is("--no-logo") || is("-n")) show_logo = 0;
        else if (is("--plain") || is("-p")) ff_color = 0;
        else if (is("--help") || is("-h"))
        {
            uart_puts("usage: fastfetch [--no-logo|-n] [--plain|-p]\r\n"
                      "  prints the RioOS logo and system information (alias: neofetch)\r\n"
                      "  --no-logo  information only     --plain  no colours\r\n");
            return;
        }
        else
        {
            uart_puts("fastfetch: unknown option '");
            for (int i = 0; i < len; ++i) uart_putc(start[i]);
            uart_puts("' (try --help)\r\n");
            return;
        }
    }

    /* split the logo into lines and measure it */
    const char* logo_line[48];
    int logo_len[48];
    int logo_lines = 0;
    int logo_width = 0;
    {
        const char* q = MYKERNEL_LOGO;
        while (*q && logo_lines < 48)
        {
            const char* s = q;
            int n = 0;
            while (*q && *q != '\n') { if (*q != '\r') ++n; ++q; }
            logo_line[logo_lines] = s;
            logo_len[logo_lines] = n;
            if (n > logo_width) logo_width = n;
            ++logo_lines;
            if (*q == '\n') ++q;
        }
    }

    static FfLine info[40];
    int info_lines = ff_build_info(info, 40);

    int columns = console_active() ? console_columns() : 80;
    if (columns < logo_width + 3 + 36) show_logo = 0;     /* too narrow for the logo next to the text */

    int rows = info_lines > logo_lines ? info_lines : logo_lines;
    if (!show_logo) rows = info_lines;

    for (int r = 0; r < rows; ++r)
    {
        if (show_logo)
        {
            int printed = 0;
            if (r < logo_lines)
            {
                if (ff_color) uart_puts("\x1B[1;36m");
                for (const char* c = logo_line[r]; *c && *c != '\n'; ++c)
                    if (*c != '\r') { uart_putc(*c); ++printed; }
                if (ff_color) uart_puts("\x1B[0m");
            }
            for (; printed < logo_width + 3; ++printed) uart_putc(' ');
        }
        if (r < info_lines) uart_puts(info[r].text);
        uart_puts("\r\n");
    }
}
