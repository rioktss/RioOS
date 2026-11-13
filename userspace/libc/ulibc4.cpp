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

/* RioOS libc part 4: POSIX signals, execve family with environment, system/popen, posix_spawn, fcntl, msync. */
#include "ulibc.h"
#include "../../include/user_api.h"

#define RS_SIGACTION 65
#define RS_SIGPROCMASK 66
#define RS_ALARM_MS 68
#define RS_PAUSE 69
#define RS_SIGPENDING 70
#define RS_FCNTL 71
#define RS_MSYNC 73
#define RS_EXEC 38

extern "C" {

static long rret(uint64_t v) { long r = (long)v; if (r < 0 && r > -4096) { errno = (int)-r; return -1; } return r; }

/* ---- signals ---------------------------------------------------------- */
#define RS_SIGQUEUE 74
#define RS_SIGALTSTACK 75
#define RS_SIGWAIT 76
#define RS_SETITIMER 77
#define RS_HTTP_GET 78
#define RS_SHA256 79
#define RS_ED25519 112
extern int __rio_sa_restart;
void __rio_sigreturn(void);
__asm__(".text\n.global __rio_sigreturn\n__rio_sigreturn:\n mov x8, #67\n svc #0\n 1: b 1b\n");

/* setjmp family: x19-x28, x29, x30, sp (+ signal mask for sigsetjmp) */
/* d8-d15 (callee-saved FP registers) are kept at byte offset 120..183 of the jmp_buf. */
__asm__(".text\n.arch_extension fp\n"
        ".global setjmp\nsetjmp:\n"
        " stp x19, x20, [x0]\n stp x21, x22, [x0, #16]\n stp x23, x24, [x0, #32]\n stp x25, x26, [x0, #48]\n stp x27, x28, [x0, #64]\n"
        " stp x29, x30, [x0, #80]\n stp d8, d9, [x0, #120]\n stp d10, d11, [x0, #136]\n stp d12, d13, [x0, #152]\n stp d14, d15, [x0, #168]\n mov x2, sp\n str x2, [x0, #96]\n mov x0, #0\n ret\n"
        ".global longjmp\nlongjmp:\n"
        " ldp x19, x20, [x0]\n ldp x21, x22, [x0, #16]\n ldp x23, x24, [x0, #32]\n ldp x25, x26, [x0, #48]\n ldp x27, x28, [x0, #64]\n"
        " ldp x29, x30, [x0, #80]\n ldp d8, d9, [x0, #120]\n ldp d10, d11, [x0, #136]\n ldp d12, d13, [x0, #152]\n ldp d14, d15, [x0, #168]\n ldr x2, [x0, #96]\n mov sp, x2\n cmp w1, #0\n csinc w0, w1, wzr, ne\n ret\n"
        ".global sigsetjmp\nsigsetjmp:\n"
        " stp x19, x20, [x0]\n stp x21, x22, [x0, #16]\n stp x23, x24, [x0, #32]\n stp x25, x26, [x0, #48]\n stp x27, x28, [x0, #64]\n"
        " stp x29, x30, [x0, #80]\n stp d8, d9, [x0, #120]\n stp d10, d11, [x0, #136]\n stp d12, d13, [x0, #152]\n stp d14, d15, [x0, #168]\n mov x2, sp\n str x2, [x0, #96]\n"
        " sxtw x1, w1\n str x1, [x0, #104]\n cbz x1, 1f\n"
        " mov x9, x0\n mov x0, #3\n mov x1, #0\n mov x8, #66\n svc #0\n str x0, [x9, #112]\n"
        "1: mov x0, #0\n ret\n"
        ".global siglongjmp\nsiglongjmp:\n"
        " mov x9, x0\n mov w10, w1\n ldr x11, [x9, #104]\n cbz x11, 2f\n"
        " ldr x1, [x9, #112]\n mov x0, #2\n mov x8, #66\n svc #0\n"
        "2: mov x0, x9\n mov w1, w10\n b longjmp\n");

int sigemptyset(sigset_t* s) { *s = 0; return 0; }
int sigfillset(sigset_t* s) { *s = 0xFFFFFFFEU; return 0; }
int sigaddset(sigset_t* s, int sig) { if (sig <= 0 || sig >= 32) { errno = EINVAL; return -1; } *s |= 1U << sig; return 0; }
int sigdelset(sigset_t* s, int sig) { if (sig <= 0 || sig >= 32) { errno = EINVAL; return -1; } *s &= ~(1U << sig); return 0; }
int sigismember(const sigset_t* s, int sig) { if (sig <= 0 || sig >= 32) { errno = EINVAL; return -1; } return (*s >> sig) & 1U; }

/* The kernel always enters __rio_sigdispatch(sig, info, uc); it calls the program's handler
   from the table, so SA_SIGINFO / SA_RESTART / per-signal masks all work from one entry point. */
static struct sigaction sig_table[32];
static char sig_init[32];
static int sig_initialized_for(int sig)
{
    if (!sig_init[sig])
    {
        sig_init[sig] = 1;
        uint64_t q = user_syscall4(RS_SIGACTION, (uint64_t)(long)sig, ~0ULL, 0, 0);
        sig_table[sig].sa_handler = (q == 1) ? SIG_IGN : SIG_DFL;      /* ignored dispositions survive exec */
        sig_table[sig].sa_mask = 0; sig_table[sig].sa_flags = 0;
    }
    return 1;
}
void __rio_sigdispatch(int sig, siginfo_t* info, void* uc)
{
    if (sig <= 0 || sig >= 32) return;
    struct sigaction a = sig_table[sig];
    if (a.sa_flags & SA_RESTART) __rio_sa_restart = 1;
    if (a.sa_flags & SA_RESETHAND) { sig_table[sig].sa_handler = SIG_DFL; sig_table[sig].sa_flags = 0; }
    if (a.sa_handler == SIG_DFL || a.sa_handler == SIG_IGN) return;
    if (a.sa_flags & SA_SIGINFO) a.sa_sigaction(sig, info, uc);
    else a.sa_handler(sig);
}

int sigaction(int sig, const struct sigaction* act, struct sigaction* old)
{
    if (sig <= 0 || sig >= 32 || sig == 9 || sig == 19) { errno = EINVAL; return -1; }
    sig_initialized_for(sig);
    struct sigaction prev = sig_table[sig];
    if (act)
    {
        uint64_t k = act->sa_handler == SIG_DFL ? 0 : act->sa_handler == SIG_IGN ? 1 : (uint64_t)(unsigned long)__rio_sigdispatch;
        long r = rret(user_syscall4(RS_SIGACTION, (uint64_t)(long)sig, k, (uint64_t)(unsigned long)__rio_sigreturn,
                                    (uint64_t)act->sa_mask | ((uint64_t)act->sa_flags << 32)));
        if (r == -1 && errno) return -1;
        sig_table[sig] = *act;
    }
    if (old) *old = prev;
    return 0;
}

sighandler_t signal(int sig, sighandler_t handler)
{
    struct sigaction a, o;
    a.sa_handler = handler; a.sa_mask = 0; a.sa_flags = SA_RESTART;
    if (sigaction(sig, &a, &o) != 0) return SIG_ERR;
    return o.sa_handler;
}

/* Runs the disposition of `sig` inside this process (used for thread-directed signals). */
int __rio_signal_local(int sig)
{
    if (sig <= 0 || sig >= 32) return -1;
    sig_initialized_for(sig);
    struct sigaction a = sig_table[sig];
    if (a.sa_handler == SIG_IGN) return 0;
    if (a.sa_handler == SIG_DFL) return kill(getpid(), sig);     /* default action affects the whole process */
    siginfo_t info; info.si_signo = sig; info.si_errno = 0; info.si_code = -6; info.si_pid = getpid(); info.si_value.sival_ptr = 0; info.si_addr = 0;
    __rio_sigdispatch(sig, &info, 0);
    return 0;
}

int raise(int sig) { return kill(getpid(), sig) == 0 ? 0 : -1; }
int sigqueue(pid_t pid, int sig, union sigval value) { return (int)rret(user_syscall3(RS_SIGQUEUE, (uint64_t)(long)pid, (uint64_t)(long)sig, (uint64_t)(unsigned long)value.sival_ptr)); }
int sigaltstack(const stack_t* ss, stack_t* old) { return (int)rret(user_syscall2(RS_SIGALTSTACK, (uint64_t)ss, (uint64_t)old)); }

int sigprocmask(int how, const sigset_t* set, sigset_t* old)
{
    long r;
    if (!set) r = rret(user_syscall2(RS_SIGPROCMASK, 3, 0));
    else r = rret(user_syscall2(RS_SIGPROCMASK, (uint64_t)(long)how, (uint64_t)*set));
    if (r == -1 && errno) return -1;
    if (old) *old = (sigset_t)r;
    return 0;
}
int sigpending(sigset_t* set) { long r = rret(user_syscall0(RS_SIGPENDING)); if (r < 0) return -1; *set = (sigset_t)r; return 0; }
int pause(void) { (void)user_syscall0(RS_PAUSE); errno = EINTR; return -1; }
int sigsuspend(const sigset_t* mask)
{
    sigset_t old; sigprocmask(SIG_SETMASK, mask, &old);
    pause();
    sigprocmask(SIG_SETMASK, &old, 0);
    errno = EINTR; return -1;
}
int sigwaitinfo(const sigset_t* set, siginfo_t* info)
{
    for (;;)
    {
        long r = rret(user_syscall3(RS_SIGWAIT, (uint64_t)*set, ~0ULL, (uint64_t)info));
        if (r == -1 && errno == EINTR) continue;
        return (int)r;
    }
}
int sigwait(const sigset_t* set, int* sig) { int r = sigwaitinfo(set, 0); if (r < 0) return errno; *sig = r; return 0; }
int sigtimedwait(const sigset_t* set, siginfo_t* info, const struct timespec* timeout)
{
    uint64_t ms = timeout ? (uint64_t)timeout->tv_sec * 1000ULL + (uint64_t)(timeout->tv_nsec + 999999L) / 1000000ULL : ~0ULL;
    long r = rret(user_syscall3(RS_SIGWAIT, (uint64_t)*set, ms, (uint64_t)info));
    return (int)r;
}
unsigned alarm(unsigned seconds)
{
    uint64_t rem = user_syscall1(RS_ALARM_MS, (uint64_t)seconds * 1000ULL);
    return (unsigned)((rem + 999ULL) / 1000ULL);
}
static uint64_t tv_ms(const struct timeval* t) { uint64_t m = (uint64_t)t->tv_sec * 1000ULL + (uint64_t)t->tv_usec / 1000ULL; if (m == 0 && (t->tv_sec || t->tv_usec)) m = 1; return m; }
int setitimer(int which, const struct itimerval* nv, struct itimerval* old)
{
    if (which != ITIMER_REAL || !nv) { errno = EINVAL; return -1; }
    uint64_t rem = user_syscall2(RS_SETITIMER, tv_ms(&nv->it_value), tv_ms(&nv->it_interval));
    if (old) { old->it_value.tv_sec = (long)(rem / 1000); old->it_value.tv_usec = (long)(rem % 1000) * 1000; old->it_interval.tv_sec = 0; old->it_interval.tv_usec = 0; }
    return 0;
}
int getitimer(int which, struct itimerval* cur)
{
    if (which != ITIMER_REAL || !cur) { errno = EINVAL; return -1; }
    uint64_t iv = 0;
    uint64_t rem = user_syscall2(RS_SETITIMER, ~0ULL, (uint64_t)&iv);
    cur->it_value.tv_sec = (long)(rem / 1000); cur->it_value.tv_usec = (long)(rem % 1000) * 1000;
    cur->it_interval.tv_sec = (long)(iv / 1000); cur->it_interval.tv_usec = (long)(iv % 1000) * 1000;
    return 0;
}

int nanosleep(const struct timespec* req, struct timespec* rem)
{
    if (!req || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) { errno = EINVAL; return -1; }
    unsigned long ms = (unsigned long)req->tv_sec * 1000UL + (unsigned long)(req->tv_nsec + 999999L) / 1000000UL;
    unsigned long left = sleep_ms((unsigned)ms);
    if (left > 0) { if (rem) { rem->tv_sec = (long)(left / 1000); rem->tv_nsec = (long)(left % 1000) * 1000000L; } errno = EINTR; return -1; }
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}
/* Wall-clock ms consistent with the monotonic clock: epoch seconds sampled once, then advanced by clock_ms. */
unsigned long long __rio_realtime_ms(void)
{
    static long long base = -1;
    if (base < 0) base = (long long)time(0) * 1000LL - (long long)clock_ms();
    return (unsigned long long)(base + (long long)clock_ms());
}
int gettimeofday(struct timeval* tv, void* tz)
{
    (void)tz; if (!tv) return 0;
    unsigned long long m = __rio_realtime_ms();
    tv->tv_sec = (long)(m / 1000ULL); tv->tv_usec = (long)(m % 1000ULL) * 1000L; return 0;
}
int clock_gettime(int clk, struct timespec* ts)
{
    if (!ts) { errno = EINVAL; return -1; }
    if (clk == CLOCK_MONOTONIC) { unsigned long m = clock_ms(); ts->tv_sec = (long)(m / 1000); ts->tv_nsec = (long)(m % 1000) * 1000000L; return 0; }
    if (clk != CLOCK_REALTIME) { errno = EINVAL; return -1; }
    { unsigned long long m = __rio_realtime_ms(); ts->tv_sec = (long)(m / 1000ULL); ts->tv_nsec = (long)(m % 1000ULL) * 1000000L; return 0; }
}

const char* strsignal(int sig)
{
    static const char* names[32] = { "Unknown signal 0", "Hangup", "Interrupt", "Quit", "Illegal instruction", "Trace/breakpoint trap", "Aborted", "Bus error",
        "Floating point exception", "Killed", "User defined signal 1", "Segmentation fault", "User defined signal 2", "Broken pipe", "Alarm clock", "Terminated",
        "Stack fault", "Child exited", "Continued", "Stopped (signal)", "Stopped", "Stopped (tty input)", "Stopped (tty output)", "Urgent I/O condition",
        "CPU time limit exceeded", "File size limit exceeded", "Virtual timer expired", "Profiling timer expired", "Window changed", "I/O possible", "Power failure", "Bad system call" };
    return (sig > 0 && sig < 32) ? names[sig] : "Unknown signal";
}
void psignal(int sig, const char* msg) { if (msg && *msg) { fputs(msg, stderr); fputs(": ", stderr); } fputs(strsignal(sig), stderr); fputc('\n', stderr); }

/* ---- msync / fcntl ----------------------------------------------------- */
int msync(void* addr, size_t len, int flags) { (void)flags; return (int)rret(user_syscall2(RS_MSYNC, (uint64_t)addr, len)); }
int fcntl(int fd, int cmd, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, cmd);
    long arg = __builtin_va_arg(ap, long);
    __builtin_va_end(ap);
    return (int)rret(user_syscall3(RS_FCNTL, (uint64_t)(long)fd, (uint64_t)(long)cmd, (uint64_t)arg));
}

