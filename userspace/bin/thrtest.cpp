#include "../libc/ulibc.h"
/* thrtest: POSIX threads (user-level M:1 threads) */
static int fails = 0;
static void chk(const char* name, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++fails; }

static void* ret_arg(void* a) { return (void*)((long)a * 2); }
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static long counter = 0;
static void* incr(void* a) { (void)a; for (int i = 0; i < 200; ++i) { pthread_mutex_lock(&mtx); long v = counter; if (i % 17 == 0) pthread_yield(); counter = v + 1; pthread_mutex_unlock(&mtx); } return 0; }

static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int queue_items = 0, consumed = 0, done_flag = 0;
static void* consumer(void* a) { (void)a; pthread_mutex_lock(&mtx); for (;;) { while (queue_items == 0 && !done_flag) pthread_cond_wait(&cv, &mtx); if (queue_items == 0 && done_flag) break; --queue_items; ++consumed; } pthread_mutex_unlock(&mtx); return 0; }

static sem_t sem; static int sem_order = 0;
static void* sem_waiter(void* a) { sem_wait(&sem); return (void*)(long)(++sem_order + (long)a * 0); }

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER; static int rw_val = 0, rw_bad = 0;
static void* rw_reader(void* a) { (void)a; for (int i = 0; i < 20; ++i) { pthread_rwlock_rdlock(&rw); int v = rw_val; pthread_yield(); if (v != rw_val) rw_bad = 1; pthread_rwlock_unlock(&rw); } return 0; }
static void* rw_writer(void* a) { (void)a; for (int i = 0; i < 10; ++i) { pthread_rwlock_wrlock(&rw); ++rw_val; pthread_yield(); pthread_rwlock_unlock(&rw); } return 0; }

static pthread_barrier_t bar; static int bar_phase[4], bar_serial = 0;
static void* bar_worker(void* a) { long id = (long)a; bar_phase[id] = 1; if (pthread_barrier_wait(&bar) == PTHREAD_BARRIER_SERIAL_THREAD) ++bar_serial; int all = 1; for (int i = 0; i < 4; ++i) if (!bar_phase[i]) all = 0; bar_phase[id] = all ? 2 : 3; return 0; }

static pthread_once_t once = PTHREAD_ONCE_INIT; static int once_count = 0;
static void once_fn(void) { ++once_count; }
static void* once_worker(void* a) { (void)a; pthread_once(&once, once_fn); return 0; }

static pthread_key_t key; static int dtor_calls = 0;
static void key_dtor(void* v) { if (v) ++dtor_calls; }
static void* key_worker(void* a) { pthread_setspecific(key, a); pthread_yield(); return (void*)(pthread_getspecific(key) == a ? 1L : 0L); }

static volatile int detached_ran = 0;
static void* det_fn(void* a) { (void)a; detached_ran = 1; return 0; }

static volatile int cancel_started = 0;
static void* cancel_fn(void* a) { (void)a; cancel_started = 1; for (;;) { sleep_ms(5); pthread_testcancel(); } return 0; }

static void* sleeper(void* a) { sleep_ms(40); return a; }
static volatile int order_log[8]; static volatile int order_n = 0;
static void* ord_a(void* a) { (void)a; order_log[order_n++] = 1; sleep_ms(30); order_log[order_n++] = 3; return 0; }
static void* ord_b(void* a) { (void)a; sleep_ms(10); order_log[order_n++] = 2; return 0; }

static volatile int ksig = 0;
static void on_usr1(int s) { ksig = s; }
static void* sig_target(void* a) { (void)a; while (!ksig) sleep_ms(5); return (void*)1L; }

