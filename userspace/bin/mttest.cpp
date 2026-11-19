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

#include "../libc/ulibc.h"
/* mttest: preemptive multitasking and 1:1 kernel threads.
   Preemption of CPU hogs, parallelism, CPU affinity, per-thread TLS/errno, priority inheritance (direct and transitive),
   robust mutexes, cancellation, per-thread signals, fork/exec/exit from threads, stack guards, allocator and stdio stress,
   raw futex semantics, process-shared locks, scheduling classes and many ELF processes running at the same time. */
static int fails = 0;
static void chk(const char* name, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); fflush(stdout); if (!ok) ++fails; }
static void skip(const char* name, const char* why) { printf("SKIP %s (%s)\n", name, why); fflush(stdout); }
#define LD(p) __atomic_load_n(&(p), __ATOMIC_SEQ_CST)
#define ST(p, v) __atomic_store_n(&(p), (v), __ATOMIC_SEQ_CST)
#define ADD(p, v) __atomic_add_fetch(&(p), (v), __ATOMIC_SEQ_CST)

static int ncpu = 1;
static int rt_ok = 0;

static void spin_ms(unsigned long ms) { unsigned long t = clock_ms(); while (clock_ms() - t < ms) { } }
static void wait_flag(int* p, unsigned long limit_ms) { unsigned long t = clock_ms(); while (!LD(*p) && clock_ms() - t < limit_ms) sleep_ms(1); }
static void abs_after(struct timespec* ts, long ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += (ms % 1000) * 1000000L; ts->tv_sec += ms / 1000;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* create a thread with an explicit policy/priority (policy < 0: attribute defaults) and CPU (cpu < 0: any) */
static int spawn(pthread_t* t, int policy, int prio, int cpu, void* (*fn)(void*), void* arg)
{
    pthread_attr_t a; pthread_attr_init(&a);
    if (policy >= 0)
    {
        struct sched_param sp; sp.sched_priority = prio;
        pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&a, policy);
        pthread_attr_setschedparam(&a, &sp);
    }
    if (cpu >= 0) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); pthread_attr_setaffinity_np(&a, sizeof s, &s); }
    int rc = pthread_create(t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return rc;
}
static void main_affinity(int cpu)   /* cpu < 0: every CPU */
{
    cpu_set_t s; CPU_ZERO(&s);
    if (cpu >= 0) CPU_SET(cpu, &s); else for (int i = 0; i < ncpu; ++i) CPU_SET(i, &s);
    sched_setaffinity(0, sizeof s, &s);
}
static int main_sched(int policy, int prio) { struct sched_param sp; sp.sched_priority = prio; return sched_setscheduler(0, policy, &sp); }

/* a child process running fn(); returns the wait status */
static int run_child(int (*fn)(void))
{
    pid_t p = fork();
    if (p < 0) return -1000;
    if (p == 0) _exit(fn());
    int st = 0;
    if (waitpid(p, &st, 0) != p) return -1001;
    return st;
}
#define EXIT_CODE(st) (((st) & 0x7F) == 0 ? (((st) >> 8) & 0xFF) : -1)
#define KILLED_BY(st) ((st) & 0x7F)

/* ------------------------------------------------------------------------------------------------------------------ */
/* preemption: CPU hogs never yield, yet the machine stays responsive and shared fairly */
static int hog_stop; static unsigned long hog_cnt[2];
static void* hog_fn(void* a) { long i = (long)a; while (!LD(hog_stop)) __atomic_fetch_add(&hog_cnt[i], 1, __ATOMIC_RELAXED); return 0; }
static void t_preempt(void)
{
    pthread_t h[2]; int rc = 0; ST(hog_stop, 0); hog_cnt[0] = hog_cnt[1] = 0;
    for (long i = 0; i < 2; ++i) rc |= spawn(&h[i], SCHED_OTHER, 0, 0, hog_fn, (void*)i);
    unsigned long t0 = clock_ms(); sleep_ms(300); unsigned long el = clock_ms() - t0;
    ST(hog_stop, 1);
    pthread_join(h[0], 0); pthread_join(h[1], 0);
    unsigned long a = hog_cnt[0], b = hog_cnt[1], lo = a < b ? a : b, hi = a < b ? b : a;
    chk("preempt-hogs-do-not-starve-main", rc == 0 && el >= 290 && el < 500);
    chk("preempt-equal-hogs-share-cpu", lo > 0 && lo * 4 >= hi);
}

/* parallelism and affinity */
static unsigned long long par_mask; static int par_go;
static void* par_fn(void* a) { (void)a; while (!LD(par_go)) { } unsigned long end = clock_ms() + 300; while (clock_ms() < end) { int c = sched_getcpu(); if (c >= 0 && c < 64) __atomic_or_fetch(&par_mask, 1ULL << c, __ATOMIC_SEQ_CST); } return 0; }
static void* pin_fn(void* a)
{
    int want = (int)(long)a, bad = 0; unsigned long end = clock_ms() + 120;
    while (clock_ms() < end) if (sched_getcpu() != want) bad = 1;
    cpu_set_t s; CPU_ZERO(&s); sched_getaffinity(0, sizeof s, &s);
    if (!(CPU_COUNT(&s) == 1 && CPU_ISSET(want, &s))) bad = 1;
    return (void*)(long)bad;
}
static int mig_stop, mig_cpu = -1;
static void* mig_fn(void* a) { (void)a; while (!LD(mig_stop)) { ST(mig_cpu, sched_getcpu()); } return 0; }
static void t_parallel(void)
{
    if (ncpu > 1)
    {
        pthread_t t[4]; par_mask = 0; ST(par_go, 0);
        for (int i = 0; i < 4; ++i) pthread_create(&t[i], 0, par_fn, 0);
        ST(par_go, 1);
        for (int i = 0; i < 4; ++i) pthread_join(t[i], 0);
        chk("parallel-threads-use-several-cpus", __builtin_popcountll(par_mask) >= 2);
    }
    else skip("parallel-threads-use-several-cpus", "1 CPU");

    int n = ncpu < 4 ? ncpu : 4, bad = 0; pthread_t t[4];
    for (long i = 0; i < n; ++i) if (spawn(&t[i], -1, 0, (int)i, pin_fn, (void*)i) != 0) bad = 1;
    for (int i = 0; i < n; ++i) { void* r = (void*)1; pthread_join(t[i], &r); if (r) bad = 1; }
    chk("affinity-pinned-threads-stay-on-cpu", !bad);

    pthread_t m; ST(mig_stop, 0); ST(mig_cpu, -1);
    int last = ncpu - 1, ok = spawn(&m, -1, 0, 0, mig_fn, 0) == 0;
    sleep_ms(40); ok = ok && LD(mig_cpu) == 0;
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(last, &s); ok = ok && pthread_setaffinity_np(m, sizeof s, &s) == 0;
    sleep_ms(80); ok = ok && LD(mig_cpu) == last;
    CPU_ZERO(&s); CPU_SET(0, &s); ok = ok && pthread_setaffinity_np(m, sizeof s, &s) == 0;
    sleep_ms(80); ok = ok && LD(mig_cpu) == 0;
    ST(mig_stop, 1); pthread_join(m, 0);
    chk("affinity-change-migrates-running-thread", ok);
    CPU_ZERO(&s); ok = sched_setaffinity(0, sizeof s, &s) == -1;
    chk("affinity-empty-mask-rejected", ok);
}