/* ---- process groups (single session: every process is its own group leader) --- */
pid_t getpgrp(void) { return getpid(); }
pid_t getpgid(pid_t pid) { return pid == 0 ? getpid() : pid; }
pid_t setsid(void) { return getpid(); }
int setpgid(pid_t pid, pid_t pgid) { (void)pid; (void)pgid; return 0; }

/* ---- exec family -------------------------------------------------------- */
#define ARGS_CAP 1024
static int push(char* out, int* n, const char* s) { while (*s) { if (*n >= ARGS_CAP - 1) return -1; out[(*n)++] = *s++; } return 0; }

/* Packs argv[1..] as a quoted argument line, then \x01\x02argv0 and \x01NAME=value entries. */
static int pack_args(char* out, char* const argv[], char* const envp[])
{
    int n = 0; out[0] = 0;
    for (int i = 1; argv && argv[i]; ++i)
    {
        const char* a = argv[i];
        int needq = (a[0] == 0), hasd = 0, hass = 0;
        for (const char* p = a; *p; ++p) { if (*p == ' ' || *p == '\t') needq = 1; if (*p == '"') hasd = 1; if (*p == '\'') hass = 1; if (*p == 1) return -1; }
        if (i > 1 && push(out, &n, " ")) return -1;
        if (needq || hasd || hass)
        {
            if (hasd && hass) return -1;
            char q[2] = { hasd ? '\'' : '"', 0 };
            if (push(out, &n, q) || push(out, &n, a) || push(out, &n, q)) return -1;
        }
        else if (push(out, &n, a)) return -1;
    }
    if (argv && argv[0] && argv[0][0]) { if (push(out, &n, "\x01\x02") || push(out, &n, argv[0])) return -1; }
    for (int i = 0; envp && envp[i]; ++i)
    {
        if (!envp[i][0] || !__builtin_strchr(envp[i], '=')) continue;
        if (push(out, &n, "\x01") || push(out, &n, envp[i])) return -1;
    }
    out[n] = 0;
    return n;
}

