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

/* RioOS libc part 5: POSIX threads (1:1 kernel threads) and every synchronization primitive built on futexes.
   See the note in ulibc.h. A thread is a kernel thread in the process's address space; threads run in parallel on
   different CPUs and are preempted by the timer. TLS (__thread) and errno are per thread: TPIDR_EL0 points at the
   thread's TLS block (AArch64 variant 1: a 16-byte TCB whose first word is the thread descriptor, then the TLS image). */
#include "ulibc.h"
#include "../../include/user_api.h"

extern "C" {

#define K_FUTEX 96
#define K_THREAD_CREATE 120
#define K_THREAD_EXIT 121
#define K_GETTID 122
#define K_TGKILL 123
#define K_ROBUST_LIST 124
#define K_SET_TID_ADDRESS 125
#define K_GETPID 3

#define DEFAULT_STACK (2UL * 1024UL * 1024UL)
#define DEFAULT_GUARD 4096UL
#define MIN_STACK 16384UL

extern unsigned long long __rio_realtime_ms(void);

/* ---- thread descriptor ------------------------------------------------------------------------------------------ */
struct TsdEntry { void* v; unsigned gen; unsigned pad; };
struct Pthread
{
    Pthread* self;                         /* first: also stored in the TCB word at TPIDR_EL0 */
    volatile uint32_t tid;                 /* kernel thread id; the kernel zeroes it (and wakes a joiner) when the thread ends */
    int detached;
    int joining;                           /* claimed by pthread_join / pthread_detach */
    int exited;
    int is_main;
    volatile int start;                    /* start gate: 0 wait, 1 go, 2 abort */
    void* retval;
    void* (*fn)(void*);
    void* arg;
    char* map_base; unsigned long map_size;        /* mapping to give back (0: caller-provided stack / main thread) */
    char* stack_lo; unsigned long stack_size; unsigned long guard_size;
    int cancel_state, cancel_type;
    volatile int cancel_pending;
    struct __pthread_cleanup* cleanup;
    TsdEntry* tsd; unsigned tsd_n;
    struct { void* next; long off; void* pending; } robust;
    char name[16];
    Pthread* reg_next; Pthread* reg_prev;
    Pthread* zombie_next;
};

__thread char __rio_tdata_anchor = 1;      /* keeps .tdata present so the linker symbols below are always defined */
extern char __tdata_start[], __tdata_end[];
/* __tls_memsz / __tls_align are absolute linker-script values (small numbers): load them as literals, an adrp could not reach them */
static inline unsigned long tls_memsz(void) { unsigned long v; __asm__("ldr %0, =__tls_memsz" : "=r"(v)); return v; }
static inline unsigned long tls_align(void) { unsigned long v; __asm__("ldr %0, =__tls_align" : "=r"(v)); return v; }

static Pthread main_thread;
static struct __rio_lock thr_lock;         /* registry, zombie list, atfork list */
static Pthread* reg_head = 0;
static Pthread* zombies = 0;
static int live_threads = 1;
int __rio_threads_started = 0;

static inline Pthread* self(void) { void** tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp)); return (Pthread*)tp[0]; }

/* ---- futex helpers -------------------------------------------------------------------------------------------- */
static long fx(volatile void* a, int op, uint32_t val, long timeout_ms, volatile void* a2, uint32_t val3)
{
    return (long)user_syscall6(K_FUTEX, (uint64_t)a, (uint64_t)op, val, timeout_ms < 0 ? ~0ULL : (uint64_t)timeout_ms, (uint64_t)a2, val3);
}
static long fx_wait(volatile int* a, int val, long timeout_ms) { return fx(a, FUTEX_WAIT, (uint32_t)val, timeout_ms, 0, 0); }
static void fx_wake(volatile int* a, int n) { (void)fx(a, FUTEX_WAKE, (uint32_t)n, 0, 0, 0); }
#define WAKE_ALL 0x7FFFFFFF

long futex(volatile uint32_t* addr, int op, uint32_t val, long timeout_ms, volatile uint32_t* addr2, uint32_t val3)
{
    long r = fx(addr, op, val, timeout_ms, addr2, val3);
    if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
    return r;
}

