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

#include "smp.h"

#ifndef HOST_TEST
#include "uart.h"
#define SMP_MAX_CPUS 8
struct CpuMail
{
    volatile uint32_t state;        /* 0 idle, 1 job posted, 2 job done */
    volatile uint64_t a, b, result;
    volatile uint64_t heartbeat;
    volatile uint32_t online;
    volatile uint64_t mpidr;
};
extern "C" {
uint64_t smp_boot_ctx[8] __attribute__((aligned(64)));
uint8_t smp_stack_area[(SMP_MAX_CPUS + 1) * 8192] __attribute__((aligned(16)));
void secondary_start();
void smp_secondary_main(uint32_t cpu);
}
static CpuMail mail[SMP_MAX_CPUS] __attribute__((aligned(64)));
static uint32_t secondaries_online = 0;
#endif

static uint32_t current_cpu_id = 0;
/* Keep this at one until secondary cores are explicitly brought online. */
static uint32_t online_cpus = 1;
static int smp_enabled_flag = 0;

void smp_init()
{
#ifdef HOST_TEST
    /* Host tests do not have access to MPIDR_EL1. */
    current_cpu_id = 0;
#else
    uint64_t mpidr = 0;
    asm volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    current_cpu_id = (uint32_t)(mpidr & 0xFFU);
#endif
    online_cpus = 1;
    smp_enabled_flag = 0;
}

uint32_t smp_cpu_id() { return current_cpu_id; }
uint32_t smp_online_cpu_count() { return online_cpus; }
int smp_is_enabled() { return smp_enabled_flag; }

void smp_spinlock_init(SmpSpinlock* lock)
{
    if (lock == 0) return;
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELAXED);
}

void smp_spin_lock(SmpSpinlock* lock)
{
    if (lock == 0) return;
    while (__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE) != 0U)
    {
#ifndef HOST_TEST
        asm volatile("yield" ::: "memory");
#endif
    }
}

int smp_spin_try_lock(SmpSpinlock* lock)
{
    if (lock == 0) return 0;
    uint32_t expected = 0U;
    return __atomic_compare_exchange_n(&lock->value, &expected, 1U, 0,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 1 : 0;
}

void smp_spin_unlock(SmpSpinlock* lock)
{
    if (lock == 0) return;
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

#ifndef HOST_TEST
static int psci_cpu_on(uint64_t target, uint64_t entry, uint64_t ctx)
{
    register uint64_t x0 asm("x0") = 0xC4000003ULL;      /* PSCI 0.2 CPU_ON (SMC64) */
    register uint64_t x1 asm("x1") = target;
    register uint64_t x2 asm("x2") = entry;
    register uint64_t x3 asm("x3") = ctx;
    asm volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
    return (int)(int32_t)x0;
}

extern "C" void smp_secondary_main(uint32_t cpu)
{
    uint64_t mpidr;
    asm volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    mail[cpu].mpidr = mpidr;
    __atomic_store_n(&mail[cpu].online, 1U, __ATOMIC_RELEASE);
    for (;;)
    {
        if (__atomic_load_n(&mail[cpu].state, __ATOMIC_ACQUIRE) == 1U)
        {
            uint64_t sum = 0;
            for (uint64_t i = mail[cpu].a; i < mail[cpu].b; ++i) sum += i * i;
            mail[cpu].result = sum;
            __atomic_store_n(&mail[cpu].state, 2U, __ATOMIC_RELEASE);
            asm volatile("dsb sy\n sev" ::: "memory");
        }
        ++mail[cpu].heartbeat;
        asm volatile("wfe");
    }
}

/* Release secondary cores through PSCI. They run on the kernel page tables in an idle/job loop;
   the scheduler, heap and drivers stay on the boot CPU (no shared-kernel locking yet). */
void smp_boot_secondaries()
{
    uint64_t v, m, t, tt, sc;
    asm volatile("mrs %0, VBAR_EL1" : "=r"(v));
    asm volatile("mrs %0, MAIR_EL1" : "=r"(m));
    asm volatile("mrs %0, TCR_EL1" : "=r"(t));
    asm volatile("mrs %0, TTBR0_EL1" : "=r"(tt));
    asm volatile("mrs %0, SCTLR_EL1" : "=r"(sc));
    smp_boot_ctx[0] = v; smp_boot_ctx[1] = m; smp_boot_ctx[2] = t; smp_boot_ctx[3] = tt; smp_boot_ctx[4] = sc;
    asm volatile("dc civac, %0\n dsb sy" :: "r"(&smp_boot_ctx[0]) : "memory");
    for (uint32_t cpu = 1; cpu < SMP_MAX_CPUS; ++cpu)
    {
        mail[cpu].state = 0; mail[cpu].online = 0;
        asm volatile("dc civac, %0\n dc civac, %1\n dsb sy" :: "r"(&mail[cpu]), "r"(&mail[cpu].heartbeat) : "memory");
        int rc = psci_cpu_on((uint64_t)cpu, (uint64_t)&secondary_start, (uint64_t)cpu);
        if (rc != 0) break;                                  /* NOT_PRESENT: no more CPUs */
        for (uint32_t spin = 0; spin < 20000000U && !__atomic_load_n(&mail[cpu].online, __ATOMIC_ACQUIRE); ++spin)
            asm volatile("yield");
        if (!__atomic_load_n(&mail[cpu].online, __ATOMIC_ACQUIRE)) break;
        ++secondaries_online;
        online_cpus = 1U + secondaries_online;
    }
}

/* Run sum(i*i, a <= i < b) on a secondary CPU; returns 0 and stores the result, negative on failure. */
int smp_run_on(uint32_t cpu, uint64_t a, uint64_t b, uint64_t* result)
{
    if (cpu < 1 || cpu >= SMP_MAX_CPUS || !__atomic_load_n(&mail[cpu].online, __ATOMIC_ACQUIRE)) return -19;
    if (__atomic_load_n(&mail[cpu].state, __ATOMIC_ACQUIRE) == 1U) return -16;
    mail[cpu].a = a; mail[cpu].b = b;
    __atomic_store_n(&mail[cpu].state, 1U, __ATOMIC_RELEASE);
    asm volatile("dsb sy\n sev" ::: "memory");
    for (uint64_t spin = 0; spin < 400000000ULL; ++spin)
    {
        if (__atomic_load_n(&mail[cpu].state, __ATOMIC_ACQUIRE) == 2U)
        {
            if (result) *result = mail[cpu].result;
            __atomic_store_n(&mail[cpu].state, 0U, __ATOMIC_RELEASE);
            return 0;
        }
        asm volatile("yield");
    }
    return -110;
}

uint64_t smp_cpu_heartbeat(uint32_t cpu) { return cpu < SMP_MAX_CPUS ? mail[cpu].heartbeat : 0; }
uint64_t smp_cpu_mpidr(uint32_t cpu) { return cpu < SMP_MAX_CPUS ? mail[cpu].mpidr : 0; }
#endif

#ifdef HOST_TEST
void smp_test_set_cpu_id(uint32_t id) { current_cpu_id = id & 0xFFU; }
#endif
