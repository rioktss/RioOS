#ifndef INTERRUPT_H
#define INTERRUPT_H

#include "types.h"
#include "exception_frame.h"

void interrupt_init();
void interrupt_enable();
void interrupt_disable();
uint64_t interrupt_count();
uint32_t interrupt_last_irq();

extern "C" void interrupt_dispatch(ExceptionFrame* frame);
extern "C" void interrupt_exception(uint64_t esr,
                                     uint64_t elr,
                                     uint64_t far,
                                     uint64_t spsr);

#endif
