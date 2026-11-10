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

#include "commands.h"
#include "jobs.h"
#include "filters.h"
#include "shellext.h"
#include "fat32.h"
#include "pci.h"
#include "usb.h"
#include "tls.h"
#include "x509.h"
#include "ssh.h"
#include "netserv.h"
#include "nvme.h"
#include "ahci.h"
#include "storage.h"
#include "script.h"

#include "uart.h"
#include "fb.h"
#include "console.h"
#include "memory.h"
#include "string.h"

#include "vfs.h"
#include "fs.h"
#include "process.h"
#include "timer.h"
#include "scheduler.h"
#include "syscall.h"
#include "user.h"
#include "mmu.h"
#include "storage.h"
#include "elf.h"
#include "virtio_net.h"
#include "netstack.h"
#include "version.h"
#include "interrupt.h"
#include "page_alloc.h"
#include "fault.h"
#include "diag.h"
#include "security.h"
#include "history.h"
#include "env.h"
#include "thread.h"
#include "rtc.h"
#include "fd.h"
#include "smp.h"

static void print_u64(
    uint64_t value
)
{
    char buffer[32];

    int pos = 0;

    if (value == 0)
    {
        uart_putc('0');
        return;
    }

    while (
        value > 0 &&
        pos < 31
    )
    {
        buffer[pos++] =
            (char)(
                '0' +
                (value % 10)
            );

        value /= 10;
    }

    while (pos > 0)
    {
        pos--;

        uart_putc(
            buffer[pos]
        );
    }
}

static int parse_uint(const char* text, int fallback)
{
    if (text == 0 || text[0] < '0' || text[0] > '9')
        return fallback;

    int value = 0;

    while (*text >= '0' && *text <= '9' && value < 100000)
    {
        value = value * 10 + (*text - '0');
        ++text;
    }

    return value;
}

static void irq_quiesce()
{
    /* Safe state: timer off, PPI masked at the GIC, CPU IRQ masked. */
    timer_stop();
    interrupt_timer_line_disable();
    interrupt_disable();
    timer_set_sched_tick(1);
}

/* Back to the normal running state: 100 Hz system tick, scheduler tick on,
   GIC line unmasked, CPU IRQ unmasked. */
static void irq_resume_system_tick()
{
    timer_set_sched_tick(1);
    interrupt_timer_line_enable();
    timer_start_periodic(100U);
    interrupt_enable();
}

static void irq_print_status()
{
    InterruptStats st;
    interrupt_get_stats(&st);

    uint32_t mode = timer_mode();

    uart_puts("IRQ status\r\n----------\r\n");
    uart_puts("CPU IRQ        : ");
    uart_puts(interrupt_cpu_irq_enabled() ? "unmasked" : "masked");
    uart_puts("\r\nTimer mode     : ");
    uart_puts(mode == TIMER_MODE_PERIODIC ? "periodic" :
              mode == TIMER_MODE_ONESHOT  ? "one-shot" : "off");
    uart_puts("\r\nTimer Hz       : "); print_u64(timer_current_hz());
    uart_puts("\r\nTimer freq     : "); print_u64(timer_frequency());
    uart_puts("\r\nIRQ total      : "); print_u64(st.total);
    uart_puts("\r\nIRQ timer      : "); print_u64(st.timer);
    uart_puts("\r\nIRQ spurious   : "); print_u64(st.spurious);
    uart_puts("\r\nIRQ unhandled  : "); print_u64(st.unhandled);
    uart_puts("\r\nIRQ nested     : "); print_u64(st.nested);
    uart_puts("\r\nIRQ max depth  : "); print_u64(st.max_depth);
    uart_puts("\r\nTimer stray    : "); print_u64(timer_stray_count());
    uart_puts("\r\nOne-shot fired : "); print_u64(timer_oneshot_fired());
    uart_puts("\r\nSched ticks    : "); print_u64(scheduler_ticks());
    uart_puts("\r\n");
}

/* Stage B: one IRQ, handler runs, timer disabled, EOI, back to shell. */
static int irq_stage_oneshot()
{
    irq_quiesce();

    uint64_t before = timer_oneshot_fired();

    interrupt_timer_line_enable();
    timer_start_oneshot_ms(50);
    interrupt_enable();

    uint64_t start = timer_millis();

    while (timer_oneshot_fired() == before && timer_millis() - start < 500ULL)
    {
    }

    int ok = timer_oneshot_fired() == before + 1ULL &&
             timer_mode() == TIMER_MODE_OFF;

    irq_quiesce();
    irq_resume_system_tick();
    return ok;
}

/* Stage C/D: periodic IRQ for ~200 ms; expects a sane number of ticks. */
static int irq_stage_periodic(int hz, int with_sched_tick, int keep_running)
{
    irq_quiesce();

    uint64_t irq_before = timer_irq_count();
    uint64_t sched_before = scheduler_ticks();

    timer_set_sched_tick(with_sched_tick);
    interrupt_timer_line_enable();
    timer_start_periodic((uint32_t)hz);
    interrupt_enable();

    uint64_t start = timer_millis();

    while (timer_millis() - start < 200ULL)
    {
    }

    uint64_t irqs = timer_irq_count() - irq_before;
    uint64_t ticks = scheduler_ticks() - sched_before;
    uint64_t expected = (uint64_t)hz / 5ULL;

    /* Accept 50%..200% of the nominal count; a storm would be far above. */
    int ok = irqs >= expected / 2ULL && irqs <= expected * 2ULL + 2ULL;

    if (with_sched_tick)
        ok = ok && ticks == irqs;
    else
        ok = ok && ticks == 0ULL;

    if (!keep_running)
    {
        irq_quiesce();
        irq_resume_system_tick();
    }

    return ok;
}

static void command_irq(const char* argument)
{
    if (argument == 0 || argument[0] == 0 || str_equal(argument, "status"))
    {
        irq_print_status();
        return;
    }

    if (str_equal(argument, "off"))
    {
        irq_quiesce();
        uart_puts("Timer IRQ disabled; CPU IRQ masked. Use 'irq on' to restart the 100 Hz system tick.\r\n");
        return;
    }

    if (str_equal(argument, "on"))
    {
        irq_quiesce();
        irq_resume_system_tick();
        uart_puts("Timer IRQ enabled: 100 Hz system tick, CPU IRQ unmasked.\r\n");
        return;
    }

    if (str_equal(argument, "oneshot"))
    {
        uart_puts(irq_stage_oneshot()
            ? "[PASS] stage B: timer one-shot IRQ\r\n"
            : "[FAIL] stage B: timer one-shot IRQ\r\n");
        return;
    }

    if (argument[0] == 's' && argument[1] == 't' && argument[2] == 'a' &&
        argument[3] == 'g' && argument[4] == 'e' &&
        (argument[5] == 'c' || argument[5] == 'd') &&
        (argument[6] == 0 || argument[6] == ' '))
    {
        int with_tick = argument[5] == 'd';
        const char* hz_text = argument[6] == ' ' ? argument + 7 : 0;

        while (hz_text != 0 && *hz_text == ' ')
            ++hz_text;

        int hz = parse_uint(hz_text, 100);

        if (hz < 1) hz = 1;
        if (hz > 1000) hz = 1000;

        /* Left running so the shell can be used while the timer ticks. */
        int ok = irq_stage_periodic(hz, with_tick, 1);

        uart_puts(ok ? "[PASS] " : "[FAIL] ");
        uart_puts(with_tick ? "stage D: periodic IRQ + scheduler tick"
                            : "stage C: periodic IRQ without scheduler");
        uart_puts(" (timer left running; use 'irq off' to stop)\r\n");
        return;
    }

    if (str_equal(argument, "selftest"))
    {
        int pass = 0;
        int total = 0;

        ++total; if (irq_stage_oneshot()) { uart_puts("[PASS] timer one-shot IRQ\r\n"); ++pass; }
        else uart_puts("[FAIL] timer one-shot IRQ\r\n");

        ++total; if (irq_stage_periodic(100, 0, 0)) { uart_puts("[PASS] timer periodic IRQ (no scheduler)\r\n"); ++pass; }
        else uart_puts("[FAIL] timer periodic IRQ (no scheduler)\r\n");

        ++total; if (irq_stage_periodic(100, 1, 0)) { uart_puts("[PASS] timer periodic IRQ + scheduler tick\r\n"); ++pass; }
        else uart_puts("[FAIL] timer periodic IRQ + scheduler tick\r\n");

        InterruptStats st;
        interrupt_get_stats(&st);

        ++total; if (st.nested == 0ULL && st.unhandled == 0ULL && st.max_depth <= 1U)
        { uart_puts("[PASS] IRQ no nesting / no unhandled\r\n"); ++pass; }
        else uart_puts("[FAIL] IRQ no nesting / no unhandled\r\n");

        uart_puts("IRQ self-test: "); print_u64((uint64_t)pass);
        uart_puts("/"); print_u64((uint64_t)total); uart_puts(" passed\r\n");
        return;
    }

    uart_puts("Usage: irq [status|on|off|oneshot|stagec [hz]|staged [hz]|selftest]\r\n");
}

/* Allocate, verify zeroing, free, then check double-free is rejected. */
static int pages_selftest()
{
    if (!page_alloc_ready())
        return 0;

    PageStats before;
    page_alloc_get_stats(&before);

    volatile uint8_t* a = (volatile uint8_t*)allocate_page();
    volatile uint8_t* b = (volatile uint8_t*)allocate_page();

    if (a == 0 || b == 0 || a == b ||
        (((uint64_t)a | (uint64_t)b) & (PAGE_SIZE_BYTES - 1ULL)) != 0)
        return 0;

    for (uint64_t i = 0; i < PAGE_SIZE_BYTES; ++i)
    {
        if (a[i] != 0 || b[i] != 0)
            return 0;
    }

    a[0] = 0x5A;

    if (free_page((void*)a) != PAGE_FREE_OK ||
        free_page((void*)a) != PAGE_FREE_DOUBLE ||
        free_page((void*)b) != PAGE_FREE_OK)
        return 0;

    PageStats after;
    page_alloc_get_stats(&after);

    return after.free_pages == before.free_pages;
}

static void command_pages()
{
    PageStats st;
    page_alloc_get_stats(&st);

    uart_puts("Physical pages\r\n--------------\r\n");
    uart_puts("Ready          : "); uart_puts(page_alloc_ready() ? "yes" : "no");
    uart_puts("\r\nTotal pages    : "); print_u64(st.total_pages);
    uart_puts("\r\nFree pages     : "); print_u64(st.free_pages);
    uart_puts("\r\nAllocations    : "); print_u64(st.allocs);
    uart_puts("\r\nFrees          : "); print_u64(st.frees);
    uart_puts("\r\nFailed allocs  : "); print_u64(st.failed_allocs);
    uart_puts("\r\nRejected frees : "); print_u64(st.rejected_frees);
    uart_puts("\r\nUser faults    : "); print_u64(fault_user_count());
    uart_puts("\r\nKernel faults  : "); print_u64(fault_kernel_count());
    uart_puts("\r\n");
}

static void command_tasks()
{
    uart_puts("Tasks\r\n-----\r\n");
    uart_puts("Processes      : "); print_u64((uint64_t)process_count());
    uart_puts("\r\nRunnable       : "); print_u64((uint64_t)scheduler_ready_count());
    uart_puts("\r\nCurrent PID    : ");

    int cur = scheduler_current_pid();

    if (cur < 0) uart_puts("none"); else print_u64((uint64_t)cur);

    uart_puts("\r\nSched ticks    : "); print_u64(scheduler_ticks());
    uart_puts("\r\n\r\n");
    process_list();
}

static int split_words(char* argument, char** words, int capacity);
static int str_contains(const char* hay, const char* needle)
{
    int n = str_len(needle);
    for (int i = 0; hay[i]; ++i)
    {
        int k = 0;
        while (k < n && hay[i + k] == needle[k]) ++k;
        if (k == n) return 1;
    }
    return 0;
}

static int dev_by_name(const char* n)
{
    if (str_equal(n, "ram0") || str_equal(n, "/dev/ram0")) return 1;
    if (str_equal(n, "ram1") || str_equal(n, "/dev/ram1")) return 5;
    if (str_equal(n, "nvme1") || str_equal(n, "nvme0n1") || str_equal(n, "/dev/nvme0n1") || str_equal(n, "/dev/nvme1")) return 3;
    if (str_equal(n, "sda") || str_equal(n, "sata0") || str_equal(n, "/dev/sda")) return 4;
    return -1;
}
static const char* dev_name(int d)
{
    return d == 0 ? "root" : d == 1 ? "ram0" : d == 5 ? "ram1" : d == 3 ? "nvme0n1" : d == 4 ? "sda" : "?";
}
static void mount_err(const char* who, int rc)
{
    uart_puts(who); uart_puts(": ");
    if (rc == -2) uart_puts("mount point not found");
    else if (rc == -20) uart_puts("mount point is not a directory");
    else if (rc == -16) uart_puts("device or mount point busy / already mounted");
    else if (rc == -19) uart_puts("no such device");
    else if (rc == -24) uart_puts("too many mounts");
    else if (rc == -22) uart_puts("invalid argument");
    else uart_puts("no valid filesystem on device (use 'mkfs <dev>' first)");
    uart_puts("\r\n");
}

static void command_mount(const char* argument)
{
    if (argument && argument[0])
    {
        if (!security_is_root()) { uart_puts("mount: permission denied (root required)\r\n"); return; }
        char buf[128]; str_copy(buf, argument, sizeof(buf));
        char* w[8]; int n = split_words(buf, w, 8);
        int ro = -1, tmpfs = 0; char* pos[3]; int np = 0;
        for (int i = 0; i < n; ++i)
        {
            if (str_equal(w[i], "-r")) ro = 1;
            else if (str_equal(w[i], "-w")) ro = 0;
            else if (str_equal(w[i], "-t") && i + 1 < n) { ++i; if (str_equal(w[i], "tmpfs")) tmpfs = 1; else if (!str_equal(w[i], "myfs")) { uart_puts("mount: unknown filesystem type (myfs, tmpfs)\r\n"); return; } }
            else if (str_equal(w[i], "-o") && i + 1 < n)
            {
                ++i;
                if (str_contains(w[i], "ro")) ro = 1;
                if (str_contains(w[i], "rw")) ro = 0;
            }
            else if (np < 3) pos[np++] = w[i];
        }
        int cwd = vfs_cwd();
        if (tmpfs)
        {
            if (np != 2) { uart_puts("usage: mount -t tmpfs none <dir>\r\n"); return; }
            int dev = -1;
            for (int k = 0; k < 2 && dev < 0; ++k) { int d = k ? 5 : 1; FsMountInfo mi; int used = 0; for (int j = 0; fs_mount_info(j, &mi) == 0; ++j) if (mi.dev == d) used = 1; if (!used) dev = d; }
            if (dev < 0) { uart_puts("mount: no free RAM disk for tmpfs\r\n"); return; }
            int rc = fs_mount_device(dev, pos[1], cwd, ro == 1, 1);
            if (rc != 0) { mount_err("mount", rc); return; }
            uart_puts("tmpfs mounted on "); uart_puts(pos[1]); uart_puts(" ("); uart_puts(dev_name(dev)); uart_puts(", volatile)\r\n");
            return;
        }
        if (np == 2)
        {
            int dev = dev_by_name(pos[0]);
            if (dev < 0) { uart_puts("mount: unknown device (ram0 ram1 nvme0n1 sda)\r\n"); return; }
            int rc = fs_mount_device(dev, pos[1], cwd, ro == 1, 0);
            if (rc != 0) { mount_err("mount", rc); return; }
            uart_puts(dev_name(dev)); uart_puts(" mounted on "); uart_puts(pos[1]); uart_puts(ro == 1 ? " (ro)\r\n" : "\r\n");
            return;
        }
        if (ro < 0 || np > 1 || (np == 1 && !str_equal(pos[0], "/"))) { uart_puts("usage: mount | mount [-r] <dev> <dir> | mount -t tmpfs none <dir> | mount -o remount,ro|rw /\r\n"); return; }
        (void)fs_sync();
        fs_set_readonly(ro);
        uart_puts(ro ? "/ remounted read-only.\r\n" : "/ remounted read-write.\r\n");
        return;
    }
    FsJournalInfo ji;
    fs_journal_status(&ji);
    uart_puts("Mounts\r\n------\r\n");
    uart_puts("block device   : "); {
        int bk = storage_backend();
        uart_puts(!storage_ready() ? "none" : bk == 2 ? "NVMe (ready)" : bk == 3 ? "AHCI/SATA (ready)" : "virtio-blk (ready)");
    }
    uart_puts("\r\nroot (/)       : myfs, ");
    uart_puts(fs_persistent() ? "persistent (disk-backed)" : "RAM only (not persistent)");
    uart_puts(fs_is_readonly() ? ", ro" : ", rw");
    uart_puts("\r\njournal        : ");
    if (ji.enabled)
    {
        uart_puts("on (sectors "); print_u64(ji.start_sector); uart_puts("+"); print_u64(ji.sectors);
        uart_puts("), commits "); print_u64(ji.commits); uart_puts(", replayed "); print_u64(ji.replayed);
    }
    else uart_puts("off");
    uart_puts("\r\n");
    int cnt = fs_mount_count();
    for (int i = 1; i < cnt; ++i)
    {
        FsMountInfo mi;
        if (fs_mount_info(i, &mi) != 0) continue;
        uart_puts(dev_name(mi.dev)); uart_puts(" on "); uart_puts(mi.dir);
        uart_puts((mi.dev == 1 || mi.dev == 5) ? " type tmpfs (" : " type myfs (");
        uart_puts(mi.readonly ? "ro" : "rw"); uart_puts(")\r\n");
    }
}

static void command_umount(const char* argument)
{
    if (!security_is_root()) { uart_puts("umount: permission denied (root required)\r\n"); return; }
    if (!argument || !argument[0]) { uart_puts("usage: umount <mountpoint>\r\n"); return; }
    if (str_equal(argument, "/")) { uart_puts("umount: /: target is busy (root filesystem; use 'mount -o remount,ro /')\r\n"); return; }
    int rc = fs_umount_path(argument, vfs_cwd(), vfs_cwd());
    if (rc == 0) { uart_puts("umount: "); uart_puts(argument); uart_puts(" unmounted\r\n"); return; }
    if (rc == -16) { uart_puts("umount: "); uart_puts(argument); uart_puts(": target is busy\r\n"); return; }
    uart_puts("umount: "); uart_puts(argument); uart_puts(": not mounted\r\n");
}

static void command_mkfs(const char* argument)
{
    if (!security_is_root()) { uart_puts("mkfs: permission denied (root required)\r\n"); return; }
    char buf[64]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[3]; int n = split_words(buf, w, 3);
    if (n != 1) { uart_puts("usage: mkfs <dev>   (ram0 ram1 nvme0n1 sda)\r\n"); return; }
    int dev = dev_by_name(w[0]);
    if (dev < 0) { uart_puts("mkfs: unknown device\r\n"); return; }
    int rc = fs_mkfs_device(dev);
    if (rc == -16) { uart_puts("mkfs: device is mounted\r\n"); return; }
    if (rc == -19) { uart_puts("mkfs: no such device\r\n"); return; }
    if (rc != 0) { uart_puts("mkfs: format failed\r\n"); return; }
    uart_puts("mkfs: "); uart_puts(dev_name(dev)); uart_puts(" formatted (myfs)\r\n");
}

static void fat_list_print(const char* name, uint32_t size, int is_dir, void* ctx)
{
    (void)ctx;
    if (is_dir) { uart_puts("[DIR]  "); uart_puts(name); uart_puts("\r\n"); }
    else { uart_puts("[FILE] "); uart_puts(name); uart_puts("  ("); print_u64(size); uart_puts(" bytes)\r\n"); }
}
static void fat_err(const char* what, int rc)
{
    uart_puts("fat: "); uart_puts(what); uart_puts(": ");
    if (rc == -1) uart_puts("not a FAT32 volume (use 'fat <dev> mkfs' to format)");
    else if (rc == -2) uart_puts("not found");
    else if (rc == -20) uart_puts("not a directory");
    else if (rc == -21) uart_puts("is a directory");
    else if (rc == -22) uart_puts("invalid name (new names must be 8.3: NAME.EXT, letters/digits)");
    else if (rc == -17) uart_puts("already exists");
    else if (rc == -28) uart_puts("volume full");
    else if (rc == -27) uart_puts("file too large for transfer (1 MiB limit)");
    else if (rc == -39) uart_puts("directory not empty");
    else if (rc == -19) uart_puts("no such device");
    else uart_puts("I/O error");
    uart_puts("\r\n");
}
static int fat_dev_mounted(int dev)
{
    FsMountInfo mi;
    for (int i = 0; fs_mount_info(i, &mi) == 0; ++i) if (i > 0 && mi.dev == dev) return 1;
    return 0;
}
static int cw_tokens(char* s, char** w, int cap);
static void command_fat(const char* argument)
{
    if (!security_is_root()) { uart_puts("fat: permission denied (root required: raw device access)\r\n"); return; }
    char buf[200]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[6]; int n = cw_tokens(buf, w, 6);
    if (n < 2) { uart_puts("usage: fat <dev> mkfs [label] | info | ls [path] | cat <path> | get <fatpath> <file> | put <file> <fatpath> | mkdir <path> | rm <path>\r\n       dev: ram0 ram1 nvme0n1 sda (not while mounted)\r\n"); return; }
    int dev = dev_by_name(w[0]);
    if (dev < 0) { uart_puts("fat: unknown device (ram0 ram1 nvme0n1 sda)\r\n"); return; }
    if (fat_dev_mounted(dev)) { uart_puts("fat: device is mounted as myfs; umount it first\r\n"); return; }
    const char* sub = w[1];
    if (str_equal(sub, "mkfs"))
    {
        int rc = fat_mkfs(dev, n > 2 ? w[2] : "RIOOS");
        if (rc != 0) { fat_err("mkfs", rc); return; }
        uart_puts("fat: "); uart_puts(dev_name(dev)); uart_puts(" formatted (FAT32)\r\n"); return;
    }
    if (str_equal(sub, "info"))
    {
        FatInfo fi; int rc = fat_info(dev, &fi);
        if (rc != 0) { fat_err("info", rc); return; }
        uart_puts("FAT32 volume '"); uart_puts(fi.label); uart_puts("'\r\n  sectors: "); print_u64(fi.total_sectors);
        uart_puts("  cluster: "); print_u64(fi.sectors_per_cluster * 512ULL); uart_puts(" bytes\r\n  clusters: "); print_u64(fi.clusters);
        uart_puts("  free: "); print_u64(fi.free_clusters); uart_puts("\r\n"); return;
    }
    if (str_equal(sub, "ls")) { int rc = fat_list(dev, n > 2 ? w[2] : "/", fat_list_print, 0); if (rc != 0) fat_err("ls", rc); return; }
    if (str_equal(sub, "mkdir") && n == 3) { int rc = fat_mkdir(dev, w[2]); if (rc != 0) fat_err("mkdir", rc); else uart_puts("fat: directory created\r\n"); return; }
    if (str_equal(sub, "rm") && n == 3) { int rc = fat_remove(dev, w[2]); if (rc != 0) fat_err("rm", rc); else uart_puts("fat: removed\r\n"); return; }
    if ((str_equal(sub, "cat") && n == 3) || (str_equal(sub, "get") && n == 4))
    {
        uint8_t* b = (uint8_t*)kmalloc(1048576 + 16);
        if (!b) { uart_puts("fat: out of memory\r\n"); return; }
        uint32_t size = 0; int rc = fat_read(dev, w[2], b, 1048576, &size);
        if (rc != 0) { fat_err(w[1], rc); kfree(b); return; }
        if (sub[0] == 'c') { for (uint32_t i = 0; i < size; ++i) { if (b[i] == '\n') uart_putc('\r'); uart_putc((char)b[i]); } uart_puts("\r\n"); }
        else
        {
            int wr = fs_write_data(w[3], vfs_cwd(), b, size);
            if (wr < 0) uart_puts("fat: cannot write destination file\r\n");
            else { uart_puts("fat: copied "); print_u64(size); uart_puts(" bytes to "); uart_puts(w[3]); uart_puts("\r\n"); }
        }
        kfree(b); return;
    }
    if (str_equal(sub, "put") && n == 4)
    {
        uint8_t* b = (uint8_t*)kmalloc(1048576 + 16);
        if (!b) { uart_puts("fat: out of memory\r\n"); return; }
        uint64_t size = 0;
        if (fs_read_file(w[2], vfs_cwd(), b, 1048576, &size) != 0) { uart_puts("fat: cannot read source file (missing or over 1 MiB)\r\n"); kfree(b); return; }
        int rc = fat_write(dev, w[3], b, (uint32_t)size);
        if (rc != 0) fat_err("put", rc); else { uart_puts("fat: wrote "); print_u64(size); uart_puts(" bytes to "); uart_puts(w[3]); uart_puts("\r\n"); }
        kfree(b); return;
    }
    uart_puts("fat: unknown subcommand or wrong arguments (run 'fat' for usage)\r\n");
}

static void command_lspci() { pci_list(); }
static void command_usb(const char* argument)
{
    if (argument && str_equal(argument, "selftest")) { if (!usb_present()) { uart_puts("usb: no xHCI controller (try QEMU: -device qemu-xhci -device usb-kbd)\r\n"); return; } int rc = usb_selftest(); uart_puts(rc == 0 ? "usb selftest: PASS\r\n" : "usb selftest: FAIL\r\n"); return; }
    usb_status();
}

static void command_nvme(const char* argument)
{
    if (!nvme_ready()) { uart_puts("nvme: no NVMe controller (try QEMU: -device nvme,drive=nv,serial=rio)\r\n"); return; }
    if (argument && str_equal(argument, "selftest"))
    {
        int rc = nvme_selftest(storage_backend() != 2);
        uart_puts(rc == 0 ? "nvme selftest: PASS\r\n" : "nvme selftest: FAIL\r\n");
        return;
    }
    nvme_status();
}

static void command_sata(const char* argument)
{
    if (!ahci_ready()) { uart_puts("sata: no AHCI disk (try QEMU: -device ich9-ahci,id=ahci -device ide-hd,drive=sd,bus=ahci.0)\r\n"); return; }
    if (argument && str_equal(argument, "selftest"))
    {
        int rc = ahci_selftest(storage_backend() != 3);
        uart_puts(rc == 0 ? "sata selftest: PASS\r\n" : "sata selftest: FAIL\r\n");
        return;
    }
    ahci_status();
}

static void command_dhcp(const char* argument)
{
    if (!net_ready()) { uart_puts("dhcp: network unavailable\r\n"); return; }
    char sub[16]; str_copy(sub, argument ? argument : "", sizeof(sub));
    uint32_t srv, lease; int bound;
    if (str_equal(sub, "status"))
    {
        net_dhcp_info(&srv, &lease, &bound);
        if (!bound) { uart_puts("dhcp: not bound\r\n"); return; }
        uart_puts("dhcp: bound, server "); net_print_ipv4(srv); uart_puts(", lease "); print_u64(lease); uart_puts(" s, remaining "); print_u64(net_dhcp_remaining()); uart_puts(" s\r\n");
        return;
    }
    if (str_equal(sub, "renew"))
    {
        int rc = net_dhcp_renew(4000U);
        if (rc == 0) { net_dhcp_info(&srv, &lease, &bound); uart_puts("dhcp: lease renewed, "); print_u64(lease); uart_puts(" s\r\n"); }
        else if (rc == -22) uart_puts("dhcp: renew: no active lease (run dhcp first)\r\n");
        else uart_puts("dhcp: renew failed\r\n");
        return;
    }
    if (str_equal(sub, "release")) { uart_puts(net_dhcp_release() == 0 ? "dhcp: lease released (address kept until next dhcp)\r\n" : "dhcp: nothing to release\r\n"); return; }
    if (sub[0]) { uart_puts("usage: dhcp [status|renew|release]\r\n"); return; }
    uart_puts("dhcp: requesting lease...\r\n");
    int rc = net_dhcp(8000U);
    if (rc != 0) { uart_puts("dhcp: no lease (timeout/NAK)\r\n"); return; }
    net_dhcp_info(&srv, &lease, &bound);
    uart_puts("dhcp: bound, server "); net_print_ipv4(srv); uart_puts(", lease "); print_u64(lease); uart_puts(" s\r\n");
    net_status();
}

static void command_ping6(const char* argument)
{
    uint8_t t[16];
    if (!argument || !argument[0] || net_ip6_parse(argument, t) != 0) { uart_puts("usage: ping6 <ipv6-address>\r\n"); return; }
    int rc = net_ping6(t, 3000U);
    if (rc >= 0) { uart_puts("Reply from "); net_ip6_print(t); uart_puts(": time="); print_u64((uint64_t)rc); uart_puts(" ms\r\n"); }
    else uart_puts("ping6: no reply\r\n");
}

/* httpd <port> [requests]: tiny HTTP/1.0 server over the TCP listen/accept API. */
static int g_sleep_abort = 0;
static int sleep_key_filter(int c) { if (c == 3) { g_sleep_abort = 1; return 1; } return 0; }

static void command_sleep(const char* argument)
{
    int secs = parse_uint(argument, -1);
    if (secs < 0 || secs > 3600) { uart_puts("sleep: usage: sleep <seconds 0-3600>\r\n"); return; }
    g_sleep_abort = 0;
    uint64_t end = timer_millis() + (uint64_t)secs * 1000ULL;
    while (timer_millis() < end && !g_sleep_abort)
    {
        uart_poll_job_keys(sleep_key_filter);
        thread_yield();
        asm volatile("wfi");
    }
    if (g_sleep_abort) { uart_puts("sleep: interrupted\r\n"); script_set_status(130); }
}

static void command_rmdir(const char* argument)
{
    if (!argument || !argument[0]) { uart_puts("rmdir: missing operand\r\n"); return; }
    int id = fs_resolve_nofollow(argument, vfs_cwd());
    if (id < 0) { uart_puts("rmdir: not found\r\n"); return; }
    if (fs_get_type(id) != FS_DIR) { uart_puts("rmdir: not a directory\r\n"); return; }
    if (fs_get_child_count(id) > 0) { uart_puts("rmdir: directory not empty\r\n"); return; }
    int rc = fs_rm(argument, vfs_cwd());
    if (rc == -13) uart_puts("rmdir: permission denied\r\n");
    else if (rc == -30) uart_puts("rmdir: read-only filesystem\r\n");
    else if (rc != 0) uart_puts("rmdir: failed\r\n");
}