/* thread-local storage and errno */
static __thread int tv_init = 0x5a5a; static __thread int tv_zero; static __thread char tv_buf[300];
static uintptr_t tls_addr[8];
static void* tls_fn(void* a)
{
    long id = (long)a; int ok = tv_init == 0x5a5a && tv_zero == 0 && tv_buf[299] == 0 && tv_buf[0] == 0;
    tls_addr[id] = (uintptr_t)&tv_init;
    tv_init = (int)id; tv_zero = (int)id * 2; memset(tv_buf, (int)id, sizeof tv_buf); errno = (int)id * 3;
    for (int i = 0; i < 20; ++i)
    {
        pthread_yield(); sleep_ms(1);
        if (tv_init != id || tv_zero != id * 2 || tv_buf[100] != (char)id || errno != id * 3) ok = 0;
    }
    errno = 0; if (close(9999) != -1 || errno != EBADF) ok = 0;
    return (void*)(long)ok;
}
static void t_tls(void)
{
    pthread_t t[8]; int ok = 1;
    for (long i = 0; i < 8; ++i) pthread_create(&t[i], 0, tls_fn, (void*)i);
    for (int i = 0; i < 8; ++i) { void* r = 0; pthread_join(t[i], &r); if (!r) ok = 0; }
    chk("tls-and-errno-are-per-thread", ok);
    int distinct = 1;
    for (int i = 0; i < 8; ++i) { if (tls_addr[i] == (uintptr_t)&tv_init) distinct = 0; for (int j = i + 1; j < 8; ++j) if (tls_addr[i] == tls_addr[j]) distinct = 0; }
    chk("tls-blocks-distinct", distinct && tv_init == 0x5a5a && tv_zero == 0);
}

/* priority inheritance: a low priority owner must not be starved by a medium priority hog while a high one waits */
static pthread_mutex_t pim1, pim2;
static int pi_l_has, pi_l2_has, pi_h_arrived, pi_h_got, pi_mm_timeout;
static void* pi_l(void* a) { (void)a; pthread_mutex_lock(&pim1); ST(pi_l_has, 1); while (!LD(pi_h_arrived)) { } spin_ms(30); pthread_mutex_unlock(&pim1); return 0; }
static void* pi_mm(void* a) { (void)a; unsigned long t = clock_ms(); while (!LD(pi_h_got) && clock_ms() - t < 600) { } ST(pi_mm_timeout, !LD(pi_h_got)); return 0; }
static void* pi_h(void* a) { (void)a; ST(pi_h_arrived, 1); pthread_mutex_lock(&pim1); ST(pi_h_got, 1); pthread_mutex_unlock(&pim1); return 0; }
static void* ch_l2(void* a) { (void)a; pthread_mutex_lock(&pim2); ST(pi_l2_has, 1); pthread_mutex_lock(&pim1); pthread_mutex_unlock(&pim1); pthread_mutex_unlock(&pim2); return 0; }
static void* ch_h(void* a) { (void)a; ST(pi_h_arrived, 1); pthread_mutex_lock(&pim2); ST(pi_h_got, 1); pthread_mutex_unlock(&pim2); return 0; }
static void pi_reset(void) { ST(pi_l_has, 0); ST(pi_l2_has, 0); ST(pi_h_arrived, 0); ST(pi_h_got, 0); ST(pi_mm_timeout, 0); }
static void t_pi(void)
{
    pthread_mutexattr_t ma; pthread_mutexattr_init(&ma); pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&pim1, &ma); pthread_mutex_init(&pim2, &ma);
    pthread_t l = 0, l2 = 0, mm = 0, h = 0;

    pi_reset();
    int ok = spawn(&l, SCHED_FIFO, 10, 0, pi_l, 0) == 0;
    wait_flag(&pi_l_has, 2000);
    ok = ok && spawn(&mm, SCHED_FIFO, 20, 0, pi_mm, 0) == 0 && spawn(&h, SCHED_FIFO, 30, 0, pi_h, 0) == 0;
    pthread_join(h, 0); pthread_join(mm, 0); pthread_join(l, 0);
    chk("pi-inversion-resolved", ok && !LD(pi_mm_timeout) && LD(pi_h_got));

    pi_reset();
    ok = spawn(&l, SCHED_FIFO, 10, 0, pi_l, 0) == 0;
    wait_flag(&pi_l_has, 2000);
    ok = ok && spawn(&l2, SCHED_FIFO, 15, 0, ch_l2, 0) == 0;
    wait_flag(&pi_l2_has, 2000); sleep_ms(5);
    ok = ok && spawn(&mm, SCHED_FIFO, 20, 0, pi_mm, 0) == 0 && spawn(&h, SCHED_FIFO, 30, 0, ch_h, 0) == 0;
    pthread_join(h, 0); pthread_join(mm, 0); pthread_join(l2, 0); pthread_join(l, 0);
    chk("pi-chain-transitive", ok && !LD(pi_mm_timeout) && LD(pi_h_got));
    pthread_mutex_destroy(&pim1); pthread_mutex_destroy(&pim2); pthread_mutexattr_destroy(&ma);
}

