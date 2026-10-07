#ifndef TIMER_H
#define TIMER_H

#include "types.h"

void timer_init();
void timer_start_periodic(uint32_t hz);
void timer_stop();
void timer_interrupt_handler();

uint64_t timer_ticks();
uint64_t timer_frequency();

uint64_t timer_millis();
uint64_t timer_seconds();

void timer_delay_ms(uint64_t ms);

/* P0 staged IRQ bring-up (additive; existing API above is unchanged). */
#define TIMER_MODE_OFF      0U
#define TIMER_MODE_ONESHOT  1U
#define TIMER_MODE_PERIODIC 2U

/* One-shot: fires once after `ms` milliseconds, then disables itself. */
void timer_start_oneshot_ms(uint32_t ms);

/* Stage C (0) = periodic IRQ without scheduler_tick, Stage D (1, default). */
void timer_set_sched_tick(int enabled);

uint32_t timer_mode();
uint32_t timer_current_hz();
uint64_t timer_irq_count();     /* handler entries with a real timer event */
uint64_t timer_stray_count();   /* handler entries without ISTATUS set     */
uint64_t timer_oneshot_fired(); /* completed one-shot events               */

#endif
