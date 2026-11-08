/*
 * MIT License
 *
 * Copyright (c) 2026 Rio okta kusuma
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files, to deal in the Software
 * without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

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