void __rio_lock_acquire(struct __rio_lock* l)
{
    int c = 0;
    if (__atomic_compare_exchange_n(&l->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    if (c != 2) c = __atomic_exchange_n(&l->v, 2, __ATOMIC_ACQUIRE);
    while (c != 0) { (void)fx_wait(&l->v, 2, -1); c = __atomic_exchange_n(&l->v, 2, __ATOMIC_ACQUIRE); }
}
void __rio_lock_release(struct __rio_lock* l)
{
    if (__atomic_exchange_n(&l->v, 0, __ATOMIC_RELEASE) == 2) fx_wake(&l->v, 1);
}

/* milliseconds until an absolute time on `clk` (0 = already past) */
static long abs_to_rel_ms(const struct timespec* abs, int clk)
{
    if (abs->tv_sec < 0) return 0;
    unsigned long long target = (unsigned long long)abs->tv_sec * 1000ULL + (unsigned long long)(abs->tv_nsec + 999999L) / 1000000ULL;
    unsigned long long now = clk == CLOCK_MONOTONIC ? (unsigned long long)clock_ms() : __rio_realtime_ms();
    if (target <= now) return 0;
    unsigned long long d = target - now;
    return d > 0x7FFFFFFFFFFFULL ? 0x7FFFFFFFFFFFL : (long)d;
}

/* ---- TLS blocks ------------------------------------------------------------------------------------------------- */
static unsigned long tls_off(void)
{
    unsigned long al = tls_align(); if (al < 16) al = 16;
    return (16 + al - 1) & ~(al - 1);
}
static unsigned long tls_block_size(void)
{
    unsigned long sz = tls_off() + tls_memsz();
    sz = (sz + 63) & ~63UL;
    return sz + ((sizeof(Pthread) + 63) & ~63UL);
}
/* lays out [TCB][TLS image][Pthread] at `base`; returns the descriptor */
static Pthread* tls_setup(char* base, Pthread* d)
{
    unsigned long off = tls_off(), filesz = (unsigned long)(__tdata_end - __tdata_start), memsz = tls_memsz();
    for (unsigned long i = 0; i < filesz; ++i) base[off + i] = __tdata_start[i];
    for (unsigned long i = filesz; i < memsz; ++i) base[off + i] = 0;
    *(void**)base = d;
    *(void**)(base + 8) = 0;
    return d;
}
static void zero(void* p, unsigned long n) { char* c = (char*)p; for (unsigned long i = 0; i < n; ++i) c[i] = 0; }

/* Called first thing by __libc_start, before anything touches errno: gives the main thread its TLS block. */
__attribute__((noinline)) void __rio_tls_init(void)
{
    unsigned long sz = (tls_block_size() + 4095UL) & ~4095UL;
    uint64_t r = user_syscall4(72 /* SYS_MMAPX */, sz, 3 /* PROT_READ | PROT_WRITE */, (uint64_t)-1L, (uint64_t)MAP_PRIVATE << 32);
    if ((long)r < 0 && (long)r > -4096) { (void)user_syscall1(USER_SYS_EXIT, 127); for (;;) {} }
    char* base = (char*)r;
    main_thread.self = &main_thread;
    main_thread.is_main = 1; main_thread.start = 1;
    main_thread.cancel_state = PTHREAD_CANCEL_ENABLE; main_thread.cancel_type = PTHREAD_CANCEL_DEFERRED;
    (void)tls_setup(base, &main_thread);
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(base) : "memory");
    main_thread.tid = (uint32_t)user_syscall1(K_SET_TID_ADDRESS, (uint64_t)&main_thread.tid);
    main_thread.robust.next = &main_thread.robust;       /* empty circular list: head points at itself */
    main_thread.robust.off = 0; main_thread.robust.pending = 0;
    (void)user_syscall1(K_ROBUST_LIST, (uint64_t)&main_thread.robust);
    reg_head = &main_thread;
}

pid_t gettid(void) { return (pid_t)self()->tid; }
int tgkill(pid_t tgid, pid_t tid, int sig)
{
    long r = (long)user_syscall3(K_TGKILL, (uint64_t)(long)tgid, (uint64_t)(long)tid, (uint64_t)(long)sig);
    if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
    return 0;
}

/* robust list: threads register their head with the kernel; the head points at itself when empty */
static void robust_init(Pthread* d) { d->robust.next = &d->robust; d->robust.off = 0; d->robust.pending = 0; }

/* ---- registry ---------------------------------------------------------------------------------------------------- */
static void reg_link(Pthread* d) { d->reg_prev = 0; d->reg_next = reg_head; if (reg_head) reg_head->reg_prev = d; reg_head = d; }
static void reg_unlink(Pthread* d)
{
    if (d->reg_prev) d->reg_prev->reg_next = d->reg_next; else if (reg_head == d) reg_head = d->reg_next;
    if (d->reg_next) d->reg_next->reg_prev = d->reg_prev;
    d->reg_next = d->reg_prev = 0;
}
static Pthread* reg_find(pthread_t t) { for (Pthread* p = reg_head; p; p = p->reg_next) if ((pthread_t)p == t) return p; return 0; }

static void zombies_reap(void)      /* detached threads whose kernel thread has ended give their mapping back */
{
    __rio_lock_acquire(&thr_lock);
    Pthread** pp = &zombies;
    Pthread* done = 0;
    while (*pp)
    {
        Pthread* z = *pp;
        if (__atomic_load_n(&z->tid, __ATOMIC_ACQUIRE) == 0) { *pp = z->zombie_next; z->zombie_next = done; done = z; }
        else pp = &z->zombie_next;
    }
    __rio_lock_release(&thr_lock);
    while (done) { Pthread* n = done->zombie_next; char* b = done->map_base; unsigned long s = done->map_size; if (b) munmap(b, s); done = n; }
}

/* ---- cancellation ------------------------------------------------------------------------------------------------- */
static void run_cleanups(Pthread* d)
{
    while (d->cleanup) { struct __pthread_cleanup* c = d->cleanup; d->cleanup = c->prev; c->fn(c->arg); }
}
static void cancel_exit(void)
{
    Pthread* d = self();
    d->cancel_state = PTHREAD_CANCEL_DISABLE;
    pthread_exit(PTHREAD_CANCELED);
}
void __rio_cancel_point(void)
{
    Pthread* d = self();
    if (__atomic_load_n(&d->cancel_pending, __ATOMIC_ACQUIRE) && d->cancel_state == PTHREAD_CANCEL_ENABLE) cancel_exit();
}
void __rio_cancel_eintr(void) { __rio_cancel_point(); }
static void cancel_handler(int sig)
{
    (void)sig;
    Pthread* d = self();
    if (__atomic_load_n(&d->cancel_pending, __ATOMIC_ACQUIRE) && d->cancel_state == PTHREAD_CANCEL_ENABLE && d->cancel_type == PTHREAD_CANCEL_ASYNCHRONOUS) cancel_exit();
}
static void threads_init(void)       /* first pthread_create: install the cancellation signal handler */
{
    static int done = 0;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
    struct sigaction sa; sa.sa_handler = cancel_handler; sa.sa_mask = 0; sa.sa_flags = 0;
    (void)sigaction(__SIGCANCEL, &sa, 0);
    __rio_threads_started = 1;
}

void __pthread_cleanup_push(struct __pthread_cleanup* c, void (*fn)(void*), void* arg)
{
    Pthread* d = self(); c->fn = fn; c->arg = arg; c->prev = d->cleanup; d->cleanup = c;
}
void __pthread_cleanup_pop(struct __pthread_cleanup* c, int execute)
{
    Pthread* d = self(); d->cleanup = c->prev;
    if (execute) c->fn(c->arg);
}

int pthread_setcancelstate(int state, int* old)
{
    if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE) return EINVAL;
    Pthread* d = self();
    if (old) *old = d->cancel_state;
    d->cancel_state = state;
    if (state == PTHREAD_CANCEL_ENABLE && d->cancel_type == PTHREAD_CANCEL_ASYNCHRONOUS) __rio_cancel_point();
    return 0;
}
int pthread_setcanceltype(int type, int* old)
{
    if (type != PTHREAD_CANCEL_DEFERRED && type != PTHREAD_CANCEL_ASYNCHRONOUS) return EINVAL;
    Pthread* d = self();
    if (old) *old = d->cancel_type;
    d->cancel_type = type;
    if (type == PTHREAD_CANCEL_ASYNCHRONOUS) __rio_cancel_point();
    return 0;
}
void pthread_testcancel(void) { __rio_cancel_point(); }

int pthread_cancel(pthread_t t)
{
    __rio_lock_acquire(&thr_lock);
    Pthread* d = reg_find(t);
    uint32_t tid = d && !d->exited ? __atomic_load_n(&d->tid, __ATOMIC_ACQUIRE) : 0;
    if (d && tid) __atomic_store_n(&d->cancel_pending, 1, __ATOMIC_RELEASE);
    __rio_lock_release(&thr_lock);
    if (!d || !tid) return ESRCH;
    if (d == self()) { if (d->cancel_type == PTHREAD_CANCEL_ASYNCHRONOUS) __rio_cancel_point(); return 0; }
    (void)tgkill((pid_t)self()->tid == 0 ? 0 : getpid(), (pid_t)tid, __SIGCANCEL);   /* interrupts a blocking call; ESRCH when it just ended */
    return 0;
}

/* ---- thread-specific data ------------------------------------------------------------------------------------------- */
static struct { unsigned used; unsigned gen; void (*dtor)(void*); } keytab[PTHREAD_KEYS_MAX];
static struct __rio_lock key_lock;

int pthread_key_create(pthread_key_t* k, void (*d)(void*))
{
    int rc = EAGAIN;
    __rio_lock_acquire(&key_lock);
    for (unsigned i = 0; i < PTHREAD_KEYS_MAX; ++i)
        if (!keytab[i].used) { keytab[i].used = 1; keytab[i].dtor = d; ++keytab[i].gen; *k = (keytab[i].gen << 10) | i; rc = 0; break; }
    __rio_lock_release(&key_lock);
    return rc;
}
int pthread_key_delete(pthread_key_t k)
{
    unsigned i = k & (PTHREAD_KEYS_MAX - 1), g = k >> 10;
    int rc = EINVAL;
    __rio_lock_acquire(&key_lock);
    if (keytab[i].used && keytab[i].gen == g) { keytab[i].used = 0; keytab[i].dtor = 0; rc = 0; }
    __rio_lock_release(&key_lock);
    return rc;
}
static int key_valid(pthread_key_t k)
{
    unsigned i = k & (PTHREAD_KEYS_MAX - 1);
    return keytab[i].used && keytab[i].gen == (k >> 10);
}
void* pthread_getspecific(pthread_key_t k)
{
    Pthread* d = self();
    unsigned i = k & (PTHREAD_KEYS_MAX - 1);
    if (!key_valid(k) || i >= d->tsd_n || d->tsd[i].gen != (k >> 10)) return 0;
    return d->tsd[i].v;
}
int pthread_setspecific(pthread_key_t k, const void* v)
{
    Pthread* d = self();
    unsigned i = k & (PTHREAD_KEYS_MAX - 1);
    if (!key_valid(k)) return EINVAL;
    if (i >= d->tsd_n)
    {
        unsigned n = i + 1; if (n < 16) n = 16;
        TsdEntry* t = (TsdEntry*)realloc(d->tsd, n * sizeof(TsdEntry));
        if (!t) return ENOMEM;
        for (unsigned j = d->tsd_n; j < n; ++j) { t[j].v = 0; t[j].gen = 0; t[j].pad = 0; }
        d->tsd = t; d->tsd_n = n;
    }
    d->tsd[i].v = (void*)v; d->tsd[i].gen = k >> 10;
    return 0;
}
static void run_destructors(Pthread* d)
{
    for (int round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; ++round)
    {
        int any = 0;
        for (unsigned i = 0; i < d->tsd_n; ++i)
        {
            void* v = d->tsd[i].v;
            if (!v) continue;
            void (*dt)(void*) = 0;
            __rio_lock_acquire(&key_lock);
            if (keytab[i].used && keytab[i].gen == d->tsd[i].gen) dt = keytab[i].dtor;
            __rio_lock_release(&key_lock);
            d->tsd[i].v = 0;
            if (dt) { dt(v); any = 1; }
        }
        if (!any) break;
    }
    if (d->tsd) { free(d->tsd); d->tsd = 0; d->tsd_n = 0; }
}