static void* many(void* a) { return a; }
static void* lock_twice(void* a) { (void)a; pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&m); pthread_mutex_lock(&m); return 0; }
static void* recursive_fn(void* a) { pthread_mutex_t* m = (pthread_mutex_t*)a; pthread_mutex_lock(m); pthread_mutex_lock(m); pthread_mutex_unlock(m); pthread_mutex_unlock(m); return (void*)1L; }
static void* exit_fn(void* a) { (void)a; pthread_exit((void*)77); return 0; }
static void* self_fn(void* a) { *(pthread_t*)a = pthread_self(); return 0; }

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    pthread_t t; void* r = 0;

    chk("create-join-retval", pthread_create(&t, 0, ret_arg, (void*)21) == 0 && pthread_join(t, &r) == 0 && (long)r == 42);
    chk("join-twice-esrch", pthread_join(t, &r) == ESRCH);
    chk("pthread_exit-value", pthread_create(&t, 0, exit_fn, 0) == 0 && pthread_join(t, &r) == 0 && (long)r == 77);
    { pthread_t id = 0, me = pthread_self(); pthread_create(&t, 0, self_fn, &id); pthread_join(t, 0); chk("self-differs", id != 0 && !pthread_equal(id, me) && pthread_equal(me, pthread_self())); }
    chk("join-self-edeadlk", pthread_join(pthread_self(), 0) == EDEADLK);

    { pthread_t th[4]; for (int i = 0; i < 4; ++i) pthread_create(&th[i], 0, incr, 0); for (int i = 0; i < 4; ++i) pthread_join(th[i], 0);
      chk("mutex-counter", counter == 800); }

    { pthread_t c1, c2; consumed = 0; queue_items = 0; done_flag = 0;
      pthread_create(&c1, 0, consumer, 0); pthread_create(&c2, 0, consumer, 0);
      for (int i = 0; i < 50; ++i) { pthread_mutex_lock(&mtx); ++queue_items; pthread_cond_signal(&cv); pthread_mutex_unlock(&mtx); if (i % 7 == 0) pthread_yield(); }
      pthread_mutex_lock(&mtx); done_flag = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mtx);
      pthread_join(c1, 0); pthread_join(c2, 0);
      chk("cond-producer-consumer", consumed == 50); }

    { pthread_t w[3]; sem_init(&sem, 0, 0); sem_order = 0;
      for (int i = 0; i < 3; ++i) pthread_create(&w[i], 0, sem_waiter, 0);
      pthread_yield(); int v = -1; sem_getvalue(&sem, &v);
      sem_post(&sem); sem_post(&sem); sem_post(&sem);
      long sum = 0; for (int i = 0; i < 3; ++i) { pthread_join(w[i], &r); sum += (long)r; }
      chk("semaphore", v == 0 && sum == 6 && sem_trywait(&sem) == -1 && errno == EAGAIN); }

    { pthread_t rd[3], wr[2]; rw_val = 0; rw_bad = 0;
      for (int i = 0; i < 3; ++i) pthread_create(&rd[i], 0, rw_reader, 0);
      for (int i = 0; i < 2; ++i) pthread_create(&wr[i], 0, rw_writer, 0);
      for (int i = 0; i < 3; ++i) pthread_join(rd[i], 0);
      for (int i = 0; i < 2; ++i) pthread_join(wr[i], 0);
      chk("rwlock", rw_val == 20 && !rw_bad); }

    { pthread_t b[4]; pthread_barrier_init(&bar, 0, 4);
      for (long i = 0; i < 4; ++i) pthread_create(&b[i], 0, bar_worker, (void*)i);
      for (int i = 0; i < 4; ++i) pthread_join(b[i], 0);
      chk("barrier", bar_serial == 1 && bar_phase[0] == 2 && bar_phase[1] == 2 && bar_phase[2] == 2 && bar_phase[3] == 2); }

    { pthread_t o[4]; for (int i = 0; i < 4; ++i) pthread_create(&o[i], 0, once_worker, 0); for (int i = 0; i < 4; ++i) pthread_join(o[i], 0);
      chk("once", once_count == 1); }

    { pthread_t k[3]; pthread_key_create(&key, key_dtor); long ok = 0;
      for (long i = 0; i < 3; ++i) pthread_create(&k[i], 0, key_worker, (void*)(i + 1));
      for (int i = 0; i < 3; ++i) { pthread_join(k[i], &r); ok += (long)r; }
      chk("tsd-private-values", ok == 3 && pthread_getspecific(key) == 0);
      chk("tsd-destructors", dtor_calls == 3); }

    { pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
      chk("detached-create", pthread_create(&t, &at, det_fn, 0) == 0);
      for (int i = 0; i < 5 && !detached_ran; ++i) sleep_ms(5);
      chk("detached-ran", detached_ran == 1 && pthread_join(t, 0) != 0); }

    { pthread_t c; chk("cancel-create", pthread_create(&c, 0, cancel_fn, 0) == 0);
      while (!cancel_started) sleep_ms(2);
      pthread_cancel(c); pthread_join(c, &r);
      chk("cancel-result", r == PTHREAD_CANCELED); }

    { unsigned long t0 = clock_ms(); pthread_t s1, s2; pthread_create(&s1, 0, sleeper, (void*)1); pthread_create(&s2, 0, sleeper, (void*)2);
      pthread_join(s1, 0); pthread_join(s2, 0); unsigned long el = clock_ms() - t0;
      chk("sleep-overlaps", el >= 35 && el < 75); }

    { pthread_t a, b; order_n = 0; pthread_create(&a, 0, ord_a, 0); pthread_create(&b, 0, ord_b, 0); pthread_join(a, 0); pthread_join(b, 0);
      chk("interleave-order", order_n == 3 && order_log[0] == 1 && order_log[1] == 2 && order_log[2] == 3); }

    { pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 30 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_t c = PTHREAD_COND_INITIALIZER; pthread_mutex_lock(&m); unsigned long t0 = clock_ms();
      int rc = pthread_cond_timedwait(&c, &m, &ts); unsigned long el = clock_ms() - t0; pthread_mutex_unlock(&m);
      chk("cond-timedwait-timeout", rc == ETIMEDOUT && el >= 20 && el < 80);
      chk("trylock", pthread_mutex_trylock(&m) == 0 && pthread_mutex_trylock(&m) == EBUSY && pthread_mutex_unlock(&m) == 0);
      pthread_mutex_lock(&m); ts.tv_sec = 0; ts.tv_nsec = 0; chk("timedlock-busy-times-out", pthread_mutex_timedlock(&m, &ts) == ETIMEDOUT); pthread_mutex_unlock(&m); }

    { pthread_mutexattr_t ma; pthread_mutex_t rm, em; pthread_mutexattr_init(&ma);
      pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&rm, &ma);
      pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK); pthread_mutex_init(&em, &ma);
      pthread_create(&t, 0, recursive_fn, &rm); pthread_join(t, &r);
      chk("mutex-recursive", (long)r == 1 && rm.lock == 0);
      pthread_mutex_lock(&em);
      chk("mutex-errorcheck", pthread_mutex_lock(&em) == EDEADLK && pthread_mutex_unlock(&em) == 0 && pthread_mutex_unlock(&em) == EPERM); }

    { signal(SIGUSR1, on_usr1); ksig = 0; pthread_t s; pthread_create(&s, 0, sig_target, 0); sleep_ms(10);
      pthread_kill(s, SIGUSR1); pthread_join(s, &r);
      chk("pthread_kill", (long)r == 1 && ksig == SIGUSR1); }

    { pthread_t m[16]; long sum = 0; int ok = 1;
      for (long i = 0; i < 16; ++i) if (pthread_create(&m[i], 0, many, (void*)(i + 1)) != 0) ok = 0;
      for (int i = 0; i < 16; ++i) { pthread_join(m[i], &r); sum += (long)r; }
      chk("sixteen-threads", ok && sum == 136); }
    { pthread_t m[40]; int created = 0; for (int i = 0; i < 40; ++i) if (pthread_create(&m[i], 0, many, 0) == 0) ++created; else break;
      for (int i = 0; i < created; ++i) pthread_join(m[i], 0);
      chk("thread-limit-eagain", created < 40 && created >= 16); }

    { pid_t p = fork();
      if (p == 0) { pthread_t x; pthread_create(&x, 0, lock_twice, 0); pthread_join(x, 0); _exit(0); }
      int st = 0; waitpid(p, &st, 0);
      chk("deadlock-detected", WIFEXITED(st) && WEXITSTATUS(st) == 134); }
    { pid_t p = fork();
      if (p == 0) { pthread_t x; pthread_create(&x, 0, ret_arg, (void*)1); _exit(pthread_join(x, 0) == 0 ? 5 : 6); }
      int st = 0; waitpid(p, &st, 0);
      chk("threads-in-forked-child", WEXITSTATUS(st) == 5); }

    printf("thrtest done fails=%d\n", fails);
    return fails ? 1 : 0;
}
