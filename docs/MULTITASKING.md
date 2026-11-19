# Preemptive multitasking, threads and scheduling (v1.14.0)

Status: implemented and covered by `mttest`, `thrtest`, `posixtest`, `ipctest`, the host tests (`sched_host_test`, `fd_host_test`, `syscall_host_test`, ...) and the QEMU regression (`tests/qemu_regression.py`, "session 15b"). Not tested on real hardware.

## What runs concurrently
- The 100 Hz timer IRQ preempts EL0 on every CPU. Several ELF programs (and shell jobs started with `&`) run at the same time; a program no longer waits for the previous one.
- User code runs in parallel on all CPUs (up to `SMP_MAX_CPUS` = 8). Kernel code runs under one big kernel lock (BKL, see limits).

## Scheduler (`process/sched_policy.cpp`, `process/scheduler.cpp`, `process/thread.cpp`)
| Class | Policies | Priority | Behaviour |
|---|---|---|---|
| Real time | `SCHED_FIFO`, `SCHED_RR` | 1..99 | strict priority, bitmap + FIFO list per level; RR quantum 100 ms |
| Fair | `SCHED_OTHER`, `SCHED_BATCH` | nice -20..19 | weighted virtual runtime (Linux weight table), wake-up preemption, sleeper credit, new tasks start behind |
| Idle | `SCHED_IDLE` | - | runs only when nothing else is runnable |

- Real-time throttling: 95 % of each 1 s period per CPU, so a spinning RT task cannot freeze the machine.
- Per-task CPU affinity (`sched_setaffinity`, `pthread_setaffinity_np`); a task is only queued/run on allowed CPUs, wake-ups kick the right CPU with a targeted IPI.
- Priority inheritance: a PI mutex/futex boosts the owner to the highest waiter's priority.
- The run queue is dynamic: there is no fixed task count, only memory.
- API: `nice`, `setpriority`, `sched_setscheduler/getscheduler/setparam/getparam/get_priority_min/max/rr_get_interval/yield/getcpu`, `sched_set/getaffinity`.

## Threads (1:1)
- Every user thread is a kernel thread with its own **32 KiB kernel stack** (unmapped guard page below it; stacks are cached for reuse) and its own saved context. Context switches save/restore the full register set, the FP/SIMD state and TLS (`TPIDR_EL0`).
- Process = thread group (`tgid` = pid, tids start at 0x20000000); `exit_group`, `exec` (kills the other threads), `fork` (only the caller is cloned), signals (process-directed signals go to a thread that does not block them; `pthread_kill`, per-thread masks), `pthread_cancel` (internal signal 16, cancellation points in blocking libc calls).
- Isolation: separate TTBR0 address space per process, user pointers validated on every syscall, kernel stack guards, per-process fd tables and rlimits.

## Synchronisation
- Kernel futex: `FUTEX_WAIT/WAKE/REQUEUE/CMP_REQUEUE/WAIT_BITSET/WAKE_BITSET`, PI futexes (`LOCK_PI/UNLOCK_PI/TRYLOCK_PI`), robust lists (a dying owner marks the mutex `OWNER_DIED`).
- libc: pthread mutex (normal/recursive/errorcheck/robust/PI), condition variables (monotonic clock too), rwlocks, barriers, spinlocks, once, TSS keys with destructors, `sem_*` (named and unnamed), `thread_local`/TLS (`.tdata/.tbss`, variant 1).
- Blocking syscalls (`read`/`write`/`wait`/`sleep`/`poll`...) sleep on a kernel event generation counter and are woken by the operation that makes them ready, with a short timeout fallback; they do not busy-wait while other tasks are runnable.

## Limits removed in this version
- Process, thread, run-queue, descriptor-table, open-file, pipe, unix-socket, eventfd, timerfd, signalfd, inotify and flock records are dynamic (memory is the only limit).
- Descriptors per process: default `RLIMIT_NOFILE` 1024, adjustable up to 65536 with `setrlimit`.
- `poll()` accepts any `nfds` up to 65536; `qsort` is heapsort (O(n log n)); 150+ threads per process tested.

## Known limits (honest list)
- **One big kernel lock.** System calls and the kernel's own work are serialised across CPUs; only user code is parallel. A fine-grained locking conversion was not attempted.
- At most 8 CPUs (GICv2 limit); RAM size is hard-coded to 128 MiB.
- Blocking calls still re-check their condition on wake-up (event wake + timeout) rather than having per-object wait queues.
- File I/O through descriptors buffers a whole file (`FD_FILE_MAX` 1 MiB), so large files are slow/limited.
- Under QEMU TCG with multi-threaded translation and `-smp 4` timing is erratic on 2-core hosts; the regression uses `-accel tcg,thread=single` for SMP runs.
- Only exercised on QEMU `virt` (aarch64); real boards were not tested.

## Tests
- `mttest` (userspace/bin/mttest.cpp): preemption of CPU hogs, 8 concurrent ELF processes, priorities and fairness, FIFO/RR, affinity, futex/PI/robust, cancellation, signals, thread limits, exec/exit paths.
- `bash linux/build.sh` runs all host tests; `python3 tests/qemu_regression.py <qemu>` runs the QEMU suite (`RIO_SMP=4 RIO_QEMU_EXTRA="-accel tcg,thread=single"` for SMP).