/* ---- thread start / exit ------------------------------------------------------------------------------------------------ */
extern long __rio_clone(void* stack_top, void* tls, volatile uint32_t* clear_tid, void (*entry)(void*), void* arg);
__asm__(".text\n.global __rio_clone\n__rio_clone:\n"
        " mov x8, #120\n svc #0\n cbnz x0, 1f\n"
        " mov x29, xzr\n mov x30, xzr\n mov x0, x4\n br x3\n"        /* child: new stack, x3 = entry, x4 = arg (copied from the caller) */
        "1: ret\n");

static void thread_exit_raw(void) { (void)user_syscall1(K_THREAD_EXIT, 0); for (;;) {} }

static void thread_start(void* a)
{
    Pthread* d = (Pthread*)a;
    while (__atomic_load_n(&d->start, __ATOMIC_ACQUIRE) == 0) (void)fx_wait(&d->start, 0, -1);
    if (d->start == 2) thread_exit_raw();
    (void)user_syscall1(K_ROBUST_LIST, (uint64_t)&d->robust);
    void* r = d->fn(d->arg);
    pthread_exit(r);
}

void pthread_exit(void* ret)
{
    Pthread* d = self();
    d->retval = ret;
    d->cancel_state = PTHREAD_CANCEL_DISABLE;
    run_cleanups(d);
    run_destructors(d);
    if (__atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL) == 0) exit(0);      /* last thread out ends the process */
    __rio_lock_acquire(&thr_lock);
    d->exited = 1;
    if (d->detached && !d->is_main) { reg_unlink(d); d->zombie_next = zombies; zombies = d; }
    __rio_lock_release(&thr_lock);
    thread_exit_raw();
    __builtin_unreachable();
}

pthread_t pthread_self(void) { return (pthread_t)self(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

/* ---- attributes ---------------------------------------------------------------------------------------------------------- */
int pthread_attr_init(pthread_attr_t* a)
{
    zero(a, sizeof(*a));
    a->detachstate = PTHREAD_CREATE_JOINABLE; a->inheritsched = PTHREAD_INHERIT_SCHED; a->schedpolicy = SCHED_OTHER; a->scope = PTHREAD_SCOPE_SYSTEM;
    a->stacksize = DEFAULT_STACK; a->guardsize = DEFAULT_GUARD;
    return 0;
}
int pthread_attr_destroy(pthread_attr_t* a) { (void)a; return 0; }
int pthread_attr_setdetachstate(pthread_attr_t* a, int s) { if (s != 0 && s != 1) return EINVAL; a->detachstate = s; return 0; }
int pthread_attr_getdetachstate(const pthread_attr_t* a, int* s) { *s = a->detachstate; return 0; }
int pthread_attr_setstacksize(pthread_attr_t* a, unsigned long sz) { if (sz < MIN_STACK) return EINVAL; a->stacksize = sz; return 0; }
int pthread_attr_getstacksize(const pthread_attr_t* a, unsigned long* sz) { *sz = a->stacksize; return 0; }
int pthread_attr_setstack(pthread_attr_t* a, void* addr, unsigned long sz)
{
    if (!addr || sz < MIN_STACK + 4096UL) return EINVAL;
    a->stackaddr = addr; a->stacksize = sz; return 0;
}
int pthread_attr_getstack(const pthread_attr_t* a, void** addr, unsigned long* sz) { *addr = a->stackaddr; *sz = a->stacksize; return 0; }
int pthread_attr_setguardsize(pthread_attr_t* a, unsigned long sz) { a->guardsize = (sz + 4095UL) & ~4095UL; return 0; }
int pthread_attr_getguardsize(const pthread_attr_t* a, unsigned long* sz) { *sz = a->guardsize; return 0; }
int pthread_attr_setschedpolicy(pthread_attr_t* a, int p)
{
    if (p != SCHED_OTHER && p != SCHED_FIFO && p != SCHED_RR && p != SCHED_BATCH && p != SCHED_IDLE) return EINVAL;
    a->schedpolicy = p; return 0;
}
int pthread_attr_getschedpolicy(const pthread_attr_t* a, int* p) { *p = a->schedpolicy; return 0; }
int pthread_attr_setschedparam(pthread_attr_t* a, const struct sched_param* p) { a->schedprio = p->sched_priority; return 0; }
int pthread_attr_getschedparam(const pthread_attr_t* a, struct sched_param* p) { p->sched_priority = a->schedprio; return 0; }
int pthread_attr_setinheritsched(pthread_attr_t* a, int i) { if (i != 0 && i != 1) return EINVAL; a->inheritsched = i; return 0; }
int pthread_attr_getinheritsched(const pthread_attr_t* a, int* i) { *i = a->inheritsched; return 0; }
int pthread_attr_setscope(pthread_attr_t* a, int s) { if (s == PTHREAD_SCOPE_PROCESS) return ENOTSUP; if (s != PTHREAD_SCOPE_SYSTEM) return EINVAL; a->scope = s; return 0; }
int pthread_attr_getscope(const pthread_attr_t* a, int* s) { *s = a->scope; return 0; }
int pthread_attr_setaffinity_np(pthread_attr_t* a, size_t size, const cpu_set_t* set)
{
    if (size < sizeof(unsigned long)) return EINVAL;
    if (!set) { a->affinity_set = 0; return 0; }
    if (set->bits[0] == 0) return EINVAL;
    a->affinity = set->bits[0]; a->affinity_set = 1; return 0;
}
int pthread_attr_getaffinity_np(const pthread_attr_t* a, size_t size, cpu_set_t* set)
{
    if (size < sizeof(unsigned long)) return EINVAL;
    set->bits[0] = a->affinity_set ? (unsigned long)a->affinity : ~0UL;
    return 0;
}

/* ---- create / join / detach ------------------------------------------------------------------------------------------------------ */
static int apply_attr(uint32_t tid, const pthread_attr_t* a)
{
    if (a->inheritsched == PTHREAD_EXPLICIT_SCHED)
    {
        struct sched_param sp; sp.sched_priority = a->schedprio;
        if (sched_setscheduler((pid_t)tid, a->schedpolicy, &sp) != 0) return errno ? errno : EPERM;
    }
    if (a->affinity_set)
    {
        cpu_set_t cs; cs.bits[0] = (unsigned long)a->affinity;
        if (sched_setaffinity((pid_t)tid, sizeof(cs), &cs) != 0) return errno ? errno : EINVAL;
    }
    return 0;
}

int pthread_create(pthread_t* t, const pthread_attr_t* attr, void* (*fn)(void*), void* arg)
{
    pthread_attr_t def;
    if (!attr) { pthread_attr_init(&def); attr = &def; }
    threads_init();
    zombies_reap();
    unsigned long ssz = (attr->stacksize + 4095UL) & ~4095UL, guard = attr->guardsize, blk = (tls_block_size() + 63UL) & ~63UL;
    char* map = 0; unsigned long map_size = 0;
    char* block;
    char* stack_lo; unsigned long stack_size;
    if (attr->stackaddr)                                       /* caller-provided stack: the TLS block is carved from its top */
    {
        char* top = (char*)attr->stackaddr + attr->stacksize;
        block = (char*)(((unsigned long)top - blk) & ~63UL);
        stack_lo = (char*)attr->stackaddr; stack_size = (unsigned long)(block - stack_lo);
        if ((long)stack_size < (long)MIN_STACK) return EINVAL;
    }
    else
    {
        guard = (guard + 4095UL) & ~4095UL;
        map_size = guard + ssz + ((blk + 4095UL) & ~4095UL);
        void* m = mmap(0, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) return EAGAIN;
        map = (char*)m;
        if (guard && mprotect(map, guard, PROT_NONE) != 0) { munmap(map, map_size); return EAGAIN; }
        block = map + map_size - blk;
        stack_lo = map + guard; stack_size = ssz;
    }
    Pthread* d = (Pthread*)(block + (((tls_off() + tls_memsz()) + 63UL) & ~63UL));
    zero(d, sizeof(*d));
    (void)tls_setup(block, d);
    d->self = d; d->fn = fn; d->arg = arg; d->map_base = map; d->map_size = map_size;
    d->stack_lo = stack_lo; d->stack_size = stack_size; d->guard_size = attr->stackaddr ? 0 : guard;
    d->detached = attr->detachstate == PTHREAD_CREATE_DETACHED;
    d->cancel_state = PTHREAD_CANCEL_ENABLE; d->cancel_type = PTHREAD_CANCEL_DEFERRED;
    robust_init(d);
    int gated = attr->inheritsched == PTHREAD_EXPLICIT_SCHED || attr->affinity_set;
    d->start = gated ? 0 : 1;
    __rio_lock_acquire(&thr_lock);
    reg_link(d);
    __rio_lock_release(&thr_lock);
    __atomic_add_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);
    long r = __rio_clone((void*)(((unsigned long)block) & ~15UL), block, &d->tid, thread_start, d);
    if (r <= 0)
    {
        __atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);
        __rio_lock_acquire(&thr_lock); reg_unlink(d); __rio_lock_release(&thr_lock);
        if (map) munmap(map, map_size);
        return r < 0 && r > -4096 ? (int)-r : EAGAIN;
    }
    if (gated)
    {
        int rc = apply_attr((uint32_t)r, attr);
        if (rc != 0)
        {
            __atomic_store_n(&d->start, 2, __ATOMIC_RELEASE); fx_wake(&d->start, 1);
            for (;;) { uint32_t tv = __atomic_load_n(&d->tid, __ATOMIC_ACQUIRE); if (!tv) break; (void)fx_wait((volatile int*)&d->tid, (int)tv, -1); }
            __atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);
            __rio_lock_acquire(&thr_lock); reg_unlink(d); __rio_lock_release(&thr_lock);
            if (map) munmap(map, map_size);
            return rc;
        }
        __atomic_store_n(&d->start, 1, __ATOMIC_RELEASE); fx_wake(&d->start, 1);
    }
    if (t) *t = (pthread_t)d;
    return 0;
}

