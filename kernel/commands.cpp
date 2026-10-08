#include "commands.h"

#include "uart.h"
#include "fb.h"
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
#include "rtc.h"

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

static void command_mount()
{
    uart_puts("Mounts\r\n------\r\n");
    uart_puts("block device   : "); uart_puts(storage_ready() ? "virtio-blk (ready)" : "none");
    uart_puts("\r\nroot (/)       : myfs, ");
    uart_puts(fs_persistent() ? "persistent (disk-backed)" : "RAM only (not persistent)");
    uart_puts("\r\n");
}

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
    {"find", "search filesystem entries", "find [path] -name <name> | -type <f/d>", "-name match entry name; -type f files; -type d directories"},
    {"chmod", "change file mode", "chmod <mode> <file>", "numeric 000-777; symbolic +r/+w/+x or -r/-w/-x"},
    {"cp", "copy files or directories", "cp [-r] [-f] <source> <destination>", "-r recursive; -f overwrite destination"},
    {"mv", "move or rename files", "mv [-f] <source> <destination>", "-f overwrite destination"},
    {"rm", "remove files or directories", "rm [-r] [-f] <file|dir>...", "-r recursive; -f force/ignore missing"},
    {"mkdir", "create a directory", "mkdir [-p] <dir>", "-p create parent directories as needed"},
    {"echo", "print text", "echo <text>", "With > or >>, redirection writes a complete line"},
    {"wc", "count lines, words, and characters", "wc [-l] [-w] [-c] <file>", "-l lines; -w words; -c characters"},
    {"du", "show disk usage", "du [-h] [-s] [path]", "-h human readable; -s summary"},
    {"df", "show filesystem free/used space", "df [-h]", "-h human readable"},
    {"nano", "edit a text file", "nano <file>", "Runs the bundled text editor"},
    {"tree", "show filesystem tree", "tree", "No options"},
    {"mount", "show mounted storage/filesystem status", "mount", "No options"},
    {"ps", "show processes", "ps", "No options"},
    {"history", "show command history", "history [10|-c]", "-c clear history; N show last N entries; Arrow Up/Down recall"}
};

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
        (type_filter == 0 || (type_filter == 'f' && fs_get_type(id) == FS_FILE) || (type_filter == 'd' && fs_get_type(id) == FS_DIR)))
    {
        uart_puts(display_path); uart_puts("\r\n");
    }
    if (fs_get_type(id) != FS_DIR) return;
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
        else if (str_equal(words[i], "-type") && i + 1 < count) { type_filter = words[++i][0]; if (type_filter != 'f' && type_filter != 'd') { uart_puts("find: type must be f or d\r\n"); return; } }
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

static uint64_t du_sum(int id)
{
    if (fs_get_type(id) == FS_FILE) return fs_file_size(id);
    uint64_t sum = 0;
    int n = fs_get_child_count(id);
    for (int i = 0; i < n; ++i) sum += du_sum(fs_get_child(id, i));
    return sum;
}