/* strict priorities and round-robin time slices */
static unsigned long fo_a_start, fo_b_end;
static void* fo_a(void* a) { (void)a; fo_a_start = clock_ms(); spin_ms(40); return 0; }
static void* fo_b(void* a) { (void)a; spin_ms(80); fo_b_end = clock_ms(); return 0; }
static unsigned long rr_start[2], rr_end[2];
static void* rr_fn(void* a) { long i = (long)a; rr_start[i] = clock_ms(); spin_ms(250); rr_end[i] = clock_ms(); return 0; }
static void t_rt(void)
{
    pthread_t a = 0, b = 0;
    fo_a_start = fo_b_end = 0;
    int ok = spawn(&a, SCHED_FIFO, 10, 0, fo_a, 0) == 0 && spawn(&b, SCHED_FIFO, 20, 0, fo_b, 0) == 0;
    pthread_join(a, 0); pthread_join(b, 0);
    chk("fifo-strict-priority-order", ok && fo_b_end != 0 && fo_a_start >= fo_b_end);
    sleep_ms(1000);   /* a fresh real-time budget period */
    ok = spawn(&a, SCHED_RR, 10, 0, rr_fn, (void*)0) == 0 && spawn(&b, SCHED_RR, 10, 0, rr_fn, (void*)1) == 0;
    pthread_join(a, 0); pthread_join(b, 0);
    chk("rr-time-slices-rotate", ok && rr_start[1] < rr_end[0] && rr_end[0] != 0 && rr_end[1] != 0);
}

/* nice values weigh the CPU share */
static pthread_barrier_t nice_bar;
static void* nice_fn(void* a)
{
    int nv = (int)(long)a; setpriority(PRIO_PROCESS, 0, nv);
    pthread_barrier_wait(&nice_bar);
    unsigned long end = clock_ms() + 500, c = 0;
    while (clock_ms() < end) ++c;
    return (void*)c;
}
static void t_nice(void)
{
    pthread_t a = 0, b = 0; void* ra = 0; void* rb = 0;
    pthread_barrier_init(&nice_bar, 0, 2);
    int ok = spawn(&a, SCHED_OTHER, 0, 0, nice_fn, (void*)0) == 0 && spawn(&b, SCHED_OTHER, 0, 0, nice_fn, (void*)10) == 0;
    pthread_join(a, &ra); pthread_join(b, &rb);
    pthread_barrier_destroy(&nice_bar);
    chk("nice-weights-cpu-share", ok && (unsigned long)ra > 2 * (unsigned long)rb && rb != 0);
}

/* robust mutexes */
static pthread_mutex_t rbm; static int rb_held;
static void* hold_and_die(void* a) { (void)a; pthread_mutex_lock(&rbm); ST(rb_held, 1); return 0; }
static void* hold_sleep_die(void* a) { (void)a; pthread_mutex_lock(&rbm); ST(rb_held, 1); sleep_ms(40); return 0; }
static int robust_case(int pi, int type)
{
    pthread_mutexattr_t ma; pthread_t t; int ok = 1;
    pthread_mutexattr_init(&ma); pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST); pthread_mutexattr_settype(&ma, type);
    if (pi) pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    /* owner dies, repaired */
    pthread_mutex_init(&rbm, &ma); ST(rb_held, 0);
    pthread_create(&t, 0, hold_and_die, 0); pthread_join(t, 0);
    ok = ok && pthread_mutex_lock(&rbm) == EOWNERDEAD && pthread_mutex_consistent(&rbm) == 0 && pthread_mutex_unlock(&rbm) == 0;
    ok = ok && pthread_mutex_lock(&rbm) == 0 && pthread_mutex_unlock(&rbm) == 0;
    pthread_mutex_destroy(&rbm);
    /* owner dies, not repaired: unrecoverable */
    pthread_mutex_init(&rbm, &ma);
    pthread_create(&t, 0, hold_and_die, 0); pthread_join(t, 0);
    ok = ok && pthread_mutex_lock(&rbm) == EOWNERDEAD && pthread_mutex_unlock(&rbm) == 0;
    ok = ok && pthread_mutex_lock(&rbm) == ENOTRECOVERABLE;
    pthread_mutex_destroy(&rbm);
    /* a waiter blocked while the owner is alive is woken with EOWNERDEAD when the owner dies */
    pthread_mutex_init(&rbm, &ma); ST(rb_held, 0);
    pthread_create(&t, 0, hold_sleep_die, 0); wait_flag(&rb_held, 2000);
    unsigned long t0 = clock_ms(); int rc = pthread_mutex_lock(&rbm); unsigned long el = clock_ms() - t0;
    ok = ok && rc == EOWNERDEAD && el >= 15 && el < 400;
    pthread_mutex_consistent(&rbm); pthread_mutex_unlock(&rbm); pthread_join(t, 0);
    pthread_mutex_destroy(&rbm); pthread_mutexattr_destroy(&ma);
    return ok;
}
static void t_robust(void)
{
    chk("robust-mutex-owner-death", robust_case(0, PTHREAD_MUTEX_NORMAL));
    chk("robust-pi-mutex-owner-death", robust_case(1, PTHREAD_MUTEX_NORMAL));
    chk("robust-errorcheck-owner-death", robust_case(0, PTHREAD_MUTEX_ERRORCHECK));
}