static int join_common(pthread_t id, void** ret, long timeout_ms, int try_only)
{
    Pthread* me = self();
    __rio_lock_acquire(&thr_lock);
    Pthread* d = reg_find(id);
    if (!d) { __rio_lock_release(&thr_lock); return ESRCH; }
    if (d == me) { __rio_lock_release(&thr_lock); return EDEADLK; }
    if (d->detached || d->joining) { __rio_lock_release(&thr_lock); return EINVAL; }
    d->joining = 1;
    __rio_lock_release(&thr_lock);
    unsigned long long deadline = timeout_ms >= 0 ? (unsigned long long)clock_ms() + (unsigned long long)timeout_ms : 0;
    for (;;)
    {
        uint32_t tv = __atomic_load_n(&d->tid, __ATOMIC_ACQUIRE);
        if (tv == 0) break;
        if (try_only) { d->joining = 0; return EBUSY; }
        long rem = -1;
        if (timeout_ms >= 0) { unsigned long long now = clock_ms(); if (now >= deadline) { d->joining = 0; return ETIMEDOUT; } rem = (long)(deadline - now); }
        long r = fx_wait((volatile int*)&d->tid, (int)tv, rem);
        if (r == -EINTR)
        {
            if (me->cancel_pending && me->cancel_state == PTHREAD_CANCEL_ENABLE) { d->joining = 0; __rio_cancel_point(); }
        }
    }
    if (ret) *ret = d->retval;
    __rio_lock_acquire(&thr_lock);
    reg_unlink(d);
    __rio_lock_release(&thr_lock);
    if (d->map_base) munmap(d->map_base, d->map_size);
    return 0;
}
int pthread_join(pthread_t id, void** ret) { __rio_cancel_point(); return join_common(id, ret, -1, 0); }
int pthread_tryjoin_np(pthread_t id, void** ret) { return join_common(id, ret, 0, 1); }
int pthread_timedjoin_np(pthread_t id, void** ret, const struct timespec* abs) { __rio_cancel_point(); return join_common(id, ret, abs ? abs_to_rel_ms(abs, CLOCK_REALTIME) : -1, 0); }

int pthread_detach(pthread_t id)
{
    int rc = 0;
    Pthread* free_me = 0;
    __rio_lock_acquire(&thr_lock);
    Pthread* d = reg_find(id);
    if (!d) rc = ESRCH;
    else if (d->detached || d->joining) rc = EINVAL;
    else
    {
        d->detached = 1;
        if (d->exited && !d->is_main) { reg_unlink(d); d->zombie_next = zombies; zombies = d; free_me = d; }
    }
    __rio_lock_release(&thr_lock);
    if (free_me) zombies_reap();
    return rc;
}

int pthread_yield(void) { sched_yield(); return 0; }

int pthread_kill(pthread_t id, int sig)
{
    if (sig < 0 || sig >= 32) return EINVAL;
    __rio_lock_acquire(&thr_lock);
    Pthread* d = reg_find(id);
    uint32_t tid = d && !d->exited ? __atomic_load_n(&d->tid, __ATOMIC_ACQUIRE) : 0;
    __rio_lock_release(&thr_lock);
    if (!d || !tid) return ESRCH;
    if (sig == 0) return 0;
    if (tgkill(getpid(), (pid_t)tid, sig) != 0) return errno;
    return 0;
}
int pthread_sigmask(int how, const sigset_t* set, sigset_t* old) { return sigprocmask(how, set, old) == 0 ? 0 : errno; }
int pthread_setconcurrency(int n) { return n < 0 ? EINVAL : 0; }
int pthread_getconcurrency(void) { return 0; }

/* scheduling of a particular thread */
static int th_tid(pthread_t id, uint32_t* tid)
{
    __rio_lock_acquire(&thr_lock);
    Pthread* d = reg_find(id);
    uint32_t t = d && !d->exited ? __atomic_load_n(&d->tid, __ATOMIC_ACQUIRE) : 0;
    __rio_lock_release(&thr_lock);
    if (!d || !t) return ESRCH;
    *tid = t; return 0;
}
int pthread_setschedparam(pthread_t id, int policy, const struct sched_param* p)
{
    uint32_t tid; int rc = th_tid(id, &tid); if (rc) return rc;
    return sched_setscheduler((pid_t)tid, policy, p) == 0 ? 0 : errno;
}
int pthread_getschedparam(pthread_t id, int* policy, struct sched_param* p)
{
    uint32_t tid; int rc = th_tid(id, &tid); if (rc) return rc;
    int pol = sched_getscheduler((pid_t)tid); if (pol < 0) return errno;
    if (sched_getparam((pid_t)tid, p) != 0) return errno;
    *policy = pol; return 0;
}
int pthread_setschedprio(pthread_t id, int prio)
{
    uint32_t tid; int rc = th_tid(id, &tid); if (rc) return rc;
    struct sched_param sp; sp.sched_priority = prio;
    return sched_setparam((pid_t)tid, &sp) == 0 ? 0 : errno;
}
int pthread_setaffinity_np(pthread_t id, size_t size, const cpu_set_t* set)
{
    uint32_t tid; int rc = th_tid(id, &tid); if (rc) return rc;
    return sched_setaffinity((pid_t)tid, size, set) == 0 ? 0 : errno;
}
int pthread_getaffinity_np(pthread_t id, size_t size, cpu_set_t* set)
{
    uint32_t tid; int rc = th_tid(id, &tid); if (rc) return rc;
    return sched_getaffinity((pid_t)tid, size, set) == 0 ? 0 : errno;
}
int pthread_setname_np(pthread_t id, const char* name)
{
    if (!name) return EINVAL;
    unsigned long n = 0; while (name[n]) ++n;
    if (n > 15) return ERANGE;
    __rio_lock_acquire(&thr_lock);
    Pthread* d = reg_find(id);
    if (d) { for (unsigned long i = 0; i <= n; ++i) d->name[i] = name[i]; }
    __rio_lock_release(&thr_lock);
    return d ? 0 : ESRCH;
}
int pthread_getname_np(pthread_t id, char* name, size_t len)
{
    int rc = 0;
    __rio_lock_acquire(&thr_lock);
    Pthread* d = reg_find(id);
    if (!d) rc = ESRCH;
    else { unsigned long n = 0; while (d->name[n]) ++n; if (len < n + 1) rc = ERANGE; else for (unsigned long i = 0; i <= n; ++i) name[i] = d->name[i]; }
    __rio_lock_release(&thr_lock);
    return rc;
}
int pthread_getattr_np(pthread_t id, pthread_attr_t* a)
{
    int rc = 0;
    pthread_attr_init(a);
    __rio_lock_acquire(&thr_lock);
    Pthread* d = reg_find(id);
    if (!d) rc = ESRCH;
    else { a->detachstate = d->detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE; a->stackaddr = d->stack_lo; a->stacksize = d->stack_size; a->guardsize = d->guard_size; }
    uint32_t tid = d ? __atomic_load_n(&d->tid, __ATOMIC_ACQUIRE) : 0;
    __rio_lock_release(&thr_lock);
    if (rc) return rc;
    if (tid)
    {
        int pol = sched_getscheduler((pid_t)tid); struct sched_param sp; cpu_set_t cs;
        if (pol >= 0 && sched_getparam((pid_t)tid, &sp) == 0) { a->schedpolicy = pol; a->schedprio = sp.sched_priority; a->inheritsched = PTHREAD_EXPLICIT_SCHED; }
        if (sched_getaffinity((pid_t)tid, sizeof(cs), &cs) == 0) { a->affinity = cs.bits[0]; a->affinity_set = 1; }
    }
    return 0;
}