int execve(const char* path, char* const argv[], char* const envp[])
{
    char buf[ARGS_CAP];
    if (!path || !path[0]) { errno = ENOENT; return -1; }
    if (pack_args(buf, argv, envp) < 0) { errno = E2BIG; return -1; }
    return (int)rret(user_syscall2(RS_EXEC, (uint64_t)path, (uint64_t)buf));
}
int execv(const char* path, char* const argv[]) { return execve(path, argv, environ); }

static int file_is_there(const char* p) { struct RioStat st; return stat_path(p, &st) == 0 && st.type == 0; }

/* Resolves `file` the way the shell does: a path as given, else PATH entries, else /bin/NAME and /bin/NAME.elf. */
static int resolve_exec(const char* file, char* out, size_t cap)
{
    if (__builtin_strchr(file, '/')) { if (strlen(file) >= cap) return -1; strcpy(out, file); return file_is_there(out) ? 0 : -1; }
    const char* path = getenv("PATH");
    if (!path || !*path) path = "/bin";
    const char* p = path;
    while (*p)
    {
        const char* e = p; while (*e && *e != ':') ++e;
        size_t dl = (size_t)(e - p);
        if (dl > 0 && dl + strlen(file) + 6 < cap)
        {
            memcpy(out, p, dl); out[dl] = '/'; strcpy(out + dl + 1, file);
            if (file_is_there(out)) return 0;
            strcat(out, ".elf");
            if (file_is_there(out)) return 0;
        }
        p = *e ? e + 1 : e;
    }
    return -1;
}
int execvpe_impl(const char* file, char* const argv[], char* const envp[])
{
    char full[160];
    if (!file || !file[0]) { errno = ENOENT; return -1; }
    if (resolve_exec(file, full, sizeof(full)) != 0) { errno = ENOENT; return -1; }
    return execve(full, argv, envp);
}
int execvp(const char* file, char* const argv[]) { return execvpe_impl(file, argv, environ); }

