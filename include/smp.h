#ifndef RIOOS_SMP_H
#define RIOOS_SMP_H

#include "types.h"

/* SMP is deliberately disabled until secondary-core boot, per-CPU state,
 * interrupt routing, and shared-kernel ownership are implemented together. */
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

#ifdef HOST_TEST
void smp_test_set_cpu_id(uint32_t id);
#endif

#endif
