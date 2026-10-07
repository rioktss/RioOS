#ifndef FAULT_H
#define FAULT_H

#include "types.h"

enum FaultKind
{
    FAULT_OTHER = 0,
    FAULT_INSTRUCTION_ABORT,
    FAULT_DATA_ABORT,
    FAULT_ALIGNMENT,
    FAULT_ILLEGAL
};

struct FaultInfo
{
    FaultKind kind;
    int from_user;      /* abort taken from EL0 */
    int is_write;       /* data abort caused by a write (WnR) */
    const char* cause;  /* "translation fault", "permission fault", ... */
    uint32_t level;     /* translation level 0..3, 4 when not applicable */
};

/* Pure decoder: no I/O, safe to unit-test on the host. */
void fault_decode(uint64_t esr, FaultInfo* out);

/* Prints a one-paragraph report for a fault and counts it. */
void fault_report(uint64_t esr, uint64_t elr, uint64_t far, uint64_t spsr);

uint64_t fault_user_count();
uint64_t fault_kernel_count();

#endif
