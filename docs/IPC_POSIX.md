# SMP, poll/epoll, IPC and POSIX libc (round 9)

## SMP scheduler (`-smp N`, up to 8 CPUs)
- User processes (one kernel thread each) run in parallel on all CPUs. Kernel code is serialized by a Big Kernel Lock (test-and-set, owner-tracked): taken at every exception entry, released before `eret` to EL0. User code is fully parallel, kernel code is not.
- Per-CPU current thread, reschedule flag and time slice; idle contexts for secondaries; thread 0 (shell/boot) is pinned to CPU0. Only CPU0 runs the scheduler tick (10 ms); idle secondaries sleep until the next timer deadline.
- With `-smp 1` nothing changes (all lock operations are no-ops).
- Signals reach a process running on another CPU at its next kernel entry (<= 10 ms). Cross-CPU wake latency is bounded by sleep deadlines / the 10 ms tick.
- `sched_getcpu()`, `sched_getaffinity()`, `sysconf(_SC_NPROCESSORS_ONLN)` report real CPUs. No `sched_setaffinity`.
- Tests: `smptest [soak-seconds]` (shared counters, futex mutex, atomics, parallel file I/O, append integrity, fork storm, COW isolation, pipe/unix-socket, signals to spinning processes, poll while all CPUs busy). `RIO_SMP=4 python3 tests/qemu_regression.py <qemu>` runs the whole regression with 4 CPUs.

## poll / epoll / timerfd / eventfd
- `poll`, `select`, `epoll_*` (emulated over poll: level-triggered, EPOLLET re-armed by I/O in the same process, EPOLLONESHOT; max 8 instances x 64 watches), `eventfd`, `timerfd_*`, `clock_nanosleep`.

## IPC
- pipes, FIFOs, unix stream/datagram sockets, futex (`futex_wait/wake`), `shm_open` (file in /tmp/shm.<name>, MAP_SHARED), POSIX `sem_open` (file in /tmp/sem.<name> + futex), process-shared pthread primitives.

## POSIX libc additions
stat/lstat/fstat, fchmod/fchown/utimensat/futimens, truncate/fsync, access, flock, getcwd/chdir, dup3, opendir/readdir/scandir, fnmatch, glob, mkstemp/mkdtemp, basename/dirname, termios (raw/cooked, `vi` uses it), getpw*/getgr*, uname/gethostname, get/setrlimit, getrandom/arc4random/drand48, getopt/getopt_long, UTF-8 multibyte/wide functions, strptime, syslog (/var/log/messages), POSIX regex (`regcomp/regexec`, backrefs, leftmost-longest), `posixtest` (193 checks).

## Known limits
24 fds per process; 64 KiB user stack; PATH_MAX 128; file I/O through fds is whole-file; no signalfd, inotify, fmemopen, open_memstream, iconv, timer_create, nice, killpg, sched_setaffinity; getrusage CPU times are zero; setrlimit is recorded, not enforced; tty ISIG/OPOST fixed; `connect()` blocks even for non-blocking TCP; `dirfd` unsupported; lchown on symlinks returns ENOSYS; epoll ET does not re-report data that arrives without intervening I/O in the process; madvise/mlock are no-ops; SMP kernel work is serialized (no kernel parallelism).

## Test notes
- On a host with few cores, MTTCG with `-smp 4` is several times slower than `-smp 1` (vCPU threads compete for host cores); this is an emulator artifact. `RIO_QEMU_EXTRA="-accel tcg,thread=single" RIO_SMP=4 python3 tests/qemu_regression.py <qemu>` gives a functional run of the whole suite under 4 CPUs; the `smp-scheduler` session always runs with real parallel vCPUs.
- `smp test` (offloaded jobs on secondaries) works together with the SMP scheduler.
