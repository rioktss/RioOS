#include "smp.h"

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

#ifdef HOST_TEST
void smp_test_set_cpu_id(uint32_t id) { current_cpu_id = id & 0xFFU; }
#endif