#define COLLECT_ARGS(first, vec, cnt) \
    char* vec[34]; int cnt = 0; vec[cnt++] = (char*)(first); \
    __builtin_va_list ap; __builtin_va_start(ap, first); \
    if (first) { while (cnt < 33) { char* a = __builtin_va_arg(ap, char*); vec[cnt++] = a; if (!a) break; } } \
    if (cnt == 33 && vec[32]) vec[33] = 0;

int execl(const char* path, const char* arg0, ...)
{
    COLLECT_ARGS(arg0, vec, cnt) (void)cnt; __builtin_va_end(ap);
    return execve(path, vec, environ);
}
int execlp(const char* file, const char* arg0, ...)
{
    COLLECT_ARGS(arg0, vec, cnt) (void)cnt; __builtin_va_end(ap);
    return execvpe_impl(file, vec, environ);
}
int execle(const char* path, const char* arg0, ...)
{
    COLLECT_ARGS(arg0, vec, cnt)
    char** envp = environ;
    if (cnt > 0 && vec[cnt - 1] == 0 && cnt < 34) envp = __builtin_va_arg(ap, char**);
    __builtin_va_end(ap);
    return execve(path, vec, envp);
}

pid_t vfork(void) { return fork(); }   /* copy-on-write fork is already cheap; semantics are a superset */