/* cancellation: asynchronous, blocked in read(), blocked in cond_wait with a cleanup handler, and disabled */
static int c_started, c_returned, c_cleanup, c_survived, c_after, cpipe[2];
static unsigned long c_spin;
static pthread_mutex_t cm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t cc = PTHREAD_COND_INITIALIZER;
static void* async_fn(void* a) { (void)a; int o; pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &o); ST(c_started, 1); for (;;) __atomic_fetch_add(&c_spin, 1, __ATOMIC_RELAXED); return 0; }
static void* read_fn(void* a) { (void)a; char c; ST(c_started, 1); ssize_t n = read(cpipe[0], &c, 1); ST(c_returned, 1); return (void*)(long)n; }
static void cleanup_unlock(void* a) { ST(c_cleanup, 1); pthread_mutex_unlock((pthread_mutex_t*)a); }
static void* cond_fn(void* a) { (void)a; pthread_mutex_lock(&cm); pthread_cleanup_push(cleanup_unlock, &cm); ST(c_started, 1); for (;;) pthread_cond_wait(&cc, &cm); pthread_cleanup_pop(1); return 0; }
static void* dis_fn(void* a)
{
    (void)a; int o; pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &o); ST(c_started, 1);
    unsigned long t = clock_ms(); while (clock_ms() - t < 60) { sleep_ms(1); pthread_testcancel(); }
    ST(c_survived, 1); pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &o); pthread_testcancel(); ST(c_after, 1);
    return 0;
}
static void cancel_reset(void) { ST(c_started, 0); ST(c_returned, 0); ST(c_cleanup, 0); ST(c_survived, 0); ST(c_after, 0); }
static void t_cancel(void)
{
    pthread_t t; void* r = 0;
    cancel_reset();
    int ok = pthread_create(&t, 0, async_fn, 0) == 0; wait_flag(&c_started, 2000); sleep_ms(10);
    ok = ok && pthread_cancel(t) == 0 && pthread_join(t, &r) == 0;
    chk("cancel-async-spinning-thread", ok && r == PTHREAD_CANCELED);

    cancel_reset(); pipe(cpipe);
    ok = pthread_create(&t, 0, read_fn, 0) == 0; wait_flag(&c_started, 2000); sleep_ms(20);
    ok = ok && pthread_cancel(t) == 0 && pthread_join(t, &r) == 0;
    chk("cancel-interrupts-blocked-read", ok && r == PTHREAD_CANCELED && !LD(c_returned));
    close(cpipe[0]); close(cpipe[1]);

    cancel_reset();
    ok = pthread_create(&t, 0, cond_fn, 0) == 0; wait_flag(&c_started, 2000); sleep_ms(20);
    ok = ok && pthread_cancel(t) == 0 && pthread_join(t, &r) == 0;
    chk("cancel-cond-wait-runs-cleanup", ok && r == PTHREAD_CANCELED && LD(c_cleanup) && pthread_mutex_trylock(&cm) == 0);
    pthread_mutex_unlock(&cm);

    cancel_reset();
    ok = pthread_create(&t, 0, dis_fn, 0) == 0; wait_flag(&c_started, 2000);
    ok = ok && pthread_cancel(t) == 0 && pthread_join(t, &r) == 0;
    chk("cancel-disabled-defers-until-enabled", ok && r == PTHREAD_CANCELED && LD(c_survived) && !LD(c_after));
}

/* signals: masks are per thread, process signals go to a thread that accepts them, tgkill targets one thread */
static int h_tid1, h_tid2, h_self, s_ready[2], s_tid[2], s_inherit, s_stop;
static void on_usr1(int s) { (void)s; ST(h_tid1, (int)gettid()); }
static void on_usr2(int s) { (void)s; ST(h_tid2, (int)gettid()); }
static void on_self(int s) { (void)s; ST(h_self, 1); }
static void* sig_worker(void* a)
{
    long i = (long)a; sigset_t cur; sigprocmask(SIG_BLOCK, 0, &cur);
    if (i == 0) { if (sigismember(&cur, SIGUSR1)) ST(s_inherit, 1); sigset_t u; sigemptyset(&u); sigaddset(&u, SIGUSR1); pthread_sigmask(SIG_UNBLOCK, &u, 0); }
    ST(s_tid[i], (int)gettid()); ST(s_ready[i], 1);
    while (!LD(s_stop)) sleep_ms(1);
    return 0;
}
static int w_sigwait_got;
static void* sigwait_fn(void* a) { sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR2); int sig = 0; ST(s_ready[0], 1); int rc = sigwait(&s, &sig); ST(w_sigwait_got, rc == 0 ? sig : -rc); (void)a; return 0; }
static void t_signals(void)
{
    struct sigaction sa; memset(&sa, 0, sizeof sa); sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_usr1; sigaction(SIGUSR1, &sa, 0);
    sa.sa_handler = on_usr2; sigaction(SIGUSR2, &sa, 0);
    sigset_t b1; sigemptyset(&b1); sigaddset(&b1, SIGUSR1);
    pthread_t w; ST(h_tid1, 0); ST(s_ready[0], 0); ST(s_stop, 0); ST(s_inherit, 0);
    pthread_sigmask(SIG_BLOCK, &b1, 0);
    pthread_create(&w, 0, sig_worker, (void*)0);
    wait_flag(&s_ready[0], 2000);
    kill(getpid(), SIGUSR1);
    for (int i = 0; i < 200 && !LD(h_tid1); ++i) sleep_ms(1);
    int got = LD(h_tid1), wt = LD(s_tid[0]);
    ST(s_stop, 1); pthread_join(w, 0);
    pthread_sigmask(SIG_UNBLOCK, &b1, 0);
    chk("signal-mask-inherited-by-new-thread", LD(s_inherit));
    chk("process-signal-goes-to-unblocked-thread", got != 0 && got == wt && got != (int)gettid());

    pthread_t x, y; ST(h_tid2, 0); ST(s_ready[0], 0); ST(s_ready[1], 0); ST(s_stop, 0);
    pthread_create(&x, 0, sig_worker, (void*)0); pthread_create(&y, 0, sig_worker, (void*)1);
    wait_flag(&s_ready[0], 2000); wait_flag(&s_ready[1], 2000);
    int ty = LD(s_tid[1]);
    int rc = tgkill(getpid(), ty, SIGUSR2);
    for (int i = 0; i < 200 && !LD(h_tid2); ++i) sleep_ms(1);
    int ok = rc == 0 && LD(h_tid2) == ty;
    ST(s_stop, 1); pthread_join(x, 0); pthread_join(y, 0);
    chk("tgkill-reaches-only-the-target-thread", ok);

    sigset_t b2; sigemptyset(&b2); sigaddset(&b2, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &b2, 0); ST(h_tid2, 0); ST(s_ready[0], 0); ST(w_sigwait_got, 0);
    pthread_create(&w, 0, sigwait_fn, 0); wait_flag(&s_ready[0], 2000); sleep_ms(10);
    kill(getpid(), SIGUSR2);
    pthread_join(w, 0);
    chk("sigwait-in-thread-consumes-process-signal", LD(w_sigwait_got) == SIGUSR2 && LD(h_tid2) == 0);
    pthread_sigmask(SIG_UNBLOCK, &b2, 0);

    sa.sa_handler = on_self; sigaction(SIGUSR1, &sa, 0); ST(h_self, 0);
    pthread_kill(pthread_self(), SIGUSR1);
    chk("pthread_kill-self-delivers-synchronously", LD(h_self) == 1);
}