/* ---- fork ------------------------------------------------------------------------------------------------------------------------ */
struct AtFork { AtFork* next; void (*prepare)(void); void (*parent)(void); void (*child)(void); };
static AtFork* atfork_head = 0;
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
    AtFork* n = (AtFork*)malloc(sizeof(AtFork));
    if (!n) return ENOMEM;
    n->prepare = prepare; n->parent = parent; n->child = child;
    __rio_lock_acquire(&thr_lock);
    n->next = atfork_head; atfork_head = n;          /* newest first: prepare runs in reverse registration order */
    __rio_lock_release(&thr_lock);
    return 0;
}
extern void __rio_libc_fork_prepare(void);
extern void __rio_libc_fork_parent(void);
extern void __rio_libc_fork_child(void);
void __rio_fork_prepare(void)
{
    for (AtFork* a = atfork_head; a; a = a->next) if (a->prepare) a->prepare();
    __rio_libc_fork_prepare();
    __rio_lock_acquire(&thr_lock);
}
void __rio_fork_parent(void)
{
    __rio_lock_release(&thr_lock);
    __rio_libc_fork_parent();
    AtFork* rev = 0;
    for (AtFork* a = atfork_head; a; a = a->next) { (void)rev; }
    /* parent and child handlers run in registration order: walk the list from the oldest entry */
    int n = 0; for (AtFork* a = atfork_head; a; a = a->next) ++n;
    for (int i = n - 1; i >= 0; --i) { AtFork* a = atfork_head; for (int k = 0; k < i; ++k) a = a->next; if (a->parent) a->parent(); }
}
void __rio_fork_child(void)
{
    /* only the calling thread exists here: forget the others (their stacks are unused), restart the locks */
    thr_lock.v = 0; key_lock.v = 0;
    Pthread* me = self();
    for (Pthread* p = reg_head; p; )
    {
        Pthread* n = p->reg_next;
        if (p != me && p->map_base) munmap(p->map_base, p->map_size);
        p = n;
    }
    for (Pthread* z = zombies; z; ) { Pthread* n = z->zombie_next; if (z != me && z->map_base) munmap(z->map_base, z->map_size); z = n; }
    zombies = 0;
    reg_head = me; me->reg_next = me->reg_prev = 0;
    live_threads = 1;
    me->is_main = 1; me->exited = 0; me->joining = 0; me->detached = 0;
    me->tid = (uint32_t)user_syscall1(K_SET_TID_ADDRESS, (uint64_t)&me->tid);
    robust_init(me);
    (void)user_syscall1(K_ROBUST_LIST, (uint64_t)&me->robust);
    __rio_libc_fork_child();
    int n = 0; for (AtFork* a = atfork_head; a; a = a->next) ++n;
    for (int i = n - 1; i >= 0; --i) { AtFork* a = atfork_head; for (int k = 0; k < i; ++k) a = a->next; if (a->child) a->child(); }
}
void __rio_thread_after_fork(void) {}

/* ---- once ------------------------------------------------------------------------------------------------------------------------------ */
static void once_undo(void* o) { __atomic_store_n((int*)o, 0, __ATOMIC_RELEASE); fx_wake((volatile int*)o, WAKE_ALL); }
int pthread_once(pthread_once_t* once, void (*fn)(void))
{
    volatile int* o = (volatile int*)once;
    for (;;)
    {
        int s = __atomic_load_n(o, __ATOMIC_ACQUIRE);
        if (s == 2) return 0;
        if (s == 0)
        {
            int z = 0;
            if (!__atomic_compare_exchange_n(o, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
            struct __pthread_cleanup c; __pthread_cleanup_push(&c, once_undo, (void*)once);
            fn();
            __pthread_cleanup_pop(&c, 0);
            __atomic_store_n(o, 2, __ATOMIC_RELEASE);
            fx_wake(o, WAKE_ALL);
            return 0;
        }
        (void)fx_wait(o, 1, -1);
    }
}

/* ---- mutex ---------------------------------------------------------------------------------------------------------------------------- */
#define MK_PI 1
#define MK_ROBUST 2
#define MK_SHARED 4
#define MK_PROTECT 8
#define MS_OK 0
#define MS_INCONSISTENT 1
#define MS_DEAD 2

int pthread_mutexattr_init(pthread_mutexattr_t* a) { zero(a, sizeof(*a)); a->type = PTHREAD_MUTEX_NORMAL; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t* a) { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t* a, int t) { if (t < 0 || t > 2) return EINVAL; a->type = t; return 0; }
int pthread_mutexattr_gettype(const pthread_mutexattr_t* a, int* t) { *t = a->type; return 0; }
int pthread_mutexattr_setprotocol(pthread_mutexattr_t* a, int p) { if (p < 0 || p > 2) return EINVAL; a->protocol = p; return 0; }
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t* a, int* p) { *p = a->protocol; return 0; }
int pthread_mutexattr_setrobust(pthread_mutexattr_t* a, int r) { if (r != 0 && r != 1) return EINVAL; a->robust = r; return 0; }
int pthread_mutexattr_getrobust(const pthread_mutexattr_t* a, int* r) { *r = a->robust; return 0; }
int pthread_mutexattr_setpshared(pthread_mutexattr_t* a, int s) { if (s != 0 && s != 1) return EINVAL; a->pshared = s; return 0; }
int pthread_mutexattr_getpshared(const pthread_mutexattr_t* a, int* s) { *s = a->pshared; return 0; }
int pthread_mutexattr_setprioceiling(pthread_mutexattr_t* a, int c) { if (c < 0 || c > 99) return EINVAL; a->ceiling = c; return 0; }
int pthread_mutexattr_getprioceiling(const pthread_mutexattr_t* a, int* c) { *c = a->ceiling; return 0; }

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a)
{
    zero(m, sizeof(*m));
    if (!a) return 0;
    m->type = a->type;
    if (a->protocol == PTHREAD_PRIO_INHERIT) m->kind |= MK_PI;
    if (a->protocol == PTHREAD_PRIO_PROTECT) { m->kind |= MK_PROTECT; m->ceiling = a->ceiling; }
    if (a->robust) m->kind |= MK_ROBUST;
    if (a->pshared) m->kind |= MK_SHARED;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t* m)
{
    if (__atomic_load_n(&m->lock, __ATOMIC_ACQUIRE) & (int)FUTEX_TID_MASK) return EBUSY;
    if (m->kind & MK_PROTECT) { /* nothing held */ }
    return 0;
}

