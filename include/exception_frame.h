#ifndef EXCEPTION_FRAME_H
#define EXCEPTION_FRAME_H

#include "types.h"

struct ExceptionFrame
{
    uint64_t x[31];
    uint64_t elr;
    uint64_t spsr;
    uint64_t esr;
    uint64_t far;
    uint64_t padding;
};

static inline int exception_from_el0(const ExceptionFrame* frame)
{
    if (frame == 0)
        return 0;

    return (frame->spsr & 0xFULL) == 0x0ULL ||
           (frame->spsr & 0xFULL) == 0x4ULL;
}

#endif
