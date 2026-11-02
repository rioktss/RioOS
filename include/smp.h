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