/* fork from a thread while other threads use the allocator */
static int fk_stop;
static void* ret2(void* a) { (void)a; return (void*)2L; }
static void* alloc_loop(void* a) { (void)a; while (!LD(fk_stop)) { void* p = malloc(48); if (p) { memset(p, 1, 48); free(p); } } return 0; }
static void* fork_thr(void* a)
{
    (void)a; int ok = 1;
    for (int i = 0; i < 20; ++i)
    {
        pid_t p = fork();
        if (p == 0)
        {
            void* m = malloc(64); pthread_t x; void* r = 0;
            int good = m != 0 && pthread_create(&x, 0, ret2, 0) == 0 && pthread_join(x, &r) == 0 && (long)r == 2;
            for (int k = 0; k < 100; ++k) { void* q = malloc(32 + k); if (!q) good = 0; free(q); }
            free(m); _exit(good ? 7 : 8);
        }
        int st = 0; if (p < 0 || waitpid(p, &st, 0) != p || EXIT_CODE(st) != 7) ok = 0;
    }
    return (void*)(long)ok;
}
static void t_fork_thread(void)
{
    pthread_t h[2], f; void* r = 0; ST(fk_stop, 0);
    pthread_create(&h[0], 0, alloc_loop, 0); pthread_create(&h[1], 0, alloc_loop, 0);
    int ok = pthread_create(&f, 0, fork_thr, 0) == 0 && pthread_join(f, &r) == 0;
    ST(fk_stop, 1); pthread_join(h[0], 0); pthread_join(h[1], 0);
    chk("fork-from-thread-with-busy-allocators", ok && (long)r == 1);
}

/* exit paths of multi-threaded processes */
static void* exit9_thr(void* a) { (void)a; sleep_ms(10); exit(9); return 0; }
static void* hog_forever(void* a) { (void)a; for (;;) { } return 0; }
static void* exit12_thr(void* a) { (void)a; sleep_ms(30); exit(12); return 0; }
static void* quiet_thr(void* a) { (void)a; sleep_ms(20); return 0; }
static void* cond_block_thr(void* a) { (void)a; pthread_mutex_lock(&cm); for (;;) pthread_cond_wait(&cc, &cm); return 0; }
static int c_exit_from_thread(void) { pthread_t t; pthread_create(&t, 0, exit9_thr, 0); for (;;) sleep_ms(1); return 0; }
static int c_exit_with_hog(void) { pthread_t t; pthread_create(&t, 0, hog_forever, 0); sleep_ms(20); _exit(10); return 0; }
static int c_pexit_main_exit_thread(void) { pthread_t t; pthread_create(&t, 0, exit12_thr, 0); pthread_exit(0); return 0; }
static int c_pexit_main_quiet(void) { pthread_t t; pthread_create(&t, 0, quiet_thr, 0); pthread_exit(0); return 0; }
static void* exec_thr(void* a) { (void)a; sleep_ms(10); char* av[3]; av[0] = (char*)"/bin/mttest.elf"; av[1] = (char*)"exec-child"; av[2] = 0; execv("/bin/mttest.elf", av); _exit(99); return 0; }
static int c_exec_from_thread(void) { pthread_t t, h; pthread_create(&h, 0, hog_forever, 0); pthread_create(&t, 0, exec_thr, 0); for (;;) sleep_ms(1); return 0; }
static void t_exits(void)
{
    chk("exit-from-thread-ends-process", EXIT_CODE(run_child(c_exit_from_thread)) == 9);
    chk("exit-with-spinning-sibling", EXIT_CODE(run_child(c_exit_with_hog)) == 10);
    chk("main-pthread_exit-then-thread-exit-code", EXIT_CODE(run_child(c_pexit_main_exit_thread)) == 12);
    chk("last-thread-return-exits-zero", EXIT_CODE(run_child(c_pexit_main_quiet)) == 0);
    chk("exec-from-thread-replaces-whole-process", EXIT_CODE(run_child(c_exec_from_thread)) == 13);

    for (int k = 0; k < 2; ++k)
    {
        int sig = k == 0 ? SIGKILL : SIGTERM;
        pid_t p = fork();
        if (p == 0) { pthread_t t[4]; for (int i = 0; i < 4; ++i) pthread_create(&t[i], 0, cond_block_thr, 0); for (;;) sleep_ms(1); }
        sleep_ms(60); kill(p, sig);
        int st = 0, w = waitpid(p, &st, 0);
        chk(k == 0 ? "sigkill-kills-all-threads" : "sigterm-kills-threaded-process", w == p && KILLED_BY(st) == sig);
    }
    int st = 0; chk("no-zombie-children-left", waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD);
}

/* stack guard: an overflowing thread kills the process; with an alternate stack a handler can run */
static volatile int ov_lim = 1 << 30;
__attribute__((noinline)) static int ov_rec(int n) { volatile char b[1024]; b[0] = (char)n; if (n < ov_lim) return ov_rec(n + 1) + b[0]; return b[1]; }
static void* ov_thr(void* a) { (void)a; ov_rec(0); return 0; }
static char* alt_mem;
static void on_segv(int s) { (void)s; char here; if (&here >= alt_mem && &here < alt_mem + 32768) _exit(21); _exit(22); }
static void* ov_alt_thr(void* a)
{
    (void)a; alt_mem = (char*)malloc(32768);
    stack_t ss; ss.ss_sp = alt_mem; ss.ss_size = 32768; ss.ss_flags = 0; sigaltstack(&ss, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_segv; sa.sa_flags = SA_ONSTACK; sigemptyset(&sa.sa_mask); sigaction(SIGSEGV, &sa, 0);
    ov_rec(0); return 0;
}
static int c_overflow(void) { pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64 * 1024); pthread_t t; pthread_create(&t, &a, ov_thr, 0); for (;;) sleep_ms(1); return 0; }
static int c_overflow_alt(void) { pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64 * 1024); pthread_t t; pthread_create(&t, &a, ov_alt_thr, 0); for (;;) sleep_ms(1); return 0; }
static void t_guard(void)
{
    int st = run_child(c_overflow);
    chk("thread-stack-overflow-hits-guard-sigsegv", KILLED_BY(st) == SIGSEGV);
    st = run_child(c_overflow_alt);
    chk("thread-sigaltstack-handles-overflow", EXIT_CODE(st) == 21);
}