static void command_free()
{
    PageStats pg; page_alloc_get_stats(&pg);
    uart_puts("              total        used        free\r\n");
    uint64_t pool = (uint64_t)pg.total_pages * 4096ULL;
    uint64_t hu = heap_used(); hu = hu > pool ? hu - pool : 0;
    uart_puts("Heap   : "); print_u64(hu + heap_free()); uart_puts("  "); print_u64(hu); uart_puts("  "); print_u64(heap_free());
    uart_puts("\r\nPages  : "); print_u64(pg.total_pages); uart_puts("  "); print_u64(pg.total_pages - pg.free_pages); uart_puts("  "); print_u64(pg.free_pages);
    uart_puts("   (4 KiB each)\r\n");
}

static int is_builtin_name(const char* name);

static void command_which(const char* argument)
{
    if (!argument || !argument[0]) { uart_puts("which: missing operand\r\n"); return; }
    char path[96];
    if (jobs_resolve_program(argument, path, sizeof(path))) { uart_puts(path); uart_puts("\r\n"); return; }
    if (is_builtin_name(argument)) { uart_puts(argument); uart_puts(": shell built-in\r\n"); return; }
    uart_puts("which: "); uart_puts(argument); uart_puts(" not found\r\n");
}

static void command_seq(const char* argument)
{
    char buf[48]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[3]; int n = split_words(buf, w, 3);
    int a = 1, b = 0;
    if (n == 1) b = parse_uint(w[0], -1);
    else if (n == 2) { a = parse_uint(w[0], -1); b = parse_uint(w[1], -1); }
    else { uart_puts("usage: seq [first] last\r\n"); return; }
    if (a < 0 || b < 0 || b - a > 100000) { uart_puts("seq: bad range\r\n"); return; }
    for (int i = a; i <= b; ++i) { print_u64((uint64_t)i); uart_puts("\r\n"); }
}

static void command_basename(const char* argument, int dir)
{
    if (!argument || !argument[0]) { uart_puts(dir ? "dirname" : "basename"); uart_puts(": missing operand\r\n"); return; }
    char t[128]; str_copy(t, argument, sizeof(t));
    int l = str_len(t);
    while (l > 1 && t[l - 1] == '/') t[--l] = 0;
    int slash = -1;
    for (int i = 0; i < l; ++i) if (t[i] == '/') slash = i;
    if (dir)
    {
        if (slash < 0) uart_puts(".");
        else if (slash == 0) uart_puts("/");
        else { t[slash] = 0; uart_puts(t); }
    }
    else uart_puts(slash < 0 ? t : t + slash + 1);
    uart_puts("\r\n");
}

/* PSCI SYSTEM_OFF / SYSTEM_RESET (QEMU virt exits on either; with -no-reboot reset also exits) */
static void command_power(int reset)
{
    (void)fs_sync();
    uart_puts(reset ? "Rebooting...\r\n" : "Powering off...\r\n");
    register uint64_t x0 asm("x0") = reset ? 0x84000009ULL : 0x84000008ULL;
    asm volatile("smc #0" : "+r"(x0) :: "memory", "x1", "x2", "x3");
    uart_puts("power: PSCI call failed\r\n");
}

static void command_sync()
{
    uart_puts(fs_sync() == 0 ? "synced.\r\n" : "sync: I/O error\r\n");
}

static int parse_u64_arg(const char* s, uint64_t* out);
static void command_ln(const char* argument)
{
    char buf[200]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[4]; int n = split_words(buf, w, 4);
    if (n == 3 && str_equal(w[0], "-s"))
    {
        int rc = fs_symlink(w[1], w[2], vfs_cwd());
        if (rc >= 0) uart_puts("Symlink created.\r\n");
        else if (rc == -30) uart_puts("ln: read-only filesystem\r\n");
        else if (rc == -17) uart_puts("ln: destination exists\r\n");
        else if (rc == -13) uart_puts("ln: permission denied\r\n");
        else uart_puts("ln: failed\r\n");
        return;
    }
    if (n == 2)
    {
        int rc = fs_link_hard(w[0], w[1], vfs_cwd());
        if (rc >= 0) { uart_puts("Hard link created ("); print_u64((uint64_t)fs_link_count(rc)); uart_puts(" links).\r\n"); }
        else if (rc == -30) uart_puts("ln: read-only filesystem\r\n");
        else if (rc == -17) uart_puts("ln: destination exists\r\n");
        else if (rc == -13) uart_puts("ln: permission denied\r\n");
        else if (rc == -2) uart_puts("ln: target not found\r\n");
        else if (rc == -21) uart_puts("ln: hard links to directories/symlinks are not allowed\r\n");
        else if (rc == -22) uart_puts("ln: cannot hard-link an empty file (write data first)\r\n");
        else if (rc == -18) uart_puts("ln: cannot link across filesystems\r\n");
        else uart_puts("ln: failed\r\n");
        return;
    }
    uart_puts("usage: ln <target> <linkname>   (hard link)\r\n       ln -s <target> <linkname>   (symbolic link)\r\n");
}

static void command_fsck(const char* argument)
{
    char buf[100]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[4]; int n = split_words(buf, w, 4);
    int repair = 0; const char* path = 0;
    for (int i = 0; i < n; ++i) { if (str_equal(w[i], "-r")) repair = 1; else if (!path) path = w[i]; else { uart_puts("usage: fsck [-r] [mountpoint]\r\n"); return; } }
    if (repair && !security_is_root()) { uart_puts("fsck: -r requires root\r\n"); return; }
    FsckInfo f;
    int rc = fs_fsck(path, vfs_cwd(), repair, &f);
    if (rc == -22) { uart_puts("fsck: not a mount point\r\n"); return; }
    if (rc != 0) { uart_puts("fsck: failed\r\n"); return; }
    uart_puts("fsck "); uart_puts(path ? path : "/"); uart_puts("\r\n");
    uart_puts("  inodes in use : "); print_u64((uint64_t)f.inodes); uart_puts(" ("); print_u64((uint64_t)f.dirs); uart_puts(" dirs, ");
    print_u64((uint64_t)f.files); uart_puts(" files, "); print_u64((uint64_t)f.symlinks); uart_puts(" symlinks, ");
    print_u64((uint64_t)f.hardlinks); uart_puts(" hard-linked)\r\n");
    uart_puts("  sectors in use: "); print_u64(f.sectors_used); uart_puts("\r\n");
    uart_puts("  structure     : "); uart_puts(f.structure_ok ? "ok" : "ERRORS (inode table inconsistent)"); uart_puts("\r\n");
    uart_puts("  bitmap        : "); if (f.bitmap_ok) uart_puts("ok");
    else { print_u64(f.leaked); uart_puts(" leaked, "); print_u64(f.missing); uart_puts(" missing"); uart_puts(f.repaired ? " -> repaired" : " (run fsck -r to repair)"); }
    uart_puts("\r\n");
    uart_puts(f.structure_ok && (f.bitmap_ok || f.repaired) ? "fsck: clean\r\n" : "fsck: problems found\r\n");
}

static void command_quota(const char* argument)
{
    char buf[100]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[4]; int n = split_words(buf, w, 4);
    uint64_t uid = 0, kb = 0;
    if (n >= 1 && str_equal(w[0], "set"))
    {
        if (!security_is_root()) { uart_puts("quota: permission denied (root required)\r\n"); return; }
        if (n != 3 || !parse_u64_arg(w[1], &uid) || !parse_u64_arg(w[2], &kb)) { uart_puts("usage: quota set <uid> <KiB>   (0 removes the limit)\r\n"); return; }
        int rc = fs_quota_set((uint32_t)uid, kb * 1024ULL);
        uart_puts(rc == 0 ? "quota set.\r\n" : "quota: failed (read-only or table full)\r\n");
        return;
    }
    uart_puts("uid    limit(KiB)  used(KiB)\r\n");
    uint64_t ids[3] = { 0, 1000, 0 }; int cnt = 2;
    if (n == 1 && parse_u64_arg(w[0], &uid)) { ids[0] = uid; cnt = 1; }
    else if (n >= 1 && !str_equal(w[0], "list")) { uart_puts("usage: quota [list|<uid>] | quota set <uid> <KiB>\r\n"); return; }
    for (int i = 0; i < cnt; ++i)
    {
        uint64_t lim = 0, used = 0; fs_quota_get((uint32_t)ids[i], &lim, &used);
        print_u64(ids[i]); uart_puts("      "); if (lim) print_u64(lim / 1024ULL); else uart_puts("none");
        uart_puts("       "); print_u64(used / 1024ULL); uart_puts("\r\n");
    }
}

static void command_realpath(const char* path)
{
    int id = fs_resolve(path, vfs_cwd());
    if (id < 0) { uart_puts(id == -40 ? "realpath: too many levels of symbolic links\r\n" : "realpath: path not found\r\n"); return; }
    char out[256]; vfs_path_of(id, out, sizeof(out)); uart_puts(out); uart_puts("\r\n");
}

static void command_readlink(const char* argument)
{
    char t[260];
    if (!argument || !argument[0]) { uart_puts("usage: readlink [-f] <link>\r\n"); return; }
    if (argument[0] == '-' && argument[1] == 'f' && argument[2] == ' ')
    {
        const char* p = argument + 3; while (*p == ' ') ++p;
        if (!*p) { uart_puts("usage: readlink [-f] <link>\r\n"); return; }
        command_realpath(p); return;
    }
    int rc = fs_readlink(argument, vfs_cwd(), t, sizeof(t));
    if (rc > 0) { uart_puts(t); uart_puts("\r\n"); }
    else uart_puts("readlink: not a symlink or not found\r\n");
}

static void command_jtest(const char* argument)
{
    if (!security_is_root()) { uart_puts("jtest: permission denied (root required)\r\n"); return; }
    const char* path = (argument && argument[0]) ? argument : "/jtest.txt";
    int rc = fs_journal_crash_test(path, vfs_cwd());
    if (rc == 0) uart_puts("Journal committed, in-place apply skipped (simulated power loss). Reset the machine; mount must replay it.\r\n");
    else uart_puts("jtest: failed\r\n");
}

static int is_builtin_name_impl(const char* name);
static int is_builtin_name(const char* name) { return is_builtin_name_impl(name); }

static void command_stats()
{
    InterruptStats irq;
    PageStats pg;
    interrupt_get_stats(&irq);
    page_alloc_get_stats(&pg);

    uart_puts("System statistics\r\n-----------------\r\n");
    uart_puts("Uptime (s)     : "); print_u64(timer_seconds());
    uart_puts("\r\nIRQ total/timer: "); print_u64(irq.total); uart_puts(" / "); print_u64(irq.timer);
    uart_puts("\r\nIRQ spurious   : "); print_u64(irq.spurious);
    uart_puts("\r\nSched ticks    : "); print_u64(scheduler_ticks());
    uart_puts("\r\nProcesses      : "); print_u64((uint64_t)process_count());
    uart_puts("\r\nHeap used/free : "); print_u64(heap_used()); uart_puts(" / "); print_u64(heap_free());
    uart_puts("\r\nPages free/tot : "); print_u64(pg.free_pages); uart_puts(" / "); print_u64(pg.total_pages);
    uart_puts("\r\nFaults user/krn: "); print_u64(fault_user_count()); uart_puts(" / "); print_u64(fault_kernel_count());
    uart_puts("\r\nLog level      : "); uart_puts(diag_level_name(diag_get_level()));
    uart_puts("\r\n");
}

static void command_log(const char* argument)
{
    if (argument != 0 && argument[0] >= '0' && argument[0] <= '3' && argument[1] == 0)
        diag_set_level(argument[0] - '0');
    else if (argument != 0 && argument[0] != 0)
    {
        uart_puts("Usage: log [0=error|1=warn|2=info|3=debug]\r\n");
        return;
    }

    uart_puts("Log level: ");
    uart_puts(diag_level_name(diag_get_level()));
    uart_puts("\r\n");
}


static int parse_u64_arg(const char* s, uint64_t* out);
static int split_words(char* argument, char** words, int capacity);

enum CommandOptionBits
{
    OPT_ALL = 1, OPT_P = 2, OPT_R = 4, OPT_F = 8, OPT_C = 16, OPT_N = 32,
    OPT_I = 64, OPT_V = 128, OPT_H = 256, OPT_S = 512, OPT_L = 1024, OPT_W = 2048
};

static int collect_operands(char** words, int count, char** operands, int capacity, int* options);
static int option_bits(const char* word);
static const char* normalize_echo_redirect_text(const char* text);

struct ManualEntry
{
    const char* name;
    const char* description;
    const char* usage;
    const char* options;
};

static const ManualEntry manual_entries[] =
{
    {"ls", "list directory contents", "ls [-l] [-a] [-R] [path]", "-l long format; -a show hidden; -R recursive"},
    {"cat", "display file contents", "cat [-n] <file>", "-n show line numbers"},
    {"grep", "search for a pattern in files", "grep [-r] [-i] [-n] [-v] [-c] <pattern> <file|dir>", "-r recursive; -i ignore case; -n show line numbers; -v invert match; -c count matches"},
    {"find", "search filesystem entries", "find [path] -name <name> | -type <f/d/l>", "-name match entry name; -type f files; -type d directories; -type l symbolic links"},
    {"chmod", "change file mode", "chmod <mode> <file>", "numeric 000-777; symbolic +r/+w/+x or -r/-w/-x; owner or root"},
    {"stat", "show file metadata", "stat [-L] <path>", "shows inode, links, type, mode, owner, size, mtime; a symlink is shown as itself with its target, -L follows it"},
    {"chown", "change file owner", "chown <uid> <gid> <path>", "root only; UID/GID are decimal numbers"},
    {"truncate", "resize a file", "truncate <path> <size>", "size 0-4194304 bytes; extended bytes are zero-filled"},
    {"cp", "copy files or directories", "cp [-r] [-f] [-P] <source> <destination>", "-r recursive; -f overwrite destination; -P copy symbolic links as links instead of following them"},
    {"mv", "move or rename files", "mv [-f] <source> <destination>", "-f overwrite destination"},
    {"rm", "remove files or directories", "rm [-r] [-f] <file|dir>...", "-r recursive; -f force/ignore missing"},
    {"mkdir", "create a directory", "mkdir [-p] <dir>", "-p create parent directories as needed"},
    {"echo", "print text", "echo <text>", "With > or >>, redirection writes a complete line"},
    {"wc", "count lines, words, and characters", "wc [-l] [-w] [-c] <file>", "-l lines; -w words; -c characters"},
    {"du", "show disk usage", "du [-h] [-s] [path]", "-h human readable; -s summary"},
    {"df", "show filesystem free/used space", "df [-h]", "-h human readable"},
    {"nano", "edit a text file", "nano <file>", "Runs the bundled text editor"},
    {"tree", "show filesystem tree", "tree [path]", "without a path the whole tree from / is printed"},
    {"mount", "show mounts / mount a device or tmpfs / remount root", "mount | mount [-r] <dev> <dir> | mount -t tmpfs none <dir> | mount -o remount,ro|rw /", "root only to change; devices: ram0 ram1 nvme0n1 sda; mount /dev needs mkfs first; /tmp is tmpfs"},
    {"mkfs", "format a device with myfs", "mkfs <dev>", "root only; dev: ram0 ram1 nvme0n1 sda; must be unmounted"},
    {"redirect", "shell output redirection", "cmd > f | cmd >> f | cmd 2> f | cmd 2>> f | cmd &> f | cmd > f 2>&1 | cmd < f", "works for every built-in; /dev/null discards; echo keeps its legacy handler"},
    {"glob", "wildcard expansion", "cmd *.txt | cmd ?.c | cmd dir/pre*", "* any chars, ? one char in unquoted words; ls/cat/wc/rm/stat/head/tail run once per match"},
    {"subst", "command substitution", "echo $(cmd) | echo `cmd`", "captures cmd output (newlines become spaces); nests up to 6 levels"},
    {"function", "shell functions", "name() { cmd; cmd; } | function name { ...; } ; return [n]", "call as: name args ($1..$9 inside); up to 16 functions of 768 chars"},
    {"seq", "print a sequence of numbers", "seq [first] last", "pipeline source: seq 5 | sort -r"},
    {"basename", "strip directory from a path", "basename <path>", "No options"},
    {"dirname", "strip last path component", "dirname <path>", "No options"},
    {"poweroff", "power the machine off (also halt, shutdown)", "poweroff", "syncs the filesystem, then PSCI SYSTEM_OFF"},
    {"reboot", "restart the machine", "reboot", "syncs the filesystem, then PSCI SYSTEM_RESET"},
    {"head", "print the first lines", "head [-n N] <file>", "also a pipeline stage; default 10 lines"},
    {"tail", "print the last lines", "tail [-n N] <file>", "also a pipeline stage; default 10 lines"},
    {"sort", "sort lines", "sort [-r] [-n] [-u] <file>", "also a pipeline stage"},
    {"uniq", "collapse adjacent duplicate lines", "uniq [-c] <file>", "also a pipeline stage"},
    {"cut", "select a delimited field", "cut -d <c> -f <n> <file>", "also a pipeline stage"},
    {"tr", "translate characters", "<cmd> | tr <set1> <set2>", "pipeline stage only"},
    {"tee", "write a pipeline to a file and pass it on", "<cmd> | tee <file>", "pipeline stage"},
    {"sleep", "wait for N seconds", "sleep <seconds>", "Ctrl-C interrupts"},
    {"rmdir", "remove an empty directory", "rmdir <dir>", "No options"},
    {"free", "show heap and page usage", "free", "No options"},
    {"which", "locate a command", "which <name>", "built-in or /bin program"},
    {"dhcp", "obtain/renew an IPv4 lease via DHCP", "dhcp [status|renew|release]", "applies address, mask, gateway and DNS; boot tries DHCP automatically; lease is renewed at 50% of its time at the prompt"},
    {"nc", "TCP connect/listen tool (IPv4 and IPv6)", "nc [-z] <host|ipv6> <port> [text] | nc -l <port>", "send text and print the reply; -z only tests the port; -l accepts one IPv4 or IPv6 connection and prints data"},
    {"udp6", "UDP over IPv6 link-local", "udp6 send <ipv6> <port> <text> | udp6 listen <port> [count]", "send waits 2.5 s for a reply; listen prints datagrams and answers 'ack6:<data>'"},
    {"telnet", "interactive TCP terminal", "telnet <host> [port]", "default port 23; Ctrl-] quits; answers telnet option negotiation with WONT/DONT"},
    {"wget", "download over HTTP or HTTPS", "wget <http[s]://host[:port]/path | host[:port] [path]> [outfile]", "GET up to 1 MiB saved to a file (default: last path part or index.html); https uses TLS 1.3 (AES-128-GCM, X25519) WITHOUT certificate verification"},
    {"httpsget", "fetch an HTTPS page (TLS 1.3)", "httpsget <host[:port]> [path] | httpsget https://host/path", "prints the first 4096 bytes of the body; the server certificate chain and host name are verified against /etc/ssl/certs.pem (+ certs add); --cacert <file> trusts one extra CA, -k/--insecure skips verification"},
    {"ping6", "ICMPv6 echo to a link-local address", "ping6 <ipv6>", "uses NDP neighbor discovery"},
    {"httpd", "HTTP file server (GET/HEAD, ranges, keep-alive, directory listings)", "httpd <port> [requests] [-d docroot] [-q]", "serves files below the document root (default /; with / and no index.html the path / is a greeting); index.html, directory listings, 301/403/404/405/416, Content-Type by extension, Range requests, HTTP/1.1 keep-alive; requests 0 = until Ctrl-C, default 1; one connection at a time; no HTTPS"},
    {"ftpd", "FTP server (passive/active, login with local accounts)", "ftpd [-p port] [-d root] [-n sessions] [-a] [-P lo-hi]", "login with a RioOS account (permissions of that user apply); USER PASS PWD CWD CDUP TYPE PASV EPSV PORT LIST NLST RETR STOR APPE DELE MKD RMD RNFR RNTO SIZE MDTM REST FEAT QUIT; -a adds a read-only anonymous login; -P lo-hi fixes the passive data ports (default 30000-31999) so QEMU hostfwd can forward them, e.g. RIOOS_FWD=\"tcp::2121-:21,tcp::30000-:30000\" with ftpd -P 30000; files up to 4 MiB; one session at a time; plain FTP (no TLS)"},
    {"usb", "xHCI USB controller / HID devices", "usb [selftest]", "lists devices on the xHCI root hub; keyboards type into the shell, mice are tracked (start QEMU with -device qemu-xhci -device usb-kbd)"},
    {"lspci", "list PCIe devices", "lspci", "bus 0 via ECAM; shows class and assigned BARs"},
    {"nvme", "NVMe disk status", "nvme [selftest]", "selftest is read-only on the root disk, read/write on others"},
    {"sata", "AHCI/SATA disk status", "sata [selftest]", "selftest is read-only on the root disk, read/write on others"},
    {"tar", "create/extract/list tar archives (user program)", "tar -c|-x|-t [-v] [-z] [-C dir] -f archive [paths] | tar czvf out.tgz dir | tar xzf in.tgz -C /dir", "ustar format, gzip with -z or a .gz/.tgz name; paths with .. are refused; -f - uses stdin/stdout; interoperates with GNU tar"},
    {"gzip", "compress files with DEFLATE (user program)", "gzip [-d -c -k -f -t -l -v -1..-9] <file>", "writes <file>.gz and removes the original unless -k; -c to stdout; -d decompress; -t test; -l list sizes"},
    {"gunzip", "decompress .gz files", "gunzip [-c -k -f -t] <file.gz>", "same as gzip -d"},
    {"zcat", "print a .gz file", "zcat <file.gz>", "same as gzip -dc; works in pipes"},
    {"diff", "compare two files or directories (user program)", "diff [-u|-U N] [-q -i -w -b -r -s] <a> <b>", "normal and unified output; -q only report; -i ignore case; -w ignore all blanks; -b ignore blank changes; -r recurse; -s report identical; exit 0 same / 1 differ / 2 error"},
    {"vi", "modal text editor (user program)", "vi [-R] [file]", "normal: h j k l arrows 0 ^ $ w b e gg G {n}G x X D dd dw d$ cc cw C yy yw p P u ^R . o O i a I A r R J ~ s S / ? n N ^F ^B, counts. ex: :w [f] :q :q! :wq :x :N :$ :d :r f :e! :s/a/b/[g] :%s/a/b/g :set nu|nonu. ZZ saves and quits. Screen 80x24."},
    {"pkg", "package manager for .rpk packages (user program)", "pkg list|info|files|install|remove|upgrade|search|verify|owner|build|update|remote|mkindex", "pkg build [-z] <dir> <out.rpk> (dir holds PKGINFO: name= version= desc= depends=a,b>=1 conflicts= provides=, plus a file tree rooted at /); pkg install <file|name> [-f] resolves dependencies from /var/cache/pkg; database /var/lib/pkg; pkg remove refuses while others depend on it; pkg verify checks size+CRC. Root needed to write system paths. Network: pkg remote add <http(s)://host/dir> [-k] [--cacert f] (list|remove), pkg update fetches <url>/INDEX (lines 'name version file.rpk sha256 | desc'), install/upgrade/search then download missing or newer packages and dependencies, each checked against the index sha256 (HTTPS certificates are verified; -k disables that); pkg mkindex <dir> writes such an INDEX. Packages up to 1 MB"},
    {"cc", "C compiler + virtual machine (user program)", "cc [-S] [-c] [-o out.rbc] [-s steps] [-D name[=val]] file.c [args] | cc -x prog.rbc [args]", "compiles a C subset to bytecode and runs it: int/long/char/unsigned/void, pointers, arrays, enum, typedef, #define (also with arguments), #ifdef, functions, recursion, all statements incl. switch, string literals, initializers, malloc/free, printf/sprintf/snprintf/scanf/puts/putchar/getchar, str*/mem*, atoi, rand. Not supported: float/double, struct/union, goto, #if expressions. Bounds-checked memory, step limit (-s). -S prints bytecode, -c only checks, -o writes an image"},
    {"help", "list commands by category", "help [category|all]", "categories: files text shell system users storage network dev libc; details of one command: man <command>"},
    {"man", "manual page of a command", "man <command>", "shows summary, usage and notes; 'help' lists all commands"},
    {"clear", "clear the screen", "clear", "sends the ANSI clear-screen sequence"},
    {"version", "kernel version and build information", "version", "prints the RioOS version"},
    {"mem", "memory overview", "mem", "total, used and free RAM; see also free, pages, stats"},
    {"hello", "hello demo (user program)", "hello", "runs /bin/hello.elf in an isolated EL0 process"},
    {"pwd", "print the current directory", "pwd", ""},
    {"cd", "change directory", "cd [dir]", "without argument goes to the home directory; .. and relative paths work across mounts"},
    {"touch", "create a file or update its time", "touch [-c] <file>", "-c: do not create a missing file"},
    {"write", "write text to a file", "write <file> <text>", "replaces the file content; use >> with echo to append"},
    {"uptime", "time since boot", "uptime", ""},
    {"sched", "scheduler information", "sched", "shows scheduler state and tick counters"},
    {"exec", "run an ELF program", "exec <file.elf> [args]", "starts an isolated EL0 process; programs in /bin also run by name"},
    {"calc", "interactive calculator", "calc [expression]", "integer arithmetic; without argument opens the interactive calculator"},
    {"elf", "inspect the built-in ELF demo", "elf", "loads and describes the ELF demo program"},
    {"syscall", "system call tester", "syscall <test>", "exercises user-space system calls from the shell"},
    {"user", "user-mode demo / password", "user demo | user [password]", "user demo runs an EL0 demo; see userdemo"},
    {"userdemo", "EL0 user-mode demonstration", "userdemo", "runs the demo process in isolated user space"},
    {"su", "switch user", "su [root|user]", "root password is 123; 'logout' returns"},
    {"logout", "leave the root shell", "logout", "returns to the normal user"},
    {"exit", "leave the shell or script", "exit [n]", "in a script ends it with status n"},
    {"level", "privilege level information", "level", "shows the current exception level and user"},
    {"mmu", "MMU and page table information", "mmu", "shows translation setup and layout validation"},
    {"disk", "VirtIO disk information", "disk", "raw disk status; see diskread, diskwrite, diskflush"},
    {"diskread", "read a raw disk sector", "diskread <sector>", "prints the sector content"},
    {"diskwrite", "write a raw disk sector", "diskwrite <sector> <text>", "root only; destroys the sector content"},
    {"diskflush", "flush disk caches", "diskflush", "writes pending data to the device"},
    {"apps", "list user programs", "apps", "lists the ELF programs in /bin"},
    {"uname", "system name", "uname", "prints kernel name, version and machine"},
    {"whoami", "current user", "whoami", ""},
    {"net", "network status", "net", "interface, address, gateway and counters"},
    {"ping", "ICMP echo", "ping <ip>", "IPv4 echo requests; see ping6 for IPv6"},
    {"selftest", "kernel self tests", "selftest", "runs the built-in kernel checks and prints a summary"},
    {"pages", "page allocator state", "pages", "total and free 4 KiB pages"},
    {"tasks", "list kernel tasks", "tasks", "scheduler tasks and states"},
    {"stats", "kernel statistics", "stats", "IRQ, fault and memory counters"},
    {"log", "kernel log level", "log [0-3]", "0 quiet .. 3 verbose; without argument shows the level"},
    {"irq", "interrupt controller", "irq [status|on|off|oneshot|selftest] [hz]", "timer interrupt control and statistics"},
    {"date", "show or set the date", "date [-u] [+format] | date -s 'YYYY-MM-DD HH:MM:SS'", "reads the RTC; -s sets it (root)"},
    {"env", "show environment variables", "env", "see export, unset"},
    {"export", "set an environment variable", "export NAME=value", "visible to $NAME expansion and to programs"},
    {"unalias", "remove aliases", "unalias <name> | unalias -a", ""},
    {"less", "page through a file", "less <file>", "space next page, enter next line, q quit; also usable after a pipe"},
    {"sh", "run a shell script", "sh <script>", "supports if/while/for/until/case, functions, $VAR, $(cmd)"},
    {"source", "run a script in the current shell", "source <script>", "variables and functions stay defined"},
    {"jobs", "list background jobs", "jobs", "see fg, bg, kill; Ctrl-Z suspends, Ctrl-C stops"},
    {"fg", "bring a job to the foreground", "fg [%n]", ""},
    {"bg", "continue a job in the background", "bg [%n]", ""},
    {"kill", "send a signal to a process or job", "kill [-9] <pid|%n>", ""},
    {"let", "integer arithmetic assignment", "let X=1+2", "stores the result in the variable"},
    {"test", "evaluate a condition", "test <expr> | [ <expr> ]", "-e -f -d -z -n, = != -eq -ne -lt -le -gt -ge; exit status 0 when true"},
    {"true", "succeed", "true", "exit status 0"},
    {"false", "fail", "false", "exit status 1"},
    {"read", "read a line into a variable", "read VAR", "reads from the terminal or from a pipe"},
    {"useradd", "create an account", "useradd <name>", "root only"},
    {"userdel", "delete an account", "userdel <name>", "root only"},
    {"passwd", "change a password", "passwd [name]", "own password, or any with root"},
    {"id", "show user and group ids", "id", ""},
    {"return", "return from a shell function", "return [n]", "sets the function exit status"},
    {"users", "list accounts", "users", "shows the accounts in /etc/passwd"},
    {"hexdump", "hex + ASCII dump of a file (user program)", "hexdump <file>", "runs /bin/hexdump.elf; uses libc fopen/fread"},
    {"libctest", "libc part 2 self-test (user program)", "libctest", "FILE stdio, sscanf, strtok_r, strcasecmp, bsearch, integer math; prints N/N passed"},
    {"fat", "FAT32 volumes on a raw device (not VFS-mounted)", "fat <dev> mkfs [label] | info | ls [path] | cat <p> | get <p> <file> | put <file> <p> | mkdir <p> | rm <p>", "root only; dev ram0 ram1 nvme0n1 sda; reads long names, new names must be 8.3; files up to 1 MiB; works on images made by other OSes"},
    {"curl", "HTTP/HTTPS client", "curl [-s] [-i|-I] [-L] [-o file] [-X METHOD] [-H 'K: V'] [-d data] <url>", "GET/POST/HEAD, custom headers, follows up to 4 redirects with -L, -i/-I show headers, -o saves the body; https uses TLS 1.3 without certificate verification"},
    {"ssh", "secure shell client (SSH-2)", "ssh [-p port] [-t|-T] [-o StrictHostKeyChecking=yes|no|accept-new] [user@]host [command]", "curve25519 key exchange, aes128-ctr + hmac-sha2-256, password login, host keys ssh-ed25519/rsa-sha2/ecdsa-nistp256 remembered in ~/.ssh_known_hosts; without a command opens a shell on a pty (~. at line start quits); with a command runs it and returns its exit status in $?; ssh -R host forgets a stored host key"},
    {"scp", "copy files over SSH", "scp [-P port] [-r] [-q] <src> <dst>", "one side is [user@]host:path (upload or download); -r copies directories; files are limited to 4 MiB; password login only"},
    {"certs", "TLS trust store (CA certificates)", "certs [list|count|add <pem>|verify <pem chain> [host]]", "the trust store is /etc/ssl/certs.pem; add (root) appends CA certificates; verify checks a PEM chain (leaf first) against the store, signatures, dates and the optional host name"},
    {"ntpdate", "set the clock from an NTP server", "ntpdate [-q] <server>", "SNTP over UDP 123; -q only queries and prints the offset; setting the RTC needs root"},
    {"fw", "IPv4 packet filter (firewall)", "fw [list] | fw add|insert <allow|deny> <in|out|both> <tcp|udp|icmp|any> [ip[/bits]|any] [port|any] | fw del <n> | fw flush | fw default <in|out> <allow|deny>", "ordered rules, first match wins, then the per-direction default policy; inbound rules apply to new connections only (SYN, new UDP flow, echo request); DHCP and DNS are exempt; changing rules needs root"},
    {"fsck", "check the filesystem", "fsck [-r] [mountpoint]", "reports inodes/sectors, structure and bitmap; -r (root) repairs a leaked/missing bitmap"},
    {"quota", "per-user disk quota", "quota [list|<uid>] | quota set <uid> <KiB>", "root sets; stored in /etc/quota; writes beyond the limit fail; root is exempt"},
    {"awk", "pattern scanning and processing", "awk [-F sep] 'prog' [file]  |  cmd | awk 'prog'", "BEGIN/END, /re/ and expr patterns, print/printf, if/while/for, arrays, $N NF NR FS; length substr index split sub gsub tolower toupper int sprintf; integer arithmetic only"},
    {"sed", "stream editor", "sed [-n] [-e script] 's/re/repl/[g]' [file]  |  cmd | sed ...", "commands s d p q y =, addresses N $ /re/ N,M and !; regex: . * + ? ^ $ [set] \\d \\w \\s a|b; & in replacement"},
    {"xargs", "build and run commands from input", "cmd | xargs [-n N] [-I{}] command [args]", "default command echo; output is passed on"},
    {"rev", "reverse each line", "cmd | rev", "pipeline stage / rev file"},
    {"tac", "print lines in reverse order", "cmd | tac", "pipeline stage / tac file"},
    {"nl", "number lines", "cmd | nl", "pipeline stage / nl file"},
    {"alias", "define command aliases", "alias | alias name=command | unalias name|-a", "only the first word is replaced; up to 16 aliases"},
    {"printf", "formatted output", "printf <format> [args...]", "%s %d %i %u %x %X %o %c %% with width/-/0 flags; \\n \\t escapes; format reused for extra args"},
    {"expr", "evaluate an expression", "expr A op B | expr length S | expr substr S POS LEN", "ops + - * / % = != < > <= >= (integers)"},
    {"more", "page through a file", "more <file>  |  less <file>", "space next page, enter next line, q quit"},
    {"umount", "unmount a filesystem", "umount <mountpoint>", "root only; / and busy mounts (cwd inside, nested mounts) refuse"},
    {"sync", "flush filesystem to disk", "sync", "No options"},
    {"ln", "create a symbolic link", "ln -s <target> <link>", "relative targets resolve from the link's directory"},
    {"readlink", "print a symlink target", "readlink [-f] <link>", "-f resolves every symlink and prints the canonical absolute path"},
    {"realpath", "canonical absolute path", "realpath <path>", "resolves every symlink, . and .. (also across mounts); fails on loops (ELOOP) or missing targets"},
    {"jtest", "journal crash-recovery test", "jtest [path]", "root only; commits to journal then skips apply; reset to see replay"},
    {"ps", "show processes", "ps", "No options"},
    {"history", "show command history", "history [10|-c|-w|-r]", "-c clear; N last N entries; -w save to /home/user/.history (loaded at boot); -r reload; Arrow Up/Down recall"},
    {"threads", "list cooperative kernel threads", "threads", "Shows state, priority, run counts, and context switches"},
    {"smp", "show CPU topology / run jobs on secondary CPUs", "smp [test]", "lists online CPUs; test offloads a computation to every secondary CPU and verifies it"},
    {"fbinfo", "show framebuffer backend status", "fbinfo", "Reports the framebuffer terminal state (QEMU window runner)"},
    {"thread-demo", "exercise voluntary context switching", "thread-demo", "Uses yield only; no preemption (tick keeps running)"},
    {"unset", "remove an environment variable", "unset NAME", "NAME must follow shell identifier rules"},
    {"fds", "list open file descriptors", "fds", "Shows standard streams and currently open descriptors"},
    {"ifconfig", "show the configured network interface", "ifconfig", "Displays static IPv4, gateway, DNS and VirtIO-Net MAC"},
    {"nslookup", "resolve a DNS A record", "nslookup <hostname>", "Uses the configured DNS server; requires working UDP and ARP"},
    {"httpget", "fetch a plain-HTTP URL over TCP", "httpget <hostname> [path]", "Outbound IPv4 TCP + HTTP/1.0; HTTPS/TLS is not implemented"},
    {"dnsserver", "set the DNS server address", "dnsserver <ipv4>", "Root only; default for QEMU user networking is 10.0.2.3"}
};