/* robust list of the calling thread: nodes are the mutexes' rnext words; the kernel reads them when the thread ends */
static void robust_add(Pthread* d, pthread_mutex_t* m)
{
    m->rnext = d->robust.next;
    __atomic_store_n(&d->robust.next, (void*)&m->rnext, __ATOMIC_RELEASE);
}
static void robust_remove(Pthread* d, pthread_mutex_t* m)
{
    void** pp = &d->robust.next;
    while (*pp != (void*)&d->robust)
    {
        if (*pp == (void*)&m->rnext) { __atomic_store_n(pp, m->rnext, __ATOMIC_RELEASE); break; }
        pp = (void**)*pp;
    }
}
static void robust_prepare(Pthread* d, pthread_mutex_t* m)
{
    d->robust.off = (long)((char*)&m->lock - (char*)&m->rnext);
    __atomic_store_n(&d->robust.pending, (void*)&m->rnext, __ATOMIC_RELEASE);
}
static void robust_done(Pthread* d) { __atomic_store_n(&d->robust.pending, (void*)0, __ATOMIC_RELEASE); }

static void protect_raise(pthread_mutex_t* m)
{
    struct sched_param sp;
    int pol = sched_getscheduler(0);
    if (pol < 0 || sched_getparam(0, &sp) != 0) { m->saved = -1; return; }
    int cur = (pol == SCHED_FIFO || pol == SCHED_RR) ? sp.sched_priority : 0;
    if (cur >= m->ceiling) { m->saved = -1; return; }
    m->saved = (pol << 8) | (sp.sched_priority & 0xFF);
    struct sched_param np; np.sched_priority = m->ceiling;
    if (sched_setscheduler(0, SCHED_FIFO, &np) != 0) m->saved = -1;
}
static void protect_restore(pthread_mutex_t* m)
{
    if (m->saved == -1 || m->saved == 0) { m->saved = 0; return; }
    struct sched_param sp; sp.sched_priority = m->saved & 0xFF;
    (void)sched_setscheduler(0, m->saved >> 8, &sp);
    m->saved = 0;
}

/* mode: 0 block, 1 trylock, 2 timed (timeout_ms) */
static int mutex_lock_impl(pthread_mutex_t* m, int mode, long timeout_ms)
{
    Pthread* d = self();
    uint32_t tid = d->tid;
    int kind = m->kind;
    unsigned long long deadline = mode == 2 ? (unsigned long long)clock_ms() + (unsigned long long)timeout_ms : 0;
    if (__atomic_load_n(&m->state, __ATOMIC_ACQUIRE) == MS_DEAD) return ENOTRECOVERABLE;
    if (kind & (MK_PI | MK_ROBUST))
    {
        int w = __atomic_load_n(&m->lock, __ATOMIC_ACQUIRE);
        if (((uint32_t)w & FUTEX_TID_MASK) == tid)
        {
            if (m->type == PTHREAD_MUTEX_RECURSIVE) { if (m->count == 0x7FFFFFFF) return EAGAIN; ++m->count; return 0; }
            if (m->type == PTHREAD_MUTEX_ERRORCHECK || mode == 1) return mode == 1 ? EBUSY : EDEADLK;
        }
        if (kind & MK_ROBUST) robust_prepare(d, m);
        int got = 0;
        int cur = 0;
        if (__atomic_compare_exchange_n(&m->lock, &cur, (int)tid, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) got = 1;
        else if (kind & MK_PI)
        {
            for (;;)
            {
                long rem = -1;
                if (mode == 2) { unsigned long long now = clock_ms(); if (now >= deadline) { if (kind & MK_ROBUST) robust_done(d); return ETIMEDOUT; } rem = (long)(deadline - now); }
                long r = fx(&m->lock, mode == 1 ? FUTEX_TRYLOCK_PI : FUTEX_LOCK_PI, 0, rem, 0, 0);
                if (r == 0) { got = 1; break; }
                if (r == -EINTR) { if (kind & MK_ROBUST) robust_done(d); __rio_cancel_point(); if (kind & MK_ROBUST) robust_prepare(d, m); continue; }
                if (kind & MK_ROBUST) robust_done(d);
                if (r == -ETIMEDOUT) return ETIMEDOUT;
                if (r == -EAGAIN) return mode == 1 ? EBUSY : EAGAIN;
                return (int)-r;
            }
        }
        else                                                           /* robust without priority inheritance */
        {
            for (;;)
            {
                int v = __atomic_load_n(&m->lock, __ATOMIC_ACQUIRE);
                if (((uint32_t)v & FUTEX_TID_MASK) == 0)
                {
                    int nv = (int)(tid | ((uint32_t)v & (FUTEX_WAITERS | FUTEX_OWNER_DIED)));
                    if (__atomic_compare_exchange_n(&m->lock, &v, nv, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { got = 1; break; }
                    continue;
                }
                if (mode == 1) { robust_done(d); return EBUSY; }
                if (!((uint32_t)v & FUTEX_WAITERS))
                {
                    int nv = (int)((uint32_t)v | FUTEX_WAITERS);
                    if (!__atomic_compare_exchange_n(&m->lock, &v, nv, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) continue;
                    v = nv;
                }
                long rem = -1;
                if (mode == 2) { unsigned long long now = clock_ms(); if (now >= deadline) { robust_done(d); return ETIMEDOUT; } rem = (long)(deadline - now); }
                long r = fx_wait(&m->lock, v, rem);
                if (r == -ETIMEDOUT) { robust_done(d); return ETIMEDOUT; }
                if (r == -EINTR) { robust_done(d); __rio_cancel_point(); robust_prepare(d, m); }
                if (__atomic_load_n(&m->state, __ATOMIC_ACQUIRE) == MS_DEAD) { robust_done(d); return ENOTRECOVERABLE; }
            }
        }
        (void)got;
        m->owner = (int)tid; m->count = 1;
        if (kind & MK_ROBUST) { robust_add(d, m); robust_done(d); }
        if ((uint32_t)__atomic_load_n(&m->lock, __ATOMIC_ACQUIRE) & FUTEX_OWNER_DIED) { m->state = MS_INCONSISTENT; return EOWNERDEAD; }
        return 0;
    }
    /* plain futex mutex: 0 free, 1 locked, 2 locked with waiters */
    if (m->type != PTHREAD_MUTEX_NORMAL && __atomic_load_n(&m->lock, __ATOMIC_ACQUIRE) != 0 && (uint32_t)m->owner == tid)
    {
        if (m->type == PTHREAD_MUTEX_RECURSIVE) { if (m->count == 0x7FFFFFFF) return EAGAIN; ++m->count; return 0; }
        return mode == 1 ? EBUSY : EDEADLK;
    }
    int c = 0;
    if (!__atomic_compare_exchange_n(&m->lock, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        if (mode == 1) return EBUSY;
        if (c != 2) c = __atomic_exchange_n(&m->lock, 2, __ATOMIC_ACQUIRE);
        while (c != 0)
        {
            long rem = -1;
            if (mode == 2) { unsigned long long now = clock_ms(); if (now >= deadline) return ETIMEDOUT; rem = (long)(deadline - now); }
            long r = fx_wait(&m->lock, 2, rem);
            if (r == -ETIMEDOUT) return ETIMEDOUT;
            if (r == -EINTR) __rio_cancel_point();
            c = __atomic_exchange_n(&m->lock, 2, __ATOMIC_ACQUIRE);
        }
    }
    m->owner = (int)tid; m->count = 1;
    if (kind & MK_PROTECT) protect_raise(m);
    return 0;
}
int pthread_mutex_lock(pthread_mutex_t* m) { return mutex_lock_impl(m, 0, -1); }
int pthread_mutex_trylock(pthread_mutex_t* m) { return mutex_lock_impl(m, 1, 0); }
int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* abs) { return mutex_lock_impl(m, 2, abs_to_rel_ms(abs, CLOCK_REALTIME)); }

int pthread_mutex_unlock(pthread_mutex_t* m)
{
    Pthread* d = self();
    uint32_t tid = d->tid;
    int kind = m->kind;
    if (kind & (MK_PI | MK_ROBUST))
    {
        int w = __atomic_load_n(&m->lock, __ATOMIC_ACQUIRE);
        if (((uint32_t)w & FUTEX_TID_MASK) != tid) return EPERM;
        if (m->type == PTHREAD_MUTEX_RECURSIVE && m->count > 1) { --m->count; return 0; }
        m->count = 0; m->owner = 0;
        if (kind & MK_ROBUST) robust_prepare(d, m);
        if (m->state == MS_INCONSISTENT)                               /* unlocked without pthread_mutex_consistent(): unusable from now on */
        {
            __atomic_store_n(&m->state, MS_DEAD, __ATOMIC_RELEASE);
            if (kind & MK_ROBUST) robust_remove(d, m);
            __atomic_store_n(&m->lock, (int)FUTEX_OWNER_DIED, __ATOMIC_RELEASE);
            fx_wake(&m->lock, WAKE_ALL);
            if (kind & MK_PI) (void)fx(&m->lock, FUTEX_UNLOCK_PI, 0, -1, 0, 0);
            if (kind & MK_ROBUST) robust_done(d);
            return 0;
        }
        if (kind & MK_ROBUST) robust_remove(d, m);
        if (kind & MK_PI)
        {
            int exp = (int)tid;
            if (!__atomic_compare_exchange_n(&m->lock, &exp, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) (void)fx(&m->lock, FUTEX_UNLOCK_PI, 0, -1, 0, 0);
        }
        else
        {
            int old = __atomic_exchange_n(&m->lock, 0, __ATOMIC_RELEASE);
            if ((uint32_t)old & FUTEX_WAITERS) fx_wake(&m->lock, 1);
        }
        if (kind & MK_ROBUST) robust_done(d);
        return 0;
    }
    int cur = __atomic_load_n(&m->lock, __ATOMIC_ACQUIRE);
    if (cur == 0) return m->type == PTHREAD_MUTEX_NORMAL ? 0 : EPERM;
    if (m->type != PTHREAD_MUTEX_NORMAL && (uint32_t)m->owner != tid) return EPERM;
    if (m->type == PTHREAD_MUTEX_RECURSIVE && m->count > 1) { --m->count; return 0; }
    m->count = 0; m->owner = 0;
    if (kind & MK_PROTECT) protect_restore(m);
    if (__atomic_exchange_n(&m->lock, 0, __ATOMIC_RELEASE) == 2) fx_wake(&m->lock, 1);
    return 0;
}
int pthread_mutex_consistent(pthread_mutex_t* m)
{
    if (!(m->kind & MK_ROBUST) || m->state != MS_INCONSISTENT || ((uint32_t)m->lock & FUTEX_TID_MASK) != self()->tid) return EINVAL;
    m->state = MS_OK;
    __atomic_and_fetch(&m->lock, (int)~FUTEX_OWNER_DIED, __ATOMIC_ACQ_REL);
    return 0;
}
int pthread_mutex_setprioceiling(pthread_mutex_t* m, int ceiling, int* old)
{
    if (!(m->kind & MK_PROTECT)) return EINVAL;
    if (ceiling < 0 || ceiling > 99) return EINVAL;
    int r = pthread_mutex_lock(m); if (r) return r;
    if (old) *old = m->ceiling;
    m->ceiling = ceiling;
    return pthread_mutex_unlock(m);
}
int pthread_mutex_getprioceiling(const pthread_mutex_t* m, int* ceiling) { if (!(m->kind & MK_PROTECT)) return EINVAL; *ceiling = m->ceiling; return 0; }

/* ---- condition variables -------------------------------------------------------------------------------------------------------------------- */
int pthread_condattr_init(pthread_condattr_t* a) { a->clock = CLOCK_REALTIME; a->pshared = 0; return 0; }
int pthread_condattr_destroy(pthread_condattr_t* a) { (void)a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t* a, int clk) { if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC) return EINVAL; a->clock = clk; return 0; }
int pthread_condattr_getclock(const pthread_condattr_t* a, int* clk) { *clk = a->clock; return 0; }
int pthread_condattr_setpshared(pthread_condattr_t* a, int s) { if (s != 0 && s != 1) return EINVAL; a->pshared = s; return 0; }
int pthread_condattr_getpshared(const pthread_condattr_t* a, int* s) { *s = a->pshared; return 0; }
int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* a)
{
    c->seq = 0; c->pad = 0; c->clock = a ? a->clock : CLOCK_REALTIME; c->shared = a ? a->pshared : 0; return 0;
}
int pthread_cond_destroy(pthread_cond_t* c) { return __atomic_load_n(&c->pad, __ATOMIC_ACQUIRE) ? EBUSY : 0; }

static int cond_wait_impl(pthread_cond_t* c, pthread_mutex_t* m, long timeout_ms)
{
    __rio_cancel_point();
    int saved_count = m->count;
    __atomic_add_fetch(&c->pad, 1, __ATOMIC_ACQ_REL);               /* waiters */
    int seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    if (m->type == PTHREAD_MUTEX_RECURSIVE) m->count = 1;            /* a recursive mutex is released completely */
    int ur = pthread_mutex_unlock(m);
    if (ur != 0) { m->count = saved_count; __atomic_sub_fetch(&c->pad, 1, __ATOMIC_ACQ_REL); return ur; }
    long r = fx_wait(&c->seq, seq, timeout_ms);
    __atomic_sub_fetch(&c->pad, 1, __ATOMIC_ACQ_REL);
    int lr = pthread_mutex_lock(m);
    if (m->type == PTHREAD_MUTEX_RECURSIVE) m->count = saved_count;
    if (lr != 0 && lr != EOWNERDEAD) return lr;
    __rio_cancel_point();
    if (lr == EOWNERDEAD) return lr;
    return r == -ETIMEDOUT ? ETIMEDOUT : 0;                          /* woken, spurious or interrupted */
}
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) { return cond_wait_impl(c, m, -1); }
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* abs)
{
    if (abs->tv_nsec < 0 || abs->tv_nsec >= 1000000000L) return EINVAL;
    return cond_wait_impl(c, m, abs_to_rel_ms(abs, c->clock));
}
int pthread_cond_signal(pthread_cond_t* c)
{
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&c->pad, __ATOMIC_ACQUIRE)) fx_wake(&c->seq, 1);
    return 0;
}
int pthread_cond_broadcast(pthread_cond_t* c)
{
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&c->pad, __ATOMIC_ACQUIRE)) fx_wake(&c->seq, WAKE_ALL);
    return 0;
}

