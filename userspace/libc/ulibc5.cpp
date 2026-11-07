/* RioOS libc part 5: POSIX threads (user-level, cooperative M:1), semaphores. See the note in ulibc.h. */
#include "ulibc.h"
#include "../../include/user_api.h"

extern "C" {

#define MAXTH 32
#define KEYS 32
#define ST_FREE 0
#define ST_RUN 1
#define ST_BLOCK 2
#define ST_SLEEP 3
#define ST_ZOMBIE 4
#define DEFAULT_STACK 65536UL
#define MIN_STACK 16384UL
#define MAX_STACK (256UL * 1024UL)

struct Th
{
    int state; unsigned long id;
    void* sp; char* stack; unsigned long ssize;
    void* (*fn)(void*); void* arg; void* ret;
    int detached; int joined;
    unsigned long long wake; void* waitobj; unsigned long waitseq; int timedout;
    int cancel_pending, cancel_state, cancel_type;
    void* keys[KEYS];
    unsigned sigpend;
    int saved_errno;
};

static Th ths[MAXTH];
static Th* cur = 0;
static unsigned long next_id = 1;
static unsigned long seq_counter = 0;
static struct { int used; void (*dtor)(void*); } keytab[KEYS];
int __rio_threads_started = 0;

extern unsigned long long __rio_realtime_ms(void);
extern int __rio_signal_local(int sig);
extern void* mmap(void*, size_t, int, int, int, long);
void __rio_ctx_switch(void** save_sp, void* new_sp);
__asm__(".text\n.global __rio_ctx_switch\n__rio_ctx_switch:\n"
        " sub sp, sp, #96\n stp x19, x20, [sp]\n stp x21, x22, [sp, #16]\n stp x23, x24, [sp, #32]\n stp x25, x26, [sp, #48]\n stp x27, x28, [sp, #64]\n stp x29, x30, [sp, #80]\n"
        " mov x2, sp\n str x2, [x0]\n mov sp, x1\n"
        " ldp x19, x20, [sp]\n ldp x21, x22, [sp, #16]\n ldp x23, x24, [sp, #32]\n ldp x25, x26, [sp, #48]\n ldp x27, x28, [sp, #64]\n ldp x29, x30, [sp, #80]\n add sp, sp, #96\n ret\n");

static void init_main(void)
{
    if (cur) return;
    ths[0].state = ST_RUN; ths[0].id = next_id++; ths[0].cancel_state = PTHREAD_CANCEL_ENABLE; ths[0].cancel_type = PTHREAD_CANCEL_DEFERRED;
    cur = &ths[0];
}

static int alive_count(void) { int n = 0; for (int i = 0; i < MAXTH; ++i) if (ths[i].state != ST_FREE && ths[i].state != ST_ZOMBIE) ++n; return n; }

static void reap(void)
{
    for (int i = 0; i < MAXTH; ++i)
        if (ths[i].state == ST_ZOMBIE && ths[i].detached && &ths[i] != cur && ths[i].stack)
        {
            munmap(ths[i].stack, ths[i].ssize); ths[i].stack = 0; ths[i].state = ST_FREE;
        }
}

static void exit_thread_final(void* ret);
static void deliver_pending(void)
{
    while (cur->sigpend)
    {
        int s = 1; while (!(cur->sigpend & (1U << s))) ++s;
        cur->sigpend &= ~(1U << s);
        (void)__rio_signal_local(s);
    }
}
static void check_cancel(void)
{
    if (cur->cancel_pending && cur->cancel_state == PTHREAD_CANCEL_ENABLE) { cur->cancel_pending = 0; exit_thread_final(PTHREAD_CANCELED); }
}

/* Picks the next runnable thread (waking sleepers and timed-out waiters) and switches to it. */
static void schedule(void)
{
    Th* me = cur;
    for (;;)
    {
        unsigned long long now = clock_ms();
        unsigned long long minwake = ~0ULL;
        for (int i = 0; i < MAXTH; ++i)
        {
            Th* t = &ths[i];
            if (t->state == ST_SLEEP && now >= t->wake) t->state = ST_RUN;
            else if (t->state == ST_BLOCK && t->wake && now >= t->wake) { t->timedout = 1; t->waitobj = 0; t->state = ST_RUN; }
            if ((t->state == ST_SLEEP || (t->state == ST_BLOCK && t->wake)) && t->wake < minwake) minwake = t->wake;
        }
        int start = (int)(me - ths);
        Th* next = 0;
        for (int k = 1; k <= MAXTH; ++k)
        {
            Th* t = &ths[(start + k) % MAXTH];
            if (t->state == ST_RUN) { next = t; break; }
        }
        if (next)
        {
            if (next != me)
            {
                me->saved_errno = errno;
                cur = next;
                __rio_ctx_switch(&me->sp, next->sp);
                /* resumed here as `me` */
                errno = me->saved_errno;
            }
            reap();
            deliver_pending();
            return;
        }
        if (minwake != ~0ULL) { unsigned long long d = minwake > now ? minwake - now : 1; (void)user_sleep_ms(d > 50 ? 50 : d); continue; }
        if (alive_count() == 0) exit(0);                       /* every thread has finished */
        fputs("pthread: deadlock - every thread is blocked\n", stderr);
        _exit(134);
    }
}

static void wake_one(void* obj)
{
    Th* best = 0;
    for (int i = 0; i < MAXTH; ++i)
        if (ths[i].state == ST_BLOCK && ths[i].waitobj == obj && (!best || ths[i].waitseq < best->waitseq)) best = &ths[i];
    if (best) { best->waitobj = 0; best->state = ST_RUN; }
}
static void wake_all(void* obj)
{
    for (int i = 0; i < MAXTH; ++i)
        if (ths[i].state == ST_BLOCK && ths[i].waitobj == obj) { ths[i].waitobj = 0; ths[i].state = ST_RUN; }
}

/* Blocks the calling thread on `obj` until woken; deadline_ms (monotonic, 0 = none). Returns 0 or ETIMEDOUT. */
static int block_on(void* obj, unsigned long long deadline)
{
    cur->state = ST_BLOCK; cur->waitobj = obj; cur->waitseq = ++seq_counter; cur->wake = deadline; cur->timedout = 0;
    schedule();
    cur->wake = 0;
    int to = cur->timedout; cur->timedout = 0;
    check_cancel();
    return to ? ETIMEDOUT : 0;
}

static unsigned long long abs_to_deadline(const struct timespec* abs)
{
    unsigned long long target = (unsigned long long)abs->tv_sec * 1000ULL + (unsigned long long)(abs->tv_nsec + 999999L) / 1000000ULL;
    unsigned long long rt = __rio_realtime_ms();
    unsigned long long d = target > rt ? target - rt : 0;
    return clock_ms() + d + 1;      /* +1: never 0 */
}

/* hooks used by sleep_ms / sched_yield / fork */
unsigned __rio_thread_sleep(unsigned ms)
{
    init_main();
    if (alive_count() <= 1 && !cur->sigpend) return 0xFFFFFFFFU;      /* single thread: caller does the real sleep */
    cur->state = ST_SLEEP; cur->wake = clock_ms() + ms;
    schedule();
    cur->wake = 0;
    check_cancel();
    return 0;
}
int __rio_thread_yield(void)
{
    init_main();
    if (alive_count() <= 1) return 0;
    cur->state = ST_RUN;
    schedule();
    check_cancel();
    return 1;
}
void __rio_thread_after_fork(void)
{
    if (!cur) return;
    for (int i = 0; i < MAXTH; ++i) if (&ths[i] != cur && ths[i].state != ST_FREE) { ths[i].state = ST_FREE; ths[i].stack = 0; }
}

static void thread_entry(void)
{
    void* r = cur->fn(cur->arg);
    exit_thread_final(r);
}

static void run_destructors(void)
{
    for (int round = 0; round < 4; ++round)
    {
        int any = 0;
        for (unsigned k = 0; k < KEYS; ++k)
            if (keytab[k].used && keytab[k].dtor && cur->keys[k]) { void* v = cur->keys[k]; cur->keys[k] = 0; keytab[k].dtor(v); any = 1; }
        if (!any) break;
    }
}

static void exit_thread_final(void* ret)
{
    run_destructors();
    cur->ret = ret;
    cur->state = ST_ZOMBIE;
    wake_all(cur);                                  /* joiners wait on the Th object */
    schedule();
    for (;;) (void)user_sleep_ms(1000);             /* unreachable */
}

void pthread_exit(void* ret) { init_main(); exit_thread_final(ret); __builtin_unreachable(); }

pthread_t pthread_self(void) { init_main(); return cur->id; }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

static Th* find_th(pthread_t id) { for (int i = 0; i < MAXTH; ++i) if (ths[i].state != ST_FREE && ths[i].id == id) return &ths[i]; return 0; }

int pthread_attr_init(pthread_attr_t* a) { a->detachstate = PTHREAD_CREATE_JOINABLE; a->stacksize = DEFAULT_STACK; return 0; }
int pthread_attr_destroy(pthread_attr_t* a) { (void)a; return 0; }
int pthread_attr_setdetachstate(pthread_attr_t* a, int s) { if (s != 0 && s != 1) return EINVAL; a->detachstate = s; return 0; }
int pthread_attr_getdetachstate(const pthread_attr_t* a, int* s) { *s = a->detachstate; return 0; }
int pthread_attr_setstacksize(pthread_attr_t* a, unsigned long sz) { if (sz < MIN_STACK || sz > MAX_STACK) return EINVAL; a->stacksize = sz; return 0; }
int pthread_attr_getstacksize(const pthread_attr_t* a, unsigned long* sz) { *sz = a->stacksize; return 0; }

int pthread_create(pthread_t* t, const pthread_attr_t* attr, void* (*fn)(void*), void* arg)
{
    init_main();
    __rio_threads_started = 1;
    reap();
    Th* n = 0;
    for (int i = 0; i < MAXTH; ++i) if (ths[i].state == ST_FREE) { n = &ths[i]; break; }
    if (!n) return EAGAIN;
    unsigned long ss = attr ? attr->stacksize : DEFAULT_STACK;
    if (ss < MIN_STACK) ss = MIN_STACK;
    void* st = mmap(0, ss, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (st == MAP_FAILED) return EAGAIN;
    for (unsigned long i = 0; i < sizeof(Th); ++i) ((char*)n)[i] = 0;
    n->stack = (char*)st; n->ssize = ss; n->fn = fn; n->arg = arg; n->id = next_id++;
    n->detached = attr ? attr->detachstate == PTHREAD_CREATE_DETACHED : 0;
    n->cancel_state = PTHREAD_CANCEL_ENABLE; n->cancel_type = PTHREAD_CANCEL_DEFERRED;
    unsigned long long top = ((unsigned long long)st + ss) & ~15ULL;
    unsigned long long* frame = (unsigned long long*)(top - 96);
    for (int i = 0; i < 12; ++i) frame[i] = 0;
    frame[11] = (unsigned long long)thread_entry;             /* x30: where `ret` lands */
    n->sp = frame;
    n->state = ST_RUN;
    if (t) *t = n->id;
    return 0;
}

int pthread_join(pthread_t id, void** ret)
{
    init_main();
    Th* t = find_th(id);
    if (!t) return ESRCH;
    if (t == cur) return EDEADLK;
    if (t->detached || t->joined) return EINVAL;
    t->joined = 1;
    while (t->state != ST_ZOMBIE)
    {
        t->joined = 1;
        (void)block_on(t, 0);
    }
    if (ret) *ret = t->ret;
    if (t->stack) { munmap(t->stack, t->ssize); t->stack = 0; }
    t->state = ST_FREE;
    return 0;
}

int pthread_detach(pthread_t id)
{
    init_main();
    Th* t = find_th(id);
    if (!t) return ESRCH;
    if (t->detached || t->joined) return EINVAL;
    t->detached = 1;
    if (t->state == ST_ZOMBIE) reap();
    return 0;
}

int pthread_yield(void) { init_main(); if (alive_count() > 1) __rio_thread_yield(); else sched_yield(); return 0; }

int pthread_cancel(pthread_t id)
{
    init_main();
    Th* t = find_th(id);
    if (!t || t->state == ST_ZOMBIE) return ESRCH;
    t->cancel_pending = 1;
    if (t == cur) { check_cancel(); return 0; }
    if ((t->state == ST_BLOCK || t->state == ST_SLEEP) && t->cancel_state == PTHREAD_CANCEL_ENABLE) { t->waitobj = 0; t->state = ST_RUN; }
    return 0;
}
int pthread_setcancelstate(int state, int* old) { init_main(); if (state != 0 && state != 1) return EINVAL; if (old) *old = cur->cancel_state; cur->cancel_state = state; return 0; }
int pthread_setcanceltype(int type, int* old) { init_main(); if (type != 0 && type != 1) return EINVAL; if (old) *old = cur->cancel_type; cur->cancel_type = type; return 0; }
void pthread_testcancel(void) { init_main(); check_cancel(); }

int pthread_kill(pthread_t id, int sig)
{
    init_main();
    Th* t = find_th(id);
    if (!t || t->state == ST_ZOMBIE) return ESRCH;
    if (sig == 0) return 0;
    if (sig < 0 || sig >= 32) return EINVAL;
    if (t == cur) { (void)__rio_signal_local(sig); return 0; }
    t->sigpend |= 1U << sig;                                  /* delivered when that thread next runs */
    if (t->state == ST_SLEEP || t->state == ST_BLOCK) { t->waitobj = 0; t->state = ST_RUN; }
    return 0;
}
int pthread_sigmask(int how, const sigset_t* set, sigset_t* old) { return sigprocmask(how, set, old) == 0 ? 0 : errno; }
int pthread_setname_np(pthread_t t, const char* name) { (void)t; (void)name; return 0; }

int pthread_once(pthread_once_t* once, void (*fn)(void))
{
    init_main();
    if (*once == 2) return 0;
    if (*once == 1) { while (*once == 1) __rio_thread_yield(); return 0; }
    *once = 1; fn(); *once = 2;
    return 0;
}

/* ---- mutex ---- */
int pthread_mutexattr_init(pthread_mutexattr_t* a) { a->type = PTHREAD_MUTEX_NORMAL; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t* a) { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t* a, int t) { if (t < 0 || t > 2) return EINVAL; a->type = t; return 0; }
int pthread_mutexattr_gettype(const pthread_mutexattr_t* a, int* t) { *t = a->type; return 0; }
int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) { m->type = a ? a->type : 0; m->lock = 0; m->owner = 0; m->count = 0; return 0; }
int pthread_mutex_destroy(pthread_mutex_t* m) { return m->lock ? EBUSY : 0; }
int pthread_mutex_trylock(pthread_mutex_t* m)
{
    init_main();
    if (m->lock)
    {
        if (m->type == PTHREAD_MUTEX_RECURSIVE && (unsigned long)m->owner == cur->id) { ++m->count; return 0; }
        return EBUSY;
    }
    m->lock = 1; m->owner = (int)cur->id; m->count = 1; return 0;
}
int pthread_mutex_lock(pthread_mutex_t* m)
{
    init_main();
    while (m->lock)
    {
        if ((unsigned long)m->owner == cur->id)
        {
            if (m->type == PTHREAD_MUTEX_RECURSIVE) { ++m->count; return 0; }
            if (m->type == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
        }
        (void)block_on(m, 0);
    }
    m->lock = 1; m->owner = (int)cur->id; m->count = 1;
    return 0;
}
int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* abs)
{
    init_main();
    unsigned long long dl = abs_to_deadline(abs);
    while (m->lock)
    {
        if ((unsigned long)m->owner == cur->id)
        {
            if (m->type == PTHREAD_MUTEX_RECURSIVE) { ++m->count; return 0; }
            if (m->type == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
        }
        if (clock_ms() >= dl) return ETIMEDOUT;
        if (block_on(m, dl) == ETIMEDOUT && m->lock) return ETIMEDOUT;
    }
    m->lock = 1; m->owner = (int)cur->id; m->count = 1;
    return 0;
}
int pthread_mutex_unlock(pthread_mutex_t* m)
{
    init_main();
    if (!m->lock) return m->type == PTHREAD_MUTEX_NORMAL ? 0 : EPERM;
    if (m->type != PTHREAD_MUTEX_NORMAL && (unsigned long)m->owner != cur->id) return EPERM;
    if (m->type == PTHREAD_MUTEX_RECURSIVE && --m->count > 0) return 0;
    m->lock = 0; m->owner = 0; m->count = 0;
    wake_all(m);
    return 0;
}

/* ---- condition variables ---- */
int pthread_condattr_init(pthread_condattr_t* a) { a->dummy = 0; return 0; }
int pthread_condattr_destroy(pthread_condattr_t* a) { (void)a; return 0; }
int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* a) { (void)a; c->seq = 0; return 0; }
int pthread_cond_destroy(pthread_cond_t* c) { (void)c; return 0; }
static int cond_wait_impl(pthread_cond_t* c, pthread_mutex_t* m, unsigned long long deadline)
{
    init_main();
    check_cancel();
    int saved_count = m->count;
    if (pthread_mutex_unlock(m) != 0 && m->type != PTHREAD_MUTEX_NORMAL) return EPERM;
    if (m->type == PTHREAD_MUTEX_RECURSIVE) { while (m->lock && (unsigned long)m->owner == cur->id) pthread_mutex_unlock(m); }
    int r = block_on(c, deadline);
    pthread_mutex_lock(m);
    if (m->type == PTHREAD_MUTEX_RECURSIVE) m->count = saved_count;
    return r;
}
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) { return cond_wait_impl(c, m, 0); }
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* abs) { return cond_wait_impl(c, m, abs_to_deadline(abs)); }
int pthread_cond_signal(pthread_cond_t* c) { init_main(); wake_one(c); return 0; }
int pthread_cond_broadcast(pthread_cond_t* c) { init_main(); wake_all(c); return 0; }

