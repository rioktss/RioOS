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

#endif
