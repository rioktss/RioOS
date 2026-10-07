#include "scheduler.h"
#include "uart.h"
#include "process.h"

#define SCHED_MAX 32

static int queue[SCHED_MAX];
static int count = 0;
static int current = -1;
static int cursor = 0;
static uint64_t ticks = 0;
static int preempt_pending = 0;

static uint64_t irq_save()
{
    uint64_t daif;

    asm volatile(
        "mrs %0, daif\n"
        "msr daifset, #0x2\n"
        "isb"
        : "=r"(daif)
        :
        : "memory");

    return daif;
}

static void irq_restore(uint64_t daif)
{
    asm volatile(
        "msr daif, %0\n"
        "isb"
        :: "r"(daif)
        : "memory");
}

static void print_u64(uint64_t value)
{
    char buffer[32];
    int position = 0;

    if (value == 0)
    {
        uart_putc('0');
        return;
    }

    while (value != 0 && position < 31)
    {
        buffer[position++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }

    while (position > 0)
        uart_putc(buffer[--position]);
}

static int runnable(int pid)
{
    int state = process_get_state(pid);
    return state == PROCESS_READY || state == PROCESS_RUNNING;
}

static void compact_locked()
{
    for (int i = 0; i < count; )
    {
        int state = process_get_state(queue[i]);

        if (state < 0 ||
            state == PROCESS_ZOMBIE ||
            state == PROCESS_UNUSED)
        {
            for (int j = i; j < count - 1; ++j)
                queue[j] = queue[j + 1];

            --count;

            if (cursor > i)
                --cursor;

            continue;
        }

        ++i;
    }

    if (count == 0)
    {
        current = -1;
        cursor = 0;
        return;
    }

    if (cursor < 0)
        cursor = 0;

    if (cursor >= count)
        cursor = count - 1;
}

static int first_runnable_locked()
{
    if (count == 0)
        return -1;

    for (int i = 0; i < count; ++i)
    {
        if (runnable(queue[i]))
            return i;
    }

    return -1;
}

static int next_runnable_locked(int start)
{
    if (count == 0)
        return -1;

    for (int offset = 1; offset <= count; ++offset)
    {
        int index = (start + offset) % count;

        if (runnable(queue[index]))
            return index;
    }

    if (start >= 0 &&
        start < count &&
        runnable(queue[start]))
    {
        return start;
    }

    return -1;
}

/*
 * Timer tick only.
 *
 * IMPORTANT:
 * We intentionally DO NOT switch current PID here.
 * Real preemption requires a real CPU context-switch implementation.
 */
int scheduler_tick()
{
    uint64_t flags = irq_save();

    ++ticks;

    /* Preemption remains disabled. */
    preempt_pending = 0;

    irq_restore(flags);

    return 0;
}

void scheduler_init()
{
    uint64_t flags = irq_save();

    count = 0;
    current = -1;
    cursor = 0;
    ticks = 0;
    preempt_pending = 0;

    for (int slot = 0;
         slot < SCHED_MAX && count < SCHED_MAX;
         ++slot)
    {
        int pid = process_pid_at(slot);

        if (pid < 0)
            continue;

        if (!runnable(pid))
            continue;

        queue[count++] = pid;
    }

    int first = first_runnable_locked();

    if (first >= 0)
    {
        cursor = first;
        current = queue[first];

        process_set_state(current, PROCESS_RUNNING);

        for (int i = 0; i < count; ++i)
        {
            if (i != first &&
                process_get_state(queue[i]) == PROCESS_RUNNING)
            {
                process_set_state(queue[i], PROCESS_READY);
            }
        }
    }

    irq_restore(flags);

    uart_puts("Scheduler initialized.\r\n");
}

int scheduler_add(int pid)
{
    if (pid < 0)
        return -1;

    uint64_t flags = irq_save();

    compact_locked();

    for (int i = 0; i < count; ++i)
    {
        if (queue[i] == pid)
        {
            irq_restore(flags);
            return 0;
        }
    }

    if (count >= SCHED_MAX)
    {
        irq_restore(flags);
        return -1;
    }

    queue[count++] = pid;

    if (current < 0)
    {
        cursor = count - 1;
        current = pid;
        process_set_state(pid, PROCESS_RUNNING);
    }
    else if (process_get_state(pid) == PROCESS_RUNNING)
    {
        process_set_state(pid, PROCESS_READY);
    }

    irq_restore(flags);

    return 0;
}

int scheduler_remove(int pid)
{
    uint64_t flags = irq_save();

    int index = -1;

    for (int i = 0; i < count; ++i)
    {
        if (queue[i] == pid)
        {
            index = i;
            break;
        }
    }

    if (index < 0)
    {
        irq_restore(flags);
        return -1;
    }

    int removing_current = (pid == current);

    for (int i = index; i < count - 1; ++i)
        queue[i] = queue[i + 1];

    --count;

    if (count == 0)
    {
        current = -1;
        cursor = 0;
        preempt_pending = 0;

        irq_restore(flags);
        return 0;
    }

    if (index < cursor)
        --cursor;

    if (cursor >= count)
        cursor = count - 1;

    if (cursor < 0)
        cursor = 0;

    if (removing_current)
    {
        int next_index = first_runnable_locked();

        if (next_index < 0)
        {
            current = -1;
            preempt_pending = 0;
        }
        else
        {
            cursor = next_index;
            current = queue[cursor];

            process_set_state(
                current,
                PROCESS_RUNNING
            );
        }
    }

    irq_restore(flags);

    return 0;
}

int scheduler_current_pid()
{
    uint64_t flags = irq_save();

    compact_locked();

    if (current < 0)
    {
        int index = first_runnable_locked();

        if (index >= 0)
        {
            cursor = index;
            current = queue[index];

            process_set_state(
                current,
                PROCESS_RUNNING
            );
        }
    }

    int result = current;

    irq_restore(flags);

    return result;
}

int scheduler_next_pid()
{
    uint64_t flags = irq_save();

    compact_locked();

    int result = -1;

    int index = next_runnable_locked(cursor);

    if (index >= 0)
        result = queue[index];

    irq_restore(flags);

    return result;
}

int scheduler_preempt_pending()
{
    uint64_t flags = irq_save();

    int result = preempt_pending;

    irq_restore(flags);

    return result;
}

void scheduler_clear_preempt_pending()
{
    uint64_t flags = irq_save();

    preempt_pending = 0;

    irq_restore(flags);
}

uint64_t scheduler_ticks()
{
    uint64_t flags = irq_save();

    uint64_t result = ticks;

    irq_restore(flags);

    return result;
}

int scheduler_ready_count()
{
    uint64_t flags = irq_save();

    compact_locked();

    int ready = 0;

    for (int i = 0; i < count; ++i)
    {
        if (runnable(queue[i]))
            ++ready;
    }

    irq_restore(flags);

    return ready;
}

void scheduler_status()
{
    int ready;
    int cur;
    int next;
    uint64_t tick_count;
    int pending;

    uint64_t flags = irq_save();

    compact_locked();

    ready = 0;

    for (int i = 0; i < count; ++i)
    {
        if (runnable(queue[i]))
            ++ready;
    }

    cur = current;

    next = -1;

    int next_index = next_runnable_locked(cursor);

    if (next_index >= 0)
        next = queue[next_index];

    tick_count = ticks;
    pending = preempt_pending;

    irq_restore(flags);

    uart_puts("Scheduler status\r\n");
    uart_puts("----------------\r\n");

    uart_puts("Ready processes : ");
    print_u64((uint64_t)ready);

    uart_puts("\r\nCurrent PID     : ");

    if (cur < 0)
        uart_puts("none");
    else
        print_u64((uint64_t)cur);

    uart_puts("\r\nScheduler ticks : ");
    print_u64(tick_count);

    uart_puts("\r\nNext PID        : ");

    if (next < 0)
        uart_puts("none");
    else
        print_u64((uint64_t)next);

    uart_puts("\r\nPreempt pending  : ");
    uart_puts(pending ? "yes" : "no");

    uart_puts("\r\n");
}