/* ---- rwlock ---- */
int pthread_rwlock_init(pthread_rwlock_t* l, const void* a) { (void)a; l->readers = 0; l->writer = 0; l->waiting_writers = 0; return 0; }
int pthread_rwlock_destroy(pthread_rwlock_t* l) { return (l->readers || l->writer) ? EBUSY : 0; }
int pthread_rwlock_rdlock(pthread_rwlock_t* l)
{
    init_main();
    while (l->writer || l->waiting_writers) (void)block_on(l, 0);
    ++l->readers; return 0;
}
int pthread_rwlock_tryrdlock(pthread_rwlock_t* l) { if (l->writer || l->waiting_writers) return EBUSY; ++l->readers; return 0; }
int pthread_rwlock_wrlock(pthread_rwlock_t* l)
{
    init_main();
    ++l->waiting_writers;
    while (l->writer || l->readers) (void)block_on(l, 0);
    --l->waiting_writers; l->writer = 1; return 0;
}
int pthread_rwlock_trywrlock(pthread_rwlock_t* l) { if (l->writer || l->readers) return EBUSY; l->writer = 1; return 0; }
int pthread_rwlock_unlock(pthread_rwlock_t* l)
{
    init_main();
    if (l->writer) l->writer = 0; else if (l->readers > 0) --l->readers; else return EPERM;
    wake_all(l);
    return 0;
}