static int is_builtin_name_impl(const char* name)
{
    for (unsigned i = 0; i < sizeof(manual_entries) / sizeof(manual_entries[0]); ++i)
        if (str_equal(name, manual_entries[i].name)) return 1;
    static const char* extra[] = {"cd", "pwd", "exit", "help", "man", "clear", "su", "logout", "whoami", "export", "jobs", "fg", "bg", "kill", "wait", "sh", "source", "let", "test", "true", "false", "mkdir", "touch", "cat", "echo", "ls"};
    for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i)
        if (str_equal(name, extra[i])) return 1;
    return 0;
}

static void command_man(const char* argument);
static void put_date_2(uint64_t value)
{
    uart_putc((char)('0' + ((value / 10ULL) % 10ULL)));
    uart_putc((char)('0' + (value % 10ULL)));
}

static void put_date_4(uint64_t value)
{
    uart_putc((char)('0' + ((value / 1000ULL) % 10ULL)));
    uart_putc((char)('0' + ((value / 100ULL) % 10ULL)));
    uart_putc((char)('0' + ((value / 10ULL) % 10ULL)));
    uart_putc((char)('0' + (value % 10ULL)));
}

static int ci_equal_char(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    return a == b;
}

static int ci_contains(const char* text, const char* pattern)
{
    if (!text || !pattern) return 0;
    if (!pattern[0]) return 1;
    for (int i = 0; text[i]; ++i)
    {
        int j = 0;
        while (text[i + j] && pattern[j] && ci_equal_char(text[i + j], pattern[j])) ++j;
        if (!pattern[j]) return 1;
    }
    return 0;
}

static void print_human_bytes(uint64_t bytes)
{
    if (bytes >= 1024ULL * 1024ULL * 1024ULL)
    {
        print_u64(bytes / (1024ULL * 1024ULL * 1024ULL)); uart_puts("G"); return;
    }
    if (bytes >= 1024ULL * 1024ULL)
    {
        print_u64(bytes / (1024ULL * 1024ULL)); uart_puts("M"); return;
    }
    if (bytes >= 1024ULL)
    {
        print_u64(bytes / 1024ULL); uart_puts("K"); return;
    }
    print_u64(bytes); uart_puts("B");
}

static uint64_t parse_oct(const char* s, int* ok)
{
    uint64_t v = 0;
    int digits = 0;
    if (!s || !s[0]) { *ok = 0; return 0; }
    while (*s)
    {
        if (*s < '0' || *s > '7') { *ok = 0; return 0; }
        v = (v << 3) | (uint64_t)(*s - '0');
        ++digits;
        ++s;
    }
    *ok = digits > 0 && v <= 0777U;
    return v;
}

static int chmod_symbolic(const char* spec, uint32_t old_mode, uint32_t* out)
{
    if (!spec || !spec[0] || !out) return 0;
    char op = 0;
    int i = 0;
    if (spec[i] == '+' || spec[i] == '-') op = spec[i++];
    if (!op) return 0;
    uint32_t bits = 0;
    for (; spec[i]; ++i)
    {
        switch (spec[i])
        {
            case 'r': bits |= 0444U; break;
            case 'w': bits |= 0222U; break;
            case 'x': bits |= 0111U; break;
            default: return 0;
        }
    }
    *out = op == '+' ? (old_mode | bits) : (old_mode & ~bits);
    return 1;
}

