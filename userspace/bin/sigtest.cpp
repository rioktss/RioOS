#include "../libc/ulibc.h"
/* sigtest: extended signal API (siginfo, sigqueue, sigaltstack, sigwait, setitimer, SIGSEGV recovery, SA_RESTART, setjmp) */
static int fails = 0;
static void chk(const char* name, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++fails; }

static volatile int g_sig = 0, g_code = 99, g_pid = 0; static volatile long g_val = 0; static volatile int g_calls = 0;
static void on_info(int s, siginfo_t* si, void* uc) { (void)uc; g_sig = s; g_code = si->si_code; g_pid = si->si_pid; g_val = (long)si->si_value.sival_ptr; ++g_calls; }
static void on_plain(int s) { g_sig = s; ++g_calls; }

static char altbuf[16384]; static volatile int on_alt = 0;
static void on_alt_stack(int s) { (void)s; char probe; on_alt = (&probe >= altbuf && &probe < altbuf + sizeof(altbuf)); }

static sigjmp_buf jb; static volatile int segv_info_ok = 0;
static void on_segv(int s, siginfo_t* si, void* uc) { (void)uc; segv_info_ok = (s == SIGSEGV && (unsigned long)si->si_addr == 0x10UL); siglongjmp(jb, 1); }

static volatile int ticks = 0; static void on_tick(int s) { (void)s; ++ticks; }
static volatile int nest_depth = 0, nest_max = 0;
static void on_nest(int s) { (void)s; ++nest_depth; if (nest_depth > nest_max) nest_max = nest_depth; if (nest_depth == 1) raise(SIGUSR2); --nest_depth; }
static volatile int resethand_calls = 0; static void on_once(int s) { (void)s; ++resethand_calls; }

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    struct sigaction sa; sigset_t m; siginfo_t si;

    /* SA_SIGINFO: sender pid + code */
    sa.sa_sigaction = on_info; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_SIGINFO;
    chk("sigaction-siginfo", sigaction(SIGUSR1, &sa, 0) == 0);
    kill(getpid(), SIGUSR1);
    chk("siginfo-kill", g_sig == SIGUSR1 && g_pid == getpid() && g_code == SI_USER);
    union sigval v; v.sival_ptr = (void*)1234;
    sigqueue(getpid(), SIGUSR1, v);
    chk("siginfo-sigqueue-value", g_code == SI_QUEUE && g_val == 1234 && g_calls == 2);

    /* sigaction old action + query */
    { struct sigaction old; sa.sa_sigaction = on_info; sigaction(SIGUSR1, 0, &old);
      chk("sigaction-query", old.sa_sigaction == on_info && (old.sa_flags & SA_SIGINFO)); }

    /* alternate stack */
    { stack_t ss; ss.ss_sp = altbuf; ss.ss_size = sizeof(altbuf); ss.ss_flags = 0;
      chk("sigaltstack-set", sigaltstack(&ss, 0) == 0);
      stack_t cur; sigaltstack(0, &cur); chk("sigaltstack-get", cur.ss_sp == altbuf && cur.ss_size == sizeof(altbuf));
      struct sigaction a2; a2.sa_handler = on_alt_stack; sigemptyset(&a2.sa_mask); a2.sa_flags = SA_ONSTACK;
      sigaction(SIGUSR2, &a2, 0); raise(SIGUSR2);
      chk("handler-on-altstack", on_alt == 1);
      a2.sa_flags = 0; sigaction(SIGUSR2, &a2, 0); raise(SIGUSR2);
      chk("handler-without-onstack-uses-normal-stack", on_alt == 0); }

    /* SIGSEGV handler + siglongjmp recovery */
    { struct sigaction s2; s2.sa_sigaction = on_segv; sigemptyset(&s2.sa_mask); s2.sa_flags = SA_SIGINFO | SA_NODEFER;
      sigaction(SIGSEGV, &s2, 0);
      int recovered = 0;
      if (sigsetjmp(jb, 1) == 0) { volatile int* p = (volatile int*)0x10; *p = 5; }
      else recovered = 1;
      chk("sigsegv-recovered-via-siglongjmp", recovered == 1 && segv_info_ok == 1);
      recovered = 0;
      if (sigsetjmp(jb, 1) == 0) { volatile int* p = (volatile int*)0x10; *p = 6; } else recovered = 1;
      chk("sigsegv-twice", recovered == 1);
      sigset_t cur; sigprocmask(SIG_BLOCK, 0, &cur); chk("sigsegv-mask-restored", sigismember(&cur, SIGSEGV) == 0); }

    /* setjmp / longjmp */
    { jmp_buf j; volatile int n = 0; int r = setjmp(j); ++n; if (r == 0) longjmp(j, 7); chk("setjmp-longjmp", r == 7 && n == 2);
      r = setjmp(j); if (r == 0) longjmp(j, 0); chk("longjmp-zero-becomes-one", r == 1); }

    /* sigwait / sigtimedwait / sigwaitinfo */
    { sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, 0);
      pid_t p = fork(); if (p == 0) { sleep_ms(40); kill(getppid(), SIGUSR1); _exit(0); }
      int got = 0; int rc = sigwait(&m, &got); waitpid(p, 0, 0);
      chk("sigwait", rc == 0 && got == SIGUSR1);
      struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 30 * 1000000L; unsigned long t0 = clock_ms();
      int r2 = sigtimedwait(&m, &si, &ts);
      chk("sigtimedwait-timeout", r2 == -1 && errno == EAGAIN && clock_ms() - t0 >= 20);
      union sigval vv; vv.sival_ptr = (void*)99; sigqueue(getpid(), SIGUSR1, vv);
      chk("sigwaitinfo-value", sigwaitinfo(&m, &si) == SIGUSR1 && si.si_code == SI_QUEUE && (long)si.si_value.sival_ptr == 99 && si.si_pid == getpid());
      sigprocmask(SIG_UNBLOCK, &m, 0); }

    /* setitimer with interval */
    { struct sigaction a3; a3.sa_handler = on_tick; sigemptyset(&a3.sa_mask); a3.sa_flags = 0; sigaction(SIGALRM, &a3, 0);
      struct itimerval it; it.it_value.tv_sec = 0; it.it_value.tv_usec = 20000; it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 20000;
      ticks = 0; setitimer(ITIMER_REAL, &it, 0);
      struct itimerval q; getitimer(ITIMER_REAL, &q);
      chk("getitimer-interval", q.it_interval.tv_usec == 20000);
      sleep_ms(30); sleep_ms(30); sleep_ms(30);
      memset(&it, 0, sizeof(it)); setitimer(ITIMER_REAL, &it, 0);
      int t = ticks; sleep_ms(60);
      chk("setitimer-periodic", t >= 3 && ticks == t); }

    /* SA_RESTART vs EINTR on a blocking pipe read */
    { int pf[2]; pipe(pf);
      pid_t p = fork(); if (p == 0) { close(pf[0]); sleep_ms(150); write(pf[1], "ok", 2); _exit(0); }
      close(pf[1]);
      struct sigaction a4; a4.sa_handler = on_tick; sigemptyset(&a4.sa_mask); a4.sa_flags = SA_RESTART; sigaction(SIGALRM, &a4, 0);
      struct itimerval it; memset(&it, 0, sizeof(it)); it.it_value.tv_usec = 40000; ticks = 0; setitimer(ITIMER_REAL, &it, 0);
      char b[4] = {0}; ssize_t n = read(pf[0], b, 2);
      chk("sa_restart-read-resumes", n == 2 && b[0] == 'o' && ticks == 1);
      waitpid(p, 0, 0); close(pf[0]);
      pipe(pf); p = fork(); if (p == 0) { close(pf[0]); sleep_ms(150); write(pf[1], "ok", 2); _exit(0); }
      close(pf[1]);
      a4.sa_flags = 0; sigaction(SIGALRM, &a4, 0); ticks = 0; setitimer(ITIMER_REAL, &it, 0);
      n = read(pf[0], b, 2);
      chk("no-sa_restart-read-eintr", n == -1 && errno == EINTR && ticks == 1);
      kill(p, SIGKILL); waitpid(p, 0, 0); close(pf[0]); }

    /* SA_NODEFER nesting, SA_RESETHAND, handler mask, signal ignore */
    { struct sigaction a5; a5.sa_handler = on_nest; sigemptyset(&a5.sa_mask); a5.sa_flags = 0; sigaction(SIGUSR1, &a5, 0);
      a5.sa_handler = on_nest; sigaction(SIGUSR2, &a5, 0);
      nest_depth = 0; nest_max = 0; raise(SIGUSR1);
      chk("nested-different-signals", nest_max == 2 && nest_depth == 0);
      a5.sa_handler = on_once; a5.sa_flags = SA_RESETHAND; sigaction(SIGUSR1, &a5, 0);
      raise(SIGUSR1);
      struct sigaction o; sigaction(SIGUSR1, 0, &o);
      chk("sa_resethand", resethand_calls == 1 && o.sa_handler == SIG_DFL); }

    /* sigsuspend */
    { struct sigaction a6; a6.sa_handler = on_plain; sigemptyset(&a6.sa_mask); a6.sa_flags = 0; sigaction(SIGUSR1, &a6, 0);
      sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, 0);
      pid_t p = fork(); if (p == 0) { sleep_ms(40); kill(getppid(), SIGUSR1); _exit(0); }
      sigset_t empty; sigemptyset(&empty); g_calls = 0;
      int r = sigsuspend(&empty);
      sigset_t after; sigprocmask(SIG_BLOCK, 0, &after);
      chk("sigsuspend", r == -1 && errno == EINTR && g_calls == 1 && sigismember(&after, SIGUSR1) == 1);
      waitpid(p, 0, 0); sigprocmask(SIG_UNBLOCK, &m, 0); }

    /* misc */
    chk("strsignal", !strcmp(strsignal(SIGSEGV), "Segmentation fault") && !strcmp(strsignal(SIGTERM), "Terminated"));
    { struct timespec rq, rm; rq.tv_sec = 0; rq.tv_nsec = 30 * 1000000L; unsigned long t0 = clock_ms(); int r = nanosleep(&rq, &rm);
      chk("nanosleep", r == 0 && clock_ms() - t0 >= 25); }
    { struct timeval tv; struct timespec a, b; gettimeofday(&tv, 0); clock_gettime(CLOCK_MONOTONIC, &a); sleep_ms(20); clock_gettime(CLOCK_MONOTONIC, &b);
      chk("clock_gettime", tv.tv_sec > 1000000000L && (b.tv_sec * 1000 + b.tv_nsec / 1000000) - (a.tv_sec * 1000 + a.tv_nsec / 1000000) >= 15); }

    /* unhandled SIGSEGV still terminates (no handler) */
    { pid_t p = fork(); if (p == 0) { signal(SIGSEGV, SIG_DFL); volatile int* q = (volatile int*)0x10; *q = 1; _exit(0); }
      int st = 0; waitpid(p, &st, 0); chk("sigsegv-default-kills", WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV); }
    /* fault inside its own handler (SEGV blocked) terminates instead of looping */
    { pid_t p = fork();
      if (p == 0) { struct sigaction s2; s2.sa_sigaction = (void (*)(int, siginfo_t*, void*))+[](int, siginfo_t*, void*) { volatile int* q = (volatile int*)0x20; *q = 1; };
                    sigemptyset(&s2.sa_mask); s2.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &s2, 0); volatile int* q = (volatile int*)0x10; *q = 1; _exit(0); }
      int st = 0; waitpid(p, &st, 0); chk("fault-in-handler-terminates", WIFSIGNALED(st)); }

    printf("sigtest done fails=%d\n", fails);
    return fails ? 1 : 0;
}
