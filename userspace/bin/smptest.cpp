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
/* smptest: multi-CPU behaviour. Parallel execution, atomics and futex locks across processes, pipes, sockets, fork storms, file I/O,
   copy-on-write isolation and signal delivery to processes running on other CPUs. Argument: soak seconds (default 0). */
static int fails = 0;
static void chk(const char* name, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++fails; }

struct Shared { volatile uint32_t lock; volatile uint32_t counter; volatile uint32_t atomic_counter; volatile uint32_t cpu_seen[8]; volatile uint32_t ready; volatile uint32_t go; volatile uint32_t flag; };
static Shared* sh;

static void mutex_lock(volatile uint32_t* m)
{
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(m, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    for (;;)
    {
        if (c == 2 || (__atomic_exchange_n(m, 2, __ATOMIC_ACQUIRE) != 0)) futex_wait(m, 2, 50);
        c = 0;
        if (__atomic_compare_exchange_n(m, &c, 2, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    }
}
static void mutex_unlock(volatile uint32_t* m) { if (__atomic_exchange_n(m, 0, __ATOMIC_RELEASE) == 2) futex_wake(m, 1); }

static int spawn_n(int n, int (*fn)(int))
{
    for (int i = 0; i < n; ++i) { pid_t c = fork(); if (c < 0) return -1; if (c == 0) _exit(fn(i)); }
    return 0;
}
static int reap_all(void)
{
    int bad = 0, st; while (waitpid(-1, &st, 0) > 0) if (st != 0) ++bad;
    return bad;
}
static void note_cpu(void) { int c = sched_getcpu(); if (c >= 0 && c < 8) __atomic_store_n(&sh->cpu_seen[c], 1, __ATOMIC_RELAXED); }

static int w_spin(int id)
{
    (void)id; unsigned long end = clock_ms() + 400; volatile unsigned x = 1;
    while (clock_ms() < end) { for (int k = 0; k < 2000; ++k) x = x * 1664525u + 1013904223u; note_cpu(); }
    return 0;
}
static int w_counter(int id)
{
    (void)id;
    for (int i = 0; i < 1500; ++i)
    {
        mutex_lock(&sh->lock); uint32_t v = sh->counter; if ((i & 63) == 0) note_cpu(); sh->counter = v + 1; mutex_unlock(&sh->lock);
        __atomic_fetch_add(&sh->atomic_counter, 1, __ATOMIC_SEQ_CST);
    }
    return 0;
}
static int w_file(int id)
{
    char p[40]; snprintf(p, sizeof p, "/tmp/smp_f%d", id);
    for (int round = 0; round < 12; ++round)
    {
        int fd = open(p, O_RDWR | O_CREAT | O_TRUNC); if (fd < 0) return 1;
        char buf[512]; for (int i = 0; i < 512; ++i) buf[i] = (char)(id * 7 + round + i);
        for (int k = 0; k < 4; ++k) if (write(fd, buf, 512) != 512) return 2;
        close(fd); fd = open(p, O_RDONLY); if (fd < 0) return 3;
        char rb[512];
        for (int k = 0; k < 4; ++k) { if (read(fd, rb, 512) != 512 || memcmp(rb, buf, 512)) return 4; }
        close(fd);
    }
    unlink(p); return 0;
}
static int w_append(int id)
{
    int fd = open("/tmp/smp_log", O_WRONLY | O_CREAT | O_APPEND); if (fd < 0) return 1;
    for (int i = 0; i < 40; ++i) { char l[16]; int n = snprintf(l, sizeof l, "w%02d-%03d\n", id, i); if (write(fd, l, (size_t)n) != n) return 2; }
    close(fd); return 0;
}
static int w_grand(int id)
{
    for (int g = 0; g < 4; ++g) { pid_t c = fork(); if (c < 0) return 1; if (c == 0) _exit(10 + g + id % 3); }
    int st, sum = 0; pid_t w; while ((w = waitpid(-1, &st, 0)) > 0) sum += (st >> 8) & 0xFF;
    return sum == (10 + 11 + 12 + 13) + (id % 3) * 4 ? 0 : 2;
}
static int w_cow(int id)
{
    char* m = (char*)mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (m == MAP_FAILED) return 1;
    memset(m, 'P', 8192);
    pid_t c = fork(); if (c < 0) return 2;
    if (c == 0) { for (int i = 0; i < 8192; i += 64) m[i] = (char)('A' + id); for (int i = 0; i < 8192; i += 64) if (m[i] != (char)('A' + id)) _exit(3); _exit(0); }
    int st; waitpid(c, &st, 0); if (st != 0) return 4;
    for (int i = 0; i < 8192; i += 64) if (m[i] != 'P') return 5;
    return 0;
}
static volatile int got_sig = 0;
static void on_usr1(int s) { (void)s; got_sig = 1; }

int main(int argc, char** argv)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN); int soak = argc > 1 ? atoi(argv[1]) : 0;
    if (argc > 1 && !strcmp(argv[1], "pp"))       /* latency probe: pipe round trips only */
    {
        int a[2], b[2]; pipe(a); pipe(b); pid_t c = fork();
        if (c == 0) { unsigned v; for (int i = 0; i < 300; ++i) { read(a[0], &v, 4); write(b[1], &v, 4); } _exit(0); }
        unsigned long t1 = clock_ms(); for (unsigned i = 0; i < 300; ++i) { unsigned v = i, r; write(a[1], &v, 4); read(b[0], &r, 4); }
        printf("pp: %lu ms for 300 round trips (cpus %ld)\n", clock_ms() - t1, ncpu); waitpid(c, 0, 0); return 0;
    }
    printf("smptest: online cpus = %ld\n", ncpu);
    sh = (Shared*)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0); chk("shared-map", sh != (Shared*)MAP_FAILED);
    cpu_set_t cs; chk("sched_getaffinity", sched_getaffinity(0, sizeof cs, &cs) > 0 && CPU_COUNT(&cs) == ncpu && sched_getcpu() >= 0 && sched_getcpu() < ncpu);

    memset((void*)sh, 0, sizeof *sh);
    unsigned long t0 = clock_ms(); int n = (int)(ncpu > 1 ? ncpu : 2) * 2;
    spawn_n(n, w_spin); chk("spin-workers-clean", reap_all() == 0);
    int distinct = 0; for (int i = 0; i < 8; ++i) distinct += sh->cpu_seen[i] ? 1 : 0;
    printf("  spin: %lu ms, distinct cpus used: %d\n", clock_ms() - t0, distinct);
    chk("work-spreads-across-cpus", ncpu < 2 || distinct >= 2);

    memset((void*)sh, 0, sizeof *sh); spawn_n(6, w_counter); chk("futex-mutex-workers-clean", reap_all() == 0);
    chk("futex-mutex-counter-exact", sh->counter == 6u * 1500u);
    chk("atomic-counter-exact", sh->atomic_counter == 6u * 1500u);

    spawn_n(6, w_file); chk("file-io-parallel", reap_all() == 0);
    unlink("/tmp/smp_log"); spawn_n(6, w_append); chk("append-workers-clean", reap_all() == 0);
    { FILE* f = fopen("/tmp/smp_log", "r"); int lines = 0, bad = 0; char l[32]; while (f && fgets(l, sizeof l, f)) { ++lines; if (strlen(l) != 8 || l[0] != 'w') ++bad; } if (f) fclose(f);
      chk("append-no-lost-or-torn-lines", lines == 240 && bad == 0); unlink("/tmp/smp_log"); }

    for (int r = 0; r < 3; ++r) { spawn_n(6, w_grand); if (reap_all() != 0) { chk("fork-storm", 0); break; } if (r == 2) chk("fork-storm", 1); }
    spawn_n(6, w_cow); chk("cow-isolation-parallel", reap_all() == 0);

    { int a[2], b[2]; pipe(a); pipe(b); pid_t c = fork();
      if (c == 0) { close(a[1]); close(b[0]); unsigned v; for (int i = 0; i < 1500; ++i) { if (read(a[0], &v, 4) != 4) _exit(1); v += 1; if (write(b[1], &v, 4) != 4) _exit(2); } _exit(0); }
      close(a[0]); close(b[1]); int ok = 1; unsigned long t1 = clock_ms();
      for (unsigned i = 0; i < 1500 && ok; ++i) { unsigned v = i * 3, r = 0; if (write(a[1], &v, 4) != 4 || read(b[0], &r, 4) != 4 || r != v + 1) ok = 0; }
      int st; waitpid(c, &st, 0); close(a[1]); close(b[0]); printf("  pipe ping-pong: %lu ms\n", clock_ms() - t1); chk("pipe-pingpong-2-processes", ok && st == 0); }

    { int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv); pid_t c = fork();
      if (c == 0) { close(sv[0]); unsigned char buf[1024]; unsigned sum = 0; long total = 0; for (;;) { long r = read(sv[1], buf, sizeof buf); if (r <= 0) break; for (long i = 0; i < r; ++i) sum += buf[i]; total += r; } write(sv[1], &sum, 4); _exit(total == 262144 ? 0 : 1); }
      close(sv[1]); unsigned char buf[1024]; unsigned sum = 0; for (int i = 0; i < 256; ++i) { for (int k = 0; k < 1024; ++k) { buf[k] = (unsigned char)(i + k); sum += buf[k]; } long off = 0; while (off < 1024) { long w = write(sv[0], buf + off, (size_t)(1024 - off)); if (w <= 0) break; off += w; } }
      shutdown(sv[0], SHUT_WR); unsigned rs = 0; long rr = read(sv[0], &rs, 4); int st; waitpid(c, &st, 0); close(sv[0]); chk("unix-stream-256k-checksum", rr == 4 && rs == sum && st == 0); }

    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_usr1; sigaction(SIGUSR1, &sa, 0);
      memset((void*)sh, 0, sizeof *sh); pid_t c = fork();
      if (c == 0) { while (!got_sig) { for (volatile int k = 0; k < 1000; ++k) {} } __atomic_store_n(&sh->flag, 1, __ATOMIC_SEQ_CST); _exit(0); }
      usleep(100000); unsigned long ts = clock_ms(); kill(c, SIGUSR1); int st; waitpid(c, &st, 0);
      chk("signal-to-spinning-process", st == 0 && sh->flag == 1 && clock_ms() - ts < 1000);
      pid_t d = fork(); if (d == 0) { for (;;) asm volatile("" ::: "memory"); }
      usleep(100000); kill(d, SIGKILL); waitpid(d, &st, 0); chk("sigkill-spinning-process", st != 0); }

    { pid_t cs2[4]; for (int i = 0; i < 4; ++i) { cs2[i] = fork(); if (cs2[i] == 0) { for (;;) asm volatile("" ::: "memory"); } }
      unsigned long ts = clock_ms(); int p[2]; pipe(p); pid_t w = fork(); if (w == 0) { usleep(80000); write(p[1], "x", 1); _exit(0); }
      struct pollfd pf; pf.fd = p[0]; pf.events = POLLIN; int r = poll(&pf, 1, 3000); unsigned long el = clock_ms() - ts;
      chk("poll-wakes-while-all-cpus-busy", r == 1 && el < 2000); waitpid(w, 0, 0);
      for (int i = 0; i < 4; ++i) kill(cs2[i], SIGKILL); reap_all(); close(p[0]); close(p[1]); }

    if (soak > 0)
    {
        unsigned long end = clock_ms() + (unsigned long)soak * 1000UL; int rounds = 0, bad = 0;
        while (clock_ms() < end && !bad)
        {
            memset((void*)sh, 0, sizeof *sh);
            spawn_n(5, w_counter); spawn_n(3, w_file); spawn_n(3, w_grand); spawn_n(3, w_cow); spawn_n(2, w_append);
            bad = reap_all(); if (!bad && (sh->counter != 5u * 1500u || sh->atomic_counter != 5u * 1500u)) bad = 1;
            unlink("/tmp/smp_log"); ++rounds;
            if (rounds % 25 == 0) printf("  soak round %d\n", rounds);
        }
        printf("  soak rounds: %d\n", rounds); chk("soak", bad == 0);
    }
    printf("smptest done fails=%d\n", fails);
    return fails ? 1 : 0;
}