static uint64_t days_from_civil(int y, int m, int d)
{
    int yy = y - (m <= 2 ? 1 : 0);
    int era = yy >= 0 ? yy / 400 : (yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned mp = (unsigned)(m + (m > 2 ? -3 : 9));
    unsigned doy = (153U * mp + 2U) / 5U + (unsigned)d - 1U;
    unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (uint64_t)(era * 146097 + (int)doe - 719468);
}

static int parse_fixed4(const char* p)
{
    int v = 0;
    for (int i = 0; i < 4; ++i) { if (p[i] < '0' || p[i] > '9') return -1; v = v * 10 + (p[i] - '0'); }
    return v;
}

static int parse_fixed2(const char* p)
{
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

static int date_string_to_epoch(const char* s, uint64_t* out)
{
    if (!s || !out) return 0;
    int len = str_len(s);
    if (len < 19) return 0;
    int y = parse_fixed4(s);
    int mo = parse_fixed2(s + 5);
    int d = parse_fixed2(s + 8);
    int h = parse_fixed2(s + 11);
    int mi = parse_fixed2(s + 14);
    int sec = parse_fixed2(s + 17);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 59) return 0;
    if (s[4] != '-' || s[7] != '-' || (s[10] != ' ' && s[10] != 'T') || s[13] != ':' || s[16] != ':') return 0;
    *out = days_from_civil(y, mo, d) * 86400ULL + (uint64_t)h * 3600ULL + (uint64_t)mi * 60ULL + (uint64_t)sec;
    return 1;
}

static void print_date_format(uint64_t seconds, const char* format)
{
    uint64_t days = seconds / 86400ULL;
    uint64_t rem = seconds % 86400ULL;
    uint64_t hour = rem / 3600ULL; rem %= 3600ULL;
    uint64_t minute = rem / 60ULL;
    uint64_t second = rem % 60ULL;
    int64_t z = (int64_t)days + 719468LL;
    int64_t era = (z >= 0 ? z : z - 146096LL) / 146097LL;
    uint64_t doe = (uint64_t)(z - era * 146097LL);
    uint64_t yoe = (doe - doe / 1460ULL + doe / 36524ULL - doe / 146096ULL) / 365ULL;
    int64_t y = era * 400LL + (int64_t)yoe;
    uint64_t doy = doe - (365ULL * yoe + yoe / 4ULL - yoe / 100ULL);
    uint64_t mp = (5ULL * doy + 2ULL) / 153ULL;
    uint64_t day = doy - (153ULL * mp + 2ULL) / 5ULL + 1ULL;
    uint64_t month = mp < 10ULL ? mp + 3ULL : mp - 9ULL;
    y += month <= 2ULL ? 1LL : 0LL;
    for (int i = 0; format[i]; ++i)
    {
        if (format[i] != '%') { uart_putc(format[i]); continue; }
        char c = format[++i];
        if (c == 'Y') { put_date_4((uint64_t)y); }
        else if (c == 'm') { put_date_2(month); }
        else if (c == 'd') { put_date_2(day); }
        else if (c == 'H') { put_date_2(hour); }
        else if (c == 'M') { put_date_2(minute); }
        else if (c == 'S') { put_date_2(second); }
        else if (c == '%') uart_putc('%');
        else { uart_putc('%'); uart_putc(c); }
    }
    uart_puts("\r\n");
}

static void command_date(char* argument)
{
    char* words[8];
    int count = split_words(argument, words, 8);
    int utc = 0;
    const char* format = "%Y-%m-%d %H:%M:%S";
    const char* set_string = 0;
    for (int i = 0; i < count; ++i)
    {
        if (str_equal(words[i], "-u")) utc = 1;
        else if (words[i][0] == '+' && words[i][1]) format = words[i] + 1;
        else if (str_equal(words[i], "-s") && i + 2 < count)
        {
            static char joined[64];
            int p = 0;
            ++i;
            while (i < count && p < (int)sizeof(joined) - 1)
            {
                if (p) joined[p++] = ' ';
                for (int j = 0; words[i][j] && p < (int)sizeof(joined) - 1; ++j) joined[p++] = words[i][j];
                ++i;
            }
            joined[p] = '\0';
            set_string = joined;
            --i;
        }
        else { uart_puts("date: unknown option\r\n"); return; }
    }
    if (set_string)
    {
        uint64_t epoch = 0;
        if (!date_string_to_epoch(set_string, &epoch) || rtc_set_epoch(epoch) != 0)
        {
            uart_puts("date: unable to set RTC (use YYYY-MM-DD HH:MM:SS)\r\n");
            return;
        }
        uart_puts("RTC updated.\r\n");
        return;
    }
    (void)utc; /* QEMU virt RTC is kept as UTC; no timezone database is assumed. */
    print_date_format(rtc_get_epoch(), format);
}

static void command_history(char* argument)
{
    if (!argument || !argument[0]) { history_print(0); return; }
    if (str_equal(argument, "-c")) { history_clear(); return; }
    uint64_t n = 0;
    if (!parse_u64_arg(argument, &n) || n > 32ULL) { uart_puts("history: usage: history [-c|count]\r\n"); return; }
    history_print((int)n);
}

static int find_substr_line(const char* line, const char* pattern, int ignore_case)
{
    if (!ignore_case)
    {
        for (int i = 0; line[i]; ++i)
        {
            int j = 0; while (line[i+j] && pattern[j] && line[i+j] == pattern[j]) ++j;
            if (!pattern[j]) return 1;
        }
        return pattern[0] == '\0';
    }
    return ci_contains(line, pattern);
}

static int str_has_slash(const char* s) { for (; *s; ++s) if (*s == '/') return 1; return 0; }

static void grep_print_file(const char* display_path, int options, const char* pattern)
{
    static uint8_t buffer[262145];
    uint64_t size = 0;
    if (fs_read_file(display_path, vfs_cwd(), buffer, sizeof(buffer) - 1ULL, &size) != 0)
    {
        uart_puts("grep: file not found or too large: ");
        uart_puts(display_path);
        uart_puts("\r\n");
        return;
    }

    buffer[size] = 0;
    uint64_t matches = 0;
    uint64_t line_no = 1;
    uint64_t start = 0;
    while (start <= size)
    {
        if (start == size && (size == 0 || buffer[size - 1] == '\n')) break;   /* no phantom empty last line */
        uint64_t end = start;
        while (end < size && buffer[end] != '\n') ++end;
        char* line = (char*)(buffer + start);
        char saved = 0;
        if (end < size) { saved = (char)buffer[end]; buffer[end] = 0; }
        int matched = find_substr_line(line, pattern, (options & OPT_I) != 0);
        if (options & OPT_V) matched = !matched;
        if (matched)
        {
            ++matches;
            if (!(options & OPT_C))
            {
                uart_puts(display_path);
                uart_putc(':');
                if (options & OPT_N) { print_u64(line_no); uart_putc(':'); }
                uart_puts(line);
                uart_puts("\r\n");
            }
        }
        if (end < size) buffer[end] = (uint8_t)saved;
        if (end == size) break;
        start = end + 1;
        ++line_no;
    }

    if (options & OPT_C)
    {
        uart_puts(display_path);
        uart_putc(':');
        print_u64(matches);
        uart_puts("\r\n");
    }
}

static void grep_walk_files(int id, const char* display_path, int options, const char* pattern)
{
    if (id < 0) return;
    if (fs_get_type(id) == FS_FILE)
    {
        grep_print_file(display_path, options, pattern);
        return;
    }
    if (fs_get_type(id) != FS_DIR) return;
    if (fs_check_access(id, FS_ACCESS_READ | FS_ACCESS_EXEC) != 0) return;

    int count = fs_get_child_count(id);
    for (int i = 0; i < count; ++i)
    {
        int child = fs_get_child(id, i);
        if (child < 0) continue;
        char child_path[256];
        if (str_equal(display_path, "/"))
        {
            child_path[0] = '/';
            str_copy(child_path + 1, fs_get_name(child), sizeof(child_path) - 1);
        }
        else
        {
            str_copy(child_path, display_path, sizeof(child_path));
            int n = str_len(child_path);
            if (n < (int)sizeof(child_path) - 1) child_path[n++] = '/';
            str_copy(child_path + n, fs_get_name(child), sizeof(child_path) - n);
        }
        if (fs_get_type(child) == FS_DIR)
            grep_walk_files(child, child_path, options, pattern);
        else
            grep_print_file(child_path, options, pattern);
    }
}

static void command_grep(char* argument)
{
    char* words[10]; char* operands[4];
    int count = split_words(argument, words, 10);
    int options = 0;
    int n = collect_operands(words, count, operands, 3, &options);
    if (n != 2 || (options & ~(OPT_R|OPT_N|OPT_I|OPT_V|OPT_C)))
    {
        uart_puts("grep: usage: grep [-r] [-i] [-n] [-v] [-c] <pattern> <file|dir>\r\n");
        return;
    }

    int id = fs_resolve(operands[1], vfs_cwd());
    if (id < 0)
    {
        uart_puts("grep: file or directory not found\r\n");
        return;
    }

    if (fs_get_type(id) == FS_DIR)
    {
        if (!(options & OPT_R))
        {
            uart_puts("grep: ");
            uart_puts(operands[1]);
            uart_puts(": is a directory (use -r)\r\n");
            return;
        }

        char display_path[256];
        if (str_equal(operands[1], "."))
            vfs_get_path(display_path, sizeof(display_path));
        else
            str_copy(display_path, operands[1], sizeof(display_path));
        grep_walk_files(id, display_path, options, operands[0]);
        return;
    }

    grep_print_file(operands[1], options, operands[0]);
}

static void find_walk(int id, const char* display_path, const char* name, int type_filter)
{
    if (id < 0) return;
    if ((name == 0 || str_equal(fs_get_name(id), name)) &&
        (type_filter == 0 || (type_filter == 'f' && fs_get_type(id) == FS_FILE) || (type_filter == 'd' && fs_get_type(id) == FS_DIR) || (type_filter == 'l' && fs_get_type(id) == FS_SYMLINK)))
    {
        uart_puts(display_path); uart_puts("\r\n");
    }
    if (fs_get_type(id) != FS_DIR) return;
    if (fs_check_access(id, FS_ACCESS_READ | FS_ACCESS_EXEC) != 0) return;
    int count = fs_get_child_count(id);
    for (int i = 0; i < count; ++i)
    {
        int child = fs_get_child(id, i);
        if (child < 0) continue;
        char child_path[256];
        if (str_equal(display_path, "/"))
        {
            child_path[0] = '/';
            str_copy(child_path + 1, fs_get_name(child), sizeof(child_path) - 1);
        }
        else
        {
            str_copy(child_path, display_path, sizeof(child_path));
            int n = str_len(child_path);
            if (n < (int)sizeof(child_path)-1) child_path[n++] = '/';
            str_copy(child_path + n, fs_get_name(child), sizeof(child_path)-n);
        }
        find_walk(child, child_path, name, type_filter);
    }
}

static void command_find(char* argument)
{
    char* words[8]; int count = split_words(argument, words, 8);
    const char* path = "."; const char* name = 0; int type_filter = 0;
    int i = 0;
    if (count > 0 && words[0][0] != '-') { path = words[0]; i = 1; }
    while (i < count)
    {
        if (str_equal(words[i], "-name") && i + 1 < count) name = words[++i];
        else if (str_equal(words[i], "-type") && i + 1 < count) { type_filter = words[++i][0]; if (type_filter != 'f' && type_filter != 'd' && type_filter != 'l') { uart_puts("find: type must be f, d or l\r\n"); return; } }
        else { uart_puts("find: usage: find [path] -name <name> | -type <f/d>\r\n"); return; }
        ++i;
    }
    if (!name && !type_filter) { uart_puts("find: missing expression\r\n"); return; }
    int root = fs_resolve(path, vfs_cwd());
    if (root < 0) { uart_puts("find: path not found\r\n"); return; }
    char start[256];
    if (str_equal(path, ".")) vfs_get_path(start, sizeof(start));
    else str_copy(start, path, sizeof(start));
    find_walk(root, start, name, type_filter);
}

static int du_sum(int id, uint64_t* out_total)
{
    if (id < 0 || out_total == 0) return -2;
    if (fs_get_type(id) == FS_FILE)
    {
        *out_total = fs_file_size(id);
        return 0;
    }
    if (fs_get_type(id) != FS_DIR) return -2;
    if (fs_check_access(id, FS_ACCESS_READ | FS_ACCESS_EXEC) != 0) return -13;

    uint64_t sum = 0;
    int n = fs_get_child_count(id);
    for (int i = 0; i < n; ++i)
    {
        int child = fs_get_child(id, i);
        if (child < 0) continue;
        uint64_t child_size = 0;
        int rc = du_sum(child, &child_size);
        if (rc != 0) return rc;
        if (child_size > 0xFFFFFFFFFFFFFFFFULL - sum) return -27;
        sum += child_size;
    }
    *out_total = sum;
    return 0;
}

static void command_du(char* argument)
{
    char* words[6]; char* operands[3]; int count = split_words(argument, words, 6);
    int options = 0; int n = collect_operands(words, count, operands, 2, &options);
    if (n < 0 || (options & ~(OPT_H|OPT_S)) || n > 1) { uart_puts("du: usage: du [-h] [-s] [path]\r\n"); return; }
    const char* path = n == 1 ? operands[0] : ".";
    int id = fs_resolve(path, vfs_cwd());
    if (id < 0) { uart_puts("du: path not found\r\n"); return; }
    uint64_t total = 0;
    int sum_rc = du_sum(id, &total);
    if (sum_rc != 0)
    {
        uart_puts(sum_rc == -13 ? "du: permission denied\r\n" : "du: unable to read metadata\r\n");
        return;
    }
    if (options & OPT_H) print_human_bytes(total); else print_u64(total);
    uart_puts("  "); uart_puts(path); uart_puts("\r\n");
    if (!(options & OPT_S) && fs_get_type(id) == FS_DIR)
    {
        int c = fs_get_child_count(id);
        for (int i = 0; i < c; ++i)
        {
            int child = fs_get_child(id, i); if (child < 0) continue;
            uint64_t v = 0;
            sum_rc = du_sum(child, &v);
            if (sum_rc != 0)
            {
                uart_puts(sum_rc == -13 ? "du: permission denied\r\n" : "du: unable to read metadata\r\n");
                return;
            }
            if (options & OPT_H) print_human_bytes(v); else print_u64(v);
            uart_puts("  "); uart_puts(fs_get_name(child)); uart_puts("\r\n");
        }
    }
}

static void command_df(char* argument)
{
    char* words[4]; int count = split_words(argument, words, 4); int options = 0;
    char* operands[2]; int n = collect_operands(words, count, operands, 1, &options);
    if (n < 0 || (options & ~OPT_H) || n > 0) { uart_puts("df: usage: df [-h]\r\n"); return; }
    uart_puts("Filesystem  Total  Used  Free\r\n");
    int cnt = fs_mount_count();
    for (int i = 0; i < cnt; ++i)
    {
        FsMountInfo mi;
        if (fs_mount_info(i, &mi) != 0) continue;
        uint64_t total = mi.total_sectors * 512ULL;
        uint64_t used = mi.used_sectors * 512ULL;
        uint64_t freeb = total > used ? total - used : 0;
        if (options & OPT_H) { print_human_bytes(total); uart_putc(' '); print_human_bytes(used); uart_putc(' '); print_human_bytes(freeb); }
        else { print_u64(total); uart_putc(' '); print_u64(used); uart_putc(' '); print_u64(freeb); }
        uart_puts("  "); uart_puts(mi.dir); uart_puts("\r\n");
    }
}

static void command_wc(char* argument)
{
    char* words[6]; char* operands[3]; int count = split_words(argument, words, 6);
    int options = 0; int n = collect_operands(words, count, operands, 1, &options);
    if (n != 1 || (options & ~(OPT_L|OPT_W|OPT_C))) { uart_puts("wc: usage: wc [-lwc] <file>\r\n"); return; }
    static uint8_t buffer[262145]; uint64_t size = 0;
    if (fs_read_file(operands[0], vfs_cwd(), buffer, sizeof(buffer)-1ULL, &size) != 0) { uart_puts("wc: file not found or too large\r\n"); return; }
    uint64_t lines = 0, words_count = 0, chars = size; int in_word = 0;
    for (uint64_t i = 0; i < size; ++i)
    {
        if (buffer[i] == '\n') ++lines;
        int space = buffer[i] == ' ' || buffer[i] == '\t' || buffer[i] == '\n' || buffer[i] == '\r';
        if (space) in_word = 0; else if (!in_word) { in_word = 1; ++words_count; }
    }

    /* Treat a non-empty final line without a trailing newline as one line.
     * This matches RioOS shell expectations and avoids wc -l reporting 0 for
     * a non-empty file created by older versions of the shell. */
    if (size > 0 && buffer[size - 1] != (uint8_t)'\n')
        ++lines;
    if (!(options & (OPT_L|OPT_W|OPT_C))) options = OPT_L|OPT_W|OPT_C;
    int first = 1;
    if (options & OPT_L) { print_u64(lines); first = 0; }
    if (options & OPT_W) { if (!first) uart_putc(' '); print_u64(words_count); first = 0; }
    if (options & OPT_C) { if (!first) uart_putc(' '); print_u64(chars); first = 0; }
    uart_puts("  "); uart_puts(operands[0]); uart_puts("\r\n");
}

static void command_chmod(char* argument)
{
    char* words[4];
    int count = split_words(argument, words, 4);
    if (count != 2) { uart_puts("chmod: usage: chmod <mode> <file>\r\n"); return; }
    int id = fs_resolve(words[1], vfs_cwd());
    if (id < 0) { uart_puts("chmod: file not found\r\n"); return; }
    uint32_t mode = 0; int ok = 0;
    mode = (uint32_t)parse_oct(words[0], &ok);
    if (!ok) ok = chmod_symbolic(words[0], fs_get_mode(id), &mode);
    if (!ok) { uart_puts("chmod: invalid mode\r\n"); return; }
    if (fs_set_mode(id, mode) != 0) { uart_puts("chmod: failed\r\n"); return; }
    uart_puts("mode: ");
    /* Print canonical octal mode. */
    char d[3]; d[0]=(char)('0'+((mode>>6)&7)); d[1]=(char)('0'+((mode>>3)&7)); d[2]=(char)('0'+(mode&7));
    uart_putc(d[0]); uart_putc(d[1]); uart_putc(d[2]); uart_puts("\r\n");
}

static void command_stat(char* argument)
{
    char* words[4];
    int count = split_words(argument, words, 4);
    int follow = 0; int pi = 0;
    if (count >= 1 && (str_equal(words[0], "-L") || str_equal(words[0], "-l"))) { follow = 1; pi = 1; }
    if (count - pi != 1) { uart_puts("stat: usage: stat [-L] <path>   (-L follows a final symbolic link)\r\n"); return; }
    const char* path = words[pi];
    FsStat st;
    int rc = follow ? fs_stat_path(path, vfs_cwd(), &st) : fs_lstat_path(path, vfs_cwd(), &st);
    if (rc != 0) { uart_puts(rc == -13 ? "stat: permission denied\r\n" : rc == -40 ? "stat: too many levels of symbolic links\r\n" : "stat: path not found\r\n"); return; }
    uart_puts("  File: "); uart_puts(path);
    if (st.type == FS_SYMLINK) { char t[260]; int tl = fs_readlink(path, vfs_cwd(), t, sizeof(t)); if (tl > 0) { uart_puts(" -> "); uart_puts(t); } }
    uart_puts("\r\n");
    uart_puts("  Inode: "); print_u64(st.inode);
    { int lc = fs_link_count(follow ? fs_resolve(path, vfs_cwd()) : fs_resolve_nofollow(path, vfs_cwd())); uart_puts("  Links: "); print_u64((uint64_t)(lc > 0 ? lc : 1)); }
    uart_puts("  Type: "); uart_puts(st.type == FS_DIR ? "directory\r\n" : st.type == FS_SYMLINK ? "symbolic link\r\n" : "regular file\r\n");
    uart_puts("  Mode: ");
    uart_putc((char)('0' + ((st.mode >> 6) & 7U)));
    uart_putc((char)('0' + ((st.mode >> 3) & 7U)));
    uart_putc((char)('0' + (st.mode & 7U)));
    uart_puts(" (owner/group/other)\r\n  UID: "); print_u64(st.uid);
    uart_puts("\r\n  GID: "); print_u64(st.gid);
    uart_puts("\r\n  Size: "); print_u64(st.size); uart_puts(" bytes\r\n  Sectors: "); print_u64(st.sector_count);
    uart_puts("\r\n  Modified (Unix epoch): "); print_u64(st.mtime); uart_puts("\r\n");
}

static void command_chown(char* argument)
{
    if (!security_is_root()) { uart_puts("chown: permission denied (root required)\r\n"); return; }
    char* words[4];
    int count = split_words(argument, words, 4);
    if (count != 3) { uart_puts("chown: usage: chown <uid> <gid> <path>\r\n"); return; }
    uint64_t uid = 0, gid = 0;
    if (!parse_u64_arg(words[0], &uid) || !parse_u64_arg(words[1], &gid) ||
        uid > 0x7FFFFFFFULL || gid > 0x7FFFFFFFULL)
    { uart_puts("chown: invalid UID/GID\r\n"); return; }
    int id = fs_resolve(words[2], vfs_cwd());
    if (id < 0) { uart_puts("chown: path not found or permission denied\r\n"); return; }
    int rc = fs_set_owner(id, (uint32_t)uid, (uint32_t)gid);
    if (rc != 0) { uart_puts("chown: update failed\r\n"); return; }
    uart_puts("owner updated\r\n");
}

static void command_truncate(char* argument)
{
    char* words[3];
    int count = split_words(argument, words, 3);
    uint64_t size = 0;
    if (count != 2 || !parse_u64_arg(count == 2 ? words[1] : "", &size))
    { uart_puts("truncate: usage: truncate <path> <size>\r\n"); return; }
    int rc = fs_truncate(words[0], vfs_cwd(), size);
    if (rc == 0) return;
    if (rc == -13) uart_puts("truncate: permission denied\r\n");
    else if (rc == -27) uart_puts("truncate: file too large (max 4194304 bytes)\r\n");
    else uart_puts("truncate: failed (path missing, not a file, or storage error)\r\n");
}

static void command_env(const char* argument)
{
    if (argument && argument[0]) { uart_puts("env: no arguments supported\r\n"); return; }
    env_print();
}

static void command_export(const char* argument)
{
    if (!argument || !argument[0] || env_set(argument) != 0)
    { uart_puts("export: usage: export VAR=val\r\n"); return; }
}

static int read_entire_file(const char* path, uint8_t* buf, uint64_t cap, uint64_t* size)
{
    return fs_read_file(path, vfs_cwd(), buf, cap, size);
}

static void command_echo_redirect(const char* text, const char* path, int append)
{
    static uint8_t old[262145];
    uint64_t size = 0;
    const uint64_t cap = sizeof(old) - 1ULL;
    const uint64_t text_len = (uint64_t)str_len(text);

    if (append)
    {
        if (read_entire_file(path, old, cap, &size) != 0)
            size = 0;

        /*
         * Echo redirection writes one complete line.  When appending to an
         * older file that was not newline-terminated, insert the missing
         * separator first so consecutive echo commands remain separate lines.
         */
        uint64_t separator = (size > 0 && old[size - 1] != (uint8_t)'\n') ? 1ULL : 0ULL;
        if (size + separator + text_len + 1ULL > cap)
        {
            uart_puts("redirect: file too large\r\n");
            return;
        }

        if (separator)
            old[size++] = (uint8_t)'\n';
        for (uint64_t i = 0; i < text_len; ++i)
            old[size++] = (uint8_t)text[i];
        old[size++] = (uint8_t)'\n';

        if (fs_write_data(path, vfs_cwd(), old, size) != 0)
            uart_puts("redirect: write failed\r\n");
        return;
    }

    /* Overwrite is also line-oriented for echo redirection, but plain
     * `write <file> <text>` is intentionally left unchanged. */
    if (text_len + 1ULL > cap)
    {
        uart_puts("redirect: file too large\r\n");
        return;
    }

    for (uint64_t i = 0; i < text_len; ++i)
        old[i] = (uint8_t)text[i];
    old[text_len] = (uint8_t)'\n';

    if (fs_write_data(path, vfs_cwd(), old, text_len + 1ULL) != 0)
        uart_puts("redirect: write failed\r\n");
}

static int handle_shell_operators(const char* line)
{
    if (!line) return 0;
    char shq = 0;
    for (int i = 0; line[i]; ++i)
    {
        if (shq) { if (line[i] == shq) shq = 0; continue; }
        if (line[i] == '"' || line[i] == '\'') { shq = line[i]; continue; }
        if (line[i] == '|')
        {
            char left[256], right[256];
            int l = i; while (l > 0 && line[l-1] == ' ') --l;
            int r = i + 1; while (line[r] == ' ') ++r;
            int llen = l; if (llen >= (int)sizeof(left)) llen = sizeof(left)-1;
            for (int k = 0; k < llen; ++k) left[k] = line[k]; left[llen] = 0;
            str_copy(right, line + r, sizeof(right));
            /* The reliable pipe primitives are deliberately in-memory: cat/echo -> grep. */
            static uint8_t pipebuf[262145]; uint64_t psz = 0;
            if (left[0]=='c'&&left[1]=='a'&&left[2]=='t'&&(left[3]==' '||left[3]=='\0'))
            {
                char* a = left + 3; while (*a==' ') ++a;
                if (!*a || read_entire_file(a, pipebuf, sizeof(pipebuf)-1ULL, &psz) != 0) { uart_puts("pipe: cat source failed\r\n"); return 1; }
            }
            else if (left[0]=='e'&&left[1]=='c'&&left[2]=='h'&&left[3]=='o'&&left[4]==' ')
            {
                const char* echo_text = normalize_echo_redirect_text(left + 5);
                str_copy((char*)pipebuf, echo_text, sizeof(pipebuf));
                psz = (uint64_t)str_len((char*)pipebuf);
            }
            else { uart_puts("pipe: supported source commands are cat and echo\r\n"); return 1; }
            char* rw[8]; int rc = split_words(right, rw, 8);
            if (rc >= 2 && str_equal(rw[0], "grep"))
            {
                int ropt = 0; const char* pattern = 0;
                for (int k=1;k<rc;++k)
                {
                    if (rw[k][0]=='-') { int b=option_bits(rw[k]); if (b<0) { uart_puts("pipe: invalid grep option\r\n"); return 1; } ropt|=b; }
                    else if (!pattern) pattern=rw[k]; else { uart_puts("pipe: grep accepts one pattern\r\n"); return 1; }
                }
                if (!pattern || (ropt & ~(OPT_N|OPT_I|OPT_V|OPT_C))) { uart_puts("pipe: usage: cat file | grep [options] pattern\r\n"); return 1; }
                pipebuf[psz] = 0; uint64_t line_no=1,matches=0,start=0;
                while (start <= psz)
                {
                    uint64_t end=start; while(end<psz && pipebuf[end]!='\n') ++end;
                    char saved=0; if(end<psz){saved=(char)pipebuf[end]; pipebuf[end]=0;}
                    int m=find_substr_line((char*)pipebuf+start,pattern,(ropt&OPT_I)!=0); if(ropt&OPT_V)m=!m;
                    if(m){++matches;if(!(ropt&OPT_C)){if(ropt&OPT_N){print_u64(line_no);uart_putc(':');}uart_puts((char*)pipebuf+start);uart_puts("\r\n");}}
                    if(end<psz)pipebuf[end]=(uint8_t)saved; if(end==psz)break; start=end+1;++line_no;
                }
                if(ropt&OPT_C){print_u64(matches);uart_puts("\r\n");}
                return 1;
            }
            if (rc >= 1 && str_equal(rw[0], "wc"))
            {
                int wopt = 0;
                for (int k = 1; k < rc; ++k)
                {
                    if (rw[k][0] != '-')
                    {
                        uart_puts("pipe: wc does not take a filename on the right side\r\n");
                        return 1;
                    }
                    int bits = option_bits(rw[k]);
                    if (bits < 0)
                    {
                        uart_puts("pipe: invalid wc option\r\n");
                        return 1;
                    }
                    wopt |= bits;
                }
                if (wopt & ~(OPT_L | OPT_W | OPT_C))
                {
                    uart_puts("pipe: usage: cat file | wc [-lwc]\r\n");
                    return 1;
                }
                if (!(wopt & (OPT_L | OPT_W | OPT_C)))
                    wopt = OPT_L | OPT_W | OPT_C;
                uint64_t lines = 0, words_count = 0, chars = psz;
                int in_word = 0;
                for (uint64_t k = 0; k < psz; ++k)
                {
                    if (pipebuf[k] == (uint8_t)'\n') ++lines;
                    int is_space = pipebuf[k] == (uint8_t)' ' ||
                                   pipebuf[k] == (uint8_t)'\t' ||
                                   pipebuf[k] == (uint8_t)'\n' ||
                                   pipebuf[k] == (uint8_t)'\r';
                    if (is_space) in_word = 0;
                    else if (!in_word) { in_word = 1; ++words_count; }
                }
                if (psz > 0 && pipebuf[psz - 1] != (uint8_t)'\n') ++lines;
                int first = 1;
                if (wopt & OPT_L) { print_u64(lines); first = 0; }
                if (wopt & OPT_W) { if (!first) uart_putc(' '); print_u64(words_count); first = 0; }
                if (wopt & OPT_C) { if (!first) uart_putc(' '); print_u64(chars); }
                uart_puts("\r\n");
                return 1;
            }
            if (rc == 2 && str_equal(rw[0], "read"))
            {
                char val[256]; int n = 0;
                for (uint64_t k = 0; k < psz && n < 200 && pipebuf[k] != (uint8_t)'\n' && pipebuf[k] != (uint8_t)'\r'; ++k) val[n++] = (char)pipebuf[k];
                val[n] = 0;
                char asg[300]; int a = 0;
                for (const char* q = rw[1]; *q && a < 60; ++q) asg[a++] = *q;
                asg[a++] = '='; for (int k = 0; k < n; ++k) asg[a++] = val[k]; asg[a] = 0;
                script_set_status(env_set(asg) == 0 ? 0 : 1);
                return 1;
            }
            uart_puts("pipe: right side must be grep, wc or read\r\n"); return 1;
        }
        if (line[i] == '>' || line[i] == '<')
        {
            int append = line[i] == '>' && line[i+1] == '>';
            int op = i;
            int target_start = i + (append ? 2 : 1);
            while (line[target_start] == ' ') ++target_start;
            char target[192]; int t=0;
            while (line[target_start+t] && line[target_start+t]!=' ' && line[target_start+t]!='\t' && t < (int)sizeof(target)-1) { target[t]=line[target_start+t]; ++t; }
            target[t]=0;
            if (!target[0]) { uart_puts("shell: redirection target missing\r\n"); return 1; }
            char command[256]; int c=op; while(c>0 && line[c-1]==' ')--c; if(c>=255)c=255; for(int k=0;k<c;++k)command[k]=line[k]; command[c]=0;
            if (line[i] == '>' && command[0]=='e'&&command[1]=='c'&&command[2]=='h'&&command[3]=='o'&&(command[4]==' '||command[4]=='\0'))
            {
                const char* raw_text = command[4]==' ' ? command+5 : "";
                const char* text = normalize_echo_redirect_text(raw_text);
                command_echo_redirect(text, target, append); return 1;
            }
            if (line[i] == '<')
            {
                char combined[256]; int n=str_len(command); if(n+(int)str_len(target)+2>=255){uart_puts("shell: command too long\r\n");return 1;} str_copy(combined,command,sizeof(combined)); combined[n++]=' '; str_copy(combined+n,target,sizeof(combined)-n); execute_command(combined); return 1;
            }
            uart_puts("redirect: only echo supports >/>> in this release\r\n"); return 1;
        }
    }
    return 0;
}

static void thread_demo_worker(void* argument)
{
    char label = ((uint64_t)argument == 1ULL) ? 'A' : 'B';
    for (uint64_t step = 1; step <= 3; ++step)
    {
        uart_puts("[thread ");
        uart_putc(label);
        uart_puts("] step ");
        print_u64(step);
        uart_puts("\r\n");
        thread_yield();
    }
}

static void command_threads()
{
    thread_list();
}

static void command_smp_status(const char* argument)
{
    if (argument && str_equal(argument, "test"))
    {
        uint32_t n = smp_online_cpu_count();
        if (n < 2U) { uart_puts("smp test: no secondary CPU online (start QEMU with -smp 4)\r\n"); return; }
        int ok = 1;
        for (uint32_t cpu = 1; cpu < n; ++cpu)
        {
            uint64_t lo = (uint64_t)cpu * 1000ULL, hi = lo + 1000ULL, got = 0, want = 0;
            for (uint64_t i = lo; i < hi; ++i) want += i * i;
            int rc = smp_run_on(cpu, lo, hi, &got);
            uart_puts("CPU "); print_u64(cpu); uart_puts(": sum[");
            print_u64(lo); uart_puts(","); print_u64(hi); uart_puts(") = "); print_u64(got);
            uart_puts(rc == 0 && got == want ? "  OK\r\n" : "  FAIL\r\n");
            if (rc != 0 || got != want) ok = 0;
        }
        uart_puts(ok ? "smp test: PASS\r\n" : "smp test: FAIL\r\n");
        return;
    }
    uart_puts("SMP / CPU topology\r\n------------------\r\nBoot CPU affinity-0: ");
    print_u64((uint64_t)smp_cpu_id());
    uart_puts("\r\nOnline CPUs       : ");
    print_u64((uint64_t)smp_online_cpu_count());
    uart_puts("\r\nSMP scheduling    : ");
    uart_puts(smp_is_enabled() ? "enabled\r\n" : "scheduler on CPU0 only; secondary CPUs run offloaded jobs ('smp test')\r\n");
    uart_puts("Spinlocks         : atomic acquire/release primitives available\r\n");
    for (uint32_t cpu = 1; cpu < smp_online_cpu_count(); ++cpu)
    {
        uart_puts("CPU "); print_u64(cpu); uart_puts("            : online, MPIDR 0x"); print_u64(smp_cpu_mpidr(cpu));
        uart_puts(", wakeups "); print_u64(smp_cpu_heartbeat(cpu)); uart_puts("\r\n");
    }
}

static void command_fbinfo()
{
    uart_puts("Framebuffer diagnostics\r\n------------------------\r\nBackend : ");
    uart_puts(fb_has_display() ? "display attached (QEMU window)\r\n" : "none (serial console only)\r\n");
    uart_puts("Surface : ");
    uart_puts(fb_is_ready() ? "initialized\r\n" : "not initialized\r\n");
    uart_puts("Geometry: "); print_u64((uint64_t)fb_get_width());
    uart_putc('x'); print_u64((uint64_t)fb_get_height());
    uart_puts(" pixels, 32 bpp\r\nTerminal: ");
    if (console_active())
    {
        print_u64((uint64_t)console_columns()); uart_putc('x'); print_u64((uint64_t)console_rows());
        uart_puts(" cells, scrollback ");
        print_u64((uint64_t)console_scrollback_lines());
        uart_puts(" lines (mouse wheel / Shift+PageUp / Shift+PageDown scroll)\r\n");
    }
    else uart_puts("inactive; UART is the only console\r\n");
}

static void command_thread_demo()
{
    if (thread_active_count() != 1)
    {
        uart_puts("thread-demo: finish/avoid other threads first\r\n");
        return;
    }

    uart_puts("Running cooperative context-switch demo (timer tick keeps running; no preemption).\r\n");
    int a = thread_create("demo-A", thread_demo_worker, (void*)1ULL, 1);
    int b = thread_create("demo-B", thread_demo_worker, (void*)2ULL, 1);
    if (a < 0 || b < 0)
    {
        uart_puts("thread-demo: could not create both demo threads\r\n");
        while (thread_active_count() > 1) thread_yield();
        return;
    }

    while (thread_active_count() > 1)
        thread_yield();

    uart_puts("thread-demo: PASS (cooperative switches completed)\r\n");
}

static void command_unset(const char* argument)
{
    if (!argument || !argument[0])
    {
        uart_puts("unset: usage: unset NAME\r\n");
        return;
    }
    for (int i = 0; argument[i]; ++i)
        if (argument[i] == ' ' || argument[i] == '\t')
        {
            uart_puts("unset: usage: unset NAME\r\n");
            return;
        }
    int rc = env_unset(argument);
    if (rc == -2) uart_puts("unset: variable not found\r\n");
    else if (rc != 0) uart_puts("unset: invalid variable name\r\n");
}

struct HelpSection { const char* key; const char* title; const char* body; };
static const HelpSection help_sections[] = {
{"files", "FILES & DIRECTORIES",
 "  ls [-l -a -R] [dir]   list            pwd / cd <dir>       where am I / go\r\n"
 "  mkdir [-p] <d>        make directory  rmdir <d>            remove empty dir\r\n"
 "  touch [-c] <f>        create/update   write <f> <text>     write a file\r\n"
 "  cat [-n] <f>          read file       rm [-r -f] <f>       delete\r\n"
 "  cp [-r -f] <a> <b>    copy            mv [-f] <a> <b>      move (also across mounts)\r\n"
 "  ln [-s] <a> <b>       hard link / symlink (-s)   readlink [-f] <l>   realpath <p>\r\n"
 "  stat <p>              inode, mode, owner, link count\r\n"
 "  chmod <mode> <p>  chown <uid> <gid> <p>  truncate <p> <size>\r\n"
 "  tree  find [path] -name <n> | -type <f|d>  du [-h -s] [p]  df [-h]  sync\r\n"
 "  nano <f>              text editor     more|less <f>        pager (space, enter, q)\r\n"
 "  hexdump <f>           hex + ASCII dump (user program)\r\n"},
{"text", "TEXT PROCESSING (work in pipes: cmd | filter)",
 "  grep [-n -i -v -c] <pat> [f]   search         wc [-l -w -c] [f]   count\r\n"
 "  head [-n N]  tail [-n N]  sort [-r -n -u]  uniq [-c]  cut -d <c> -f <n>  tr [-d] a b  tee <f>\r\n"
 "  awk [-F sep] 'prog' [f]        patterns, print/printf, arrays, integer math\r\n"
 "  sed [-n] 's/re/x/g' [f]        stream editor: s d p q y =, addresses, regex\r\n"
 "  xargs [-n N] [-I{}] cmd        build command lines from stdin\r\n"
 "  rev  tac  nl [f]               reverse chars / reverse lines / number lines\r\n"
 "  seq [a] b   basename <p>  dirname <p>   echo <text>   printf <fmt> [args]   expr A op B\r\n"},
{"shell", "SHELL & SCRIPTING",
 "  > >> < 2> &> 2>&1  |  &  &&  ||  ;     redirection, pipes, background, lists  (man redirect)\r\n"
 "  *.txt  ?.c   $(cmd)  `cmd`  $VAR  $?    wildcards, substitution (man glob, man subst)\r\n"
 "  name() { cmd; }  return n               functions (man function)\r\n"
 "  if / while / for ... do ... done        let X=1+2   test / [ ... ]   read VAR   true  false\r\n"
 "  env  export V=x  unset V                environment\r\n"
 "  alias [n=cmd]  unalias n|-a             command aliases\r\n"
 "  history [n|-c|-w|-r]                    list / clear / save to ~/.history / reload\r\n"
 "  jobs  fg [%n]  bg [%n]  kill [-9] <pid|%n>  wait    job control (Ctrl-C stops, Ctrl-Z suspends)\r\n"
 "  sh <script>  source <script>  sleep <s>  which <cmd>  man <cmd>  clear  exit\r\n"},
{"system", "SYSTEM & PROCESSES",
 "  version  uname  uptime  date [-u] [+fmt] | date -s 'YYYY-MM-DD HH:MM:SS'\r\n"
 "  mem  free  pages  stats  ps  tasks  sched  threads  thread-demo  fds  smp  level  mmu\r\n"
 "  exec <elf>  apps  elf  hello  calc [expr]  syscall <t>  user demo  userdemo\r\n"
 "  log [0-3]  irq [status|on|off|oneshot|selftest] [hz]  fbinfo  selftest  jtest <file>\r\n"
 "  poweroff | halt | shutdown   reboot\r\n"},
{"users", "USERS & SECURITY",
 "  whoami  id  users   su [root|user]  logout  user [password]   (root password: 123)\r\n"
 "  useradd <n>  userdel <n>  passwd [n]       account management (root)\r\n"
 "  chmod / chown                              file permissions\r\n"
 "  quota [list|<uid>] | quota set <uid> <KiB> per-user disk quota (root sets)\r\n"
 "  fw ...                                     packet filter, see 'help network'\r\n"},
{"storage", "DISKS & FILESYSTEMS",
 "  disk  diskread <sec>  diskwrite <sec> <text>  diskflush     raw VirtIO disk access\r\n"
 "  lspci  nvme  sata                          PCI, NVMe and AHCI/SATA controllers\r\n"
 "  mount                                      list mounts;  mount [-r] <dev> <dir>  (ram0 ram1 nvme0n1 sda)\r\n"
 "  mount -t tmpfs none <dir>  mount -o remount,ro|rw   mkfs <dev>   umount <dir>\r\n"
 "  df [-h]  du  fsck [-r] [mnt]               usage and consistency check (-r repairs)\r\n"
 "  ln <a> <b>                                 hard link (shared data, link count in stat)\r\n"
 "  fat <dev> mkfs|info|ls|cat|get|put|mkdir|rm ...   FAT32 volumes with long names (man fat)\r\n"
 "  usb [selftest]                             xHCI USB, HID keyboard and mouse\r\n"},
{"network", "NETWORK",
 "  net  ifconfig  dhcp [status|renew|release]  dnsserver <ip>  nslookup <host>\r\n"
 "  ping <ip>  ping6 <ipv6>                    ICMP echo over IPv4 / IPv6\r\n"
 "  httpget <host> [path]   wget <url> [file]   httpsget <host> [path]   HTTP/HTTPS (TLS 1.3, certificates verified; -k skips, --cacert f)\r\n"
 "  certs [list|add <pem>|verify <pem>]        CA trust store (/etc/ssl/certs.pem)\r\n"
 "  ssh [-p n] [-t] [-o StrictHostKeyChecking=yes|no] user@host [cmd]   ssh -R host   SSH-2 client\r\n"
 "  scp [-r] [-P n] <src> <dst>                copy files over SSH (user@host:path)\r\n"
 "  ftpd [-p port] [-n sessions] [-a] [-d dir] [-P lo-hi]  FTP server (PASV/EPSV/PORT, per-user permissions)\r\n"
 "  curl [-s -i -I -L -o f -X M -H h -d data] <url>   full HTTP client\r\n"
 "  nc [-z] <host> <port> [text]  nc -l <port>   TCP client/server (IPv4 + IPv6)\r\n"
 "  udp6 send|listen ...   telnet <host> [port]   httpd [port] [n] [-d dir] [-q]   web server (HTTP/1.1, Range)\r\n"
 "  ntpdate [-q] <server>                      set the clock over NTP\r\n"
 "  fw [list]  fw add|insert <allow|deny> <in|out|both> <tcp|udp|icmp|any> [ip[/n]] [port]\r\n"
 "  fw del <n>  fw flush  fw default <in|out> <allow|deny>      firewall (root changes rules)\r\n"},
{"dev", "ARCHIVES, DEVELOPMENT & EDITING (user programs)",
 "  tar -c|-x|-t [-v] [-z] [-C dir] -f a.tar [paths]   archives (ustar; -z gzip; also: tar czvf / xzf)\r\n"
 "  gzip [-d -c -k -f -t -l -v -1..-9] <f>  gunzip <f>  zcat <f>   gzip files (DEFLATE)\r\n"
 "  diff [-u|-U N] [-q -i -w -b -r -s] <a> <b>   compare files/dirs (exit 0 same, 1 differ, 2 error)\r\n"
 "  vi [-R] [file]    modal editor: i a o O x dd yy p u ^R . / ? n N w b e gg G :w :q :wq :q! :s :set nu\r\n"
 "  pkg list|info|files|install|remove|upgrade|search|verify|owner|build|update   package manager (man pkg)\r\n"
 "  cc [-S -c -o out.rbc -s N -D x=v] file.c [args]   C compiler + VM;  cc -x out.rbc   run image (man cc)\r\n"},
{"libc", "USER PROGRAMS & LIBC",
 "  hexdump <f>   libctest   hello  spin  upper  lcount  pipetest  ktest  dltest (see 'apps')\r\n"
 "  libctest checks printf/stdio/scanf/string/math(float)/locale/time of the user libc (54 tests)\r\n"
 "  any /bin/<name>.elf runs by typing its name; exec <file> runs an ELF path\r\n"},
};

static void command_help(const char* argument)
{
    const int count = (int)(sizeof(help_sections) / sizeof(help_sections[0]));
    char topic[24]; topic[0] = 0;
    if (argument) { int i = 0; while (*argument == ' ') ++argument; while (argument[i] && argument[i] != ' ' && i < 23) { topic[i] = argument[i]; ++i; } topic[i] = 0; }
    if (topic[0] && !str_equal(topic, "all"))
    {
        for (int i = 0; i < count; ++i)
            if (str_equal(topic, help_sections[i].key))
            {
                uart_puts(help_sections[i].title); uart_puts("\r\n"); uart_puts(help_sections[i].body);
                uart_puts("  (details of any command: man <command>)\r\n");
                return;
            }
        uart_puts("help: no category '"); uart_puts(topic); uart_puts("'. Try: help\r\n");
        return;
    }
    if (!topic[0])
    {
        uart_puts("RioOS help\r\n==========\r\n"
                  "  help <category>   show one category;  help all   show everything;  man <command>   manual page\r\n\r\n"
                  "  files    ls cd mkdir cat cp mv rm ln stat chmod find du df tree nano more hexdump ...\r\n"
                  "  text     grep awk sed xargs sort uniq cut tr head tail wc rev tac nl printf expr ...\r\n"
                  "  shell    redirects, pipes, jobs, functions, alias, history, env, scripts\r\n"
                  "  system   version uname uptime date mem ps tasks sched irq poweroff reboot ...\r\n"
                  "  users    su useradd passwd id quota permissions\r\n"
                  "  storage  disk mount mkfs umount fsck fat nvme sata lspci usb\r\n"
                  "  network  dhcp ping wget curl nc telnet httpd ntpdate fw ping6 udp6 ...\r\n"
                  "  dev      tar gzip gunzip zcat diff vi pkg cc\r\n"
                  "  libc     user programs (hexdump, libctest, ...)\r\n");
        return;
    }
    uart_puts("RioOS commands\r\n==============\r\n");
    for (int i = 0; i < count; ++i)
    { uart_puts("\r\n["); uart_puts(help_sections[i].title); uart_puts("]\r\n"); uart_puts(help_sections[i].body); }
}

static void command_mem()
{
    uart_puts(
        "Memory information\r\n"
        "------------------\r\n"
        "RAM : "
    );

    print_u64(
        memory_total() /
        (1024ULL * 1024ULL)
    );

    uart_puts(
        " MB\r\nHeap total : "
    );

    print_u64(
        heap_total() / 1024ULL
    );

    uart_puts(
        " KB\r\nHeap used  : "
    );

    print_u64(
        heap_used()
    );

    uart_puts(
        " bytes\r\nHeap free  : "
    );

    print_u64(
        heap_free() / 1024ULL
    );

    uart_puts(
        " KB\r\n"
    );
}

static void command_echo(
    const char* text
)
{
    if (text == 0)
    {
        uart_puts("\r\n");
        return;
    }

    /* quote removal: 'a b' and "a b" lose their quotes (an unmatched quote is kept literally) */
    char buf[1024]; int w = 0; char q = 0;
    for (int i = 0; text[i] && w < (int)sizeof(buf) - 1; ++i)
    {
        char c = text[i];
        if (q) { if (c == q) { q = 0; continue; } buf[w++] = c; continue; }
        if (c == '"' || c == '\'')
        {
            int j = i + 1; while (text[j] && text[j] != c) ++j;
            if (text[j] == c) { q = c; continue; }
        }
        buf[w++] = c;
    }
    buf[w] = 0;
    uart_puts(buf);
    uart_puts("\r\n");
}

static void normalize_shell_whitespace(char* line)
{
    if (!line) return;
    int r = 0, w = 0;
    int in_single = 0;
    int in_double = 0;
    int pending_space = 0;

    while (line[r])
    {
        char c = line[r++];
        if ((c == ' ' || c == '\t') && !in_single && !in_double)
        {
            pending_space = 1;
            continue;
        }

        if (pending_space && w > 0)
            line[w++] = ' ';
        pending_space = 0;

        if (c == '\'' && !in_double) in_single = !in_single;
        else if (c == '"' && !in_single) in_double = !in_double;
        line[w++] = c;
    }

    while (w > 0 && line[w - 1] == ' ') --w;
    line[w] = '\0';
}

static const char* normalize_echo_redirect_text(const char* text)
{
    static char clean[256];
    if (!text) { clean[0] = '\0'; return clean; }

    int len = str_len(text);
    if (len >= 2 && ((text[0] == '"' && text[len - 1] == '"') ||
                     (text[0] == '\'' && text[len - 1] == '\'')))
    {
        int n = len - 2;
        if (n > (int)sizeof(clean) - 1) n = sizeof(clean) - 1;
        for (int i = 0; i < n; ++i) clean[i] = text[i + 1];
        clean[n] = '\0';
        return clean;
    }

    str_copy(clean, text, sizeof(clean));
    return clean;
}

static int split_words(char* argument, char** words, int capacity)
{
    if (argument == 0 || words == 0 || capacity <= 0)
        return 0;

    int count = 0;
    char* p = argument;
    while (*p && count < capacity)
    {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        words[count++] = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (*p) *p++ = '\0';
    }
    return count;
}


static int option_bits(const char* word)
{
    if (!word || word[0] != '-' || word[1] == '\0') return 0;
    if (word[1] == '-' && word[2] == '\0') return 0;
    int bits = 0;
    for (int i = 1; word[i]; ++i)
    {
        if (word[i] == 'a') bits |= OPT_ALL;
        else if (word[i] == 'p' || word[i] == 'P') bits |= OPT_P;
        else if (word[i] == 'r') bits |= OPT_R;
        else if (word[i] == 'f') bits |= OPT_F;
        else if (word[i] == 'c') bits |= OPT_C;
        else if (word[i] == 'n') bits |= OPT_N;
        else if (word[i] == 'i') bits |= OPT_I;
        else if (word[i] == 'v') bits |= OPT_V;
        else if (word[i] == 'h') bits |= OPT_H;
        else if (word[i] == 's') bits |= OPT_S;
        else if (word[i] == 'l') bits |= OPT_L;
        else if (word[i] == 'w') bits |= OPT_W;
        else return -1;
    }
    return bits;
}

static int collect_operands(char** words, int count, char** operands, int capacity, int* options)
{
    int n = 0;
    *options = 0;
    int end_options = 0;
    for (int i = 0; i < count; ++i)
    {
        if (!end_options && str_equal(words[i], "--"))
        {
            end_options = 1;
            continue;
        }
        if (!end_options && words[i][0] == '-')
        {
            int bits = option_bits(words[i]);
            if (bits < 0) return -1;
            *options |= bits;
        }
        else
        {
            if (n >= capacity) return -2;
            operands[n++] = words[i];
        }
    }
    return n;
}

static void command_mkdir(char* argument)
{
    char* words[4]; char* operands[2];
    int count = split_words(argument, words, 4);
    int options = 0;
    int n = collect_operands(words, count, operands, 1, &options);
    if (n == -1 || (options & ~(OPT_P))) { uart_puts("mkdir: invalid option\r\n"); return; }
    if (n != 1) { uart_puts("mkdir: missing operand\r\n"); return; }

    int result = (options & OPT_P) ? vfs_mkdir_p(operands[0]) : vfs_mkdir(operands[0]);
    if (result == -2) uart_puts("mkdir: cannot create directory: file exists\r\n");
    else if (result < 0) uart_puts("mkdir: cannot create directory\r\n");
}

static void command_touch(char* argument)
{
    char* words[4]; char* operands[2];
    int count = split_words(argument, words, 4);
    int options = 0;
    int n = collect_operands(words, count, operands, 1, &options);
    if (n == -1 || (options & ~(OPT_C))) { uart_puts("touch: invalid option\r\n"); return; }
    if (n != 1) { uart_puts("touch: missing operand\r\n"); return; }

    int existing = fs_resolve(operands[0], vfs_cwd());
    if (existing < 0 && (options & OPT_C))
        return;
    int result = vfs_touch(operands[0]);
    if (result == -2) return; /* Existing regular file: fs_touch updates mtime. */
    if (result < 0) uart_puts("touch: cannot create file\r\n");
}

static void command_cd(const char* arg)
{
    int result = vfs_cd(arg);
    if (result == -1) uart_puts("cd: no such directory\r\n");
    else if (result == -2) uart_puts("cd: not a directory\r\n");
}

static void command_cat(char* argument)
{
    char* words[4]; char* operands[2];
    int count = split_words(argument, words, 4);
    int options = 0;
    int n = collect_operands(words, count, operands, 1, &options);
    if (n == -1 || (options & ~OPT_N)) { uart_puts("cat: invalid option\r\n"); return; }
    if (n != 1) { uart_puts("cat: missing operand\r\n"); return; }

    int result = (options & OPT_N) ? vfs_cat_numbered(operands[0]) : vfs_cat(operands[0]);
    if (result == -1) uart_puts("cat: file not found\r\n");
    else if (result == -2) uart_puts("cat: is a directory\r\n");
    else if (result < 0) uart_puts("cat: read error\r\n");
    else if (!uart_at_line_start()) uart_puts("\r\n");   /* a file that ends with a newline needs none */
}

static void command_write(
    char* argument
)
{
    if (argument == 0 ||
        argument[0] == '\0')
    {
        uart_puts(
            "write: usage: write <file> <text>\r\n"
        );

        return;
    }

    char filename[256];

    int i = 0;

    while (
        argument[i] != '\0' &&
        argument[i] != ' ' &&
        i < 255
    )
    {
        filename[i] =
            argument[i];

        i++;
    }

    filename[i] =
        '\0';

    while (
        argument[i] == ' '
    )
    {
        i++;
    }

    if (argument[i] == '\0')
    {
        uart_puts(
            "write: missing text\r\n"
        );

        return;
    }

    int result =
        vfs_write(
            filename,
            argument + i
        );

    if (result == 0)
    {
        uart_puts(
            "File written.\r\n"
        );
    }
    else if (result == -2)
    {
        uart_puts(
            "write: not a file\r\n"
        );
    }
    else
    {
        uart_puts(
            "write: failed\r\n"
        );
    }
}

#define CP_MAX_SIZE 4194304ULL
static uint8_t cp_buffer[CP_MAX_SIZE];

static int path_parent(const char* path, char* out, int cap)
{
    if (!path || !out || cap <= 0) return -1;
    int len = str_len(path);
    while (len > 1 && path[len - 1] == '/') --len;
    int slash = -1;
    for (int i = 0; i < len; ++i) if (path[i] == '/') slash = i;
    if (slash < 0) { str_copy(out, ".", cap); return 0; }
    if (slash == 0) { str_copy(out, "/", cap); return 0; }
    if (slash >= cap) return -1;
    for (int i = 0; i < slash; ++i) out[i] = path[i];
    out[slash] = '\0';
    return 0;
}

static int directory_contains_path(int ancestor, const char* path, int cwd)
{
    int id = fs_resolve(path, cwd);
    if (id >= 0)
    {
        for (int hops = 0; id >= 0 && hops < 64; ++hops, id = fs_get_parent(id))
            if (id == ancestor) return 1;
        return 0;
    }

    char parent_path[256];
    if (path_parent(path, parent_path, sizeof(parent_path)) != 0) return 0;
    id = fs_resolve(parent_path, cwd);
    for (int hops = 0; id >= 0 && hops < 64; ++hops, id = fs_get_parent(id))
        if (id == ancestor) return 1;
    return 0;
}

static int build_child_path(const char* dir, const char* name, char* out, int cap)
{
    if (!dir || !name || !out || cap <= 0) return -1;
    int n = str_len(dir);
    if (n == 0) return -1;
    if (str_equal(dir, "/"))
    {
        if (1 + str_len(name) >= cap) return -1;
        out[0] = '/';
        str_copy(out + 1, name, cap - 1);
        return 0;
    }
    if (n + 1 + str_len(name) >= cap) return -1;
    str_copy(out, dir, cap);
    if (out[n - 1] != '/') out[n++] = '/';
    str_copy(out + n, name, cap - n);
    return 0;
}

static int g_two_opts = 0;   /* option bits seen by the last parse_two_operands call */
static int parse_two_operands(char* argument, int allowed_options, int* force, int* recursive,
                              char** from, char** to)
{
    char* words[6]; char* operands[3];
    int count = split_words(argument, words, 6);
    int options = 0;
    int n = collect_operands(words, count, operands, 2, &options);
    if (n < 0 || (options & ~allowed_options)) return -1;
    g_two_opts = options;
    *force = (options & OPT_F) != 0;
    *recursive = (options & OPT_R) != 0;
    if (n != 2) return 0;
    *from = operands[0]; *to = operands[1];
    return 1;
}

static int copy_tree_node(int src, const char* target, int cwd, int recursive, int force)
{
    int type = fs_get_type(src);
    if (type == FS_FILE)
    {
        /* Source paths are resolved by the caller before this helper. */
        return -1;
    }
    if (type != FS_DIR) return -1;
    if (fs_check_access(src, FS_ACCESS_READ | FS_ACCESS_EXEC) != 0) return -13;
    if (!recursive) return -2;

    int existing = fs_resolve(target, cwd);
    if (existing >= 0 && existing != src && fs_get_type(existing) != FS_DIR)
    {
        if (!force || fs_rm_recursive(target, cwd) != 0) return -3;
        existing = -1;
    }
    if (existing < 0)
    {
        existing = fs_mkdir(target, cwd);
        if (existing < 0 && existing != -2) return -4;
    }
    if (fs_get_type(existing) != FS_DIR) return -5;

    int count = fs_get_child_count(src);
    for (int i = 0; i < count; ++i)
    {
        int child = fs_get_child(src, i);
        if (child < 0) continue;
        char child_target[256];
        if (build_child_path(target, fs_get_name(child), child_target, sizeof(child_target)) != 0)
            return -6;
        if (fs_get_type(child) == FS_SYMLINK && (g_two_opts & OPT_P))
        {
            /* cp -P: copy the link itself, not what it points to */
            char lp[256], tgt[260];
            vfs_path_of(child, lp, sizeof(lp));
            int tl = fs_readlink(lp, 0, tgt, sizeof(tgt) - 1);
            if (tl <= 0) return -9;
            tgt[tl] = 0;
            if (fs_resolve_nofollow(child_target, cwd) >= 0) { if (!force) return -7; (void)fs_rm(child_target, cwd); }
            if (fs_symlink(tgt, child_target, cwd) < 0) return -10;
        }
        else if (fs_get_type(child) == FS_DIR)
        {
            int rc = copy_tree_node(child, child_target, cwd, 1, force);
            if (rc != 0) return rc;
        }
        else
        {
            int old = fs_resolve(child_target, cwd);
            if (old >= 0 && !force) return -7;
            if (old >= 0 && fs_get_type(old) == FS_DIR) return -8;
            uint64_t size = 0;
            if (fs_read_file(fs_get_name(child), src, cp_buffer, CP_MAX_SIZE, &size) != 0)
                return -9;
            if (fs_write_data(child_target, cwd, cp_buffer, size) != 0)
                return -10;
            int new_id = fs_resolve(child_target, cwd);
            if (new_id >= 0) (void)fs_set_mode(new_id, fs_get_mode(child));
        }
    }
    (void)fs_set_mode(existing, fs_get_mode(src));
    return 0;
}

static void command_cp(char* argument)
{
    int force = 0, recursive = 0; char* from = 0; char* to = 0;
    int parsed = parse_two_operands(argument, OPT_R | OPT_F | OPT_P, &force, &recursive, &from, &to);
    if (parsed != 1)
    {
        uart_puts("cp: usage: cp [-r] [-f] [-P] <source> <destination>\r\n");
        return;
    }

    int cwd = vfs_cwd();
    if (g_two_opts & OPT_P)
    {
        FsStat ls;
        if (fs_lstat_path(from, cwd, &ls) == 0 && ls.type == FS_SYMLINK)
        {
            /* cp -P <link> <dst>: duplicate the symbolic link itself */
            char tgt[260]; int tl = fs_readlink(from, cwd, tgt, sizeof(tgt) - 1);
            if (tl <= 0) { uart_puts("cp: cannot read link\r\n"); return; }
            tgt[tl] = 0;
            char lt[256]; int dd = fs_resolve(to, cwd);
            if (dd >= 0 && fs_get_type(dd) == FS_DIR) { const char* b = from; for (const char* q = from; *q; ++q) if (*q == '/' && q[1]) b = q + 1; if (build_child_path(to, b, lt, sizeof(lt)) != 0) { uart_puts("cp: path too long\r\n"); return; } }
            else str_copy(lt, to, sizeof(lt));
            if (fs_resolve_nofollow(lt, cwd) >= 0) { if (!force) { uart_puts("cp: destination exists (use -f to overwrite)\r\n"); return; } (void)fs_rm(lt, cwd); }
            if (fs_symlink(tgt, lt, cwd) < 0) { uart_puts("cp: cannot create symbolic link\r\n"); return; }
            uart_puts("Copied symbolic link.\r\n");
            return;
        }
    }
    int src = fs_resolve(from, cwd);
    if (src < 0) { uart_puts("cp: source not found\r\n"); return; }

    int dst = fs_resolve(to, cwd);
    char target[256];
    if (dst >= 0 && fs_get_type(dst) == FS_DIR)
    {
        if (build_child_path(to, fs_get_name(src), target, sizeof(target)) != 0)
        { uart_puts("cp: path too long\r\n"); return; }
    }
    else
    {
        str_copy(target, to, sizeof(target));
    }

    int target_id = fs_resolve(target, cwd);
    if (fs_get_type(src) == FS_FILE)
    {
        uint64_t size = 0;
        if (target_id >= 0 && fs_get_type(target_id) == FS_DIR)
        { uart_puts("cp: destination is a directory\r\n"); return; }
        if (target_id >= 0 && !force)
        { uart_puts("cp: destination exists (use -f to overwrite)\r\n"); return; }
        if (fs_read_file(from, cwd, cp_buffer, CP_MAX_SIZE, &size) != 0)
        { uart_puts("cp: read failed (file too large)\r\n"); return; }
        if (fs_write_data(target, cwd, cp_buffer, size) != 0)
        { uart_puts("cp: write failed\r\n"); return; }
        target_id = fs_resolve(target, cwd);
        if (target_id >= 0) (void)fs_set_mode(target_id, fs_get_mode(src));
        uart_puts("Copied "); print_u64(size); uart_puts(" bytes.\r\n");
        return;
    }

    /* A directory copied into one of its descendants would recurse forever. */
    if (!recursive) { uart_puts("cp: -r is required to copy directories\r\n"); return; }
    if (directory_contains_path(src, target, cwd))
    {
        uart_puts("cp: cannot copy a directory into itself\r\n");
        return;
    }
    int rc = copy_tree_node(src, target, cwd, 1, force);
    if (rc != 0) { uart_puts("cp: recursive copy failed\r\n"); return; }
    uart_puts("Copied directory.\r\n");
}

static void command_mv(char* argument)
{
    int force = 0, recursive = 0; char* from = 0; char* to = 0;
    int parsed = parse_two_operands(argument, OPT_F, &force, &recursive, &from, &to);
    (void)recursive;
    if (parsed != 1)
    {
        uart_puts("mv: usage: mv [-f] <source> <destination>\r\n");
        return;
    }

    int cwd = vfs_cwd();
    int rc = fs_rename(from, to, cwd);
    if (rc == 0) { uart_puts("Moved.\r\n"); return; }
    if (rc == -2 && force)
    {
        int dst = fs_resolve(to, cwd);
        char target[256];
        if (dst >= 0 && fs_get_type(dst) == FS_DIR)
        {
            if (build_child_path(to, fs_get_name(fs_resolve(from, cwd)), target, sizeof(target)) != 0)
            { uart_puts("mv: path too long\r\n"); return; }
        }
        else str_copy(target, to, sizeof(target));
        int existing = fs_resolve(target, cwd);
        if (existing >= 0 && fs_get_type(existing) == FS_DIR)
        {
            uart_puts("mv: cannot replace a directory\r\n");
            return;
        }
        if (existing >= 0 && fs_rm(target, cwd) != 0)
        { uart_puts("mv: cannot remove destination\r\n"); return; }
        rc = fs_rename(from, to, cwd);
        if (rc == 0) { uart_puts("Moved.\r\n"); return; }
    }
    if (rc == -18)
    {
        /* different filesystems: copy then remove */
        int src = fs_resolve(from, cwd);
        if (src < 0) { uart_puts("mv: source not found\r\n"); return; }
        int dst = fs_resolve(to, cwd);
        char target[256];
        if (dst >= 0 && fs_get_type(dst) == FS_DIR)
        { if (build_child_path(to, fs_get_name(src), target, sizeof(target)) != 0) { uart_puts("mv: path too long\r\n"); return; } }
        else str_copy(target, to, sizeof(target));
        int tid = fs_resolve(target, cwd);
        if (tid >= 0 && !force) { uart_puts("mv: destination already exists\r\n"); return; }
        if (fs_get_type(src) == FS_FILE)
        {
            uint64_t size = 0;
            if (tid >= 0 && fs_get_type(tid) == FS_DIR) { uart_puts("mv: cannot replace a directory\r\n"); return; }
            if (fs_read_file(from, cwd, cp_buffer, CP_MAX_SIZE, &size) != 0) { uart_puts("mv: read failed (file too large)\r\n"); return; }
            if (fs_write_data(target, cwd, cp_buffer, size) != 0) { uart_puts("mv: write failed (destination full?)\r\n"); return; }
            tid = fs_resolve(target, cwd);
            if (tid >= 0) (void)fs_set_mode(tid, fs_get_mode(src));
            if (fs_rm(from, cwd) != 0) { uart_puts("mv: copied but cannot remove source\r\n"); return; }
        }
        else
        {
            if (directory_contains_path(src, target, cwd)) { uart_puts("mv: cannot move a directory into itself\r\n"); return; }
            if (copy_tree_node(src, target, cwd, 1, force) != 0) { uart_puts("mv: copy failed\r\n"); return; }
            if (fs_rm_recursive(from, cwd) != 0) { uart_puts("mv: copied but cannot remove source\r\n"); return; }
        }
        uart_puts("Moved (across filesystems).\r\n");
        return;
    }
    if (rc == -1) uart_puts("mv: source not found\r\n");
    else if (rc == -2) uart_puts("mv: destination already exists\r\n");
    else if (rc == -3) uart_puts("mv: cannot move the root directory\r\n");
    else if (rc == -4) uart_puts("mv: cannot move a directory into itself\r\n");
    else if (rc == -5) uart_puts("mv: invalid destination\r\n");
    else uart_puts("mv: failed (disk error)\r\n");
}

static void command_rm(char* argument)
{
    char* words[6]; char* operands[6];
    int count = split_words(argument, words, 6);
    int options = 0;
    int n = collect_operands(words, count, operands, 6, &options);
    int recursive = (options & OPT_R) != 0;
    int force = (options & OPT_F) != 0;
    if (n < 0) { uart_puts("rm: invalid option\r\n"); return; }
    if (n == 0) { uart_puts("rm: missing operand\r\n"); return; }

    int failures = 0;
    for (int i = 0; i < n; ++i)
    {
        int id = fs_resolve_nofollow(operands[i], vfs_cwd());
        if (id < 0)
        {
            if (!force) { uart_puts("rm: not found\r\n"); failures = 1; }
            continue;
        }
        if (id == fs_get_root())
        { uart_puts("rm: cannot remove root\r\n"); failures = 1; continue; }
        int type = fs_get_type(id);
        int rc;
        if (type == FS_DIR)
        {
            if (!recursive)
            { uart_puts("rm: cannot remove directory (use -r)\r\n"); failures = 1; continue; }
            rc = vfs_rm_recursive(operands[i]);
        }
        else rc = vfs_rm(operands[i]);
        if (rc != 0)
        {
            if (!(force && rc == -1))
            { uart_puts("rm: remove failed\r\n"); failures = 1; }
        }
    }
    (void)failures;
}

static int parse_u64_arg(const char* s, uint64_t* out)
{
    if (s == 0 || out == 0 || *s == '\0')
        return 0;

    uint64_t value = 0;
    int digits = 0;
    while (*s)
    {
        if (*s < '0' || *s > '9')
            return 0;
        uint64_t d = (uint64_t)(*s - '0');
        if (value > (0xFFFFFFFFFFFFFFFFULL - d) / 10ULL)
            return 0;
        value = value * 10ULL + d;
        ++digits;
        ++s;
    }
    if (digits == 0)
        return 0;
    *out = value;
    return 1;
}

static void command_disk()
{
    storage_status();
}

static void command_diskread(const char* arg)
{
    if (!security_is_root()) { uart_puts("diskread: permission denied (root required)\r\n"); return; }
    uint64_t sector = 0;
    if (!parse_u64_arg(arg, &sector))
    {
        uart_puts("diskread: usage: diskread <sector>\r\n");
        return;
    }

    if (!storage_ready())
    {
        uart_puts("diskread: disk not ready\r\n");
        return;
    }

    uint8_t buffer[512];
    int rc = storage_read_sector(sector, buffer);
    if (rc < 0)
    {
        uart_puts("diskread: I/O error\r\n");
        return;
    }

    uart_puts("Sector ");
    print_u64(sector);
    uart_puts(" first 64 bytes:\r\n");

    for (int row = 0; row < 4; ++row)
    {
        for (int i = 0; i < 16; ++i)
        {
            uint8_t v = buffer[row * 16 + i];
            static const char d[] = "0123456789ABCDEF";
            uart_putc(d[(v >> 4) & 0xF]);
            uart_putc(d[v & 0xF]);
            uart_putc(' ');
        }
        uart_puts("|");
        for (int i = 0; i < 16; ++i)
        {
            uint8_t v = buffer[row * 16 + i];
            uart_putc((v >= 32 && v <= 126) ? (char)v : '.');
        }
        uart_puts("|\r\n");
    }
}

static void command_diskwrite(char* arg)
{
    if (!security_is_root()) { uart_puts("diskwrite: permission denied (root required)\r\n"); return; }
    if (arg == 0 || arg[0] == '\0')
    {
        uart_puts("diskwrite: usage: diskwrite <sector> <text>\r\n");
        return;
    }

    char* p = arg;
    while (*p && *p != ' ')
        ++p;

    if (*p == '\0')
    {
        uart_puts("diskwrite: missing text\r\n");
        return;
    }

    *p = '\0';
    ++p;
    while (*p == ' ')
        ++p;

    uint64_t sector = 0;
    if (!parse_u64_arg(arg, &sector))
    {
        uart_puts("diskwrite: invalid sector\r\n");
        return;
    }

    if (!storage_ready())
    {
        uart_puts("diskwrite: disk not ready\r\n");
        return;
    }

    uint8_t buffer[512];
    for (int i = 0; i < 512; ++i)
        buffer[i] = 0;

    int i = 0;
    while (p[i] && i < 511)
    {
        buffer[i] = (uint8_t)p[i];
        ++i;
    }

    int rc = storage_write_sector(sector, buffer);
    if (rc < 0)
    {
        uart_puts("diskwrite: I/O error (disk may be read-only)\r\n");
        return;
    }

    storage_flush();
    uart_puts("Sector written and flushed.\r\n");
}

static void command_diskflush()
{
    if (!security_is_root()) { uart_puts("diskflush: permission denied (root required)\r\n"); return; }
    int rc = storage_flush();
    uart_puts("diskflush: ");
    uart_puts(rc == 0 ? "OK\r\n" : "failed\r\n");
}


static void command_apps()
{
    uart_puts("Bundled user programs\r\n----------------------\r\n");
    uart_puts("/bin/init.elf    - userspace self-test/demo\r\n");
    uart_puts("/bin/nano.elf    - text editor (command: nano)\r\n");
    uart_puts("/bin/calc.elf    - full terminal calculator (command: calc)\r\n");
    uart_puts("/bin/hexdump.elf - hex + ASCII dump of a file (command: hexdump <file>)\r\n");
    uart_puts("/bin/libctest.elf - libc part 2 self-test: FILE stdio, sscanf, strtok, math (command: libctest)\r\n");
    uart_puts("/bin/hello.elf, upper.elf, lcount.elf, spin.elf, ktest.elf, dltest.elf, pipetest.elf - demos and tests\r\n");
}

static void command_uname()
{
    uart_puts("MyKernel "); uart_puts(MYKERNEL_VERSION_STRING); uart_puts(" ARM64 bare-metal\r\n");
}

static void command_user_login(const char* argument)
{
    if (argument && str_equal(argument, "demo"))
    {
        uart_puts("Starting preserved EL0 userspace demo...\r\n");
        user_execute("/bin/init.elf");
        return;
    }

    if (argument && argument[0])
    {
        if (security_login_user(argument) == 0)
            uart_puts("Logged in as user.\r\n");
        else
            uart_puts("user: authentication failed.\r\n");
        return;
    }

    char password[32];
    uart_puts("user login\r\n");
    uart_puts("Password: ");

    if (security_read_password(password, sizeof(password)) < 0)
    {
        uart_puts("user: password input error.\r\n");
        return;
    }

    if (security_login_user(password) == 0)
        uart_puts("Login successful. Welcome, user.\r\n");
    else
        uart_puts("Login failed. Password is incorrect.\r\n");
}

static void command_su(const char* argument)
{
    const char* target = (argument && argument[0]) ? argument : "root";
    char password[48];
    uart_puts(target); uart_puts(" password: ");
    if (security_read_password(password, sizeof(password)) < 0 ||
        security_login_as(target, password) != 0)
    {
        uart_puts("su: authentication failed\r\n");
        return;
    }
    if (security_is_root()) uart_puts("Switched to root.\r\n");
    else { uart_puts("Switched to "); uart_puts(security_username()); uart_puts(".\r\n"); }
}

static void command_useradd(const char* argument)
{
    if (!security_is_root()) { uart_puts("useradd: permission denied (root required)\r\n"); return; }
    if (!argument || !argument[0]) { uart_puts("useradd: usage: useradd <name>\r\n"); return; }
    char pw1[48], pw2[48];
    uart_puts("New password: ");
    if (security_read_password(pw1, sizeof(pw1)) <= 0) { uart_puts("useradd: empty password rejected\r\n"); return; }
    uart_puts("Retype password: ");
    if (security_read_password(pw2, sizeof(pw2)) < 0 || !str_equal(pw1, pw2)) { uart_puts("useradd: passwords do not match\r\n"); return; }
    int rc = security_user_add(argument, pw1);
    if (rc == 0) uart_puts("User created (home: /home/<name>).\r\n");
    else if (rc == -2) uart_puts("useradd: name exists or invalid\r\n");
    else if (rc == -3) uart_puts("useradd: name may only contain a-z 0-9 _\r\n");
    else uart_puts("useradd: failed\r\n");
}

static void command_userdel(const char* argument)
{
    if (!security_is_root()) { uart_puts("userdel: permission denied (root required)\r\n"); return; }
    if (!argument || !argument[0]) { uart_puts("userdel: usage: userdel <name>\r\n"); return; }
    int rc = security_user_del(argument);
    if (rc == 0) uart_puts("User removed.\r\n");
    else if (rc == -3) uart_puts("userdel: cannot remove root or the current account\r\n");
    else uart_puts("userdel: no such user\r\n");
}

static void command_passwd(const char* argument)
{
    const char* who = (argument && argument[0]) ? argument : security_username();
    char old_pw[48] = {0}, pw1[48], pw2[48];
    if (!security_is_root())
    {
        uart_puts("Current password: ");
        if (security_read_password(old_pw, sizeof(old_pw)) < 0) return;
    }
    uart_puts("New password: ");
    if (security_read_password(pw1, sizeof(pw1)) <= 0) { uart_puts("passwd: empty password rejected\r\n"); return; }
    uart_puts("Retype password: ");
    if (security_read_password(pw2, sizeof(pw2)) < 0 || !str_equal(pw1, pw2)) { uart_puts("passwd: passwords do not match\r\n"); return; }
    int rc = security_set_password(who, old_pw, pw1);
    if (rc == 0) uart_puts("Password updated.\r\n");
    else if (rc == -1) uart_puts("passwd: permission denied\r\n");
    else if (rc == -3) uart_puts("passwd: current password is wrong\r\n");
    else uart_puts("passwd: no such user\r\n");
}

static void command_id()
{
    uart_puts("uid="); print_u64(security_uid()); uart_puts("("); uart_puts(security_username());
    uart_puts(") gid="); print_u64(security_gid()); uart_puts("\r\n");
}

static void command_logout()
{
    if (security_is_root() || !str_equal(security_username(), "user"))
    {
        security_logout_to_user();
        uart_puts("Logged out. Back to user.\r\n");
    }
    else
    {
        uart_puts("Already logged in as user.\r\n");
    }
}

static void command_whoami()
{
    uart_puts("Username: ");
    uart_puts(security_username());
    uart_puts("\r\nUID      : ");
    print_u64(security_uid());
    uart_puts("\r\nDomain   : kernel shell\r\n");
    int pid = user_current_pid();
    uart_puts("EL0 app  : ");
    uart_puts(pid >= 0 ? "running\r\n" : "none\r\n");
}

static void command_net()
{
    net_status();
    if (virtio_net_present()) virtio_net_status();
}

static void command_ping(const char* arg)
{
    if (!arg || !arg[0]) { uart_puts("ping: usage: ping <ipv4>\r\n"); return; }
    uint32_t ip=0;
    if (!net_parse_ipv4(arg,&ip)) { uart_puts("ping: invalid IPv4 address\r\n"); return; }
    if (!net_ready()) { uart_puts("ping: network not ready\r\n"); return; }
    uart_puts("PING "); net_print_ipv4(ip); uart_puts(" ...\r\n");
    uint64_t start=timer_millis(); int rc=net_ping(ip,2000); uint64_t elapsed=timer_millis()-start;
    if (rc==0) { uart_puts("Reply from ");net_print_ipv4(ip);uart_puts(": time=");print_u64(elapsed);uart_puts(" ms\r\n"); }
    else uart_puts("ping: timeout or network error\r\n");
}

static void command_nslookup(const char* argument)
{
    if (!argument || !argument[0] || !net_ready())
    {
        uart_puts(!net_ready() ? "nslookup: network not ready\r\n" : "nslookup: usage: nslookup <hostname>\r\n");
        return;
    }
    uint32_t ip = 0;
    if (net_parse_ipv4(argument, &ip))
    {
        uart_puts(argument); uart_puts(" is already an IPv4 address\r\n");
        return;
    }
    uart_puts("Server: "); net_print_ipv4(net_dns_server()); uart_puts("\r\n");
    uint64_t start = timer_millis();
    int rc = net_dns_resolve(argument, &ip, 2500U);
    if (rc == 0)
    {
        uart_puts("Name: "); uart_puts(argument); uart_puts("\r\nAddress: ");
        net_print_ipv4(ip); uart_puts("\r\n");
    }
    else if (rc == -2) uart_puts("nslookup: no A record found (or NXDOMAIN)\r\n");
    else if (rc == -110) uart_puts("nslookup: DNS request timed out\r\n");
    else if (rc == -19) uart_puts("nslookup: network device unavailable\r\n");
    else uart_puts("nslookup: DNS/network error\r\n");
    if (rc == 0) { uart_puts("Query time: "); print_u64(timer_millis() - start); uart_puts(" ms\r\n"); }
}

static int http_append(char* output, uint32_t capacity, uint32_t* used, const char* value)
{
    if (!output || !used || !value) return 0;
    for (uint32_t i = 0; value[i]; ++i)
    {
        if (*used + 1U >= capacity) return 0;
        output[(*used)++] = value[i];
    }
    output[*used] = '\0';
    return 1;
}

static void command_httpget(const char* argument)
{
    if (!net_ready())
    {
        uart_puts("httpget: network not ready\r\n");
        return;
    }
    if (!argument || !argument[0])
    {
        uart_puts("httpget: usage: httpget <hostname> [path]\r\n");
        return;
    }

    while (*argument == ' ' || *argument == '\t') ++argument;
    char hostname[254];
    char path_input[256];
    uint32_t host_len = 0;
    while (argument[host_len] && argument[host_len] != ' ' && argument[host_len] != '\t')
    {
        if (host_len + 1U >= sizeof(hostname))
        {
            uart_puts("httpget: hostname too long\r\n");
            return;
        }
        hostname[host_len] = argument[host_len];
        ++host_len;
    }
    if (host_len == 0) { uart_puts("httpget: missing hostname\r\n"); return; }
    hostname[host_len] = '\0';

    const char* path_arg = argument + host_len;
    while (*path_arg == ' ' || *path_arg == '\t') ++path_arg;
    uint32_t path_len = 0;
    if (!*path_arg) path_input[path_len++] = '/';
    else
    {
        if (*path_arg != '/') path_input[path_len++] = '/';
        while (*path_arg && path_len + 1U < sizeof(path_input))
            path_input[path_len++] = *path_arg++;
        if (*path_arg)
        {
            uart_puts("httpget: path too long\r\n");
            return;
        }
    }
    path_input[path_len] = '\0';

    uint32_t target_ip = 0;
    if (!net_parse_ipv4(hostname, &target_ip))
    {
        uart_puts("Resolving "); uart_puts(hostname); uart_puts(" ...\r\n");
        int dns_rc = net_dns_resolve(hostname, &target_ip, 3000U);
        if (dns_rc != 0)
        {
            uart_puts("httpget: DNS lookup failed (try nslookup ");
            uart_puts(hostname); uart_puts(")\r\n");
            return;
        }
    }

    int socket = net_tcp_socket(0);
    if (socket < 0)
    {
        uart_puts("httpget: could not allocate TCP socket\r\n");
        return;
    }
    uart_puts("Connecting to "); uart_puts(hostname); uart_puts(" (");
    net_print_ipv4(target_ip); uart_puts(") port 80 ...\r\n");
    int rc = net_tcp_connect(0, socket, target_ip, 80U, 5000U);
    if (rc != 0)
    {
        uart_puts("httpget: TCP connection failed/timeout\r\n");
        (void)net_tcp_close(0, socket);
        return;
    }

    char request[768];
    uint32_t request_len = 0;
    request[0] = '\0';
    int built = http_append(request, sizeof(request), &request_len, "GET ") &&
        http_append(request, sizeof(request), &request_len, path_input) &&
        http_append(request, sizeof(request), &request_len, " HTTP/1.0\r\nHost: ") &&
        http_append(request, sizeof(request), &request_len, hostname) &&
        http_append(request, sizeof(request), &request_len,
                    "\r\nUser-Agent: MyKernel/1.0\r\nConnection: close\r\n\r\n");
    if (!built)
    {
        uart_puts("httpget: request too large\r\n");
        (void)net_tcp_close(0, socket);
        return;
    }

    rc = net_tcp_send(0, socket, request, request_len, 5000U);
    if (rc < 0 || (uint32_t)rc != request_len)
    {
        uart_puts("httpget: request send failed\r\n");
        (void)net_tcp_close(0, socket);
        return;
    }

    uart_puts("--- HTTP response (maximum 4096 bytes) ---\r\n");
    char response[600];
    uint32_t total = 0;
    while (total < 4096U)
    {
        uint32_t cap = sizeof(response);
        if (cap > 4096U - total) cap = 4096U - total;
        int got = net_tcp_recv(0, socket, response, cap, 2500U);
        if (got == 0) break;
        if (got < 0)
        {
            if (got == -110 && total != 0) break;
            uart_puts("\r\nhttpget: receive timeout/network error\r\n");
            break;
        }
        for (int i = 0; i < got; ++i) uart_putc(response[i]);
        total += (uint32_t)got;
    }
    if (total >= 4096U) uart_puts("\r\n[response truncated at 4096 bytes]\r\n");
    uart_puts("\r\n--- end response ---\r\n");
    (void)net_tcp_close(0, socket);
}


/* ---- nc / telnet / wget ------------------------------------------------------------- */
static int net_resolve_host(const char* name, uint32_t* ip, const char* who)
{
    if (net_parse_ipv4(name, ip)) return 0;
    if (net_dns_resolve(name, ip, 3000U) != 0) { uart_puts(who); uart_puts(": cannot resolve "); uart_puts(name); uart_puts("\r\n"); return -1; }
    return 0;
}

static int host_is_v6(const char* h) { for (; *h; ++h) if (*h == ':') return 1; return 0; }

/* Connect a new TCP socket to an IPv4 name/address or an IPv6 literal. Returns the handle, or -1 (quiet=1: no message). */
static int tcp_open_any(const char* host, int port, uint32_t timeout, const char* who, int quiet)
{
    int sk = net_tcp_socket(0);
    if (sk < 0) { if (!quiet) { uart_puts(who); uart_puts(": no socket\r\n"); } return -1; }
    int rc;
    if (host_is_v6(host))
    {
        uint8_t a6[16];
        if (net_ip6_parse(host, a6) != 0) { if (!quiet) { uart_puts(who); uart_puts(": bad IPv6 address\r\n"); } net_tcp_close(0, sk); return -1; }
        rc = net_tcp6_connect(0, sk, a6, (uint16_t)port, timeout);
    }
    else
    {
        uint32_t ip;
        if (net_resolve_host(host, &ip, who) != 0) { net_tcp_close(0, sk); return -1; }
        rc = net_tcp_connect(0, sk, ip, (uint16_t)port, timeout);
    }
    if (rc != 0) { net_tcp_close(0, sk); return -1; }
    return sk;
}

static int ctrl_break_seen = 0;
static int ctrl_break_filter(int c) { if (c == 3 || c == 0x1D) { ctrl_break_seen = 1; return 1; } return 0; }

static void command_nc(const char* argument)
{
    char buf[200]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[16]; int n = split_words(buf, w, 16);
    int listen_mode = 0, zero = 0, a = 0;
    while (a < n && w[a][0] == '-' && w[a][1]) { if (w[a][1] == 'l') listen_mode = 1; else if (w[a][1] == 'z') zero = 1; else { uart_puts("nc: unknown option\r\n"); return; } ++a; }
    if (!net_ready()) { uart_puts("nc: network not ready\r\n"); return; }
    if (listen_mode)
    {
        int port = a < n ? parse_uint(w[a], -1) : -1;
        if (port <= 0 || port > 65535) { uart_puts("usage: nc -l <port>\r\n"); return; }
        int ls = net_tcp_socket(0);
        if (ls < 0 || net_tcp_listen(0, ls, (uint16_t)port) != 0) { uart_puts("nc: cannot listen\r\n"); if (ls >= 0) net_tcp_close(0, ls); return; }
        uart_puts("nc: listening on port "); print_u64((uint64_t)port); uart_puts(" (Ctrl-C aborts)\r\n");
        uint32_t pip = 0; uint16_t pp = 0; int cs = -1;
        ctrl_break_seen = 0;
        for (int t = 0; t < 60 && cs < 0 && !ctrl_break_seen; ++t)
        {
            uart_poll_job_keys(ctrl_break_filter);
            cs = net_tcp_accept(0, ls, &pip, &pp, 500U);
        }
        net_tcp_close(0, ls);
        if (cs < 0) { uart_puts("nc: no connection\r\n"); return; }
        uart_puts("nc: connection from ");
        { uint8_t p6[16]; if (net_tcp_peer6(0, cs, p6)) net_ip6_print(p6); else net_print_ipv4(pip); }
        uart_puts("\r\n");
        char rb[256];
        for (int idle = 0; idle < 10 && !ctrl_break_seen; )
        {
            uart_poll_job_keys(ctrl_break_filter);
            int g = net_tcp_recv(0, cs, rb, sizeof(rb), 500U);
            if (g == 0) break;
            if (g < 0) { ++idle; continue; }
            idle = 0;
            for (int i = 0; i < g; ++i) if (rb[i] != '\r') uart_putc(rb[i]);
        }
        net_tcp_close(0, cs);
        uart_puts("\r\nnc: closed\r\n");
        return;
    }
    if (n - a < 2) { uart_puts("usage: nc [-z] <host> <port> [text...]  |  nc -l <port>\r\n"); return; }
    int port = parse_uint(w[a + 1], -1);
    if (port <= 0 || port > 65535) { uart_puts("nc: bad port\r\n"); return; }
    int sk = tcp_open_any(w[a], port, 4000U, "nc", zero);
    if (sk < 0)
    {
        if (zero) { uart_puts("nc: "); uart_puts(w[a]); uart_puts(" "); uart_puts(w[a + 1]); uart_puts(" closed\r\n"); }
        else { uart_puts("nc: connect failed: "); uart_puts(w[a]); uart_puts(" "); uart_puts(w[a + 1]); uart_puts("\r\n"); }
        return;
    }
    if (zero) { uart_puts("nc: "); uart_puts(w[a]); uart_puts(" "); uart_puts(w[a + 1]); uart_puts(" open\r\n"); net_tcp_close(0, sk); return; }
    if (a + 2 < n)
    {
        char msg[160]; int mp = 0;
        for (int i = a + 2; i < n; ++i) { if (mp) msg[mp++] = ' '; for (const char* q = w[i]; *q && mp < 156; ++q) msg[mp++] = *q; }
        msg[mp++] = '\n';
        net_tcp_send(0, sk, msg, (uint32_t)mp, 3000U);
    }
    char rb[256]; int got_any = 0;
    for (int idle = 0; idle < 4; )
    {
        int g = net_tcp_recv(0, sk, rb, sizeof(rb), 800U);
        if (g == 0) break;
        if (g < 0) { ++idle; continue; }
        got_any = 1;
        for (int i = 0; i < g; ++i) if (rb[i] != '\r') uart_putc(rb[i]);
    }
    if (got_any) uart_puts("\r\n");
    net_tcp_close(0, sk);
}

/* udp6 send <addr> <port> <text> | udp6 listen <port> [count] : UDP over IPv6 link-local */
static void command_udp6(const char* argument)
{
    char buf[220]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[16]; int n = split_words(buf, w, 16);
    if (n < 2 || !net_ready()) { uart_puts("usage: udp6 send <ipv6> <port> <text>  |  udp6 listen <port> [count]\r\n"); return; }
    int sk = net_udp_socket(0);
    if (sk < 0) { uart_puts("udp6: no socket\r\n"); return; }
    if (str_equal(w[0], "send") && n >= 4)
    {
        uint8_t a6[16]; int port = parse_uint(w[2], -1);
        if (net_ip6_parse(w[1], a6) != 0 || port <= 0 || port > 65535) { uart_puts("udp6: bad address or port\r\n"); net_udp_close(0, sk); return; }
        char msg[160]; int mp = 0;
        for (int i = 3; i < n; ++i) { if (mp) msg[mp++] = ' '; for (const char* q = w[i]; *q && mp < 158; ++q) msg[mp++] = *q; }
        int rc = net_udp6_sendto(0, sk, a6, (uint16_t)port, msg, (uint32_t)mp);
        if (rc < 0) { uart_puts("udp6: send failed\r\n"); net_udp_close(0, sk); return; }
        char rb[200]; uint8_t from[16]; uint16_t fp = 0;
        int g = net_udp6_recvfrom(0, sk, from, &fp, rb, sizeof(rb), 2500U);
        if (g < 0) uart_puts("udp6: sent, no reply\r\n");
        else { uart_puts("udp6: reply from "); net_ip6_print(from); uart_puts(": "); for (int i = 0; i < g; ++i) uart_putc(rb[i]); uart_puts("\r\n"); }
        net_udp_close(0, sk);
        return;
    }
    if (str_equal(w[0], "listen"))
    {
        int port = parse_uint(w[1], -1), count = n > 2 ? parse_uint(w[2], -1) : 1;
        if (port <= 0 || port > 65535 || count <= 0 || count > 50 || net_udp_bind(0, sk, (uint16_t)port) != 0) { uart_puts("udp6: bad arguments or port in use\r\n"); net_udp_close(0, sk); return; }
        uart_puts("udp6: listening on port "); print_u64((uint64_t)port); uart_puts(" (Ctrl-C aborts)\r\n");
        ctrl_break_seen = 0;
        int got = 0;
        for (int t = 0; t < 60 && got < count && !ctrl_break_seen; ++t)
        {
            uart_poll_job_keys(ctrl_break_filter);
            char rb[200]; uint8_t from[16]; uint16_t fp = 0;
            int g = net_udp6_recvfrom(0, sk, from, &fp, rb, sizeof(rb) - 8, 500U);
            if (g < 0) continue;
            ++got;
            uart_puts("udp6: from "); net_ip6_print(from); uart_puts(": "); for (int i = 0; i < g; ++i) uart_putc(rb[i]); uart_puts("\r\n");
            char rep[220]; int rp = 0; const char* pre = "ack6:";
            for (int i = 0; pre[i]; ++i) rep[rp++] = pre[i];
            for (int i = 0; i < g && rp < 210; ++i) rep[rp++] = rb[i];
            (void)net_udp6_sendto(0, sk, from, fp, rep, (uint32_t)rp);
        }
        net_udp_close(0, sk);
        return;
    }
    uart_puts("usage: udp6 send <ipv6> <port> <text>  |  udp6 listen <port> [count]\r\n");
    net_udp_close(0, sk);
}

static void command_telnet(const char* argument)
{
    char buf[120]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[4]; int n = split_words(buf, w, 4);
    if (n < 1) { uart_puts("usage: telnet <host> [port]   (Ctrl-] quits)\r\n"); return; }
    if (!net_ready()) { uart_puts("telnet: network not ready\r\n"); return; }
    int port = n > 1 ? parse_uint(w[1], -1) : 23;
    if (port <= 0 || port > 65535) { uart_puts("telnet: bad port\r\n"); return; }
    uart_puts("Trying "); uart_puts(w[0]); uart_puts("...\r\n");
    int sk = tcp_open_any(w[0], port, 5000U, "telnet", 0);
    if (sk < 0) { uart_puts("telnet: connection failed\r\n"); return; }
    uart_puts("Connected. Escape character is '^]'.\r\n");
    ctrl_break_seen = 0;
    uint8_t rb[256];
    int closed = 0;
    while (!ctrl_break_seen && !closed)
    {
        uart_poll_job_keys(ctrl_break_filter);
        int c;
        while ((c = uart_try_getc()) >= 0)
        {
            char out[2]; int ol = 0;
            if (c == '\r') { out[0] = '\r'; out[1] = '\n'; ol = 2; } else { out[0] = (char)c; ol = 1; }
            net_tcp_send(0, sk, out, (uint32_t)ol, 1000U);
        }
        int g = net_tcp_recv(0, sk, rb, sizeof(rb), 30U);
        if (g == 0) { closed = 1; break; }
        if (g < 0) continue;
        for (int i = 0; i < g; ++i)
        {
            if (rb[i] == 0xFF && i + 2 < g + 0 && i + 2 <= g - 1 + 0)
            {
                uint8_t cmd = rb[i + 1], opt = rb[i + 2];
                if (cmd == 253 || cmd == 251) { uint8_t rep[3] = {0xFF, (uint8_t)(cmd == 253 ? 252 : 254), opt}; net_tcp_send(0, sk, rep, 3, 500U); }
                i += 2; continue;
            }
            if (rb[i] == '\r') continue;
            if (rb[i] == '\n') uart_putc('\r');
            uart_putc((char)rb[i]);
        }
    }
    net_tcp_close(0, sk);
    uart_puts(closed ? "\r\nConnection closed by foreign host.\r\n" : "\r\ntelnet: connection closed.\r\n");
}

/* TLS verification options shared by wget / httpsget / curl:  -k | --insecure   and   --cacert <file> */
static int g_tls_insecure = 0;
static char g_tls_cacert[160];
static int strip_tls_opts(char** w, int n)
{
    int o = 0; g_tls_insecure = 0; g_tls_cacert[0] = 0;
    for (int i = 0; i < n; ++i)
    {
        if (str_equal(w[i], "-k") || str_equal(w[i], "--insecure")) { g_tls_insecure = 1; continue; }
        if (str_equal(w[i], "--cacert") && i + 1 < n) { str_copy(g_tls_cacert, w[++i], sizeof(g_tls_cacert)); continue; }
        w[o++] = w[i];
    }
    return o;
}
static int tls_connect_checked(int sk, const char* host, const char* who)
{
    tls_set_verify(!g_tls_insecure);
    tls_set_extra_ca(g_tls_cacert);
    int rc = tls_connect(sk, host);
    if (rc != 0)
    {
        uart_puts(who); uart_puts(": TLS handshake failed ("); print_u64((uint64_t)(-rc)); uart_puts(")");
        const char* e = tls_last_error(); if (e && e[0]) { uart_puts(": "); uart_puts(e); }
        uart_puts("\r\n");
        if (e && e[0] && !g_tls_insecure) { uart_puts(who); uart_puts(": the server could not be verified; add its CA with 'certs add <file>' / --cacert <file>, or use -k to skip verification\r\n"); }
    }
    return rc;
}

static int g_http_quiet = 0;
/* HTTP(S) GET into a freshly allocated buffer (caller kfree()s). Returns body length or <0. */
static int http_fetch(const char* host, int port, const char* path, int use_tls, uint8_t** out_body, int* out_status, const char* who)
{
    uint32_t ip;
    if (net_resolve_host(host, &ip, who) != 0) return -1;
    int sk = net_tcp_socket(0);
    if (sk < 0) { uart_puts(who); uart_puts(": no socket\r\n"); return -1; }
    if (net_tcp_connect(0, sk, ip, (uint16_t)port, 5000U) != 0) { uart_puts(who); uart_puts(": connection failed\r\n"); net_tcp_close(0, sk); return -1; }
    if (use_tls)
    {
        int rc = tls_connect_checked(sk, host, who);
        if (rc != 0) { net_tcp_close(0, sk); return -1; }
    }
    char req[512]; uint32_t rl = 0; req[0] = 0;
    http_append(req, sizeof(req), &rl, "GET "); http_append(req, sizeof(req), &rl, path);
    http_append(req, sizeof(req), &rl, " HTTP/1.0\r\nHost: "); http_append(req, sizeof(req), &rl, host);
    http_append(req, sizeof(req), &rl, "\r\nUser-Agent: RioOS/1.0\r\nConnection: close\r\n\r\n");
    int sent = use_tls ? tls_send(req, rl) : net_tcp_send(0, sk, req, rl, 4000U);
    if (sent < 0) { uart_puts(who); uart_puts(": send failed\r\n"); if (use_tls) tls_close(); net_tcp_close(0, sk); return -1; }
    const uint32_t CAP = 1048576U;
    uint8_t* body = (uint8_t*)kmalloc(CAP);
    if (!body) { uart_puts(who); uart_puts(": out of memory\r\n"); if (use_tls) tls_close(); net_tcp_close(0, sk); return -1; }
    uint32_t total = 0; int idle = 0;
    ctrl_break_seen = 0;
    while (total < CAP && idle < 6 && !ctrl_break_seen)
    {
        uart_poll_job_keys(ctrl_break_filter);
        uint32_t want = CAP - total > 1024U ? 1024U : CAP - total;
        int g = use_tls ? tls_recv(body + total, want, 1000U) : net_tcp_recv(0, sk, body + total, want, 1000U);
        if (g == 0) break;
        if (g < 0) { if (use_tls && g != -110) break; ++idle; continue; }
        idle = 0; total += (uint32_t)g;
    }
    if (use_tls) tls_close();
    net_tcp_close(0, sk);
    uint32_t hdr = 0; int found = 0;
    for (uint32_t i = 0; i + 3 < total; ++i) if (body[i] == '\r' && body[i+1] == '\n' && body[i+2] == '\r' && body[i+3] == '\n') { hdr = i + 4; found = 1; break; }
    if (!found || total < 12) { uart_puts(who); uart_puts(": no valid HTTP response\r\n"); kfree(body); return -1; }
    char status[48]; uint32_t sl = 0;
    while (sl < 47 && sl < total && body[sl] != '\r' && body[sl] != '\n') { status[sl] = (char)body[sl]; ++sl; }
    status[sl] = 0;
    if (!g_http_quiet) { uart_puts(status); uart_puts("\r\n"); }
    *out_status = (sl > 9 && status[9] >= '0' && status[9] <= '9') ? (status[9] - '0') : 0;
    uint32_t blen = total - hdr;
    for (uint32_t i = 0; i < blen; ++i) body[i] = body[hdr + i];
    *out_body = body;
    return (int)blen;
}

/* Parse "[http[s]://]host[:port][/path]" or "host[:port] [path]". */
static int parse_url_args(char** w, int n, char* host, int host_cap, int* port, char* path, int path_cap, int* use_tls, int* next)
{
    const char* u = w[0]; *use_tls = 0; *port = 80; *next = 1;
    int has_scheme = 0;
    if (u[0]=='h'&&u[1]=='t'&&u[2]=='t'&&u[3]=='p'&&u[4]==':'&&u[5]=='/'&&u[6]=='/') { u += 7; has_scheme = 1; }
    else if (u[0]=='h'&&u[1]=='t'&&u[2]=='t'&&u[3]=='p'&&u[4]=='s'&&u[5]==':'&&u[6]=='/'&&u[7]=='/') { u += 8; *use_tls = 1; *port = 443; has_scheme = 1; }
    int i = 0;
    while (u[i] && u[i] != ':' && u[i] != '/' && i < host_cap - 1) { host[i] = u[i]; ++i; }
    host[i] = 0;
    if (!host[0]) return -1;
    if (u[i] == ':') { *port = parse_uint(u + i + 1, -1); while (u[i] && u[i] != '/') ++i; }
    if (*port <= 0 || *port > 65535) return -1;
    const char* pa = "/";
    if (u[i] == '/') pa = u + i;
    else if (!has_scheme && n > 1 && w[1][0] != 0) { pa = w[1]; *next = 2; }
    if (pa[0] != '/') { path[0] = '/'; str_copy(path + 1, pa, path_cap - 1); } else str_copy(path, pa, path_cap);
    return 0;
}


/* Kernel-side HTTP(S) GET for user programs (pkg): verified TLS unless insecure. Returns body length or -errno. */
int net_http_get_url(const char* url, int insecure, const char* cacert, uint8_t* out, uint32_t cap)
{
    char buf[300]; str_copy(buf, url, sizeof(buf)); char* w[2] = { buf, 0 };
    char host[128], path[200]; int port, tls, next;
    if (!net_ready()) return -100;
    if (!(buf[0]=='h'&&buf[1]=='t'&&buf[2]=='t'&&buf[3]=='p')) return -22;
    if (parse_url_args(w, 1, host, sizeof(host), &port, path, sizeof(path), &tls, &next) != 0) return -22;
    g_tls_insecure = insecure ? 1 : 0; str_copy(g_tls_cacert, cacert ? cacert : "", sizeof(g_tls_cacert));
    uint8_t* body = 0; int status = 0; g_http_quiet = 1;
    int blen = http_fetch(host, port, path, tls, &body, &status, "pkg");
    g_http_quiet = 0; g_tls_insecure = 0; g_tls_cacert[0] = 0;
    if (blen < 0) return -5;
    if (status != 2) { kfree(body); return -2; }          /* non-2xx: treated as "not found" */
    if ((uint32_t)blen > cap) { kfree(body); return -27; }
    for (int i = 0; i < blen; ++i) out[i] = body[i];
    kfree(body);
    return blen;
}

/* wget <url | host[:port] [path]> [outfile] : HTTP/1.0 or HTTPS (TLS 1.3, certificate verified unless -k) download. */
static void command_wget(const char* argument)
{
    char buf[400]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[10]; int n = split_words(buf, w, 10);
    n = strip_tls_opts(w, n);
    if (n < 1) { uart_puts("usage: wget [-k] [--cacert file] <http[s]://host[:port]/path | host[:port] [path]> [outfile]\r\n"); return; }
    if (!net_ready()) { uart_puts("wget: network not ready\r\n"); return; }
    char host[128], path[200]; int port, tls, next;
    if (parse_url_args(w, n, host, sizeof(host), &port, path, sizeof(path), &tls, &next) != 0) { uart_puts("wget: bad URL\r\n"); return; }
    char outname[64];
    if (next < n) str_copy(outname, w[next], sizeof(outname));
    else { const char* b = path; for (const char* q = path; *q; ++q) if (*q == '/') b = q + 1; str_copy(outname, *b ? b : "index.html", sizeof(outname)); }
    uint8_t* body = 0; int status = 0;
    int blen = http_fetch(host, port, path, tls, &body, &status, "wget");
    if (blen < 0) return;
    if (status != 2) { uart_puts("wget: server returned an error, nothing saved\r\n"); kfree(body); return; }
    if (fs_write_data(outname, vfs_cwd(), body, (uint64_t)blen) < 0) { uart_puts("wget: cannot write "); uart_puts(outname); uart_puts("\r\n"); kfree(body); return; }
    uart_puts("saved "); uart_puts(outname); uart_puts(" ("); print_u64((uint64_t)blen); uart_puts(" bytes)\r\n");
    kfree(body);
}

/* httpsget <host[:port]> [path] | httpsget https://host/path : print an HTTPS response body (TLS 1.3). */
static void command_httpsget(const char* argument)
{
    char buf[400]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[10]; int n = split_words(buf, w, 10);
    n = strip_tls_opts(w, n);
    if (n < 1) { uart_puts("usage: httpsget [-k] [--cacert file] <host[:port]> [path]   (certificate is verified unless -k)\r\n"); return; }
    if (!net_ready()) { uart_puts("httpsget: network not ready\r\n"); return; }
    char host[128], path[200]; int port, tls, next;
    if (parse_url_args(w, n, host, sizeof(host), &port, path, sizeof(path), &tls, &next) != 0) { uart_puts("httpsget: bad URL\r\n"); return; }
    if (!tls) { tls = 1; if (port == 80) port = 443; }
    uint8_t* body = 0; int status = 0;
    int blen = http_fetch(host, port, path, 1, &body, &status, "httpsget");
    if (blen < 0) return;
    uart_puts("--- body ("); print_u64((uint64_t)blen); uart_puts(" bytes, first 4096 shown) ---\r\n");
    for (int i = 0; i < blen && i < 4096; ++i) { if (body[i] == '\n') uart_putc('\r'); uart_putc((char)body[i]); }
    uart_puts("\r\n--- end ---\r\n");
    kfree(body);
}

/* ============================ curl, ntpdate, fw ============================ */
static int cw_tokens(char* s, char** w, int cap)           /* quote-aware split ('...' and "...") */
{
    int n = 0;
    while (*s && n < cap)
    {
        while (*s == ' ' || *s == '\t') ++s;
        if (!*s) break;
        char* o = s; w[n++] = o; char q = 0;
        while (*s && (q || (*s != ' ' && *s != '\t')))
        {
            if (q) { if (*s == q) q = 0; else *o++ = *s; }
            else if (*s == '"' || *s == '\'') q = *s;
            else *o++ = *s;
            ++s;
        }
        if (*s) ++s;
        *o = 0;
    }
    return n;
}

/* One HTTP(S) request. Returns the response size (headers + body) in *out, or -1. status = numeric code. */
static int curl_request(const char* host, int port, const char* path, int tls, const char* method, const char* hdrs, const char* body, uint32_t blen,
                        uint8_t** out, int* status, uint32_t* hdr_len)
{
    uint32_t ip;
    if (net_resolve_host(host, &ip, "curl") != 0) return -1;
    int sk = net_tcp_socket(0);
    if (sk < 0) { uart_puts("curl: no socket\r\n"); return -1; }
    if (net_tcp_connect(0, sk, ip, (uint16_t)port, 5000U) != 0) { uart_puts("curl: connection failed\r\n"); net_tcp_close(0, sk); return -1; }
    if (tls)
    {
        int rc = tls_connect_checked(sk, host, "curl");
        if (rc != 0) { net_tcp_close(0, sk); return -1; }
    }
    char req[1024]; uint32_t rl = 0; req[0] = 0;
    http_append(req, sizeof(req), &rl, method); http_append(req, sizeof(req), &rl, " "); http_append(req, sizeof(req), &rl, path);
    http_append(req, sizeof(req), &rl, " HTTP/1.0\r\nHost: "); http_append(req, sizeof(req), &rl, host);
    http_append(req, sizeof(req), &rl, "\r\nUser-Agent: curl/rioos\r\nAccept: */*\r\nConnection: close\r\n");
    if (hdrs) http_append(req, sizeof(req), &rl, hdrs);
    if (body)
    {
        char cl[16]; int cn = 0; uint32_t v = blen; char t[12]; int tn = 0;
        do { t[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (tn) cl[cn++] = t[--tn]; cl[cn] = 0;
        http_append(req, sizeof(req), &rl, "Content-Length: "); http_append(req, sizeof(req), &rl, cl); http_append(req, sizeof(req), &rl, "\r\n");
    }
    http_append(req, sizeof(req), &rl, "\r\n");
    if (body && rl + blen < sizeof(req)) { for (uint32_t i = 0; i < blen; ++i) req[rl + i] = body[i]; rl += blen; }
    int sent = tls ? tls_send(req, rl) : net_tcp_send(0, sk, req, rl, 4000U);
    if (sent < 0) { uart_puts("curl: send failed\r\n"); if (tls) tls_close(); net_tcp_close(0, sk); return -1; }
    const uint32_t CAP = 1048576U;
    uint8_t* buf = (uint8_t*)kmalloc(CAP);
    if (!buf) { uart_puts("curl: out of memory\r\n"); if (tls) tls_close(); net_tcp_close(0, sk); return -1; }
    uint32_t total = 0; int idle = 0;
    ctrl_break_seen = 0;
    while (total < CAP && idle < 6 && !ctrl_break_seen)
    {
        uart_poll_job_keys(ctrl_break_filter);
        uint32_t want = CAP - total > 1024U ? 1024U : CAP - total;
        int g = tls ? tls_recv(buf + total, want, 1000U) : net_tcp_recv(0, sk, buf + total, want, 1000U);
        if (g == 0) break;
        if (g < 0) { if (tls && g != -110) break; ++idle; continue; }
        idle = 0; total += (uint32_t)g;
    }
    if (tls) tls_close();
    net_tcp_close(0, sk);
    uint32_t hl = 0; int found = 0;
    for (uint32_t i = 0; i + 3 < total; ++i) if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') { hl = i + 4; found = 1; break; }
    if (!found || total < 12 || buf[0] != 'H') { uart_puts("curl: no valid HTTP response\r\n"); kfree(buf); return -1; }
    *status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
    *hdr_len = hl; *out = buf;
    return (int)total;
}

static int curl_find_header(const uint8_t* b, uint32_t hl, const char* name, char* val, int cap)
{
    int nl = str_len(name);
    for (uint32_t i = 0; i + (uint32_t)nl + 1 < hl; ++i)
    {
        if (i != 0 && b[i - 1] != '\n') continue;
        int k = 0; while (k < nl && ((b[i + k] | 32) == (name[k] | 32))) ++k;
        if (k != nl || b[i + nl] != ':') continue;
        uint32_t p = i + (uint32_t)nl + 1; while (p < hl && b[p] == ' ') ++p;
        int v = 0; while (p < hl && b[p] != '\r' && b[p] != '\n' && v < cap - 1) val[v++] = (char)b[p++];
        val[v] = 0; return 1;
    }
    return 0;
}

/* certs [list | add <pem> | verify <pem chain> [host] | path] : TLS trust store (/etc/ssl/certs.pem) */
static void command_certs(const char* argument)
{
    char buf[300]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[6]; int n = split_words(buf, w, 6);
    const char* sub = n > 0 ? w[0] : "list";
    if (str_equal(sub, "list") || str_equal(sub, "count"))
    {
        int c = tls_trust_reload();
        if (c < 0) c = 0;
        uart_puts("trust store: /etc/ssl/certs.pem, "); print_u64((uint64_t)c); uart_puts(" certificate(s)\r\n");
        if (str_equal(sub, "list")) for (int i = 0; i < c; ++i) { char nm[96]; if (x509_trust_name(i, nm, sizeof(nm)) < 0) nm[0] = 0; print_u64((uint64_t)(i + 1)); uart_puts(". "); uart_puts(nm[0] ? nm : "(unnamed)"); uart_puts("\r\n"); }
        return;
    }
    if (str_equal(sub, "add") && n == 2)
    {
        if (!security_is_root()) { uart_puts("certs: only root can change the trust store\r\n"); return; }
        uint8_t* b = (uint8_t*)kmalloc(65536);
        if (!b) { uart_puts("certs: out of memory\r\n"); return; }
        uint64_t sz = 0;
        if (fs_read_file(w[1], vfs_cwd(), b, 65535, &sz) != 0 || sz == 0) { uart_puts("certs: cannot read file (missing or over 64 KiB)\r\n"); kfree(b); return; }
        uint8_t der[4096]; uint32_t off = 0, used = 0; int added = 0;
        while (off < sz && x509_pem_to_der(b + off, (uint32_t)(sz - off), der, sizeof(der), &used) > 0) { ++added; off += used; }
        if (added == 0) { uart_puts("certs: no PEM certificate found in "); uart_puts(w[1]); uart_puts("\r\n"); kfree(b); return; }
        uint64_t cur = 0; uint8_t* all = (uint8_t*)kmalloc(600000);
        if (!all) { uart_puts("certs: out of memory\r\n"); kfree(b); return; }
        if (fs_read_file("/etc/ssl/certs.pem", vfs_cwd(), all, 520000, &cur) != 0) cur = 0;
        if (cur + sz + 2 > 590000) { uart_puts("certs: trust store is full\r\n"); kfree(b); kfree(all); return; }
        if (cur && all[cur - 1] != '\n') all[cur++] = '\n';
        for (uint64_t i = 0; i < sz; ++i) all[cur + i] = b[i];
        cur += sz; if (all[cur - 1] != '\n') all[cur++] = '\n';
        (void)fs_mkdir_p("/etc/ssl", fs_get_root());
        int rc = fs_write_data("/etc/ssl/certs.pem", vfs_cwd(), all, cur);
        kfree(b); kfree(all);
        if (rc < 0) { uart_puts("certs: cannot write /etc/ssl/certs.pem\r\n"); return; }
        uart_puts("certs: added "); print_u64((uint64_t)added); uart_puts(" certificate(s); store now has "); print_u64((uint64_t)tls_trust_reload()); uart_puts("\r\n");
        return;
    }
    if (str_equal(sub, "verify") && n >= 2)
    {
        uint8_t* b = (uint8_t*)kmalloc(65536);
        if (!b) { uart_puts("certs: out of memory\r\n"); return; }
        uint64_t sz = 0;
        if (fs_read_file(w[1], vfs_cwd(), b, 65535, &sz) != 0 || sz == 0) { uart_puts("certs: cannot read file\r\n"); kfree(b); return; }
        static uint8_t der[8][4096]; const uint8_t* cp[8]; uint32_t cl[8]; int cn = 0; uint32_t off = 0, used = 0;
        while (cn < 8 && off < sz) { int r = x509_pem_to_der(b + off, (uint32_t)(sz - off), der[cn], 4096, &used); if (r <= 0) break; cp[cn] = der[cn]; cl[cn] = (uint32_t)r; ++cn; off += used; }
        kfree(b);
        if (cn == 0) { uart_puts("certs: no PEM certificate found\r\n"); return; }
        if (tls_trust_reload() <= 0) { uart_puts("certs: trust store is empty\r\n"); return; }
        X509Key key; const char* err = "";
        int rc = x509_verify_chain(cp, cl, cn, n > 2 ? w[2] : 0, rtc_get_epoch(), &key, &err);
        if (rc == 0) { uart_puts("certs: chain of "); print_u64((uint64_t)cn); uart_puts(" certificate(s) is valid\r\n"); }
        else { uart_puts("certs: verification FAILED: "); uart_puts(err); uart_puts("\r\n"); }
        return;
    }
    uart_puts("usage: certs [list | count | add <pem file> | verify <pem chain file> [host]]\r\n");
}

/* ---- ssh / scp ------------------------------------------------------------------------ */
struct SshTarget { char user[48]; char host[96]; char path[200]; int has_host; };

/* "[user@]host:path" (colon before any slash) or "[user@]host" */
static int ssh_split_target(const char* s, SshTarget* t, int need_colon)
{
    t->user[0] = 0; t->host[0] = 0; t->path[0] = 0; t->has_host = 0;
    const char* at = 0; const char* colon = 0;
    for (const char* p = s; *p; ++p) { if (*p == '/') break; if (*p == '@' && !at && !colon) at = p; if (*p == ':' ) { colon = p; break; } }
    if (need_colon && !colon) return 0;
    const char* hs = at ? at + 1 : s;
    if (at && (int)(at - s) < (int)sizeof(t->user)) { int n = (int)(at - s); for (int i = 0; i < n; ++i) t->user[i] = s[i]; t->user[n] = 0; }
    const char* he = colon ? colon : s + str_len(s);
    int hl = (int)(he - hs); if (hl <= 0 || hl >= (int)sizeof(t->host)) return 0;
    for (int i = 0; i < hl; ++i) t->host[i] = hs[i]; t->host[hl] = 0;
    if (colon) str_copy(t->path, colon + 1, sizeof(t->path));
    t->has_host = 1; return 1;
}

static int ssh_prompt_connect(const SshTarget* t, int port, int strict, const char* who, int* sk_out)
{
    if (!net_ready()) { uart_puts(who); uart_puts(": network not ready\r\n"); return -1; }
    int sk = tcp_open_any(t->host, port, 6000U, who, 0);
    if (sk < 0) { uart_puts(who); uart_puts(": connection failed\r\n"); return -1; }
    char user[48]; str_copy(user, t->user[0] ? t->user : security_username(), sizeof(user));
    char pw[128];
    uart_puts(user); uart_putc('@'); uart_puts(t->host); uart_puts("'s password: ");
    if (security_read_password(pw, sizeof(pw)) < 0) { net_tcp_close(0, sk); uart_puts("\r\n"); return -1; }
    SshOpts o; o.host = t->host; o.port = (uint32_t)port; o.user = user; o.password = pw; o.strict = strict;
    int rc = ssh_connect(sk, &o);
    for (int i = 0; i < (int)sizeof(pw); ++i) pw[i] = 0;
    if (rc != 0) { uart_puts(who); uart_puts(": "); uart_puts(ssh_error()); uart_puts("\r\n"); net_tcp_close(0, sk); return -1; }
    *sk_out = sk; return 0;
}

static int ssh_parse_o(const char* opt, int* strict)
{
    if (opt[0] == 'S' && str_equal(opt, "StrictHostKeyChecking=yes")) { *strict = 1; return 1; }
    if (str_equal(opt, "StrictHostKeyChecking=no")) { *strict = 2; return 1; }
    if (str_equal(opt, "StrictHostKeyChecking=accept-new") || str_equal(opt, "StrictHostKeyChecking=ask")) { *strict = 0; return 1; }
    return 0;
}

static void command_ssh(const char* argument)
{
    char buf[400]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[24]; int n = split_words(buf, w, 24);
    int port = 22, strict = 0, force_tty = 0, no_tty = 0, i = 0;
    for (; i < n && w[i][0] == '-' && w[i][1]; ++i)
    {
        if (str_equal(w[i], "-p") && i + 1 < n) port = parse_uint(w[++i], -1);
        else if (str_equal(w[i], "-t")) force_tty = 1;
        else if (str_equal(w[i], "-T")) no_tty = 1;
        else if (str_equal(w[i], "-o") && i + 1 < n) { if (!ssh_parse_o(w[++i], &strict)) { uart_puts("ssh: unsupported option "); uart_puts(w[i]); uart_puts("\r\n"); return; } }
        else if (str_equal(w[i], "-R") && i + 1 < n)
        {
            SshTarget t; if (!ssh_split_target(w[i + 1], &t, 0)) { uart_puts("ssh: bad host\r\n"); return; }
            if (ssh_forget_host(t.host, (uint32_t)port) == 0) { uart_puts("ssh: removed host key of "); uart_puts(t.host); uart_puts("\r\n"); } else uart_puts("ssh: no host key stored for that host\r\n");
            return;
        }
        else { i = -1; break; }
    }
    if (i < 0 || i >= n || port <= 0 || port > 65535) { uart_puts("usage: ssh [-p port] [-t|-T] [-o StrictHostKeyChecking=yes|no|accept-new] [user@]host [command]\r\n       ssh -R [-p port] host      (forget a stored host key)\r\n"); return; }
    SshTarget t; if (!ssh_split_target(w[i], &t, 0)) { uart_puts("ssh: bad host\r\n"); return; }
    /* the command is everything after the host word, kept verbatim */
    char cmd[300]; cmd[0] = 0;
    for (int k = i + 1; k < n; ++k) { if (cmd[0]) str_copy(cmd + str_len(cmd), " ", 2); str_copy(cmd + str_len(cmd), w[k], (int)sizeof(cmd) - str_len(cmd)); }
    int sk;
    if (ssh_prompt_connect(&t, port, strict, "ssh", &sk) != 0) return;
    int pty = cmd[0] ? force_tty : !no_tty;
    if (ssh_open_channel(cmd[0] ? cmd : 0, pty) != 0) { uart_puts("ssh: "); uart_puts(ssh_error()); uart_puts("\r\n"); ssh_close(); return; }
    ctrl_break_seen = 0;
    uint8_t rb[1024]; int fin = 0; int line_start = 1; int tilde = 0; int aborted = 0;
    while (!fin)
    {
        if (cmd[0]) { uart_poll_job_keys(ctrl_break_filter); if (ctrl_break_seen) { aborted = 1; break; } }
        else
        {
            int c;
            while ((c = uart_try_getc()) >= 0)
            {
                if (line_start && c == '~') { tilde = 1; line_start = 0; continue; }
                if (tilde) { tilde = 0; if (c == '.') { fin = 2; break; } uint8_t t2 = '~'; ssh_send(&t2, 1); }
                uint8_t ch = (uint8_t)c; if (ssh_send(&ch, 1) != 0) { fin = 1; break; }
                line_start = (c == '\r' || c == '\n');
            }
            if (fin) break;
        }
        int g = ssh_recv(rb, sizeof(rb), 40U);
        if (g == 0) break;
        if (g == -2) { uart_puts("\r\nssh: "); uart_puts(ssh_error()); uart_puts("\r\n"); fin = 1; break; }
        if (g < 0) continue;
        for (int k = 0; k < g; ++k)
        {
            if (!pty && rb[k] == '\n') uart_putc('\r');
            if (!pty && rb[k] == '\r') continue;
            uart_putc((char)rb[k]);
        }
    }
    int st = ssh_exit_status();
    ssh_close();
    if (aborted) { uart_puts("\r\nssh: interrupted\r\n"); script_set_status(130); }
    else if (fin == 2) { uart_puts("\r\nConnection to "); uart_puts(t.host); uart_puts(" closed.\r\n"); script_set_status(0); }
    else if (cmd[0] || st > 0) script_set_status(st < 0 ? 0 : st);
    else { uart_puts("\r\nConnection to "); uart_puts(t.host); uart_puts(" closed.\r\n"); }
}

/* scp helpers */
static int scp_read_exact(uint8_t* b, uint32_t n)
{
    uint32_t got = 0; uint64_t t0 = timer_millis();
    while (got < n)
    {
        uart_poll_job_keys(ctrl_break_filter); if (ctrl_break_seen) return -1;
        int r = ssh_recv(b + got, n - got, 200U);
        if (r == 0 || r == -2) return -1;
        if (r < 0) { if (timer_millis() - t0 > 30000) return -1; continue; }
        got += (uint32_t)r; t0 = timer_millis();
    }
    return 0;
}
static int scp_ack(const char* who)           /* 0 ok, <0 failed */
{
    uint8_t c; if (scp_read_exact(&c, 1) != 0) { uart_puts(who); uart_puts(": lost connection\r\n"); return -1; }
    if (c == 0) return 0;
    char msg[160]; int m = 0; uint8_t d;
    while (m < 150 && scp_read_exact(&d, 1) == 0 && d != '\n') msg[m++] = (char)d;
    msg[m] = 0; uart_puts(who); uart_puts(": remote: "); uart_puts(msg); uart_puts("\r\n");
    return -1;
}
static int scp_readline(char* out, int cap)
{
    int n = 0; uint8_t c;
    for (;;) { if (scp_read_exact(&c, 1) != 0) return -1; if (c == '\n') break; if (n < cap - 1) out[n++] = (char)c; }
    out[n] = 0; return n;
}
static void scp_octal(uint32_t v, char* out) { out[0] = (char)('0' + ((v >> 9) & 7)); out[1] = (char)('0' + ((v >> 6) & 7)); out[2] = (char)('0' + ((v >> 3) & 7)); out[3] = (char)('0' + (v & 7)); out[4] = 0; }
static void scp_quote(const char* in, char* out, int cap)
{
    int n = 0; out[n++] = '\'';
    for (; *in && n < cap - 6; ++in) { if (*in == '\'') { out[n++] = '\''; out[n++] = '\\'; out[n++] = '\''; out[n++] = '\''; } else out[n++] = *in; }
    out[n++] = '\''; out[n] = 0;
}
static void scp_report(const char* name, uint64_t size, int quiet)
{
    if (quiet) return;
    uart_puts(name); uart_puts("   100%   "); print_u64(size); uart_puts(" bytes\r\n");
}

static int scp_send_path(const char* path, int recursive, int quiet, int depth)
{
    FsStat st; if (fs_stat_path(path, vfs_cwd(), &st) != 0) { uart_puts("scp: "); uart_puts(path); uart_puts(": no such file or directory\r\n"); return -1; }
    int id = fs_resolve(path, vfs_cwd());
    const char* base = id >= 0 ? fs_get_name(id) : path; if (!base || !base[0]) base = "file";
    char name[100]; str_copy(name, base, sizeof(name));
    char mode[6]; scp_octal(st.mode & 0777U, mode);
    char hdr[220]; int h = 0;
    if (st.type == FS_DIR)
    {
        if (!recursive) { uart_puts("scp: "); uart_puts(path); uart_puts(": not a regular file (use -r)\r\n"); return -1; }
        if (depth > 8) { uart_puts("scp: directory tree too deep\r\n"); return -1; }
        hdr[h++] = 'D'; for (int i = 0; i < 4; ++i) hdr[h++] = mode[i]; hdr[h++] = ' '; hdr[h++] = '0'; hdr[h++] = ' ';
        for (const char* p = name; *p; ++p) hdr[h++] = *p; hdr[h++] = '\n';
        if (ssh_send((const uint8_t*)hdr, (uint32_t)h) != 0 || scp_ack("scp") != 0) return -1;
        int cnt = fs_get_child_count(id);
        for (int k = 0; k < cnt; ++k)
        {
            int c = fs_get_child(id, k); if (c < 0) continue;
            char sub[300]; str_copy(sub, path, sizeof(sub)); int l = str_len(sub); if (l && sub[l - 1] != '/') { sub[l++] = '/'; sub[l] = 0; }
            str_copy(sub + l, fs_get_name(c), (int)sizeof(sub) - l);
            if (scp_send_path(sub, recursive, quiet, depth + 1) != 0) return -1;
        }
        if (ssh_send((const uint8_t*)"E\n", 2) != 0 || scp_ack("scp") != 0) return -1;
        return 0;
    }
    if (st.type != FS_FILE) { uart_puts("scp: "); uart_puts(path); uart_puts(": not a regular file\r\n"); return -1; }
    uint8_t* data = (uint8_t*)kmalloc(st.size + 1);
    if (!data) { uart_puts("scp: out of memory\r\n"); return -1; }
    uint64_t got = 0;
    if (fs_read_file(path, vfs_cwd(), data, st.size, &got) != 0) { uart_puts("scp: cannot read "); uart_puts(path); uart_puts("\r\n"); kfree(data); return -1; }
    hdr[h++] = 'C'; for (int i = 0; i < 4; ++i) hdr[h++] = mode[i]; hdr[h++] = ' ';
    { char num[24]; int nn = 0; uint64_t v = got; char rev[24]; int rn = 0; do { rev[rn++] = (char)('0' + v % 10); v /= 10; } while (v); while (rn) num[nn++] = rev[--rn]; for (int i = 0; i < nn; ++i) hdr[h++] = num[i]; }
    hdr[h++] = ' '; for (const char* p = name; *p; ++p) hdr[h++] = *p; hdr[h++] = '\n';
    int rc = 0;
    if (ssh_send((const uint8_t*)hdr, (uint32_t)h) != 0 || scp_ack("scp") != 0) rc = -1;
    if (!rc && got) { /* send in slices so Ctrl-C and window updates stay responsive */
        for (uint64_t off = 0; off < got && !rc; ) { uint32_t n = (uint32_t)(got - off > 16384 ? 16384 : got - off); if (ssh_send(data + off, n) != 0) rc = -1; off += n; uart_poll_job_keys(ctrl_break_filter); if (ctrl_break_seen) rc = -1; } }
    if (!rc) { uint8_t z = 0; if (ssh_send(&z, 1) != 0 || scp_ack("scp") != 0) rc = -1; }
    kfree(data);
    if (!rc) scp_report(name, got, quiet);
    return rc;
}

static int scp_parse_c(const char* line, uint32_t* mode, uint64_t* size, const char** name)
{
    const char* p = line + 1; uint32_t m = 0;
    while (*p >= '0' && *p <= '7') m = m * 8 + (uint32_t)(*p++ - '0');
    if (*p++ != ' ') return -1;
    uint64_t sz = 0; if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') sz = sz * 10 + (uint64_t)(*p++ - '0');
    if (*p++ != ' ' || !*p) return -1;
    *mode = m; *size = sz; *name = p; return 0;
}
static int scp_safe_name(const char* n)
{
    if (!n[0] || str_equal(n, ".") || str_equal(n, "..")) return 0;
    for (; *n; ++n) if (*n == '/') return 0;
    return 1;
}

static int scp_receive(const char* dst, int recursive, int quiet)
{
    char dirs[10][300]; int depth = 0;
    FsStat ds; int dst_is_dir = fs_stat_path(dst, vfs_cwd(), &ds) == 0 && ds.type == FS_DIR;
    str_copy(dirs[0], dst, sizeof(dirs[0]));
    uint8_t z = 0; if (ssh_send(&z, 1) != 0) return -1;
    int files = 0; char line[400];
    for (;;)
    {
        uint8_t c; uint64_t t0 = timer_millis(); int r;
        for (;;)
        {
            uart_poll_job_keys(ctrl_break_filter); if (ctrl_break_seen) return -1;
            r = ssh_recv(&c, 1, 200U);
            if (r != -1) break;
            if (timer_millis() - t0 > 30000) { uart_puts("scp: timeout\r\n"); return -1; }
        }
        if (r <= 0) return files ? 0 : (uart_puts("scp: no data from the server\r\n"), -1);
        if (c == 0x01 || c == 0x02) { char msg[160]; int m = 0; uint8_t d; while (m < 150 && scp_read_exact(&d, 1) == 0 && d != '\n') msg[m++] = (char)d; msg[m] = 0; uart_puts("scp: remote: "); uart_puts(msg); uart_puts("\r\n"); return -1; }
        line[0] = (char)c;
        if (scp_readline(line + 1, (int)sizeof(line) - 1) < 0) return -1;
        if (c == 'T') { if (ssh_send(&z, 1) != 0) return -1; continue; }
        if (c == 'E') { if (depth > 0) --depth; if (ssh_send(&z, 1) != 0) return -1; if (depth == 0 && recursive) { /* wait for the remote to finish */ } continue; }
        if (c != 'C' && c != 'D') { uart_puts("scp: protocol error\r\n"); return -1; }
        uint32_t mode; uint64_t size; const char* name;
        if (scp_parse_c(line, &mode, &size, &name) != 0 || !scp_safe_name(name)) { uart_puts("scp: unsafe or malformed name from the server\r\n"); return -1; }
        char target[320];
        if (depth == 0 && !dst_is_dir) str_copy(target, dirs[0], sizeof(target));       /* new name for the file / top directory */
        else { str_copy(target, dirs[depth], sizeof(target)); int l = str_len(target); if (l && target[l - 1] != '/') { target[l++] = '/'; target[l] = 0; } str_copy(target + l, name, (int)sizeof(target) - l); }
        if (c == 'D')
        {
            if (!recursive) { uart_puts("scp: server sent a directory (use -r)\r\n"); return -1; }
            if (depth >= 9) { uart_puts("scp: directory tree too deep\r\n"); return -1; }
            FsStat s2; int exists = fs_stat_path(target, vfs_cwd(), &s2) == 0;
            if (!exists && fs_mkdir(target, vfs_cwd()) < 0) { uart_puts("scp: cannot create directory "); uart_puts(target); uart_puts("\r\n"); return -1; }
            ++depth; str_copy(dirs[depth], target, sizeof(dirs[depth]));
            if (ssh_send(&z, 1) != 0) return -1;
            continue;
        }
        if (size > 4U * 1024U * 1024U) { uart_puts("scp: file too large for this file system (4 MiB limit)\r\n"); return -1; }
        uint8_t* data = (uint8_t*)kmalloc(size + 1);
        if (!data) { uart_puts("scp: out of memory\r\n"); return -1; }
        if (ssh_send(&z, 1) != 0) { kfree(data); return -1; }
        if (size && scp_read_exact(data, (uint32_t)size) != 0) { uart_puts("scp: transfer interrupted\r\n"); kfree(data); return -1; }
        uint8_t fin; if (scp_read_exact(&fin, 1) != 0 || fin != 0) { uart_puts("scp: transfer failed\r\n"); kfree(data); return -1; }
        int wr = fs_write_data(target, vfs_cwd(), data, size);
        kfree(data);
        if (wr < 0) { uart_puts("scp: cannot write "); uart_puts(target); uart_puts("\r\n"); return -1; }
        (void)fs_set_mode_path(target, vfs_cwd(), mode & 0777U);
        if (ssh_send(&z, 1) != 0) return -1;
        ++files; scp_report(name, size, quiet);
    }
}

static void command_scp(const char* argument)
{
    char buf[500]; str_copy(buf, argument ? argument : "", sizeof(buf));
    char* w[16]; int n = split_words(buf, w, 16);
    int port = 22, recursive = 0, quiet = 0, strict = 0, i = 0;
    for (; i < n && w[i][0] == '-' && w[i][1]; ++i)
    {
        if (str_equal(w[i], "-P") && i + 1 < n) port = parse_uint(w[++i], -1);
        else if (str_equal(w[i], "-r")) recursive = 1;
        else if (str_equal(w[i], "-q")) quiet = 1;
        else if (str_equal(w[i], "-o") && i + 1 < n) { if (!ssh_parse_o(w[++i], &strict)) { uart_puts("scp: unsupported option "); uart_puts(w[i]); uart_puts("\r\n"); return; } }
        else { i = -1; break; }
    }
    if (i < 0 || n - i != 2 || port <= 0 || port > 65535) { uart_puts("usage: scp [-P port] [-r] [-q] [-o StrictHostKeyChecking=...] <src> <dst>   (exactly one of them is [user@]host:path)\r\n"); return; }
    SshTarget a, b; int ra = ssh_split_target(w[i], &a, 1), rb = ssh_split_target(w[i + 1], &b, 1);
    if (ra == rb) { uart_puts("scp: exactly one of source and destination must be remote ([user@]host:path)\r\n"); return; }
    const SshTarget* rem = ra ? &a : &b; const char* local = ra ? w[i + 1] : w[i];
    int sk; if (ssh_prompt_connect(rem, port, strict, "scp", &sk) != 0) return;
    char qp[260]; scp_quote(rem->path[0] ? rem->path : ".", qp, sizeof(qp));
    char cmd[300]; str_copy(cmd, ra ? "scp -f " : "scp -t ", sizeof(cmd)); if (recursive) str_copy(cmd + str_len(cmd), "-r ", 4); str_copy(cmd + str_len(cmd), qp, (int)sizeof(cmd) - str_len(cmd));
    if (ssh_open_channel(cmd, 0) != 0) { uart_puts("scp: "); uart_puts(ssh_error()); uart_puts("\r\n"); ssh_close(); return; }
    ctrl_break_seen = 0; int rc;
    if (ra) rc = scp_receive(local, recursive, quiet);
    else
    {
        rc = scp_ack("scp");
        if (rc == 0) rc = scp_send_path(local, recursive, quiet, 0);
    }
    if (ctrl_break_seen) uart_puts("scp: interrupted\r\n");
    ssh_send_eof(); ssh_close();
    script_set_status(rc == 0 ? 0 : 1);
}

static void command_curl(const char* argument)
{
    char line[600]; str_copy(line, argument ? argument : "", sizeof(line));
    char* w[24]; int n = cw_tokens(line, w, 24);
    int silent = 0, show_hdr = 0, head = 0, follow = 0; const char* outfile = 0; const char* method = 0; const char* data = 0; const char* url = 0;
    char hdrs[400]; hdrs[0] = 0;
    g_tls_insecure = 0; g_tls_cacert[0] = 0;
    for (int i = 0; i < n; ++i)
    {
        if (w[i][0] == '-' && w[i][1] && w[i][1] != '-' && w[i][2])            /* clustered flags: -sk, -sI, -sL ... */
        {
            int all_flags = 1; for (const char* q = w[i] + 1; *q; ++q) if (*q != 's' && *q != 'k' && *q != 'i' && *q != 'I' && *q != 'L') all_flags = 0;
            if (all_flags)
            {
                for (const char* q = w[i] + 1; *q; ++q)
                {
                    if (*q == 's') silent = 1; else if (*q == 'k') g_tls_insecure = 1; else if (*q == 'i') show_hdr = 1;
                    else if (*q == 'I') { head = 1; show_hdr = 1; } else follow = 1;
                }
                continue;
            }
        }
        if (str_equal(w[i], "-s")) silent = 1;
        else if (str_equal(w[i], "-k") || str_equal(w[i], "--insecure")) g_tls_insecure = 1;
        else if (str_equal(w[i], "--cacert") && i + 1 < n) str_copy(g_tls_cacert, w[++i], sizeof(g_tls_cacert));
        else if (str_equal(w[i], "-i")) show_hdr = 1;
        else if (str_equal(w[i], "-I")) { head = 1; show_hdr = 1; }
        else if (str_equal(w[i], "-L")) follow = 1;
        else if (str_equal(w[i], "-o") && i + 1 < n) outfile = w[++i];
        else if (str_equal(w[i], "-X") && i + 1 < n) method = w[++i];
        else if (str_equal(w[i], "-d") && i + 1 < n) data = w[++i];
        else if (str_equal(w[i], "-H") && i + 1 < n) { uint32_t u = (uint32_t)str_len(hdrs); http_append(hdrs, sizeof(hdrs), &u, w[++i]); http_append(hdrs, sizeof(hdrs), &u, "\r\n"); }
        else if (w[i][0] == '-' && w[i][1]) { uart_puts("curl: unknown option "); uart_puts(w[i]); uart_puts("\r\n"); return; }
        else if (!url) url = w[i];
        else { uart_puts("curl: only one URL is supported\r\n"); return; }
    }
    if (!url) { uart_puts("usage: curl [-s] [-k] [--cacert file] [-i|-I] [-L] [-o file] [-X METHOD] [-H 'K: V'] [-d data] <url>\r\n"); return; }
    if (!net_ready()) { uart_puts("curl: network not ready\r\n"); return; }
    if (!method) method = head ? "HEAD" : (data ? "POST" : "GET");
    char host[96], path[200]; int port, tls, next; char* uw[1] = { (char*)url };
    if (parse_url_args(uw, 1, host, sizeof(host), &port, path, sizeof(path), &tls, &next) != 0) { uart_puts("curl: bad URL\r\n"); return; }
    uint8_t* resp = 0; int status = 0; uint32_t hl = 0; int total = -1;
    for (int hop = 0; hop < 4; ++hop)
    {
        if (resp) { kfree(resp); resp = 0; }
        total = curl_request(host, port, path, tls, method, hdrs[0] ? hdrs : 0, data, data ? (uint32_t)str_len(data) : 0, &resp, &status, &hl);
        if (total < 0) return;
        if (follow && status >= 300 && status < 400)
        {
            char loc[200];
            if (!curl_find_header(resp, hl, "Location", loc, sizeof(loc))) break;
            if (loc[0] == '/') str_copy(path, loc, sizeof(path));
            else { char* lw[1] = { loc }; if (parse_url_args(lw, 1, host, sizeof(host), &port, path, sizeof(path), &tls, &next) != 0) break; }
            if (!silent) { uart_puts("curl: redirected to "); uart_puts(loc); uart_puts("\r\n"); }
            if (status == 301 || status == 302 || status == 303) { method = "GET"; data = 0; }
            continue;
        }
        break;
    }
    uint32_t blen = (uint32_t)total - hl;
    if (show_hdr) { for (uint32_t i = 0; i < hl; ++i) { if (resp[i] == '\n' && (i == 0 || resp[i - 1] != '\r')) uart_putc('\r'); uart_putc((char)resp[i]); } }
    else if (!silent && outfile) { uart_puts("HTTP status "); print_u64((uint64_t)status); uart_puts("\r\n"); }
    if (outfile)
    {
        if (!head && fs_write_data(outfile, vfs_cwd(), resp + hl, blen) < 0) uart_puts("curl: cannot write output file\r\n");
        else if (!silent) { uart_puts("curl: saved "); uart_puts(outfile); uart_puts(" ("); print_u64(blen); uart_puts(" bytes)\r\n"); }
    }
    else if (!head)
    {
        for (uint32_t i = 0; i < blen; ++i) { if (resp[hl + i] == '\n') uart_putc('\r'); uart_putc((char)resp[hl + i]); }
        if (blen && resp[hl + blen - 1] != '\n') uart_puts("\r\n");
    }
    kfree(resp);
}

/* ntpdate [-q] <server> : SNTP query (UDP 123); sets the RTC unless -q (root only) */
static void command_ntpdate(const char* argument)
{
    char line[120]; str_copy(line, argument ? argument : "", sizeof(line));
    char* w[4]; int n = cw_tokens(line, w, 4);
    int query = 0; const char* server = 0;
    for (int i = 0; i < n; ++i) { if (str_equal(w[i], "-q")) query = 1; else if (!server) server = w[i]; else server = 0; }
    if (!server) { uart_puts("usage: ntpdate [-q] <server>   (-q: only query, do not set the clock)\r\n"); return; }
    if (!net_ready()) { uart_puts("ntpdate: network not ready\r\n"); return; }
    if (!query && !security_is_root()) { uart_puts("ntpdate: setting the clock needs root (use -q to only query)\r\n"); return; }
    uint32_t ip; if (net_resolve_host(server, &ip, "ntpdate") != 0) return;
    int sk = net_udp_socket(0);
    if (sk < 0) { uart_puts("ntpdate: no socket\r\n"); return; }
    uint8_t pkt[48]; for (int i = 0; i < 48; ++i) pkt[i] = 0; pkt[0] = 0x23;
    uint8_t rsp[96]; uint32_t sip = 0; uint16_t sp = 0; int got = -1;
    for (int attempt = 0; attempt < 3 && got < 48; ++attempt)
    {
        if (net_udp_sendto(0, sk, ip, 123, pkt, 48) < 0) { uart_puts("ntpdate: send failed (blocked or no route)\r\n"); net_udp_close(0, sk); return; }
        got = net_udp_recvfrom(0, sk, &sip, &sp, rsp, sizeof(rsp), 1500U);
    }
    net_udp_close(0, sk);
    if (got < 48 || (rsp[0] & 7) != 4 || rsp[1] == 0) { uart_puts("ntpdate: no valid reply from "); uart_puts(server); uart_puts("\r\n"); return; }
    uint32_t secs = ((uint32_t)rsp[40] << 24) | ((uint32_t)rsp[41] << 16) | ((uint32_t)rsp[42] << 8) | rsp[43];
    if (secs < 2208988800U) { uart_puts("ntpdate: invalid timestamp\r\n"); return; }
    uint64_t epoch = (uint64_t)secs - 2208988800ULL;
    uint64_t mine = rtc_get_epoch();
    uart_puts("server "); uart_puts(server); uart_puts(", stratum "); print_u64(rsp[1]); uart_puts(", offset ");
    if (epoch >= mine) { uart_putc('+'); print_u64(epoch - mine); } else { uart_putc('-'); print_u64(mine - epoch); }
    uart_puts(" s\r\n  time: ");
    print_date_format(epoch, "%Y-%m-%d %H:%M:%S UTC");
    if (query) return;
    if (rtc_set_epoch(epoch) != 0) { uart_puts("ntpdate: cannot set the RTC\r\n"); return; }
    uart_puts("ntpdate: clock set\r\n");
}

static const char* fw_proto_name(int p) { return p == 6 ? "tcp" : p == 17 ? "udp" : p == 1 ? "icmp" : "any"; }
static void fw_print_ip(uint32_t ip) { print_u64((ip >> 24) & 255); uart_putc('.'); print_u64((ip >> 16) & 255); uart_putc('.'); print_u64((ip >> 8) & 255); uart_putc('.'); print_u64(ip & 255); }
static void command_fw(const char* argument)
{
    char line[160]; str_copy(line, argument ? argument : "", sizeof(line));
    char* w[8]; int n = cw_tokens(line, w, 8);
    if (n == 0 || str_equal(w[0], "list"))
    {
        int di, dout; uint32_t pi, po; net_fw_get_state(&di, &dout, &pi, &po);
        uart_puts("Packet filter (IPv4)\r\n  default inbound : "); uart_puts(di ? "allow" : "deny");
        uart_puts("\r\n  default outbound: "); uart_puts(dout ? "allow" : "deny"); uart_puts("\r\n");
        uart_puts("  #  action dir  proto remote            port\r\n");
        NetFwRule r; int any = 0;
        for (int i = 0; net_fw_get(i, &r) == 0; ++i)
        {
            any = 1; print_u64((uint64_t)i); uart_puts("  "); uart_puts(r.allow ? "allow " : "deny  "); uart_puts(r.dir == 0 ? "in   " : r.dir == 1 ? "out  " : "both ");
            uart_puts(fw_proto_name(r.proto)); uart_puts("   ");
            if (r.prefix == 0) uart_puts("any"); else { fw_print_ip(r.ip); uart_putc('/'); print_u64(r.prefix); }
            uart_puts("   "); if (r.port) print_u64(r.port); else uart_puts("any"); uart_puts("\r\n");
        }
        if (!any) uart_puts("  (no rules)\r\n");
        uart_puts("  dropped: inbound "); print_u64(pi); uart_puts(", outbound "); print_u64(po); uart_puts("\r\n");
        return;
    }
    if (!security_is_root()) { uart_puts("fw: permission denied (root required)\r\n"); return; }
    if (str_equal(w[0], "flush")) { net_fw_flush(); uart_puts("fw: rules cleared\r\n"); return; }
    if (str_equal(w[0], "default") && n == 3 && (str_equal(w[1], "in") || str_equal(w[1], "out")) && (str_equal(w[2], "allow") || str_equal(w[2], "deny")))
    { net_fw_set_default(w[1][0] == 'o', str_equal(w[2], "allow")); uart_puts("fw: default policy set\r\n"); return; }
    if (str_equal(w[0], "del") && n == 2) { int idx = parse_uint(w[1], -1); if (net_fw_del(idx) != 0) uart_puts("fw: no such rule\r\n"); else uart_puts("fw: rule deleted\r\n"); return; }
    if ((str_equal(w[0], "add") || str_equal(w[0], "insert")) && n >= 4 && n <= 7)
    {
        NetFwRule r; r.used = 1; r.ip = 0; r.prefix = 0; r.port = 0;
        if (str_equal(w[1], "allow")) r.allow = 1; else if (str_equal(w[1], "deny")) r.allow = 0; else goto usage;
        if (str_equal(w[2], "in")) r.dir = 0; else if (str_equal(w[2], "out")) r.dir = 1; else if (str_equal(w[2], "both")) r.dir = 2; else goto usage;
        if (str_equal(w[3], "tcp")) r.proto = 6; else if (str_equal(w[3], "udp")) r.proto = 17; else if (str_equal(w[3], "icmp")) r.proto = 1; else if (str_equal(w[3], "any")) r.proto = 0; else goto usage;
        if (n >= 5 && !str_equal(w[4], "any"))
        {
            char ipb[24]; int k = 0; r.prefix = 32;
            for (; w[4][k] && w[4][k] != '/' && k < 23; ++k) ipb[k] = w[4][k];
            ipb[k] = 0;
            if (w[4][k] == '/') { int pf = parse_uint(w[4] + k + 1, -1); if (pf < 0 || pf > 32) goto usage; r.prefix = (uint8_t)pf; }
            if (!net_parse_ipv4(ipb, &r.ip)) goto usage;
            if (r.prefix == 0) r.ip = 0;
        }
        if (n >= 6 && !str_equal(w[5], "any")) { int pt = parse_uint(w[5], -1); if (pt <= 0 || pt > 65535) goto usage; r.port = (uint16_t)pt; }
        int idx = net_fw_add(&r, str_equal(w[0], "insert") ? 0 : -1);
        if (idx < 0) { uart_puts("fw: rule table full\r\n"); return; }
        uart_puts("fw: rule added at "); print_u64((uint64_t)idx); uart_puts("\r\n");
        return;
    }
usage:
    uart_puts("usage: fw [list] | fw add|insert <allow|deny> <in|out|both> <tcp|udp|icmp|any> [ip[/bits]|any] [port|any]\r\n"
              "       fw del <n> | fw flush | fw default <in|out> <allow|deny>\r\n");
}

static void command_dnsserver(const char* argument)
{
    if (!security_is_root())
    {
        uart_puts("dnsserver: permission denied (root required)\r\n");
        return;
    }
    if (!argument || !argument[0])
    {
        uart_puts("dnsserver: usage: dnsserver <ipv4>\r\n");
        return;
    }
    uint32_t ip = 0;
    if (!net_parse_ipv4(argument, &ip) || net_dns_set_server(ip) != 0)
    {
        uart_puts("dnsserver: invalid DNS server address\r\n");
        return;
    }
    uart_puts("DNS server set to "); net_print_ipv4(ip); uart_puts("\r\n");
}

static void command_selftest()
{
    int pass=0,total=0;
    uart_puts("MyKernel self-test\r\n==================\r\n");
    ++total; if (mmu_enabled()) {uart_puts("[PASS] MMU enabled\r\n");++pass;} else uart_puts("[FAIL] MMU enabled\r\n");
    ++total; if (storage_ready()) {uart_puts("[PASS] storage ready\r\n");++pass;} else uart_puts("[FAIL] storage ready\r\n");
    ++total; if (fs_persistent()) {uart_puts("[PASS] filesystem mounted\r\n");++pass;} else uart_puts("[FAIL] filesystem mounted\r\n");
    ++total; {
        const char* test_path = "/storage/home/.mkselftest";
        uint8_t out[16]; uint64_t got = 0; const char* marker = "persistent-ok";
        int old = fs_resolve(test_path, fs_get_root());
        if (old >= 0) (void)fs_rm(test_path, fs_get_root());
        int ok = fs_touch(test_path, fs_get_root()) >= 0 &&
                 fs_write_data(test_path, fs_get_root(), (const uint8_t*)marker, 13) == 0 &&
                 fs_read_file(test_path, fs_get_root(), out, sizeof(out), &got) == 0 &&
                 got == 13;
        if (ok) for (int i=0;i<13;i++) if (out[i] != (uint8_t)marker[i]) ok = 0;
        if (fs_rm(test_path, fs_get_root()) != 0) ok = 0;
        if(ok){uart_puts("[PASS] persistent file I/O\r\n");++pass;} else uart_puts("[FAIL] persistent file I/O\r\n");
    }
    ++total; uint64_t sp=syscall_invoke(SYS_PING); if(sp==0x4D594B56ULL){uart_puts("[PASS] SVC/syscall\r\n");++pass;}else uart_puts("[FAIL] SVC/syscall\r\n");
    ++total; if (pages_selftest()) {uart_puts("[PASS] page allocator\r\n");++pass;} else uart_puts("[FAIL] page allocator\r\n");
    ++total; int pid=process_create("selftest"); if(pid>=0){process_set_state(pid,PROCESS_READY);scheduler_add(pid);scheduler_remove(pid);process_destroy(pid);uart_puts("[PASS] process lifecycle\r\n");++pass;}else uart_puts("[FAIL] process lifecycle\r\n");
    ++total; uint64_t entry=0; if(elf_load("/bin/init.elf",&entry)==0 && entry!=0){uart_puts("[PASS] ELF loader\r\n");++pass;}else uart_puts("[FAIL] ELF loader\r\n"); mmu_user_prepare();
    ++total; if(virtio_net_present()){uart_puts("[PASS] VirtIO-Net detected\r\n");++pass;}else uart_puts("[SKIP] VirtIO-Net unavailable\r\n");
    uart_puts("Summary: ");print_u64(pass);uart_putc('/');print_u64(total);uart_puts(" checks passed.\r\n");
}

static void command_man(const char* argument)
{
    char query[64];
    query[0] = '\0';
    if (argument && argument[0])
    {
        int i = 0;
        while (argument[i] && argument[i] != ' ' && argument[i] != '\t' && i < (int)sizeof(query) - 1)
        {
            query[i] = argument[i];
            ++i;
        }
        query[i] = '\0';
        while (argument[i] == ' ' || argument[i] == '\t') ++i;
        if (argument[i] != '\0')
        {
            uart_puts("man: usage: man <command>\r\n");
            return;
        }
    }
    if (!query[0])
    {
        uart_puts("man: usage: man <command>\r\n");
        uart_puts("Available manuals: ls cat grep find chmod cp mv rm mkdir echo wc du df nano tree mount ps history threads thread-demo unset fds ifconfig nslookup httpget dnsserver\r\n");
        return;
    }

    for (unsigned i = 0; i < sizeof(manual_entries) / sizeof(manual_entries[0]); ++i)
    {
        const ManualEntry& m = manual_entries[i];
        if (str_equal(query, m.name))
        {
            uart_puts("NAME: ");
            uart_puts(m.name);
            uart_puts(" - ");
            uart_puts(m.description);
            uart_puts("\r\nUSAGE: ");
            uart_puts(m.usage);
            uart_puts("\r\nOPTIONS: ");
            uart_puts(m.options);
            uart_puts("\r\n");
            return;
        }
    }
    uart_puts("man: no manual entry for ");
    uart_puts(query);
    uart_puts("\r\n");
}

static void execute_command_inner(char* line);

void execute_command(
    char* line
)
{
    if (line == 0) return;
    char* w = line;
    while (*w == ' ') ++w;
    char name[24]; int nl = 0;
    while (w[nl] && w[nl] != ' ' && nl < 23) { name[nl] = w[nl]; ++nl; }
    name[nl] = 0;
    UartProbeState saved;
    uart_error_probe_begin(name, nl, &saved);
    execute_command_inner(line);
    int hit = uart_error_probe_end(&saved);
    if (hit == 2) script_set_status(127);
    else if (hit == 1) script_set_status(1);
}

static void execute_command_inner(
    char* line
)
{
    if (line == 0)
        return;

    while (*line == ' ')
        line++;

    if (*line == '\0')
        return;

    {
        static char aliased[1024];
        if (shellext_alias_expand(line, aliased, sizeof(aliased))) line = aliased;
    }

    /* Compound statements (if/for/while) and sh/source run before expansion. */
    if (script_handle_compound(line))
        return;

    /* Expand $VAR $? $# $0-$9 before parsing, then handle shell operators. */
    char expanded[1024];
    script_expand(line, expanded, sizeof(expanded));
    line = expanded;
    normalize_shell_whitespace(line);
    if (*line == '\0') return;

    if (script_handle_simple(line))
        return;
    script_set_status(0);

    if (jobs_handle_line(line))
        return;

    if (filters_handle_pipeline(line))
        return;

    if (filters_handle_redirect(line))
        return;

    {
        static char globbed[1024];
        filters_glob(line, globbed, sizeof(globbed));
        if (!str_equal(globbed, line))
        {
            /* single-operand built-ins run once per expanded name */
            char tmp[1024]; str_copy(tmp, globbed, sizeof(tmp));
            char* w[40]; int n = 0; char* p = tmp;
            while (*p && n < 40) { while (*p == ' ') ++p; if (!*p) break; w[n++] = p; while (*p && *p != ' ') ++p; if (*p) *p++ = 0; }
            const char* c = n ? w[0] : "";
            if (n > 2 && (str_equal(c, "ls") || str_equal(c, "cat") || str_equal(c, "wc") || str_equal(c, "rm") || str_equal(c, "stat") || str_equal(c, "head") || str_equal(c, "tail")))
            {
                int fl = 1; while (fl < n && w[fl][0] == '-') ++fl;
                if (n - fl > 1)
                {
                    for (int k = fl; k < n; ++k)
                    {
                        char one[300]; int op = 0;
                        for (const char* q = c; *q && op < 250; ++q) one[op++] = *q;
                        for (int f = 1; f < fl; ++f) { one[op++] = ' '; for (const char* q = w[f]; *q && op < 270; ++q) one[op++] = *q; }
                        one[op++] = ' ';
                        for (const char* q = w[k]; *q && op < 298; ++q) one[op++] = *q;
                        one[op] = 0;
                        if (!str_equal(c, "rm") && !str_equal(c, "ls")) { uart_puts("==> "); uart_puts(w[k]); uart_puts(" <==\r\n"); }
                        execute_command(one);
                    }
                    return;
                }
            }
        }
        str_copy(expanded, globbed, sizeof(expanded));
        line = expanded;
    }

    if (handle_shell_operators(line))
        return;

    char* command = line;
    char* argument = 0;

    while (*line)
    {
        if (*line == ' ')
        {
            *line = '\0';

            line++;

            while (*line == ' ')
                line++;

            if (*line)
                argument = line;

            break;
        }

        line++;
    }

    if (str_equal(command, "help"))
    {
        command_help(argument);
        return;
    }

    if (str_equal(command, "man"))
    {
        command_man(argument);
        return;
    }

    if (str_equal(command, "clear"))
    {
        uart_puts("\x1B[2J");
        uart_puts("\x1B[H");
        return;
    }

    if (str_equal(command, "version"))
    {
        command_version();
        return;
    }

    if (str_equal(command, "mem"))
    {
        command_mem();
        return;
    }

    if (str_equal(command, "hello"))
    {
        uart_puts(
            "Hello from MyKernel!\r\n"
        );

        return;
    }

    if (str_equal(command, "echo"))
    {
        command_echo(argument);
        return;
    }

    if (str_equal(command, "grep")) { command_grep(argument); return; }
    if (str_equal(command, "find")) { command_find(argument); return; }
    if (shellext_handle(command, argument)) return;
    if (str_equal(command, "history")) { command_history(argument); return; }
    if (str_equal(command, "date")) { command_date(argument); return; }
    if (str_equal(command, "du")) { command_du(argument); return; }
    if (str_equal(command, "df")) { command_df(argument); return; }
    if (str_equal(command, "wc")) { command_wc(argument); return; }
    if (str_equal(command, "chmod")) { command_chmod(argument); return; }
    if (str_equal(command, "stat")) { command_stat(argument); return; }
    if (str_equal(command, "chown")) { command_chown(argument); return; }
    if (str_equal(command, "truncate")) { command_truncate(argument); return; }
    if (str_equal(command, "env")) { command_env(argument); return; }
    if (str_equal(command, "export")) { command_export(argument); return; }
    if (str_equal(command, "unset")) { command_unset(argument); return; }

    if (str_equal(command, "ls"))
    {
        vfs_ls(argument);
        return;
    }

    if (str_equal(command, "pwd"))
    {
        vfs_pwd();
        return;
    }

    if (str_equal(command, "cd"))
    {
        command_cd(argument);
        return;
    }

    if (str_equal(command, "mkdir"))
    {
        command_mkdir(argument);
        return;
    }

    if (str_equal(command, "touch"))
    {
        command_touch(argument);
        return;
    }

    if (str_equal(command, "write"))
    {
        command_write(argument);
        return;
    }

    if (str_equal(command, "cat"))
    {
        command_cat(argument);
        return;
    }

    if (str_equal(command, "rm"))
    {
        command_rm(argument);
        return;
    }

    if (str_equal(command, "cp"))
    {
        command_cp(argument);
        return;
    }

    if (str_equal(command, "mv"))
    {
        command_mv(argument);
        return;
    }

    if (str_equal(command, "tree"))
    {
        vfs_tree_path(argument && argument[0] ? argument : 0);
        return;
    }

    if (str_equal(command, "ps"))
    {
        process_list();
        return;
    }

    if (str_equal(command, "sched"))
    {
        scheduler_status();
        return;
    }
    if (str_equal(command, "mmu"))
    {
        mmu_status();
        return;
    }

    if (str_equal(command, "disk"))
    {
        command_disk();
        return;
    }

    if (str_equal(command, "diskread"))
    {
        command_diskread(argument);
        return;
    }

    if (str_equal(command, "diskwrite"))
    {
        command_diskwrite(argument);
        return;
    }

    if (str_equal(command, "diskflush"))
    {
        command_diskflush();
        return;
    }

    if (str_equal(command, "apps")) { command_apps(); return; }
    if (str_equal(command, "uname")) { command_uname(); return; }
    if (str_equal(command, "whoami")) { command_whoami(); return; }
    if (str_equal(command, "useradd")) { command_useradd(argument); return; }
    if (str_equal(command, "userdel")) { command_userdel(argument); return; }
    if (str_equal(command, "passwd")) { command_passwd(argument); return; }
    if (str_equal(command, "id")) { command_id(); return; }
    if (str_equal(command, "users")) { security_list_accounts(); return; }
    if (str_equal(command, "net")) { command_net(); return; }
    if (str_equal(command, "ifconfig")) { command_net(); return; }
    if (str_equal(command, "ping")) { command_ping(argument); return; }
    if (str_equal(command, "nslookup")) { command_nslookup(argument); return; }
    if (str_equal(command, "httpget")) { command_httpget(argument); return; }
    if (str_equal(command, "dnsserver")) { command_dnsserver(argument); return; }
    if (str_equal(command, "selftest")) { command_selftest(); return; }
    if (str_equal(command, "pages")) { command_pages(); return; }
    if (str_equal(command, "tasks")) { command_tasks(); return; }
    if (str_equal(command, "threads")) { command_threads(); return; }
    if (str_equal(command, "smp")) { command_smp_status(argument); return; }
    if (str_equal(command, "fbinfo")) { command_fbinfo(); return; }
    if (str_equal(command, "fds")) { fd_list(); return; }
    if (str_equal(command, "thread-demo")) { command_thread_demo(); return; }
    if (filters_run_file_command(command, argument)) return;
    if (str_equal(command, "seq")) { command_seq(argument); return; }
    if (str_equal(command, "basename")) { command_basename(argument, 0); return; }
    if (str_equal(command, "dirname")) { command_basename(argument, 1); return; }
    if (str_equal(command, "poweroff") || str_equal(command, "halt") || str_equal(command, "shutdown")) { command_power(0); return; }
    if (str_equal(command, "reboot")) { command_power(1); return; }
    if (str_equal(command, "sleep")) { command_sleep(argument); return; }
    if (str_equal(command, "rmdir")) { command_rmdir(argument); return; }
    if (str_equal(command, "free")) { command_free(); return; }
    if (str_equal(command, "which")) { command_which(argument); return; }
    if (str_equal(command, "dhcp")) { command_dhcp(argument); return; }
    if (str_equal(command, "nc")) { command_nc(argument); return; }
    if (str_equal(command, "udp6")) { command_udp6(argument); return; }
    if (str_equal(command, "telnet")) { command_telnet(argument); return; }
    if (str_equal(command, "wget")) { command_wget(argument); return; }
    if (str_equal(command, "curl")) { command_curl(argument); return; }
    if (str_equal(command, "ntpdate")) { command_ntpdate(argument); return; }
    if (str_equal(command, "certs")) { command_certs(argument); return; }
    if (str_equal(command, "ssh")) { command_ssh(argument); return; }
    if (str_equal(command, "scp")) { command_scp(argument); return; }
    if (str_equal(command, "fw")) { command_fw(argument); return; }
    if (str_equal(command, "httpsget")) { command_httpsget(argument); return; }
    if (str_equal(command, "ping6")) { command_ping6(argument); return; }
    if (str_equal(command, "httpd")) { command_httpd(argument); return; }
    if (str_equal(command, "ftpd")) { command_ftpd(argument); return; }
    if (str_equal(command, "lspci")) { command_lspci(); return; }
    if (str_equal(command, "usb")) { command_usb(argument); return; }
    if (str_equal(command, "nvme")) { command_nvme(argument); return; }
    if (str_equal(command, "sata")) { command_sata(argument); return; }
    if (str_equal(command, "umount")) { command_umount(argument); return; }
    if (str_equal(command, "sync")) { command_sync(); return; }
    if (str_equal(command, "ln")) { command_ln(argument); return; }
    if (str_equal(command, "readlink")) { command_readlink(argument); return; }
    if (str_equal(command, "realpath")) { if (!argument || !argument[0]) uart_puts("usage: realpath <path>\r\n"); else command_realpath(argument); return; }
    if (str_equal(command, "jtest")) { command_jtest(argument); return; }
    if (str_equal(command, "mount")) { command_mount(argument); return; }
    if (str_equal(command, "mkfs")) { command_mkfs(argument); return; }
    if (str_equal(command, "fsck")) { command_fsck(argument); return; }
    if (str_equal(command, "fat")) { command_fat(argument); return; }
    if (str_equal(command, "quota")) { command_quota(argument); return; }
    if (str_equal(command, "stats")) { command_stats(); return; }
    if (str_equal(command, "log")) { command_log(argument); return; }
    if (str_equal(command, "irq")) { command_irq(argument); return; }

    if (str_equal(command, "level"))
    {
        uint64_t current_el = 0;
        asm volatile("mrs %0, CurrentEL" : "=r"(current_el));
        current_el = (current_el >> 2) & 3ULL;
        uart_puts("Current EL: ");
        print_u64(current_el);
        uart_puts("\r\n");
        return;
    }

    if (str_equal(command, "user"))
    {
        command_user_login(argument);
        return;
    }

    if (str_equal(command, "userdemo"))
    {
        user_execute("/bin/init.elf");
        return;
    }

    if (str_equal(command, "su"))
    {
        command_su(argument);
        return;
    }

    if (str_equal(command, "logout") || str_equal(command, "exit"))
    {
        command_logout();
        return;
    }

    if (str_equal(command, "nano"))
    {
        if (argument == 0 || argument[0] == '\0')
        {
            uart_puts("nano: usage: nano <file>\r\n");
            return;
        }
        if (str_len(argument) >= USER_ARGS_MAX)
        {
            uart_puts("nano: file name too long\r\n");
            return;
        }
        int existing = fs_resolve(argument, vfs_cwd());
        if (existing >= 0 && fs_get_type(existing) == FS_DIR)
        {
            uart_puts("nano: is a directory\r\n");
            return;
        }
        user_execute("/bin/nano.elf", argument);
        return;
    }

    if (str_equal(command, "calc"))
    {
        user_execute("/bin/calc.elf", argument);
        return;
    }

    if (str_equal(command, "exec"))
    {
        if (argument == 0 || argument[0] == '\0')
        {
            uart_puts("exec: usage: exec <file> [args]\r\n");
            return;
        }

        char path[96];
        char args[USER_ARGS_MAX];
        int pi = 0;
        const char* p = argument;

        while (*p == ' ') ++p;
        while (*p && *p != ' ' && pi < (int)sizeof(path) - 1)
            path[pi++] = *p++;
        path[pi] = '\0';
        {   /* a bare program name resolves like at the prompt (/bin/<name>[.elf]) */
            char resolved[96];
            if (!str_has_slash(path) && jobs_resolve_program(path, resolved, (int)sizeof(resolved)) > 0)
            { int k = 0; while (resolved[k] && k < (int)sizeof(path) - 1) { path[k] = resolved[k]; ++k; } path[k] = '\0'; }
        }

        while (*p == ' ') ++p;
        int ai = 0;
        while (*p && ai < USER_ARGS_MAX - 1)
            args[ai++] = *p++;
        args[ai] = '\0';

        user_execute(path, args);
        return;
    }

    if (str_equal(command, "elf"))
    {
        elf_status();
        return;
    }

    if (str_equal(command, "syscall"))
    {
        if (argument == 0 || argument[0] == '\0')
        {
            uart_puts(
                "syscall: usage: syscall <ping|uptime|pid|tick|mem|write|fd> [text]\r\n"
            );
            return;
        }

        if (str_equal(argument, "ping"))
        {
            uint64_t result = syscall_invoke(SYS_PING);

            uart_puts("syscall ping: ");
            if (result == 0x4D594B56ULL)
                uart_puts("OK");
            else
                uart_puts("FAILED");
            uart_puts("\r\n");
            return;
        }

        if (str_equal(argument, "uptime"))
        {
            uint64_t result = syscall_invoke(SYS_UPTIME);
            uart_puts("syscall uptime: ");
            print_u64(result);
            uart_puts(" seconds\r\n");
            return;
        }

        if (str_equal(argument, "pid"))
        {
            uint64_t result = syscall_invoke(SYS_GETPID);
            uart_puts("syscall pid: ");
            print_u64(result);
            uart_puts("\r\n");
            return;
        }

        if (str_equal(argument, "tick"))
        {
            uint64_t result = syscall_invoke(SYS_SCHED_TICK);
            uart_puts("syscall scheduler ticks: ");
            print_u64(result);
            uart_puts("\r\n");
            return;
        }

        if (str_equal(argument, "mem"))
        {
            uint64_t result = syscall_invoke(SYS_MEM_FREE);
            uart_puts("syscall heap free: ");
            print_u64(result);
            uart_puts(" bytes\r\n");
            return;
        }

        if (str_equal(argument, "fd"))
        {
            const char* path = "/storage/home/.rioos-fd-selftest.tmp";
            static const char payload[] = "fd-ok";
            char received[sizeof(payload)];
            for (unsigned i = 0; i < sizeof(received); ++i) received[i] = '\0';

            if (fs_resolve(path, vfs_cwd()) >= 0)
            {
                uart_puts("syscall fd: refusing to overwrite existing test file\r\n");
                return;
            }

            fd_close_all();
            int64_t descriptor = (int64_t)syscall_invoke(
                SYS_FD_OPEN, (uint64_t)path,
                FD_O_CREAT | FD_O_RDWR | FD_O_TRUNC, 0, 0, 0);
            int passed = descriptor >= 3;
            if (passed)
            {
                uint64_t written = syscall_invoke(SYS_FD_WRITE,
                    (uint64_t)descriptor, (uint64_t)payload, sizeof(payload) - 1, 0, 0, 0);
                uint64_t sought = syscall_invoke(SYS_FD_SEEK,
                    (uint64_t)descriptor, 0, 0, 0, 0, 0);
                uint64_t read_count = syscall_invoke(SYS_FD_READ,
                    (uint64_t)descriptor, (uint64_t)received, sizeof(payload) - 1, 0, 0, 0);
                passed = written == sizeof(payload) - 1 && sought == 0 &&
                         read_count == sizeof(payload) - 1;
                for (unsigned i = 0; passed && i < sizeof(payload); ++i)
                    if (received[i] != payload[i]) passed = 0;

                /* Duplicates share the underlying open-file cursor. */
                int duplicate = (int)(int64_t)syscall_invoke(
                    SYS_FD_DUP, (uint64_t)descriptor, 0, 0, 0, 0, 0);
                char dup_bytes[4] = {0, 0, 0, 0};
                int dup_ok = duplicate >= 3 &&
                    syscall_invoke(SYS_FD_SEEK, (uint64_t)descriptor, 0, 0, 0, 0, 0) == 0 &&
                    syscall_invoke(SYS_FD_READ, (uint64_t)duplicate,
                        (uint64_t)dup_bytes, 2, 0, 0, 0) == 2 &&
                    syscall_invoke(SYS_FD_READ, (uint64_t)descriptor,
                        (uint64_t)(dup_bytes + 2), 2, 0, 0, 0) == 2 &&
                    dup_bytes[0] == 'f' && dup_bytes[1] == 'd' &&
                    dup_bytes[2] == '-' && dup_bytes[3] == 'o';
                if (duplicate >= 3 && syscall_invoke(
                    SYS_FD_CLOSE, (uint64_t)duplicate, 0, 0, 0, 0, 0) != 0)
                    dup_ok = 0;
                if (!dup_ok) passed = 0;

                if (syscall_invoke(SYS_FD_CLOSE, (uint64_t)descriptor, 0, 0, 0, 0, 0) != 0)
                    passed = 0;
            }

            /* The same ordinary read/write syscalls must support a kernel pipe. */
            int pipefd[2] = {-1, -1};
            static const char pipe_payload[] = "pipe-ok";
            char pipe_received[sizeof(pipe_payload)];
            for (unsigned i = 0; i < sizeof(pipe_received); ++i) pipe_received[i] = '\0';
            int pipe_ok = syscall_invoke(SYS_FD_PIPE, (uint64_t)pipefd, 0, 0, 0, 0, 0) == 0;
            if (pipe_ok)
            {
                uint64_t sent = syscall_invoke(SYS_FD_WRITE, (uint64_t)pipefd[1],
                    (uint64_t)pipe_payload, sizeof(pipe_payload) - 1, 0, 0, 0);
                uint64_t got = syscall_invoke(SYS_FD_READ, (uint64_t)pipefd[0],
                    (uint64_t)pipe_received, sizeof(pipe_payload) - 1, 0, 0, 0);
                pipe_ok = sent == sizeof(pipe_payload) - 1 && got == sent;
                for (unsigned i = 0; pipe_ok && i < sizeof(pipe_payload) - 1; ++i)
                    if (pipe_received[i] != pipe_payload[i]) pipe_ok = 0;
                if (syscall_invoke(SYS_FD_CLOSE, (uint64_t)pipefd[1], 0, 0, 0, 0, 0) != 0)
                    pipe_ok = 0;
                if (syscall_invoke(SYS_FD_READ, (uint64_t)pipefd[0],
                    (uint64_t)pipe_received, 1, 0, 0, 0) != 0) pipe_ok = 0;
                if (syscall_invoke(SYS_FD_CLOSE, (uint64_t)pipefd[0], 0, 0, 0, 0, 0) != 0)
                    pipe_ok = 0;
            }
            if (!pipe_ok) passed = 0;

            fd_close_all();
            (void)fs_rm(path, vfs_cwd());
            uart_puts(passed ? "syscall fd: PASS (open/read/write/seek/close/dup/pipe)\r\n"
                             : "syscall fd: FAIL\r\n");
            return;
        }

        if (argument[0] == 'w' &&
            argument[1] == 'r' &&
            argument[2] == 'i' &&
            argument[3] == 't' &&
            argument[4] == 'e' &&
            argument[5] == ' ')
        {
            uint64_t result =
                syscall_invoke(
                    SYS_WRITE,
                    (uint64_t)(argument + 6)
                );

            uart_puts("\r\nsyscall write bytes: ");
            print_u64(result);
            uart_puts("\r\n");
            return;
        }

        uart_puts("syscall: unknown test\r\n");
        return;
    }

    if (str_equal(command, "uptime"))
    {
        uart_puts(
            "Uptime: "
        );

        print_u64(
            timer_seconds()
        );

        uart_puts(
            " seconds\r\n"
        );

        return;
    }

    {
        /* Not a built-in: run it as a program (/bin/<name>[.elf], absolute or ./path). */
        char program[96];
        if (jobs_resolve_program(command, program, sizeof(program)))
        {
            user_execute(program, argument ? argument : "");
            return;
        }
    }

    uart_puts(
        "Unknown command: "
    );

    uart_puts(
        command
    );

    uart_puts(
        "\r\n"
    );
}