/* ---- posix_spawn ---------------------------------------------------------- */
int posix_spawn_file_actions_init(posix_spawn_file_actions_t* a) { a->n = 0; return 0; }
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* a) { a->n = 0; return 0; }
static int fa_add(posix_spawn_file_actions_t* a, int kind, int fd, int fd2, const char* path, int flags)
{
    if (a->n >= 8) return ENOMEM;
    int i = a->n++; a->kind[i] = kind; a->fd[i] = fd; a->fd2[i] = fd2; a->path[i] = path; a->flags[i] = flags;
    return 0;
}
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* a, int fd, int newfd) { return fa_add(a, 1, fd, newfd, 0, 0); }
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* a, int fd) { return fa_add(a, 2, fd, 0, 0, 0); }
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* a, int fd, const char* path, int flags, int mode) { (void)mode; return fa_add(a, 3, fd, 0, path, flags); }

static int spawn_common(pid_t* pid, const char* path, const posix_spawn_file_actions_t* act, char* const argv[], char* const envp[], int search)
{
    char full[160];
    if (search) { if (resolve_exec(path, full, sizeof(full)) != 0) return ENOENT; path = full; }
    else if (!file_is_there(path)) return ENOENT;
    pid_t c = fork();
    if (c < 0) return errno ? errno : ENOMEM;
    if (c == 0)
    {
        for (int i = 0; act && i < act->n; ++i)
        {
            if (act->kind[i] == 1) { if (act->fd[i] == act->fd2[i]) fcntl(act->fd[i], F_SETFD, 0); else if (dup2(act->fd[i], act->fd2[i]) < 0) _exit(127); }
            else if (act->kind[i] == 2) close(act->fd[i]);
            else if (act->kind[i] == 3)
            {
                int fd = open(act->path[i], act->flags[i]);
                if (fd < 0) _exit(127);
                if (fd != act->fd[i]) { dup2(fd, act->fd[i]); close(fd); }
            }
        }
        execve(path, argv, envp ? envp : environ);
        _exit(127);
    }
    if (pid) *pid = c;
    return 0;
}
int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* a, const void* attr, char* const argv[], char* const envp[])
{ (void)attr; return spawn_common(pid, path, a, argv, envp, 0); }
int posix_spawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* a, const void* attr, char* const argv[], char* const envp[])
{ (void)attr; return spawn_common(pid, file, a, argv, envp, 1); }