/* ---- barrier / spinlock ---- */
int pthread_barrier_init(pthread_barrier_t* b, const void* a, unsigned count) { (void)a; if (count == 0) return EINVAL; b->count = count; b->waiting = 0; b->phase = 0; return 0; }
int pthread_barrier_destroy(pthread_barrier_t* b) { return b->waiting ? EBUSY : 0; }
int pthread_barrier_wait(pthread_barrier_t* b)
{
    init_main();
    if (++b->waiting == b->count) { b->waiting = 0; ++b->phase; wake_all(b); return PTHREAD_BARRIER_SERIAL_THREAD; }
    unsigned p = b->phase;
    while (b->phase == p) (void)block_on(b, 0);
    return 0;
}
int pthread_spin_init(pthread_spinlock_t* s, int shared) { (void)shared; *s = 0; return 0; }
int pthread_spin_destroy(pthread_spinlock_t* s) { (void)s; return 0; }
int pthread_spin_lock(pthread_spinlock_t* s) { init_main(); while (*s) pthread_yield(); *s = 1; return 0; }
int pthread_spin_trylock(pthread_spinlock_t* s) { if (*s) return EBUSY; *s = 1; return 0; }
int pthread_spin_unlock(pthread_spinlock_t* s) { *s = 0; return 0; }

