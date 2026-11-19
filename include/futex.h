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

#ifndef FUTEX_H
#define FUTEX_H

#include "types.h"

/* Futex operation numbers are the Linux ones. Every waiter is a record on the sleeping thread's own kernel stack, so there is
   no table to fill: the number of waiters is bounded by the number of threads. Private memory is keyed by (process, address),
   MAP_SHARED memory by physical address. */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

#define FUTEX_WAITERS 0x80000000U
#define FUTEX_OWNER_DIED 0x40000000U
#define FUTEX_TID_MASK 0x3FFFFFFFU

/* Called with the kernel lock held on behalf of user process `pid`.
   timeout_ms is relative; ~0 means no timeout. Returns the syscall result (>= 0 or -errno). */
int64_t futex_op(int pid, uint64_t uaddr, int op, uint64_t val, uint64_t timeout_ms, uint64_t uaddr2, uint64_t val3);
int futex_wake_user(int pid, uint64_t uaddr, int n);        /* number of waiters woken */
void futex_thread_exit(int pid, int thread);                /* clear_child_tid and robust-list handling for an exiting thread */

#endif
