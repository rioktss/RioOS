#ifndef RIOOS_FPU_H
#define RIOOS_FPU_H

#include "types.h"

/* Floating point / Advanced SIMD state of an EL0 context: V0-V31 + FPCR + FPSR.
   The kernel itself is built with -mgeneral-regs-only and never touches these registers, so the live
   register file always belongs to the user thread that last ran; it is saved/restored on thread switches
   (process/thread.cpp), copied on fork, cleared on exec and stored in the signal frame. */
struct FpState
{
    uint64_t q[64];       /* V0..V31 as 128-bit values (low, high) */
    uint32_t fpcr;
    uint32_t fpsr;
    uint32_t pad[2];
} __attribute__((aligned(16)));

extern "C" void fp_save(FpState* state);
extern "C" void fp_restore(const FpState* state);
void fp_state_clear(FpState* state);

#endif
