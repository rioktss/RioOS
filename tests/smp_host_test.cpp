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