/* ---- rwlock: state > 0 readers, -1 writer; writers waiting hold off new readers ----------------------------------------------------------------- */
int pthread_rwlockattr_init(pthread_rwlockattr_t* a) { a->pshared = 0; a->kind = 0; return 0; }
int pthread_rwlockattr_destroy(pthread_rwlockattr_t* a) { (void)a; return 0; }
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t* a, int s) { if (s != 0 && s != 1) return EINVAL; a->pshared = s; return 0; }
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t* a, int* s) { *s = a->pshared; return 0; }
int pthread_rwlock_init(pthread_rwlock_t* l, const pthread_rwlockattr_t* a) { l->state = 0; l->waiters = 0; l->wwait = 0; l->shared = a ? a->pshared : 0; return 0; }
int pthread_rwlock_destroy(pthread_rwlock_t* l) { return __atomic_load_n(&l->state, __ATOMIC_ACQUIRE) != 0 ? EBUSY : 0; }

static int rw_try_read(pthread_rwlock_t* l)
{
    int s = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);
    while (s >= 0 && __atomic_load_n(&l->wwait, __ATOMIC_ACQUIRE) == 0)
    {
        if (s == 0x7FFFFFFF) return EAGAIN;
        if (__atomic_compare_exchange_n(&l->state, &s, s + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) return 0;
    }
    return EBUSY;
}
static int rw_try_write(pthread_rwlock_t* l)
{
    int z = 0;
    return __atomic_compare_exchange_n(&l->state, &z, -1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : EBUSY;
}
static int rw_lock(pthread_rwlock_t* l, int write, long timeout_ms)
{
    unsigned long long deadline = timeout_ms >= 0 ? (unsigned long long)clock_ms() + (unsigned long long)timeout_ms : 0;
    if (write) __atomic_add_fetch(&l->wwait, 1, __ATOMIC_ACQ_REL);
    int rc = 0;
    for (;;)
    {
        rc = write ? rw_try_write(l) : rw_try_read(l);
        if (rc == 0 || rc == EAGAIN) break;
        long rem = -1;
        if (timeout_ms >= 0) { unsigned long long now = clock_ms(); if (now >= deadline) { rc = ETIMEDOUT; break; } rem = (long)(deadline - now); }
        __atomic_add_fetch(&l->waiters, 1, __ATOMIC_ACQ_REL);
        int s = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);
        int blocked = write ? s != 0 : (s < 0 || __atomic_load_n(&l->wwait, __ATOMIC_ACQUIRE) != 0);
        long r = 0;
        if (blocked) r = fx_wait(&l->state, s, rem);
        __atomic_sub_fetch(&l->waiters, 1, __ATOMIC_ACQ_REL);
        if (r == -ETIMEDOUT) { rc = ETIMEDOUT; break; }
        if (r == -EINTR) { if (write) __atomic_sub_fetch(&l->wwait, 1, __ATOMIC_ACQ_REL); __rio_cancel_point(); if (write) __atomic_add_fetch(&l->wwait, 1, __ATOMIC_ACQ_REL); }
    }
    if (write)
    {
        __atomic_sub_fetch(&l->wwait, 1, __ATOMIC_ACQ_REL);
        if (rc != 0 && __atomic_load_n(&l->waiters, __ATOMIC_ACQUIRE)) fx_wake(&l->state, WAKE_ALL);    /* readers held off by this writer may go */
    }
    return rc;
}
int pthread_rwlock_rdlock(pthread_rwlock_t* l) { return rw_lock(l, 0, -1); }
int pthread_rwlock_wrlock(pthread_rwlock_t* l) { return rw_lock(l, 1, -1); }
int pthread_rwlock_tryrdlock(pthread_rwlock_t* l) { int rc = rw_try_read(l); return rc; }
int pthread_rwlock_trywrlock(pthread_rwlock_t* l) { return rw_try_write(l); }
int pthread_rwlock_timedrdlock(pthread_rwlock_t* l, const struct timespec* abs) { return rw_lock(l, 0, abs_to_rel_ms(abs, CLOCK_REALTIME)); }
int pthread_rwlock_timedwrlock(pthread_rwlock_t* l, const struct timespec* abs) { return rw_lock(l, 1, abs_to_rel_ms(abs, CLOCK_REALTIME)); }
int pthread_rwlock_unlock(pthread_rwlock_t* l)
{
    int s = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);
    if (s == 0) return EPERM;
    if (s < 0) __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
    else if (__atomic_sub_fetch(&l->state, 1, __ATOMIC_ACQ_REL) != 0) return 0;
    if (__atomic_load_n(&l->waiters, __ATOMIC_ACQUIRE)) fx_wake(&l->state, WAKE_ALL);
    return 0;
}

