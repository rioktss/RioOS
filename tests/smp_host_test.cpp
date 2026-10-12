#include "smp.h"

#include <stdio.h>

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main()
{
    smp_init();
    if (smp_cpu_id() != 0U) return fail("host boot CPU defaults to zero");
    if (smp_online_cpu_count() != 1U) return fail("SMP remains single-core until secondary startup exists");
    if (smp_is_enabled() != 0) return fail("SMP must remain disabled by default");

    smp_test_set_cpu_id(0x123U);
    if (smp_cpu_id() != 0x23U) return fail("CPU Aff0 ID is masked to 8 bits");

    SmpSpinlock lock;
    smp_spinlock_init(&lock);
    if (!smp_spin_try_lock(&lock)) return fail("try-lock acquires free lock");
    if (smp_spin_try_lock(&lock)) return fail("try-lock rejects held lock");
    smp_spin_unlock(&lock);
    if (!smp_spin_try_lock(&lock)) return fail("unlock releases lock");
    smp_spin_unlock(&lock);
    smp_spin_lock(&lock);
    smp_spin_unlock(&lock);
    smp_spinlock_init(0);
    smp_spin_lock(0);
    smp_spin_unlock(0);

    puts("SMP host test: PASS");
    return 0;
}
