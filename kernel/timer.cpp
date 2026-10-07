#include "timer.h"
#include "uart.h"
#include "scheduler.h"

static uint64_t initial_ticks = 0;
static uint64_t frequency = 0;
static uint32_t timer_hz = 0;
static uint64_t period_ticks = 0;
static volatile uint32_t mode = TIMER_MODE_OFF;
static volatile uint64_t irq_events = 0;
static volatile uint64_t stray_events = 0;
static volatile uint64_t oneshot_done = 0;
static volatile int sched_tick_enabled = 1;

static uint64_t read_counter()
{
    uint64_t value;

    asm volatile(
        "mrs %0, cntpct_el0"
        : "=r"(value)
        :
        : "memory");

    return value;
}

static uint64_t read_frequency()
{
    uint64_t value;

    asm volatile(
        "mrs %0, cntfrq_el0"
        : "=r"(value)
        :
        : "memory");

    return value;
}

static void write_tval(uint64_t value)
{
    asm volatile(
        "msr cntp_tval_el0, %0\n"
        "isb\n"
        :: "r"(value)
        : "memory");
}

static uint64_t read_ctl()
{
    uint64_t value;

    asm volatile(
        "mrs %0, cntp_ctl_el0"
        : "=r"(value)
        :
        : "memory");

    return value;
}

/* Absolute compare value: 64-bit, so no TVAL signed 32-bit wrap issues. */
static void write_cval(uint64_t value)
{
    asm volatile(
        "msr cntp_cval_el0, %0\n"
        "isb\n"
        :: "r"(value)
        : "memory");
}

static void write_ctl(uint64_t value)
{
    asm volatile(
        "msr cntp_ctl_el0, %0\n"
        "isb\n"
        :: "r"(value)
        : "memory");
}

void timer_init()
{
    frequency = read_frequency();

    if (frequency == 0)
        frequency = 1000000ULL;

    initial_ticks = read_counter();
    timer_hz = 0;

    /* Keep the physical timer disabled until GIC + scheduler are ready. */
    write_ctl(0);
    write_tval(0);
    mode = TIMER_MODE_OFF;

    uart_puts("Timer initialized.\r\n");
}

void timer_start_periodic(uint32_t hz)
{
    if (hz == 0 || frequency == 0)
        return;

    uint64_t interval = frequency / (uint64_t)hz;

    if (interval == 0)
        interval = 1;

    /* Disable -> program first absolute deadline -> enable. */
    write_ctl(0);

    timer_hz = hz;
    period_ticks = interval;
    mode = TIMER_MODE_PERIODIC;

    write_cval(read_counter() + interval);
    asm volatile("dsb sy\n" ::: "memory");
    write_ctl(1);
    asm volatile("dsb sy\n" ::: "memory");
}

void timer_start_oneshot_ms(uint32_t ms)
{
    if (ms == 0 || frequency == 0)
        return;

    uint64_t interval = (frequency * (uint64_t)ms) / 1000ULL;

    if (interval == 0)
        interval = 1;

    write_ctl(0);

    timer_hz = 0;
    period_ticks = 0;
    mode = TIMER_MODE_ONESHOT;

    write_cval(read_counter() + interval);
    asm volatile("dsb sy\n" ::: "memory");
    write_ctl(1);
    asm volatile("dsb sy\n" ::: "memory");
}

void timer_stop()
{
    write_ctl(0);
    asm volatile("dsb sy\n" ::: "memory");

    timer_hz = 0;
    period_ticks = 0;
    mode = TIMER_MODE_OFF;
}

void timer_interrupt_handler()
{
    uint64_t ctl = read_ctl();

    /* ISTATUS (bit 2) must be set for a genuine timer event. */
    if ((ctl & 4ULL) == 0)
    {
        ++stray_events;
        return;
    }

    ++irq_events;

    if (mode == TIMER_MODE_PERIODIC && period_ticks != 0)
    {
        /* Re-arm from the previous deadline (no drift). If we fell behind,
           resynchronise to "now" so the line cannot stay asserted. */
        uint64_t now = read_counter();
        uint64_t next = now + period_ticks;

        write_cval(next);

        if (sched_tick_enabled)
            (void)scheduler_tick();

        return;
    }

    /* One-shot, or a timer left enabled while mode == OFF: disable the
       timer so the level-triggered PPI is de-asserted before EOI. This
       also guarantees there can never be an interrupt storm. */
    write_ctl(0);

    if (mode == TIMER_MODE_ONESHOT)
        ++oneshot_done;

    mode = TIMER_MODE_OFF;
    timer_hz = 0;
    period_ticks = 0;
}

void timer_set_sched_tick(int enabled)
{
    sched_tick_enabled = enabled ? 1 : 0;
}

uint32_t timer_mode()
{
    return mode;
}

uint32_t timer_current_hz()
{
    return timer_hz;
}

uint64_t timer_irq_count()
{
    return irq_events;
}

uint64_t timer_stray_count()
{
    return stray_events;
}

uint64_t timer_oneshot_fired()
{
    return oneshot_done;
}

uint64_t timer_ticks()
{
    return read_counter() - initial_ticks;
}

uint64_t timer_frequency()
{
    return frequency;
}

uint64_t timer_millis()
{
    if (frequency == 0)
        return 0;

    return (timer_ticks() * 1000ULL) / frequency;
}

uint64_t timer_seconds()
{
    if (frequency == 0)
        return 0;

    return timer_ticks() / frequency;
}

void timer_delay_ms(uint64_t ms)
{
    uint64_t start = timer_millis();

    while (timer_millis() - start < ms)
    {
    }
}
