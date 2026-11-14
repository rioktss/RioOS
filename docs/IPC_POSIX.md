# SMP, poll/epoll, IPC and POSIX libc (round 9)

## SMP scheduler (`-smp N`, up to 8 CPUs)
- User processes (one kernel thread each) run in parallel on all CPUs. Kernel code is serialized by a Big Kernel Lock (test-and-set, owner-tracked): taken at every exception entry, released before `eret` to EL0. User code is fully parallel, kernel code is not.
- Per-CPU current thread, reschedule flag and time slice; idle contexts for secondaries; thread 0 (shell/boot) is pinned to CPU0. Only CPU0 runs the scheduler tick (10 ms); idle secondaries sleep until the next timer deadline.
- With `-smp 1` nothing changes (all lock operations are no-ops).
- Signals reach a process running on another CPU at its next kernel entry (<= 10 ms). Cross-CPU wake latency is bounded by sleep deadlines / the 10 ms tick.
- `sched_getcpu()`, `sched_getaffinity()`, `sysconf(_SC_NPROCESSORS_ONLN)` report real CPUs. `sched_setaffinity` pins threads (see Round 10).
- Tests: `smptest [soak-seconds]` (shared counters, futex mutex, atomics, parallel file I/O, append integrity, fork storm, COW isolation, pipe/unix-socket, signals to spinning processes, poll while all CPUs busy). `RIO_SMP=4 python3 tests/qemu_regression.py <qemu>` runs the whole regression with 4 CPUs.

## poll / epoll / timerfd / eventfd
- `poll`, `select`, `epoll_*` (emulated over poll: level-triggered, EPOLLET driven by kernel arrival counters (see Round 10), EPOLLONESHOT; max 8 instances x 64 watches), `eventfd`, `timerfd_*`, `clock_nanosleep`.

## IPC
- pipes, FIFOs, unix stream/datagram sockets, futex (`futex_wait/wake`), `shm_open` (file in /tmp/shm.<name>, MAP_SHARED), POSIX `sem_open` (file in /tmp/sem.<name> + futex), process-shared pthread primitives.

## POSIX libc additions
stat/lstat/fstat, fchmod/fchown/utimensat/futimens, truncate/fsync, access, flock, getcwd/chdir, dup3, opendir/readdir/scandir, fnmatch, glob, mkstemp/mkdtemp, basename/dirname, termios (raw/cooked, `vi` uses it), getpw*/getgr*, uname/gethostname, get/setrlimit, getrandom/arc4random/drand48, getopt/getopt_long, UTF-8 multibyte/wide functions, strptime, syslog (/var/log/messages), POSIX regex (`regcomp/regexec`, backrefs, leftmost-longest), `posixtest` (about 270 checks).

## Round 10: former limits that are now fixed
- **epoll EPOLLET**: the kernel keeps an arrival counter per descriptor (pipe, eventfd, timerfd, unix socket, TCP/UDP socket, signalfd, inotify; syscall `SYS_FDGEN`). An edge for IN/HUP/ERR is "the counter moved", also when data arrives while the process does nothing; data that is only left over after a partial read is not re-reported (like Linux). EPOLLOUT edges are not-writable → writable transitions. Descriptors without a counter (regular files, tty) keep the mask-transition rule.
- **setrlimit is enforced** (`SYS_RLIMIT`, per process, inherited by fork, raising the hard limit needs root): `RLIMIT_NOFILE` (descriptor allocation → EMFILE), `RLIMIT_FSIZE` (writes are cut at the limit, then SIGXFSZ + EFBIG), `RLIMIT_CPU` (SIGXCPU at the soft limit and every second after, SIGKILL at the hard limit), `RLIMIT_NPROC` (fork → EAGAIN for non-root, counted per uid), `RLIMIT_AS`/`RLIMIT_DATA` (heap + mmap area; brk/mmap → ENOMEM). `RLIMIT_STACK`, `CORE`, `RSS`, `MEMLOCK` are recorded only (fixed 64 KiB stack, no core dumps).
- **getrusage** reports real user and system CPU time (10 ms timer ticks charged to the process running on that CPU; a tick inside a system call counts as system time), also for `RUSAGE_CHILDREN` and `wait4`. `ru_maxrss` and context-switch counters are 0.
- **sched_setaffinity / sched_getaffinity** (`SYS_AFFINITY`): per-process CPU mask, inherited by fork, enforced by the SMP scheduler (a thread is only picked by an allowed CPU; it migrates at its next reschedule). Under `-smp 1` only CPU 0 exists (a mask without it is EINVAL).
- **signalfd** (`SFD_NONBLOCK`, `SFD_CLOEXEC`, mask update with an existing fd): `read` returns 128-byte `signalfd_siginfo` records (signo, code, pid, uid, sigqueue value); works with poll/epoll. The signals have to be blocked with `sigprocmask`, as on Linux.
- **inotify** (`inotify_init/init1`, `add_watch`, `rm_watch`, `IN_NONBLOCK`): IN_ACCESS, IN_MODIFY, IN_ATTRIB, IN_CLOSE_WRITE/NOWRITE, IN_OPEN, IN_MOVED_FROM/TO (with cookie), IN_MOVE_SELF, IN_CREATE, IN_DELETE, IN_DELETE_SELF, IN_IGNORED, IN_Q_OVERFLOW, IN_ISDIR, IN_ONESHOT, IN_MASK_ADD, IN_ONLYDIR, IN_DONT_FOLLOW; identical consecutive events are merged; 4 instances, 16 watches each, 64 queued events. Events come from the filesystem layer, so shell commands, `pkg`, `ssh-keygen` etc. are observed too.
- **iconv**: UTF-8, UTF-16 (LE/BE/BOM), UTF-32 (LE/BE/BOM), WCHAR_T, ASCII, ISO-8859-1; `//TRANSLIT` (unrepresentable → `?`), `//IGNORE`; EILSEQ, EINVAL (incomplete sequence), E2BIG with correct pointer/length updates.

## Known limits
24 fds per process; 64 KiB user stack; PATH_MAX 128; file I/O through fds is whole-file; no fmemopen, open_memstream, timer_create, nice, killpg; ISIG/OPOST of the tty are fixed; `connect()` blocks even for non-blocking TCP; `dirfd` unsupported; lchown on symlinks returns ENOSYS; madvise/mlock are no-ops.
- **SMP**: kernel code is still serialized by the big kernel lock; only user code runs in parallel. Replacing the BKL with fine-grained locks would touch every subsystem (scheduler, fd tables, filesystem, network stack, MMU) and was not attempted: a half-converted kernel is a worse risk than the lock. On a host with 2 cores `-smp 4` under MTTCG is slower than `-smp 1` because the vCPUs compete for host cores; that is a property of the emulator host, not of the kernel (use `-accel tcg,thread=single` or `-smp` ≤ host cores).
- affinity changes take effect at the next reschedule of the thread; AS/DATA limits count heap + mmap area (not the program image) and mmap space is not returned for reuse below the high-water mark; inotify does not report intermediate directories created by `mkdir -p`, nor events on other mounts' internal nodes beyond what the filesystem layer reports; RUSAGE_CHILDREN counts a child as soon as it exits, not when it is waited for; rlimit CPU time is counted in 10 ms ticks.