static void command_du(char* argument)
{
    char* words[6]; char* operands[3]; int count = split_words(argument, words, 6);
    int options = 0; int n = collect_operands(words, count, operands, 2, &options);
    if (n < 0 || (options & ~(OPT_H|OPT_S)) || n > 1) { uart_puts("du: usage: du [-h] [-s] [path]\r\n"); return; }
    const char* path = n == 1 ? operands[0] : ".";
    int id = fs_resolve(path, vfs_cwd());
    if (id < 0) { uart_puts("du: path not found\r\n"); return; }
    uint64_t total = du_sum(id);
    if (options & OPT_H) print_human_bytes(total); else print_u64(total);
    uart_puts("  "); uart_puts(path); uart_puts("\r\n");
    if (!(options & OPT_S) && fs_get_type(id) == FS_DIR)
    {
        int c = fs_get_child_count(id);
        for (int i = 0; i < c; ++i)
        {
            int child = fs_get_child(id, i); if (child < 0) continue;
            uint64_t v = du_sum(child);
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
    uint64_t total = fs_total_sectors() * 512ULL;
    uint64_t used = fs_used_sectors() * 512ULL;
    uint64_t freeb = total > used ? total - used : 0;
    uart_puts("Filesystem  Total  Used  Free\r\n");
    if (options & OPT_H) { print_human_bytes(total); uart_putc(' '); print_human_bytes(used); uart_putc(' '); print_human_bytes(freeb); }
    else { print_u64(total); uart_putc(' '); print_u64(used); uart_putc(' '); print_u64(freeb); }
    uart_puts("  /\r\n");
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
    for (int i = 0; line[i]; ++i)
    {
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
                str_copy((char*)pipebuf, left + 5, sizeof(pipebuf)); psz = (uint64_t)str_len((char*)pipebuf);
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
            uart_puts("pipe: right side must be grep\r\n"); return 1;
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

static void command_help()
{
    uart_puts(
        "Available commands:\r\n"
        "  help                 Show help\r\n"
        "  man <command>        Show per-command manual\r\n"
        "  clear                Clear screen\r\n"
        "  version              Show kernel version\r\n"
        "  mem                  Show memory\r\n"
        "  echo <text>          Print text\r\n"
        "  hello                Test kernel\r\n"
        "  ls [options] [dir]   List files (-l long, -a hidden, -R recursive)\r\n"
        "  pwd                  Show current path\r\n"
        "  cd <path>            Change directory\r\n"
        "  mkdir <name>         Create directory; -p creates parents\r\n"
        "  touch <name>         Create/update file; -c skips missing files\r\n"
        "  write <file> <text>  Write file\r\n"
        "  cat <file>           Read file; -n numbers lines\r\n"
        "  rm <file>            Delete file; -r recursive, -f force\r\n"
        "  cp <src> <dst>       Copy file; -r recursive, -f overwrite\r\n"
        "  mv <src> <dst>       Move/rename; -f overwrite\r\n"
        "  grep [options] <pattern> <file>  Search file (-n line, -i ignore case, -v invert, -c count)\r\n"
        "  find [path] -name <name>         Find by name\r\n"
        "  find [path] -type <f|d>          Find by type\r\n"
        "  history [n|-c]       Show/clear command history\r\n"
        "  date [-u] [format]   Show RTC date/time; date -s YYYY-MM-DD HH:MM:SS\r\n"
        "  du [-h] [-s] [path]  Show disk usage\r\n"
        "  df [-h]              Show filesystem free/used space\r\n"
        "  wc [-lwc] <file>     Count lines/words/chars\r\n"
        "  chmod <mode> <file>  Change file mode\r\n"
        "  env                  Show environment variables\r\n"
        "  export VAR=val       Set environment variable\r\n"
        "  shell: > >> < |      Redirection and simple cat/echo -> grep pipe\r\n"
        "  tree                 Show filesystem tree\r\n"
        "  ps                   Show processes\r\n"
        "  uptime               Show uptime\r\n"
        "  sched                Show scheduler status\r\n"
        "  exec <file>          Execute an ELF program\r\n"
        "  nano <file>          Text editor (relative to current dir)\r\n"
        "  calc [expr]          Calculator app (interactive if no expr)\r\n"
        "  elf                  Show ELF loader status\r\n"
        "  syscall <test>        Test syscall interface\r\n"
        "  user [password]      Login as user (password: 123)\r\n"
        "  user demo            Run preserved EL0 userspace demo\r\n"
        "  userdemo             Run preserved EL0 userspace demo\r\n"
        "  su [root|user]       Switch security user\r\n"
        "  logout               Return from root to user\r\n"
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
        "  tasks                Show scheduler and processes\r\n"
        "  mount                Show storage and filesystem mount\r\n"
        "  stats                Show system statistics\r\n"
        "  log [0-3]            Show/set log level\r\n"
        "  irq [status|off|oneshot|stagec|staged|selftest] [hz]  Timer IRQ bring-up\r\n"
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
        else if (word[i] == 'p') bits |= OPT_P;
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
    else uart_puts("\r\n");
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

#define CP_MAX_SIZE 262144ULL
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

static int parse_two_operands(char* argument, int allowed_options, int* force, int* recursive,
                              char** from, char** to)
{
    char* words[6]; char* operands[3];
    int count = split_words(argument, words, 6);
    int options = 0;
    int n = collect_operands(words, count, operands, 2, &options);
    if (n < 0 || (options & ~allowed_options)) return -1;
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
        if (fs_get_type(child) == FS_DIR)
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
    int parsed = parse_two_operands(argument, OPT_R | OPT_F, &force, &recursive, &from, &to);
    if (parsed != 1)
    {
        uart_puts("cp: usage: cp [-r] [-f] <source> <destination>\r\n");
        return;
    }

    int cwd = vfs_cwd();
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
        int id = fs_resolve(operands[i], vfs_cwd());
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
    uart_puts("/bin/nano.elf    - text editor (command: nano)\r\n");
    uart_puts("/bin/calc.elf    - full terminal calculator (command: calc)\r\n");
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
    fb_print("Password: ", 0xFFFFFF);

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
    if (!argument || !argument[0] || str_equal(argument, "root"))
    {
        security_login_root();
        uart_puts("Switched to root.\r\n");
        return;
    }

    if (str_equal(argument, "user"))
    {
        command_user_login(0);
        return;
    }

    uart_puts("su: usage: su [root|user]\r\n");
}

static void command_logout()
{
    if (security_is_root())
    {
        security_logout_to_user();
        uart_puts("Logged out of root. Back to user.\r\n");
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
    print_u64(security_is_root() ? 0ULL : 1000ULL);
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
        uart_puts("Available manuals: ls cat grep find chmod cp mv rm mkdir echo wc du df nano tree mount ps history\r\n");
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

    /* Expand $VAR before parsing, then handle shell operators. */
    char expanded[1024];
    int ep = 0;
    for (int i = 0; line[i] && ep < (int)sizeof(expanded)-1; )
    {
        if (line[i] == '$')
        {
            ++i;
            char name[32]; int np = 0;
            if (line[i] == '{')
            {
                ++i; while(line[i] && line[i] != '}' && np < 31) name[np++]=line[i++]; if(line[i]=='}')++i;
            }
            else
            {
                while(line[i] && ((line[i]>='A'&&line[i]<='Z')||(line[i]>='a'&&line[i]<='z')||(line[i]>='0'&&line[i]<='9')||line[i]=='_') && np<31) name[np++]=line[i++];
            }
            name[np]=0;
            const char* val = env_get(name);
            if (val) for (int k=0; val[k] && ep < (int)sizeof(expanded)-1; ++k) expanded[ep++]=val[k];
            else if (!name[0] && ep < (int)sizeof(expanded)-1) expanded[ep++]='$';
            continue;
        }
        expanded[ep++] = line[i++];
    }
    expanded[ep] = 0;
    line = expanded;
    normalize_shell_whitespace(line);
    if (*line == '\0') return;

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
        command_help();
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
    if (str_equal(command, "history")) { command_history(argument); return; }
    if (str_equal(command, "date")) { command_date(argument); return; }
    if (str_equal(command, "du")) { command_du(argument); return; }
    if (str_equal(command, "df")) { command_df(argument); return; }
    if (str_equal(command, "wc")) { command_wc(argument); return; }
    if (str_equal(command, "chmod")) { command_chmod(argument); return; }
    if (str_equal(command, "env")) { command_env(argument); return; }
    if (str_equal(command, "export")) { command_export(argument); return; }

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
    if (str_equal(command, "tasks")) { command_tasks(); return; }
    if (str_equal(command, "mount")) { command_mount(); return; }
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