/* ---- barrier / spinlock ----------------------------------------------------------------------------------------------------------------------------- */
int pthread_barrierattr_init(pthread_barrierattr_t* a) { a->pshared = 0; return 0; }
int pthread_barrierattr_destroy(pthread_barrierattr_t* a) { (void)a; return 0; }
int pthread_barrierattr_setpshared(pthread_barrierattr_t* a, int s) { if (s != 0 && s != 1) return EINVAL; a->pshared = s; return 0; }
int pthread_barrierattr_getpshared(const pthread_barrierattr_t* a, int* s) { *s = a->pshared; return 0; }
int pthread_barrier_init(pthread_barrier_t* b, const pthread_barrierattr_t* a, unsigned count)
{
    (void)a;
    if (count == 0) return EINVAL;
    b->count = count; b->waiting = 0; b->phase = 0; b->lock = 0; return 0;
}
int pthread_barrier_destroy(pthread_barrier_t* b)
{
    struct __rio_lock* l = (struct __rio_lock*)&b->lock;
    __rio_lock_acquire(l); unsigned w = b->waiting; __rio_lock_release(l);
    return w ? EBUSY : 0;
}
int pthread_barrier_wait(pthread_barrier_t* b)
{
    struct __rio_lock* l = (struct __rio_lock*)&b->lock;
    __rio_lock_acquire(l);
    if (++b->waiting == b->count)
    {
        b->waiting = 0;
        __atomic_add_fetch(&b->phase, 1, __ATOMIC_ACQ_REL);
        __rio_lock_release(l);
        fx_wake((volatile int*)&b->phase, WAKE_ALL);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    unsigned p = __atomic_load_n(&b->phase, __ATOMIC_ACQUIRE);
    __rio_lock_release(l);
    while (__atomic_load_n(&b->phase, __ATOMIC_ACQUIRE) == p) (void)fx_wait((volatile int*)&b->phase, (int)p, -1);
    return 0;
}
int pthread_spin_init(pthread_spinlock_t* s, int shared) { (void)shared; *s = 0; return 0; }
int pthread_spin_destroy(pthread_spinlock_t* s) { (void)s; return 0; }
int pthread_spin_lock(pthread_spinlock_t* s)
{
    unsigned spins = 0;
    for (;;)
    {
        if (!__atomic_exchange_n(s, 1, __ATOMIC_ACQUIRE)) return 0;
        while (__atomic_load_n(s, __ATOMIC_RELAXED))
        {
            __asm__ volatile("yield");
            if (++spins > 2000) { sched_yield(); spins = 0; }
        }
    }
}
int pthread_spin_trylock(pthread_spinlock_t* s) { return __atomic_exchange_n(s, 1, __ATOMIC_ACQUIRE) ? EBUSY : 0; }
int pthread_spin_unlock(pthread_spinlock_t* s) { __atomic_store_n(s, 0, __ATOMIC_RELEASE); return 0; }

/* ---- semaphores (futex on the value word; process-shared ones live in shared memory and use the same code) ----------------------------------------- */
static int sem_take(sem_t* s)
{
    unsigned v = __atomic_load_n(&s->value, __ATOMIC_ACQUIRE);
    while (v > 0)
        if (__atomic_compare_exchange_n(&s->value, &v, v - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
    return 0;
}
int sem_init(sem_t* s, int pshared, unsigned value)
{
    if (value > 0x7FFFFFFFU) { errno = EINVAL; return -1; }
    s->value = value; s->shared = pshared ? 1U : 0U; s->waiters = 0; return 0;
}
int sem_destroy(sem_t* s) { (void)s; return 0; }
int sem_post(sem_t* s)
{
    unsigned v = __atomic_load_n(&s->value, __ATOMIC_ACQUIRE);
    for (;;)
    {
        if (v == 0x7FFFFFFFU) { errno = EOVERFLOW; return -1; }
        if (__atomic_compare_exchange_n(&s->value, &v, v + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    if (__atomic_load_n(&s->waiters, __ATOMIC_ACQUIRE)) fx_wake((volatile int*)&s->value, 1);
    return 0;
}
int sem_trywait(sem_t* s) { if (sem_take(s)) return 0; errno = EAGAIN; return -1; }
static int sem_wait_impl(sem_t* s, long timeout_ms)
{
    __rio_cancel_point();
    unsigned long long deadline = timeout_ms >= 0 ? (unsigned long long)clock_ms() + (unsigned long long)timeout_ms : 0;
    if (sem_take(s)) return 0;
    __atomic_add_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL);
    int rc = 0;
    for (;;)
    {
        if (sem_take(s)) break;
        long rem = -1;
        if (timeout_ms >= 0) { unsigned long long now = clock_ms(); if (now >= deadline) { errno = ETIMEDOUT; rc = -1; break; } rem = (long)(deadline - now); }
        long r = fx_wait((volatile int*)&s->value, 0, rem);
        if (r == -EINTR) { __atomic_sub_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL); __rio_cancel_point(); errno = EINTR; return -1; }
    }
    __atomic_sub_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL);
    return rc;
}
int sem_wait(sem_t* s) { return sem_wait_impl(s, -1); }
int sem_timedwait(sem_t* s, const struct timespec* abs) { return sem_wait_impl(s, abs_to_rel_ms(abs, CLOCK_REALTIME)); }
int sem_getvalue(sem_t* s, int* v) { *v = (int)__atomic_load_n(&s->value, __ATOMIC_ACQUIRE); return 0; }

} /* extern "C" */