/* ---- system / popen: a small command interpreter ---------------------------
   Supports: words with '..' and ".." quotes, $VAR, NAME=value prefixes, | pipelines, < > >> redirection,
   ; && || sequencing, trailing &, and the builtins cd (no-op error), echo, exit, true, false, export, unset.
   Commands run as ELF programs from /bin (or a path). Kernel-shell built-ins such as ls are not ELF programs. */
struct Cmd { char* argv[34]; int argc; char* in; char* out; int append; };

static char* expand_word(const char* w, char* buf, int cap)
{
    int n = 0; char q = 0;
    for (const char* p = w; *p && n < cap - 1; ++p)
    {
        if (q) { if (*p == q) { q = 0; continue; } }
        else if (*p == '"' || *p == '\'') { q = *p; continue; }
        if (*p == '$' && q != '\'')
        {
            char name[48]; int k = 0; const char* v = p + 1;
            if (*v == '?') { name[k++] = '?'; ++v; } else while ((*v >= 'A' && *v <= 'Z') || (*v >= 'a' && *v <= 'z') || (*v >= '0' && *v <= '9') || *v == '_') { if (k < 47) name[k++] = *v; ++v; }
            name[k] = 0;
            if (k) { const char* val = getenv(name); if (val) while (*val && n < cap - 1) buf[n++] = *val++; p = v - 1; continue; }
        }
        buf[n++] = *p;
    }
    buf[n] = 0;
    return buf;
}

