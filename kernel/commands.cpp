#include "commands.h"

#include "uart.h"
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
        irq_quiesce();

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
        uart_puts("Timer IRQ disabled; CPU IRQ masked.\r\n");
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

    uart_puts("Usage: irq [status|off|oneshot|stagec [hz]|staged [hz]|selftest]\r\n");
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

static void command_help()
{
    uart_puts(
        "Available commands:\r\n"
        "  help                 Show help\r\n"
        "  clear                Clear screen\r\n"
        "  version              Show kernel version\r\n"
        "  mem                  Show memory\r\n"
        "  echo <text>          Print text\r\n"
        "  hello                Test kernel\r\n"
        "  ls                   List directory\r\n"
        "  pwd                  Show current path\r\n"
        "  cd <path>            Change directory\r\n"
        "  mkdir <name>         Create directory\r\n"
        "  touch <name>         Create file\r\n"
        "  write <file> <text>  Write file\r\n"
        "  cat <file>           Read file\r\n"
        "  rm <file>            Delete file\r\n"
        "  tree                 Show filesystem tree\r\n"
        "  ps                   Show processes\r\n"
        "  uptime               Show uptime\r\n"
        "  sched                Show scheduler status\r\n"
        "  exec <file>          Execute an ELF program\r\n"
        "  elf                  Show ELF loader status\r\n"
        "  syscall <test>        Test syscall interface\r\n"
        "  user                 Run EL0 userspace demo\r\n"
        "  level                Show current exception level\r\n"
        "  mmu                  Show MMU status\r\n"
        "  disk                 Show VirtIO disk status\r\n"
        "  diskread <sector>    Read and inspect a 512-byte sector\r\n"
        "  diskwrite <sector> <text>  Write text to a sector\r\n"
        "  diskflush            Flush disk cache\r\n"
        "  apps                 List bundled user programs\r\n"
        "  uname                Show kernel identity\r\n"
        "  whoami               Show current security domain\r\n"
        "  net                  Show network status\r\n"
        "  ping <ip>            Send ICMP echo request\r\n"
        "  selftest             Run kernel integration checks\r\n"
        "  pages                Show physical page allocator\r\n"
        "  irq [status|off|oneshot|stagec|staged|selftest] [hz]  Timer IRQ bring-up\r\n"
    );
}

static void command_version()
{
    uart_puts(
        "MyKernel v" MYKERNEL_VERSION_STRING "\r\n"
        "ARM64 Bare-Metal Kernel\r\n"
        "QEMU virt machine\r\n"
    );
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

    uart_puts(text);
    uart_puts("\r\n");
}

static void command_mkdir(
    const char* arg
)
{
    if (arg == 0 ||
        arg[0] == '\0')
    {
        uart_puts(
            "mkdir: missing operand\r\n"
        );

        return;
    }

    int result =
        vfs_mkdir(arg);

    if (result == -2)
    {
        uart_puts(
            "mkdir: already exists\r\n"
        );
    }
    else if (result < 0)
    {
        uart_puts(
            "mkdir: cannot create directory\r\n"
        );
    }
}

static void command_touch(
    const char* arg
)
{
    if (arg == 0 ||
        arg[0] == '\0')
    {
        uart_puts(
            "touch: missing operand\r\n"
        );

        return;
    }

    int result =
        vfs_touch(arg);

    if (result == -2)
    {
        uart_puts(
            "touch: already exists\r\n"
        );
    }
    else if (result < 0)
    {
        uart_puts(
            "touch: cannot create file\r\n"
        );
    }
}

static void command_cd(
    const char* arg
)
{
    int result =
        vfs_cd(arg);

    if (result == -1)
    {
        uart_puts(
            "cd: no such directory\r\n"
        );
    }
    else if (result == -2)
    {
        uart_puts(
            "cd: not a directory\r\n"
        );
    }
}

static void command_cat(
    const char* arg
)
{
    if (arg == 0 ||
        arg[0] == '\0')
    {
        uart_puts(
            "cat: missing operand\r\n"
        );

        return;
    }

    int result =
        vfs_cat(arg);

    if (result == -1)
    {
        uart_puts(
            "cat: file not found\r\n"
        );
    }
    else if (result == -2)
    {
        uart_puts(
            "cat: is a directory\r\n"
        );
    }
    else
    {
        uart_puts(
            "\r\n"
        );
    }
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

static void command_rm(
    const char* arg
)
{
    if (arg == 0 ||
        arg[0] == '\0')
    {
        uart_puts(
            "rm: missing operand\r\n"
        );

        return;
    }

    int result =
        vfs_rm(arg);

    if (result == -1)
    {
        uart_puts(
            "rm: not found\r\n"
        );
    }
    else if (result == -2)
    {
        uart_puts(
            "rm: directory not empty\r\n"
        );
    }
    else if (result == -3)
    {
        uart_puts(
            "rm: cannot remove root\r\n"
        );
    }
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
    int rc = storage_flush();
    uart_puts("diskflush: ");
    uart_puts(rc == 0 ? "OK\r\n" : "failed\r\n");
}


static void command_apps()
{
    uart_puts("Bundled user programs\r\n----------------------\r\n");
    uart_puts("/bin/init.elf    - userspace self-test/demo\r\n");
}

static void command_uname()
{
    uart_puts("MyKernel "); uart_puts(MYKERNEL_VERSION_STRING); uart_puts(" ARM64 bare-metal\r\n");
}

static void command_whoami()
{
    int pid = user_current_pid();
    uart_puts(pid >= 0 ? "userspace\r\n" : "kernel\r\n");
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

void execute_command(
    char* line
)
{
    if (line == 0)
        return;

    while (*line == ' ')
        line++;

    if (*line == '\0')
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
        command_help();
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

    if (str_equal(command, "ls"))
    {
        vfs_ls();
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

    if (str_equal(command, "tree"))
    {
        vfs_tree();
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
    if (str_equal(command, "net")) { command_net(); return; }
    if (str_equal(command, "ping")) { command_ping(argument); return; }
    if (str_equal(command, "selftest")) { command_selftest(); return; }
    if (str_equal(command, "pages")) { command_pages(); return; }
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
        user_execute("/bin/init.elf");
        return;
    }

    if (str_equal(command, "exec"))
    {
        if (argument == 0 || argument[0] == '\0')
        {
            uart_puts("exec: usage: exec <file>\r\n");
            return;
        }
        user_execute(argument);
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
                "syscall: usage: syscall <ping|uptime|pid|tick|mem|write> [text]\r\n"
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