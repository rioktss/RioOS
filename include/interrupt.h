#ifndef INTERRUPT_H
#define INTERRUPT_H

#include "types.h"
#include "exception_frame.h"

void interrupt_init();
void interrupt_enable();
void interrupt_disable();
uint64_t interrupt_count();
uint32_t interrupt_last_irq();

/* P0: IRQ statistics and staged timer-IRQ control (additive). */
struct InterruptStats
{
    uint64_t total;        /* real IRQs acknowledged and EOI'd        */
    uint64_t timer;        /* PPI 30 deliveries                       */
    uint64_t spurious;     /* IAR returned 1020..1023 (no EOI sent)   */
    uint64_t unhandled;    /* valid IRQ ID with no handler            */
    uint64_t nested;       /* dispatch entered while already inside   */
    uint32_t last_irq;
    uint32_t max_depth;
};

void interrupt_get_stats(InterruptStats* out);
int interrupt_cpu_irq_enabled();

/* Unmask/mask only the timer PPI at the GIC distributor (CPU I-bit is
   controlled separately by interrupt_enable()/interrupt_disable()). */
void interrupt_timer_line_enable();
void interrupt_timer_line_disable();

extern "C" void interrupt_dispatch(ExceptionFrame* frame);
extern "C" void interrupt_exception(uint64_t esr,
                                     uint64_t elr,
                                     uint64_t far,
                                     uint64_t spsr);

#endif