static int run_pipeline(Cmd* cmds, int n, int bg, int* last)
{
    int pids[8]; int prev = -1; int started = 0;
    for (int i = 0; i < n; ++i)
    {
        Cmd* c = &cmds[i];
        if (c->argc == 0) continue;
        int pf[2] = { -1, -1 };
        if (i < n - 1 && pipe(pf) != 0) { *last = 1; return -1; }
        pid_t pid = fork();
        if (pid == 0)
        {
            if (prev >= 0) { dup2(prev, 0); close(prev); }
            if (pf[1] >= 0) { dup2(pf[1], 1); close(pf[0]); close(pf[1]); }
            if (c->in) { int fd = open(c->in, O_RDONLY); if (fd < 0) { fputs("sh: cannot open input\n", stderr); _exit(1); } dup2(fd, 0); close(fd); }
            if (c->out) { int fd = open(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC)); if (fd < 0) { fputs("sh: cannot open output\n", stderr); _exit(1); } dup2(fd, 1); close(fd); }
            c->argv[c->argc] = 0;
            execvp(c->argv[0], c->argv);
            fputs("sh: command not found: ", stderr); fputs(c->argv[0], stderr); fputs("\n", stderr);
            _exit(127);
        }
        if (prev >= 0) close(prev);
        if (pf[1] >= 0) close(pf[1]);
        prev = pf[0];
        if (pid > 0 && started < 8) pids[started++] = pid;
    }
    if (prev >= 0) close(prev);
    if (bg) { *last = 0; return 0; }
    int st = 0;
    for (int i = 0; i < started; ++i) { int s2 = 0; waitpid(pids[i], &s2, 0); if (i == started - 1) st = s2; }
    *last = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return 0;
}

static int sh_status = 0;
static int sh_run(char* line, int* exited)
{
    static char wbuf[40][256];
    char* p = line; int last = sh_status; int nextop = 0;   /* 0 always, 1 &&, 2 || */
    while (*p)
    {
        Cmd cmds[8]; int nc = 0; int bg = 0; int wi = 0;
        memset(cmds, 0, sizeof(cmds));
        int endop = 0;
        for (;;)
        {
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p) break;
            if (*p == ';') { ++p; endop = 0; break; }
            if (*p == '&' && p[1] == '&') { p += 2; endop = 1; break; }
            if (*p == '|' && p[1] == '|') { p += 2; endop = 2; break; }
            if (*p == '&') { bg = 1; ++p; endop = 0; break; }
            if (*p == '|') { if (nc < 7) ++nc; ++p; continue; }
            int mode = 0;
            if (*p == '<') { mode = 1; ++p; }
            else if (*p == '>') { mode = 2; ++p; if (*p == '>') { mode = 3; ++p; } }
            while (*p == ' ') ++p;
            char* start = p; char q = 0;
            while (*p && (q || (*p != ' ' && *p != '\t' && *p != ';' && *p != '|' && *p != '&' && *p != '<' && *p != '>')))
            { if (q) { if (*p == q) q = 0; } else if (*p == '"' || *p == '\'') q = *p; ++p; }
            char save = *p; *p = 0;
            if (wi < 40)
            {
                expand_word(start, wbuf[wi], 256);
                char* w = wbuf[wi++];
                Cmd* c = &cmds[nc];
                if (mode == 1) c->in = w;
                else if (mode >= 2) { c->out = w; c->append = mode == 3; }
                else if (c->argc < 32) c->argv[c->argc++] = w;
            }
            *p = save;
        }
        int ncmds = nc + 1;
        int skip = (nextop == 1 && last != 0) || (nextop == 2 && last == 0);
        if (!skip && cmds[0].argc > 0)
        {
            Cmd* c = &cmds[0];
            const char* a = c->argv[0];
            if (ncmds == 1 && !strcmp(a, "exit")) { *exited = 1; return c->argc > 1 ? atoi(c->argv[1]) : last; }
            else if (ncmds == 1 && !strcmp(a, "true")) last = 0;
            else if (ncmds == 1 && !strcmp(a, "false")) last = 1;
            else if (ncmds == 1 && !strcmp(a, "echo") && !c->out && !c->in)
            { for (int i = 1; i < c->argc; ++i) { if (i > 1) fputc(' ', stdout); fputs(c->argv[i], stdout); } fputc('\n', stdout); fflush(stdout); last = 0; }
            else if (ncmds == 1 && !strcmp(a, "export")) { for (int i = 1; i < c->argc; ++i) putenv(c->argv[i]); last = 0; }
            else if (ncmds == 1 && !strcmp(a, "unset")) { for (int i = 1; i < c->argc; ++i) unsetenv(c->argv[i]); last = 0; }
            else if (ncmds == 1 && !strcmp(a, "cd")) { fputs("sh: cd is not available in a subprocess\n", stderr); last = 1; }
            else if (ncmds == 1 && strchr(a, '=') && a[0] != '=' ) { for (int i = 0; i < c->argc; ++i) if (strchr(c->argv[i], '=')) putenv(c->argv[i]); last = 0; }
            else { fflush(stdout); (void)run_pipeline(cmds, ncmds, bg, &last); }
        }
        nextop = endop;
        sh_status = last;
        if (!*p) break;
    }
    return last;
}