/* ---- thread-specific data ---- */
int pthread_key_create(pthread_key_t* k, void (*d)(void*))
{
    for (unsigned i = 0; i < KEYS; ++i) if (!keytab[i].used) { keytab[i].used = 1; keytab[i].dtor = d; for (int t = 0; t < MAXTH; ++t) ths[t].keys[i] = 0; *k = i; return 0; }
    return EAGAIN;
}
int pthread_key_delete(pthread_key_t k) { if (k >= KEYS || !keytab[k].used) return EINVAL; keytab[k].used = 0; keytab[k].dtor = 0; return 0; }
void* pthread_getspecific(pthread_key_t k) { init_main(); return (k < KEYS && keytab[k].used) ? cur->keys[k] : 0; }
int pthread_setspecific(pthread_key_t k, const void* v) { init_main(); if (k >= KEYS || !keytab[k].used) return EINVAL; cur->keys[k] = (void*)v; return 0; }

/* ---- semaphores ---- */
int sem_init(sem_t* s, int pshared, unsigned value) { (void)pshared; s->value = value; return 0; }
int sem_destroy(sem_t* s) { (void)s; return 0; }
int sem_post(sem_t* s) { init_main(); ++s->value; wake_one(s); return 0; }
int sem_trywait(sem_t* s) { if (s->value == 0) { errno = EAGAIN; return -1; } --s->value; return 0; }
int sem_wait(sem_t* s)
{
    init_main();
    while (s->value == 0) (void)block_on(s, 0);
    --s->value; return 0;
}
int sem_timedwait(sem_t* s, const struct timespec* abs)
{
    init_main();
    unsigned long long dl = abs_to_deadline(abs);
    while (s->value == 0)
    {
        if (clock_ms() >= dl) { errno = ETIMEDOUT; return -1; }
        if (block_on(s, dl) == ETIMEDOUT && s->value == 0) { errno = ETIMEDOUT; return -1; }
    }
    --s->value; return 0;
}
int sem_getvalue(sem_t* s, int* v) { *v = (int)s->value; return 0; }

} /* extern "C" */
