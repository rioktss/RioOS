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
struct CpuMail
{
    volatile uint32_t state;        /* 0 idle, 1 job posted, 2 job done */
    volatile uint64_t a, b, result;
    volatile uint64_t heartbeat;
    volatile uint32_t online;
    volatile uint64_t mpidr;
    volatile uint32_t sched_ready;
};
extern "C" {
uint64_t smp_boot_ctx[8] __attribute__((aligned(64)));
uint8_t smp_stack_area[(SMP_MAX_CPUS + 1) * 16384] __attribute__((aligned(16)));
void secondary_start();
void smp_secondary_main(uint32_t cpu);
}
static CpuMail mail[SMP_MAX_CPUS] __attribute__((aligned(64)));
static uint32_t secondaries_online = 0;
#endif

volatile int smp_sched_on = 0;
#ifndef HOST_TEST
static volatile int smp_go = 0;
static volatile uint32_t bkl_owner = 0;
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

#ifndef HOST_TEST
uint32_t smp_cpu_id() { return smp_cpu_id_fast(); }
#else
uint32_t smp_cpu_id() { return current_cpu_id; }
#endif
uint32_t smp_online_cpu_count() { return online_cpus; }
int smp_is_enabled() { return smp_enabled_flag; }

#ifdef HOST_TEST
void bkl_lock() {}
void bkl_unlock() {}
int bkl_held() { return 0; }
void smp_window() {}
void smp_kick_others() {}
void smp_enable_scheduling() {}
void smp_idle_wait() {}
#endif

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


extern "C" void interrupt_init_secondary();
extern "C" void interrupt_kick_others();
extern "C" void timer_arm_secondary();
extern void thread_secondary_adopt(uint32_t cpu);
extern void thread_idle_schedule();
extern void thread_reload_context();
extern uint64_t thread_next_wake_ms();
extern "C" void timer_arm_deadline_ms(uint64_t ms);

static inline uint64_t daif_mask()
{
    uint64_t d; asm volatile("mrs %0, daif\n msr daifset, #0x2\n isb" : "=r"(d) :: "memory"); return d;
}
static inline void daif_restore(uint64_t d) { asm volatile("msr daif, %0\n isb" :: "r"(d) : "memory"); }

/* Test-and-set lock with owner id. A ticket lock would convoy when a waiting virtual CPU is descheduled by the host at the moment
   its turn comes; an unfair lock does not, and the hand-over windows below give waiters a chance. */
void bkl_lock()
{
    if (!smp_sched_on) return;
    uint64_t d = daif_mask();
    uint32_t me = smp_cpu_id_fast() + 1U;
    for (;;)
    {
        uint32_t expect = 0;
        if (__atomic_load_n(&bkl_owner, __ATOMIC_RELAXED) == 0U &&
            __atomic_compare_exchange_n(&bkl_owner, &expect, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        asm volatile("yield" ::: "memory");
    }
    daif_restore(d);
}
void bkl_unlock()
{
    if (!smp_sched_on) return;
    __atomic_store_n(&bkl_owner, 0U, __ATOMIC_RELEASE);
}
int bkl_held() { return smp_sched_on && __atomic_load_n(&bkl_owner, __ATOMIC_RELAXED) == smp_cpu_id_fast() + 1U; }

void smp_window()
{
    if (!smp_sched_on) return;
    bkl_unlock();
    for (int i = 0; i < 200 && __atomic_load_n(&bkl_owner, __ATOMIC_RELAXED) == 0U; ++i) asm volatile("yield" ::: "memory");
    bkl_lock();
    thread_reload_context();
}

void smp_kick_others() { if (smp_sched_on) interrupt_kick_others(); }

void smp_idle_wait()
{
    if (!smp_sched_on) return;
    uint64_t d = daif_mask();
    uint64_t wake = thread_next_wake_ms();
    bkl_unlock();
    if (wake) timer_arm_deadline_ms(wake);
    asm volatile("msr daifclr, #2\n wfi\n msr daifset, #2" ::: "memory");
    bkl_lock();
    thread_reload_context();
    daif_restore(d);
}

extern "C" void bkl_enter_user()
{
    if (!smp_sched_on) return;
    bkl_lock();
    thread_reload_context();
}
extern "C" void bkl_leave_exception(void* frame)
{
    if (!smp_sched_on) return;
    uint64_t spsr = ((const uint64_t*)frame)[32];        /* ExceptionFrame.spsr */
    if ((spsr & 0xFULL) == 0) bkl_unlock();
}
extern "C" int bkl_enter_el1()
{
    if (!smp_sched_on) return 0;
    if (bkl_held()) return 0;
    bkl_lock();
    return 1;
}
extern "C" void bkl_leave_el1(int took) { if (took) bkl_unlock(); }
extern "C" void bkl_leave_to_user() { bkl_unlock(); }

static void secondary_sched_main(uint32_t cpu)
{
    interrupt_init_secondary();
    thread_secondary_adopt(cpu);
    timer_arm_secondary();
    __atomic_store_n(&mail[cpu].sched_ready, 1U, __ATOMIC_RELEASE);
    for (;;)
    {
        if (__atomic_load_n(&mail[cpu].state, __ATOMIC_ACQUIRE) == 1U)     /* offloaded job (smp test) */
        {
            uint64_t sum = 0;
            for (uint64_t i = mail[cpu].a; i < mail[cpu].b; ++i) sum += i * i;
            mail[cpu].result = sum;
            __atomic_store_n(&mail[cpu].state, 2U, __ATOMIC_RELEASE);
        }
        ++mail[cpu].heartbeat;
        bkl_lock();
        thread_idle_schedule();
        uint64_t wake = thread_next_wake_ms();
        bkl_unlock();
        if (wake) timer_arm_deadline_ms(wake);
        asm volatile("msr daifclr, #2\n wfi\n msr daifset, #2" ::: "memory");
    }
}

void smp_enable_scheduling()
{
    if (online_cpus <= 1U || smp_sched_on) return;
    uint64_t d = daif_mask();
    bkl_owner = 0;
    asm volatile("dsb sy" ::: "memory");
    smp_sched_on = 1;
    bkl_lock();                          /* the boot CPU runs the shell with the lock held */
    daif_restore(d);
    __atomic_store_n(&smp_go, 1, __ATOMIC_RELEASE);
    asm volatile("dsb sy\n sev" ::: "memory");
    for (uint32_t cpu = 1; cpu < online_cpus; ++cpu)
    {
        for (uint64_t spin = 0; spin < 200000000ULL && !__atomic_load_n(&mail[cpu].sched_ready, __ATOMIC_ACQUIRE); ++spin)
        {
            if ((spin & 0xFFFFULL) == 0) smp_window();      /* never starve a CPU that needs the lock to finish its setup */
            asm volatile("yield");
        }
    }
    smp_enabled_flag = 1;
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
        if (__atomic_load_n(&smp_go, __ATOMIC_ACQUIRE)) secondary_sched_main(cpu);
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
        if (smp_sched_on && (spin & 0xFFFFULL) == 0) smp_window();   /* let the target CPU take the lock and reach its job check */
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
