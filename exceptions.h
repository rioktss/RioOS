#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

extern "C" void exceptions_init();
extern "C" void exception_sync_el1(void* frame);
extern "C" void exception_sync_el0(void* frame);
extern "C" void exception_irq_unhandled();

#endif