int system(const char* cmd)
{
    if (!cmd) return 1;
    char line[512];
    if (strlen(cmd) >= sizeof(line)) { errno = E2BIG; return -1; }
    strcpy(line, cmd);
    int exited = 0;
    int st = sh_run(line, &exited);
    return (st & 0xFF) << 8;           /* wait()-style status */
}

static pid_t popen_pid[8]; static FILE* popen_fp[8];
FILE* popen(const char* cmd, const char* mode)
{
    int rd = mode && mode[0] == 'r';
    if (!mode || (mode[0] != 'r' && mode[0] != 'w')) { errno = EINVAL; return 0; }
    int slot = -1; for (int i = 0; i < 8; ++i) if (!popen_fp[i]) { slot = i; break; }
    if (slot < 0) { errno = EMFILE; return 0; }
    int pf[2]; if (pipe(pf) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) { close(pf[0]); close(pf[1]); return 0; }
    if (pid == 0)
    {
        if (rd) { dup2(pf[1], 1); } else { dup2(pf[0], 0); }
        close(pf[0]); close(pf[1]);
        int exited = 0; char line[512];
        if (strlen(cmd) >= sizeof(line)) _exit(127);
        strcpy(line, cmd);
        int st = sh_run(line, &exited);
        fflush(stdout);
        _exit(st);
    }
    FILE* f;
    if (rd) { close(pf[1]); f = fdopen(pf[0], "r"); } else { close(pf[0]); f = fdopen(pf[1], "w"); }
    if (!f) { kill(pid, SIGKILL); return 0; }
    popen_pid[slot] = pid; popen_fp[slot] = f;
    return f;
}
int pclose(FILE* f)
{
    int slot = -1; for (int i = 0; i < 8; ++i) if (popen_fp[i] == f) { slot = i; break; }
    if (slot < 0) { errno = EINVAL; return -1; }
    pid_t pid = popen_pid[slot]; popen_fp[slot] = 0;
    fclose(f);
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return st;
}

} /* extern "C" */

/* ---- network helpers for pkg ------------------------------------------- */
/* HTTP(S) GET (certificate verified unless insecure): body length, or -1 with errno (ENOENT: non-2xx, EIO: transport/TLS, EMSGSIZE-like ERANGE: too big). */
long http_get(const char* url, void* buf, size_t cap, int insecure, const char* cacert)
{
    return rret(user_syscall5(RS_HTTP_GET, (uint64_t)url, (uint64_t)buf, cap, insecure ? 1U : 0U, (uint64_t)cacert));
}
void sha256_buf(const void* data, size_t len, unsigned char out[32]) { (void)user_syscall3(RS_SHA256, (uint64_t)data, len, (uint64_t)out); }
int ed25519_pubkey(const unsigned char seed[32], unsigned char pub[32]) { uint64_t a[4] = {(uint64_t)seed, 0, 0, (uint64_t)pub}; return (int)user_syscall2(RS_ED25519, 0, (uint64_t)a); }
int ed25519_sign(const unsigned char seed[32], const void* msg, size_t len, unsigned char sig[64]) { uint64_t a[4] = {(uint64_t)seed, (uint64_t)msg, len, (uint64_t)sig}; return (int)user_syscall2(RS_ED25519, 1, (uint64_t)a); }
int ed25519_verify(const unsigned char pub[32], const void* msg, size_t len, const unsigned char sig[64]) { uint64_t a[4] = {(uint64_t)pub, (uint64_t)msg, len, (uint64_t)sig}; return (int)user_syscall2(RS_ED25519, 2, (uint64_t)a); }
int rng_ready(void) { return (int)user_syscall2(RS_ED25519, 3, 0); }