/* allocator and stdio under contention */
static void* mal_fn(void* a)
{
    long id = (long)a; unsigned seed = (unsigned)id * 7919U + 1U; void* p[64]; unsigned sz[64]; int bad = 0;
    memset(p, 0, sizeof p); memset(sz, 0, sizeof sz);
    for (int it = 0; it < 3000; ++it)
    {
        unsigned k = rand_r(&seed) % 64; unsigned char tag = (unsigned char)(k + (unsigned)id);
        if (p[k])
        {
            unsigned char* c = (unsigned char*)p[k];
            for (unsigned j = 0; j < sz[k]; j += 17) if (c[j] != tag) bad = 1;
            if (it & 1)
            {
                unsigned ns = rand_r(&seed) % 700 + 1; unsigned char* q = (unsigned char*)realloc(p[k], ns);
                if (!q) { bad = 1; continue; }
                unsigned keep = ns < sz[k] ? ns : sz[k];
                for (unsigned j = 0; j < keep; j += 17) if (q[j] != tag) bad = 1;
                memset(q, tag, ns); p[k] = q; sz[k] = ns;
            }
            else { free(p[k]); p[k] = 0; }
        }
        else
        {
            sz[k] = rand_r(&seed) % 700 + 1; p[k] = malloc(sz[k]);
            if (!p[k]) { bad = 1; continue; }
            memset(p[k], tag, sz[k]);
        }
    }
    for (int k = 0; k < 64; ++k) free(p[k]);
    return (void*)(long)bad;
}
static FILE* sf;
static void* io_fn(void* a) { long id = (long)a; for (int i = 0; i < 200; ++i) fprintf(sf, "T%ld line %d end\n", id, i); return 0; }
static void t_stress(void)
{
    pthread_t t[8]; int bad = 0;
    for (long i = 0; i < 8; ++i) pthread_create(&t[i], 0, mal_fn, (void*)i);
    for (int i = 0; i < 8; ++i) { void* r = (void*)1; pthread_join(t[i], &r); if (r) bad = 1; }
    chk("malloc-free-realloc-8-threads", !bad);

    sf = fopen("/tmp/mttest_io.txt", "w"); int ok = sf != 0;
    if (ok)
    {
        for (long i = 0; i < 6; ++i) pthread_create(&t[i], 0, io_fn, (void*)i);
        for (int i = 0; i < 6; ++i) pthread_join(t[i], 0);
        fclose(sf);
        FILE* f = fopen("/tmp/mttest_io.txt", "r"); char line[96]; int count = 0, next[6] = { 0, 0, 0, 0, 0, 0 };
        if (!f) ok = 0;
        else
        {
            while (fgets(line, sizeof line, f))
            {
                ++count; char* e = 0;
                if (line[0] != 'T') { ok = 0; continue; }
                long id = strtol(line + 1, &e, 10);
                if (id < 0 || id > 5 || strncmp(e, " line ", 6) != 0) { ok = 0; continue; }
                char* e2 = 0; long n = strtol(e + 6, &e2, 10);
                if (strcmp(e2, " end\n") != 0 || n != next[id]) ok = 0; else ++next[id];
            }
            fclose(f);
        }
        ok = ok && count == 1200;
        unlink("/tmp/mttest_io.txt");
    }
    chk("stdio-lines-intact-6-threads", ok);
}

/* raw futex semantics */
static volatile uint32_t fx, fx2; static int fw_woke;
static void* fw_fn(void* a) { (void)a; long r = futex(&fx, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, -1, 0, 0); if (r == 0) ADD(fw_woke, 1); return 0; }
static void* fwb_fn(void* a) { (void)a; long r = futex(&fx, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, 0, -1, 0, 0x2); if (r == 0) ADD(fw_woke, 1); return 0; }
static void t_futex(void)
{
    pthread_t t[5]; fx = 0; fx2 = 0; ST(fw_woke, 0);
    for (int i = 0; i < 5; ++i) pthread_create(&t[i], 0, fw_fn, 0);
    sleep_ms(50);
    long r1 = futex(&fx, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 2, 0, 0, 0); sleep_ms(30); int w1 = LD(fw_woke);
    long r2 = futex(&fx, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 0x7FFFFFFF, 0, 0, 0);
    for (int i = 0; i < 5; ++i) pthread_join(t[i], 0);
    chk("futex-wake-counts", r1 == 2 && w1 == 2 && r2 == 3 && LD(fw_woke) == 5);
    chk("futex-wake-no-waiters-zero", futex(&fx, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0) == 0);
    errno = 0; long rv = futex(&fx, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 1, 10, 0, 0); int e1 = errno;
    unsigned long t0 = clock_ms(); errno = 0; long rt = futex(&fx, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, 30, 0, 0); int e2 = errno; unsigned long el = clock_ms() - t0;
    chk("futex-wait-eagain-and-timeout", rv == -1 && e1 == EAGAIN && rt == -1 && e2 == ETIMEDOUT && el >= 25 && el < 200);

    ST(fw_woke, 0);
    for (int i = 0; i < 4; ++i) pthread_create(&t[i], 0, fw_fn, 0);
    sleep_ms(50);
    errno = 0; long bad = futex(&fx, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG, 1, 100, &fx2, 5); int e3 = errno;
    long moved = futex(&fx, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG, 1, 100, &fx2, 0);
    sleep_ms(30); int w2 = LD(fw_woke);
    long r3 = futex(&fx2, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 0x7FFFFFFF, 0, 0, 0);
    for (int i = 0; i < 4; ++i) pthread_join(t[i], 0);
    chk("futex-cmp-requeue", bad == -1 && e3 == EAGAIN && moved == 4 && w2 == 1 && r3 == 3 && LD(fw_woke) == 4);

    ST(fw_woke, 0); pthread_create(&t[0], 0, fwb_fn, 0); sleep_ms(40);
    long b1 = futex(&fx, FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0x1); sleep_ms(10); int wb = LD(fw_woke);
    long b2 = futex(&fx, FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0x2);
    pthread_join(t[0], 0);
    chk("futex-bitset-selective-wake", b1 == 0 && wb == 0 && b2 == 1 && LD(fw_woke) == 1);
}

