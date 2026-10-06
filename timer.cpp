#include "timer.h"
#include "uart.h"
#include "scheduler.h"

static uint64_t initial_ticks = 0;
static uint64_t frequency = 0;
static uint32_t timer_hz = 0;

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

    uart_puts("Timer initialized.\r\n");
}

void timer_start_periodic(uint32_t hz)
{
    if (hz == 0 || frequency == 0)
        return;

    uint64_t interval = frequency / (uint64_t)hz;

    if (interval == 0)
        interval = 1;

    timer_hz = hz;

    /* Disable -> program first deadline -> enable. */
    write_ctl(0);
    write_tval(interval);
    asm volatile("dsb sy\n" ::: "memory");
    write_ctl(1);
    asm volatile("dsb sy\n" ::: "memory");
}

void timer_stop()
{
    timer_hz = 0;
    write_ctl(0);
    asm volatile("dsb sy\n" ::: "memory");
}

void timer_interrupt_handler()
{
    /* Always re-arm before returning from the IRQ. */
    if (timer_hz != 0)
    {
        uint64_t interval = frequency / (uint64_t)timer_hz;

        if (interval == 0)
            interval = 1;

        write_tval(interval);
    }

    /* Count the tick only; no preemption/context switch yet. */
    (void)scheduler_tick();
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
