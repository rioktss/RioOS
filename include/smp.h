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

#ifndef RIOOS_SMP_H
#define RIOOS_SMP_H

#include "types.h"

/* Secondary cores are released via PSCI and run an idle/job loop on the kernel tables.
 * The scheduler, heap and drivers stay on the boot CPU until the shared kernel is made SMP-safe. */
typedef struct SmpSpinlock
{
    uint32_t value;
} SmpSpinlock;

void smp_init();
uint32_t smp_cpu_id();
uint32_t smp_online_cpu_count();
int smp_is_enabled();

void smp_spinlock_init(SmpSpinlock* lock);
void smp_spin_lock(SmpSpinlock* lock);
int smp_spin_try_lock(SmpSpinlock* lock);
void smp_spin_unlock(SmpSpinlock* lock);

#ifndef HOST_TEST
void smp_boot_secondaries();                         /* PSCI CPU_ON for every present secondary core */
int smp_run_on(uint32_t cpu, uint64_t a, uint64_t b, uint64_t* result);   /* sum of squares on a secondary CPU */
uint64_t smp_cpu_heartbeat(uint32_t cpu);
uint64_t smp_cpu_mpidr(uint32_t cpu);
#endif

#ifdef HOST_TEST
void smp_test_set_cpu_id(uint32_t id);
#endif

#endif