/* timed operations */
static pthread_mutex_t tm_mtx = PTHREAD_MUTEX_INITIALIZER; static int tm_held;
static void* tm_holder(void* a) { (void)a; pthread_mutex_lock(&tm_mtx); ST(tm_held, 1); sleep_ms(200); pthread_mutex_unlock(&tm_mtx); return 0; }
static void* tm_sleeper(void* a) { (void)a; sleep_ms(100); return (void*)5L; }
static pthread_mutex_t tc_m = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t tc_c = PTHREAD_COND_INITIALIZER; static int tc_flag;
static void* tc_signaler(void* a) { (void)a; sleep_ms(20); pthread_mutex_lock(&tc_m); tc_flag = 1; pthread_cond_signal(&tc_c); pthread_mutex_unlock(&tc_m); return 0; }
static void t_timed(void)
{
    pthread_t h; struct timespec ts; ST(tm_held, 0);
    pthread_create(&h, 0, tm_holder, 0); wait_flag(&tm_held, 2000);
    abs_after(&ts, 40); unsigned long t0 = clock_ms(); int rc = pthread_mutex_timedlock(&tm_mtx, &ts); unsigned long el = clock_ms() - t0;
    chk("mutex-timedlock-times-out", rc == ETIMEDOUT && el >= 30 && el < 150);
    pthread_join(h, 0);

    void* r = 0; pthread_create(&h, 0, tm_sleeper, 0);
    abs_after(&ts, 20); int rj = pthread_timedjoin_np(h, &r, &ts);
    abs_after(&ts, 2000); int rj2 = pthread_timedjoin_np(h, &r, &ts);
    chk("timedjoin-timeout-then-success", rj == ETIMEDOUT && rj2 == 0 && (long)r == 5);

    sem_t s; sem_init(&s, 0, 0); abs_after(&ts, 30); errno = 0; t0 = clock_ms();
    int rs = sem_timedwait(&s, &ts); int es = errno; el = clock_ms() - t0; sem_destroy(&s);
    chk("sem-timedwait-times-out", rs == -1 && es == ETIMEDOUT && el >= 20 && el < 150);

    tc_flag = 0; pthread_create(&h, 0, tc_signaler, 0);
    pthread_mutex_lock(&tc_m); abs_after(&ts, 1000); t0 = clock_ms(); int rcw = 0;
    while (!tc_flag && rcw == 0) rcw = pthread_cond_timedwait(&tc_c, &tc_m, &ts);
    el = clock_ms() - t0; pthread_mutex_unlock(&tc_m); pthread_join(h, 0);
    chk("cond-timedwait-signalled-early", rcw == 0 && tc_flag && el < 400);
}

/* locks and semaphores shared between processes, robust across process death */
struct Shared { pthread_mutex_t rob; pthread_mutex_t m2; sem_t s; int counter; };
static void t_pshared(void)
{
    Shared* sh = (Shared*)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) { chk("pshared-mmap", 0); return; }
    memset(sh, 0, sizeof *sh);
    pthread_mutexattr_t ma; pthread_mutexattr_init(&ma); pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&sh->m2, &ma);
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST); pthread_mutex_init(&sh->rob, &ma);
    sem_init(&sh->s, 1, 0);
    pid_t p = fork();
    if (p == 0)
    {
        pthread_mutex_lock(&sh->rob);
        for (int i = 0; i < 500; ++i) { pthread_mutex_lock(&sh->m2); int v = sh->counter; if (i % 50 == 0) sched_yield(); sh->counter = v + 1; pthread_mutex_unlock(&sh->m2); }
        sem_post(&sh->s);
        _exit(0);   /* dies owning the robust mutex */
    }
    for (int i = 0; i < 500; ++i) { pthread_mutex_lock(&sh->m2); int v = sh->counter; if (i % 50 == 0) sched_yield(); sh->counter = v + 1; pthread_mutex_unlock(&sh->m2); }
    int rw = sem_wait(&sh->s);
    int st = 0; waitpid(p, &st, 0);
    chk("pshared-mutex-and-semaphore-two-processes", rw == 0 && sh->counter == 1000 && EXIT_CODE(st) == 0);
    int rc = pthread_mutex_lock(&sh->rob);
    chk("pshared-robust-mutex-owner-process-death", rc == EOWNERDEAD && pthread_mutex_consistent(&sh->rob) == 0 && pthread_mutex_unlock(&sh->rob) == 0);
    munmap(sh, 4096);
}

/* many short-lived processes that each use threads */
static int c_threads_child(void)
{
    pthread_t t[3]; void* r = 0; long sum = 0;
    for (long i = 0; i < 3; ++i) if (pthread_create(&t[i], 0, ret2, 0) != 0) return 1;
    for (int i = 0; i < 3; ++i) { if (pthread_join(t[i], &r) != 0) return 2; sum += (long)r; }
    return sum == 6 ? 0 : 3;
}
static void t_storm(void)
{
    int n = 16, started = 0, bad = 0;
    for (int i = 0; i < n; ++i) { pid_t p = fork(); if (p == 0) _exit(c_threads_child()); if (p > 0) ++started; }
    for (int i = 0; i < started; ++i) { int st = 0; if (waitpid(-1, &st, 0) < 0 || EXIT_CODE(st) != 0) bad = 1; }
    chk("process-storm-with-threads", started == n && !bad);
}

/* separate ELF processes running at the same time: start events must all precede the first end event */
static int run_elfs(int n, unsigned ms, int* concurrent, unsigned long* elapsed)
{
    int pf[2]; if (pipe(pf) < 0) return -1;
    unsigned long t0 = clock_ms(); int started = 0;
    for (int i = 0; i < n; ++i)
    {
        pid_t p = fork();
        if (p < 0) break;
        if (p == 0)
        {
            char a2[16], a3[16], a4[16]; snprintf(a2, sizeof a2, "%d", i); snprintf(a3, sizeof a3, "%u", ms); snprintf(a4, sizeof a4, "%d", pf[1]);
            char* av[6]; av[0] = (char*)"/bin/mttest.elf"; av[1] = (char*)"worker"; av[2] = a2; av[3] = a3; av[4] = a4; av[5] = 0;
            close(pf[0]); execv("/bin/mttest.elf", av); _exit(99);
        }
        ++started;
    }
    close(pf[1]);
    char buf[512]; int len = 0; ssize_t r;
    while (len < (int)sizeof buf && (r = read(pf[0], buf + len, sizeof buf - (size_t)len)) > 0) len += (int)r;
    close(pf[0]);
    int bad = 0;
    for (int i = 0; i < started; ++i) { int st = 0; if (waitpid(-1, &st, 0) < 0 || EXIT_CODE(st) != 0) bad = 1; }
    *elapsed = clock_ms() - t0;
    int starts_before_first_end = 0, seen_end = 0, nstart = 0, nend = 0;
    for (int i = 0; i + 4 <= len; i += 4)
    {
        if (buf[i] == 'S') { ++nstart; if (!seen_end) ++starts_before_first_end; }
        else if (buf[i] == 'E') { ++nend; seen_end = 1; }
        else bad = 1;
    }
    *concurrent = starts_before_first_end == n && nstart == n && nend == n;
    return bad ? -2 : started;
}
static void t_elfs(void)
{
    for (int pass = 0; pass < 2; ++pass)
    {
        int n = pass == 0 ? 4 : 8, conc = 0; unsigned long el = 0;
        int rc = run_elfs(n, 400, &conc, &el);
        char name[64]; snprintf(name, sizeof name, "%d-elf-processes-run-concurrently", n);
        printf("info: %d elfs rc=%d concurrent=%d elapsed=%lu ms\n", n, rc, conc, el);
        chk(name, rc == n && conc && el < 30000);
    }
}

/* scheduling attributes of threads */
static int sa_stop;
static void* attr_probe(void* a)
{
    (void)a; struct sched_param sp; int pol = sched_getscheduler(0); sched_getparam(0, &sp);
    cpu_set_t s; CPU_ZERO(&s); sched_getaffinity(0, sizeof s, &s);
    return (void*)(long)((pol == SCHED_RR && sp.sched_priority == 5 && CPU_COUNT(&s) == 1 && CPU_ISSET(0, &s)) ? 1 : 0);
}
static void* park_fn(void* a) { (void)a; while (!LD(sa_stop)) sleep_ms(2); return 0; }
static void* inherit_probe(void* a) { (void)a; struct sched_param sp; int pol = sched_getscheduler(0); sched_getparam(0, &sp); return (void*)(long)(pol == SCHED_RR && sp.sched_priority == 11); }
static void t_sched_attrs(void)
{
    pthread_t t; void* r = 0;
    int ok = spawn(&t, SCHED_RR, 5, 0, attr_probe, 0) == 0 && pthread_join(t, &r) == 0;
    chk("thread-attr-policy-priority-affinity-applied", ok && (long)r == 1);

    ST(sa_stop, 0); ok = pthread_create(&t, 0, park_fn, 0) == 0;
    int pol = -1; struct sched_param sp; sp.sched_priority = 0;
    ok = ok && pthread_getschedparam(t, &pol, &sp) == 0 && pol == SCHED_OTHER && sp.sched_priority == 0;
    sp.sched_priority = 9; ok = ok && pthread_setschedparam(t, SCHED_FIFO, &sp) == 0;
    pol = -1; sp.sched_priority = 0; ok = ok && pthread_getschedparam(t, &pol, &sp) == 0 && pol == SCHED_FIFO && sp.sched_priority == 9;
    ok = ok && pthread_setschedprio(t, 12) == 0;
    ok = ok && pthread_getschedparam(t, &pol, &sp) == 0 && sp.sched_priority == 12;
    sp.sched_priority = 100; ok = ok && pthread_setschedparam(t, SCHED_FIFO, &sp) != 0;
    ST(sa_stop, 1); pthread_join(t, 0);
    chk("pthread_setschedparam-roundtrip-and-range-check", ok);

    if (main_sched(SCHED_RR, 11) == 0)
    {
        ok = pthread_create(&t, 0, inherit_probe, 0) == 0 && pthread_join(t, &r) == 0;
        chk("new-thread-inherits-creator-policy", ok && (long)r == 1);
        main_sched(SCHED_OTHER, 0);
    }
    else skip("new-thread-inherits-creator-policy", "needs root");
}

static void watchdog(int s) { (void)s; static const char m[] = "FAIL watchdog expired\n"; write(1, m, sizeof m - 1); _exit(2); }

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "exec-child") == 0) return gettid() == getpid() ? 13 : 14;
    if (argc > 4 && strcmp(argv[1], "worker") == 0)
    {
        int id = atoi(argv[2]), fd = atoi(argv[4]); unsigned long ms = (unsigned long)atoi(argv[3]); char ev[8];
        snprintf(ev, sizeof ev, "S%02d\n", id); write(fd, ev, 4);
        spin_ms(ms);
        snprintf(ev, sizeof ev, "E%02d\n", id); write(fd, ev, 4);
        return 0;
    }

    signal(SIGALRM, watchdog); alarm(280);
    ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN); if (ncpu < 1) ncpu = 1; if (ncpu > 8) ncpu = 8;
    printf("mttest: %d CPU(s)\n", ncpu);

    t_preempt();
    t_parallel();
    t_tls();
    t_robust();
    t_cancel();
    t_signals();
    t_fork_thread();
    t_exits();
    t_guard();
    t_stress();
    t_futex();
    t_timed();
    t_pshared();
    t_storm();
    t_elfs();
    t_sched_attrs();
    t_nice();

    /* real-time scheduling needs privileges: run as the highest priority thread pinned to CPU 0 */
    if (main_sched(SCHED_FIFO, 40) == 0)
    {
        rt_ok = 1; main_affinity(0);
        t_pi();
        t_rt();
        main_sched(SCHED_OTHER, 0); main_affinity(-1);
    }
    else { skip("pi-inversion-resolved", "needs root"); skip("pi-chain-transitive", "needs root"); skip("fifo-strict-priority-order", "needs root"); skip("rr-time-slices-rotate", "needs root"); }
    (void)rt_ok;

    printf("mttest done fails=%d\n", fails);
    return fails ? 1 : 0;
}
