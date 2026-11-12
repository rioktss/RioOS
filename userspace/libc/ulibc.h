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

#ifndef RIOOS_ULIBC_H
#define RIOOS_ULIBC_H
/* RioOS user-space C library (freestanding, AArch64, no FP). */

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long size_t;
typedef long ssize_t;
typedef long intptr_t;
typedef unsigned long uintptr_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
typedef int pid_t;
typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, t) __builtin_va_arg(v, t)
#define NULL ((void*)0)
/* Soft-float ABI note: variadic doubles are passed in integer registers, so read them with va_arg_double(), never va_arg(ap, double). */
#define va_arg_double(v) __builtin_bit_cast(double, va_arg(v, unsigned long long))

int main(int argc, char** argv);   /* provided by the program */

/* errno */
extern int errno;
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#ifndef EFBIG
#define EFBIG 27
#endif
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EEXIST 17
#define EINVAL 22
#define EMFILE 24
#define EPIPE 32
#define ENOSYS 38
#ifndef E2BIG
#define E2BIG 7
#define ENOEXEC 8
#define ERANGE 34
#define ELOOP 40
#endif
#ifndef ENOTDIR
#define ENXIO 6
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EXDEV 18
#define ENOTTY 25
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EDOM 33
#define ENAMETOOLONG 36
#define ENOTEMPTY 39
#define EOVERFLOW 75
#define EILSEQ 84
#define ENOTSUP 95
#define EOPNOTSUPP 95
#endif

/* string.h */
void* memcpy(void* d, const void* s, size_t n);
void* memmove(void* d, const void* s, size_t n);
void* memset(void* d, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strcpy(char* d, const char* s);
char* strncpy(char* d, const char* s, size_t n);
char* strcat(char* d, const char* s);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* h, const char* n);
char* strdup(const char* s);

/* ctype */
int isdigit(int c); int isalpha(int c); int isalnum(int c); int isspace(int c);
int isupper(int c); int islower(int c); int toupper(int c); int tolower(int c);

/* stdlib */
void* malloc(size_t n);
void* calloc(size_t c, size_t n);
void* realloc(void* p, size_t n);
void free(void* p);
int atoi(const char* s);
long strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
int abs(int v);
void qsort(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*));
int rand(void);
void srand(unsigned seed);
void exit(int code) __attribute__((noreturn));
void _exit(int code) __attribute__((noreturn));
int atexit(void (*fn)(void));
void abort(void) __attribute__((noreturn));

/* stdio (unbuffered, descriptor based) */
int printf(const char* fmt, ...);
int dprintf(int fd, const char* fmt, ...);
int snprintf(char* out, size_t cap, const char* fmt, ...);
int sprintf(char* out, const char* fmt, ...);
int vsnprintf(char* out, size_t cap, const char* fmt, va_list ap);
int vdprintf(int fd, const char* fmt, va_list ap);
int puts(const char* s);
int putchar(int c);
int getchar(void);                 /* blocking read from fd 0 */
char* fgets_fd(int fd, char* buf, int cap);

/* unistd / POSIX-style process API */
#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_EXCL   0x2000
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define WNOHANG 1
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_FAILED ((void*)-1)
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MS_ASYNC 1
#define MS_SYNC 4
#define O_CLOEXEC 0x1000
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1

#define SIGHUP 1
#define SIGINT 2
#define SIGKILL 9
#define SIGUSR1 10
#define SIGQUIT 3
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGALRM 14
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20

#define WIFEXITED(s) (((s) & 0xFF) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s) (((s) & 0xFF) != 0)
#define WTERMSIG(s) ((s) & 0xFF)

typedef struct FILE FILE;
typedef void (*sighandler_t)(int);
typedef unsigned int sigset_t;
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000U
#define SA_SIGINFO 0x00000004
#define SA_ONSTACK 0x08000000
#define SI_USER 0
#define SI_QUEUE (-1)
#define SI_TIMER 0x80
#define SEGV_MAPERR 1
#define SIGILL 4
#define SIGBUS 7
#define SIGFPE 8
#define SS_DISABLE 2
#define MINSIGSTKSZ 2048
#define SIGSTKSZ 8192
union sigval { int sival_int; void* sival_ptr; };
typedef struct { int si_signo; int si_errno; int si_code; int si_pid; union sigval si_value; void* si_addr; } siginfo_t;
typedef struct { void* ss_sp; int ss_flags; size_t ss_size; } stack_t;
struct sigaction
{
    union { sighandler_t sa_handler; void (*sa_sigaction)(int, siginfo_t*, void*); };
    sigset_t sa_mask;
    unsigned int sa_flags;
};
typedef unsigned long long jmp_buf_word;
typedef jmp_buf_word jmp_buf[24];
typedef jmp_buf_word sigjmp_buf[26];
int setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
int sigsetjmp(sigjmp_buf env, int savemask) __attribute__((returns_twice));
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));
int sigaltstack(const stack_t* ss, stack_t* old);
int sigqueue(pid_t pid, int sig, union sigval value);
int sigwait(const sigset_t* set, int* sig);
int sigwaitinfo(const sigset_t* set, siginfo_t* info);
struct timeval { long tv_sec; long tv_usec; };
struct timespec { long tv_sec; long tv_nsec; };
struct itimerval { struct timeval it_interval; struct timeval it_value; };
#define ITIMER_REAL 0
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
int setitimer(int which, const struct itimerval* nv, struct itimerval* old);
int getitimer(int which, struct itimerval* cur);
long http_get(const char* url, void* buf, size_t cap, int insecure, const char* cacert);   /* HTTP(S) GET, TLS verified unless insecure; body length or -1 */
void sha256_buf(const void* data, size_t len, unsigned char out[32]);
int nanosleep(const struct timespec* req, struct timespec* rem);
int gettimeofday(struct timeval* tv, void* tz);
int clock_gettime(int clk, struct timespec* ts);
int sigtimedwait(const sigset_t* set, siginfo_t* info, const struct timespec* timeout);
const char* strsignal(int sig);
void psignal(int sig, const char* msg);
int sigaction(int sig, const struct sigaction* act, struct sigaction* old);
sighandler_t signal(int sig, sighandler_t handler);
int raise(int sig);
int sigemptyset(sigset_t* s); int sigfillset(sigset_t* s);
int sigaddset(sigset_t* s, int sig); int sigdelset(sigset_t* s, int sig); int sigismember(const sigset_t* s, int sig);
int sigprocmask(int how, const sigset_t* set, sigset_t* old);
int sigpending(sigset_t* set);
int sigsuspend(const sigset_t* mask);
unsigned alarm(unsigned seconds);
int pause(void);
/* process creation: POSIX fork/exec family (programs are ELF files; argv[0] is preserved) */
int execve(const char* path, char* const argv[], char* const envp[]);
int execv(const char* path, char* const argv[]);
int execvp(const char* file, char* const argv[]);
int execl(const char* path, const char* arg0, ...);
int execlp(const char* file, const char* arg0, ...);
int execle(const char* path, const char* arg0, ...);
pid_t vfork(void);
int system(const char* cmd);
FILE* popen(const char* cmd, const char* mode);
int pclose(FILE* f);
FILE* fdopen(int fd, const char* mode);
struct posix_spawn_file_actions { int n; int kind[8]; int fd[8]; int fd2[8]; const char* path[8]; int flags[8]; };
typedef struct posix_spawn_file_actions posix_spawn_file_actions_t;
int posix_spawn_file_actions_init(posix_spawn_file_actions_t* a);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* a, int fd, int newfd);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* a, int fd);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* a, int fd, const char* path, int flags, int mode);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* a);
int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* actions, const void* attr, char* const argv[], char* const envp[]);
int posix_spawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* actions, const void* attr, char* const argv[], char* const envp[]);
int fcntl(int fd, int cmd, ...);
int msync(void* addr, size_t len, int flags);
pid_t getpgrp(void); pid_t getpgid(pid_t pid); pid_t setsid(void); int setpgid(pid_t pid, pid_t pgid);
#define WIFSTOPPED(s) (0)
pid_t fork(void);
int execv_args(const char* path, const char* args);   /* RioOS exec: single argument string */
pid_t waitpid(pid_t pid, int* status, int options);
pid_t wait(int* status);
pid_t getpid(void);
pid_t getppid(void);
int kill(pid_t pid, int sig);
unsigned getuid(void);
unsigned getgid(void);
int setuid(unsigned uid, unsigned gid);
pid_t spawn(const char* path, const char* args, int background);
pid_t spawn_fd(const char* path, const char* args, int in_fd, int out_fd);
void sched_yield(void);
unsigned sleep_ms(unsigned ms);
unsigned long clock_ms(void);
int open(const char* path, int flags);
int close(int fd);
ssize_t read(int fd, void* buf, size_t n);
ssize_t write(int fd, const void* buf, size_t n);
long lseek(int fd, long off, int whence);
int dup(int fd);
int dup2(int a, int b);
int pipe(int fds[2]);
void* sbrk(long inc);
int brk(void* addr);
void* mmap(void* addr, size_t len, int prot, int flags, int fd, long off);
int munmap(void* addr, size_t len);
int mprotect(void* addr, size_t len, int prot);
const char* getargs(void);
/* filesystem (RioOS syscalls 55-59) */
struct RioStat { uint32_t inode, type, mode, uid, gid, sector_count; uint64_t size, mtime; };   /* type: 0 file 1 dir 2 symlink */
int mkdir(const char* path, int mode);
int unlink(const char* path);
int rmdir(const char* path);                 /* empty directories only */
int rename(const char* from, const char* to);
long readdir_text(const char* path, char* buf, size_t cap);   /* lines "<f|d|l> <size> <name>\n"; bytes or -1 */
long time_epoch(void);                       /* seconds since 1970 (RTC, UTC) */
int stat_path(const char* path, struct RioStat* st);          /* follows symlinks */
int lstat_path(const char* path, struct RioStat* st);         /* does not follow a final symlink (type 2) */
int symlink(const char* target, const char* linkpath);
int link(const char* target, const char* linkpath);           /* hard link */
long readlink(const char* path, char* buf, size_t cap);        /* length, not NUL terminated; -1 on error */
char* realpath(const char* path, char* out);                   /* out: 256 bytes or NULL for a static buffer */
int chmod(const char* path, unsigned mode);

/* dynamic loading (ET_DYN shared objects) */
void* dlopen(const char* path, int flags);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);
const char* dlerror(void);


/* ---- libc part 2 (ulibc2.cpp): buffered stdio ---- */
typedef struct FILE FILE;
extern FILE* stdin; extern FILE* stdout; extern FILE* stderr;
#define EOF (-1)
FILE* fopen(const char* path, const char* mode);   /* r w a r+ w+ a+ */
int fclose(FILE* f);
int fflush(FILE* f);                                /* NULL flushes all */
int fgetc(FILE* f); int getc(FILE* f); int ungetc(int c, FILE* f);
char* fgets(char* s, int cap, FILE* f);
size_t fread(void* buf, size_t sz, size_t n, FILE* f);
size_t fwrite(const void* buf, size_t sz, size_t n, FILE* f);
int fputc(int c, FILE* f); int putc(int c, FILE* f); int fputs(const char* s, FILE* f);
int fprintf(FILE* f, const char* fmt, ...);
int vfprintf(FILE* f, const char* fmt, va_list ap);
int fseek(FILE* f, long off, int whence); long ftell(FILE* f); void rewind(FILE* f);
int feof(FILE* f); int ferror(FILE* f); void clearerr(FILE* f); int fileno(FILE* f);
void perror(const char* s);
int sscanf(const char* s, const char* fmt, ...);
int vsscanf(const char* s, const char* fmt, va_list ap);

/* string / stdlib extras */
size_t strnlen(const char* s, size_t n); char* strncat(char* d, const char* s, size_t n);
void* memchr(const void* s, int c, size_t n);
int strcasecmp(const char* a, const char* b); int strncasecmp(const char* a, const char* b, size_t n);
size_t strspn(const char* s, const char* set); size_t strcspn(const char* s, const char* set); char* strpbrk(const char* s, const char* set);
char* strtok(char* s, const char* delim); char* strtok_r(char* s, const char* delim, char** save);
const char* strerror(int e);
long atol(const char* s); long long atoll(const char* s);
long long strtoll(const char* s, char** end, int base); unsigned long long strtoull(const char* s, char** end, int base);
long labs(long v); long long llabs(long long v);
void* bsearch(const void* key, const void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*));
int isxdigit(int c); int isprint(int c); int ispunct(int c);

/* integer math (no FPU in the kernel/libc) */
unsigned long isqrt_u(unsigned long v); long gcd_l(long a, long b); long lcm_l(long a, long b);
long ipow_l(long base, unsigned exp); long clamp_l(long v, long lo, long hi);

/* ---- libc part 3 (ulibc3.cpp, ulibm.cpp, softfloat.cpp): float, scanf, locale, time, env ---- */
int fscanf(FILE* f, const char* fmt, ...); int scanf(const char* fmt, ...);
int vfscanf(FILE* f, const char* fmt, va_list ap); int vscanf(const char* fmt, va_list ap);
ssize_t getline(char** lineptr, size_t* n, FILE* f); ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* f);
double strtod(const char* s, char** end); float strtof(const char* s, char** end); double atof(const char* s);
typedef struct { int quot, rem; } div_t; typedef struct { long quot, rem; } ldiv_t;
div_t div(int a, int b); ldiv_t ldiv(long a, long b);
int isgraph(int c); int iscntrl(int c); int isblank(int c); int isascii(int c); int toascii(int c);
void* memmem(const void* h, size_t hl, const void* n, size_t nl); char* strndup(const char* s, size_t n); char* strcasestr(const char* h, const char* n);
char* strsep(char** sp, const char* delim); size_t strlcpy(char* d, const char* s, size_t n); size_t strlcat(char* d, const char* s, size_t n);
void* mempcpy(void* d, const void* s, size_t n); unsigned usleep(unsigned us); unsigned sleep(unsigned s);
void* aligned_alloc(size_t align, size_t size); int posix_memalign(void** out, size_t align, size_t size); void* memalign(size_t align, size_t size);
size_t malloc_usable_size(void* p);
/* locale */
#define LC_CTYPE 0
#define LC_NUMERIC 1
#define LC_TIME 2
#define LC_COLLATE 3
#define LC_MONETARY 4
#define LC_MESSAGES 5
#define LC_ALL 6
struct lconv { char* decimal_point; char* thousands_sep; char* grouping; char* int_curr_symbol; char* currency_symbol; char* mon_decimal_point; char* mon_thousands_sep; char* mon_grouping; char* positive_sign; char* negative_sign;
               char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn; };
char* setlocale(int category, const char* locale); struct lconv* localeconv(void);
/* environment (process-local) */
extern char** environ;
char* getenv(const char* name); int setenv(const char* name, const char* value, int overwrite); int unsetenv(const char* name); int putenv(char* s);
/* time (UTC only) */
typedef long time_t; typedef long clock_t;
#define CLOCKS_PER_SEC 1000000L
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
time_t time(time_t* t); clock_t clock(void); double difftime(time_t a, time_t b);
struct tm* gmtime(const time_t* t); struct tm* gmtime_r(const time_t* t, struct tm* r); struct tm* localtime(const time_t* t); struct tm* localtime_r(const time_t* t, struct tm* r);
time_t mktime(struct tm* tm); size_t strftime(char* out, size_t cap, const char* fmt, const struct tm* tm);
char* asctime_r(const struct tm* tm, char* buf); char* ctime_r(const time_t* t, char* buf);
/* math (IEEE double, soft-float; also compiled float/double arithmetic) */
#define M_PI 3.14159265358979323846
#define M_E 2.71828182845904523536
#define M_SQRT2 1.41421356237309504880
#define HUGE_VAL (1.0 / 0.0)
#define INFINITY ((float)(1.0 / 0.0))
#define NAN ((float)(0.0 / 0.0))
double fabs(double); double copysign(double, double); double ldexp(double, int); double frexp(double, int*); double trunc(double); double floor(double); double ceil(double); double round(double); double rint(double);
double modf(double, double*); double fmod(double, double); double fmin(double, double); double fmax(double, double);
double exp(double); double log(double); double log2(double); double log10(double); double log1p(double); double expm1(double); double pow(double, double);
double sqrt(double); double cbrt(double); double hypot(double, double);
double sin(double); double cos(double); double tan(double); double atan(double); double atan2(double, double); double asin(double); double acos(double);
double sinh(double); double cosh(double); double tanh(double);
float sqrtf(float); float fabsf(float); float floorf(float); float ceilf(float); float sinf(float); float cosf(float); float tanf(float); float atanf(float); float atan2f(float, float);
float expf(float); float logf(float); float powf(float, float); float fmodf(float, float);
int isnan_d(double); int isinf_d(double); int isfinite_d(double); int signbit_d(double);
#define isnan(x) isnan_d((double)(x))
#define isinf(x) isinf_d((double)(x))
#define isfinite(x) isfinite_d((double)(x))
/* deflate / gzip / crc32 (deflate.cpp). Output buffers are malloc'ed; free() them. */
uint32_t rio_crc32(uint32_t crc, const uint8_t* p, unsigned long n);
int rio_deflate(const uint8_t* in, unsigned long inlen, uint8_t** out, unsigned long* outlen, int level);
int rio_inflate(const uint8_t* in, unsigned long inlen, uint8_t** out, unsigned long* outlen, unsigned long* consumed);
int rio_gzip_compress(const uint8_t* in, unsigned long inlen, const char* name, uint32_t mtime, uint8_t** out, unsigned long* outlen, int level);
int rio_gzip_decompress(const uint8_t* in, unsigned long inlen, uint8_t** out, unsigned long* outlen, char* name_out, int name_cap);   /* 0 ok, -1 not gzip, -2 corrupt, -3 crc */
int rio_fmt_double(char* out, int cap, double v, char conv, int prec, int plus, int space, int alt);


/* ---- POSIX threads: user-level (M:1) threads inside one process ----------------------------
   Threads share the address space and descriptors and are scheduled cooperatively: a switch happens at
   pthread_yield/sched_yield, sleep/usleep/nanosleep, pthread_join, mutex/cond/rwlock/barrier/semaphore waits
   and thread exit. A thread blocked in a kernel call (read on a pipe, waitpid) blocks the whole process. */
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#define EBUSY 16
#define EDEADLK 35
#endif
typedef unsigned long pthread_t;
typedef struct { int detachstate; unsigned long stacksize; } pthread_attr_t;
typedef struct { int type; int lock; int owner; int count; } pthread_mutex_t;
typedef struct { int type; } pthread_mutexattr_t;
typedef struct { int dummy; } pthread_condattr_t;
typedef struct { int seq; } pthread_cond_t;
typedef struct { int readers; int writer; int waiting_writers; } pthread_rwlock_t;
typedef struct { unsigned count; unsigned waiting; unsigned phase; } pthread_barrier_t;
typedef int pthread_spinlock_t;
typedef int pthread_once_t;
typedef unsigned pthread_key_t;
typedef struct { unsigned value; unsigned shared; } sem_t;
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT 0
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_CANCELED ((void*)-1)
#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1
#define PTHREAD_CANCEL_DEFERRED 0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
#define PTHREAD_MUTEX_INITIALIZER { 0, 0, 0, 0 }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER { 1, 0, 0, 0 }
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER { 2, 0, 0, 0 }
#define PTHREAD_COND_INITIALIZER { 0 }
#define PTHREAD_RWLOCK_INITIALIZER { 0, 0, 0 }
#define PTHREAD_ONCE_INIT 0
int pthread_create(pthread_t* t, const pthread_attr_t* attr, void* (*fn)(void*), void* arg);
int pthread_join(pthread_t t, void** ret);
int pthread_detach(pthread_t t);
void pthread_exit(void* ret) __attribute__((noreturn));
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_yield(void);
int pthread_cancel(pthread_t t);
int pthread_setcancelstate(int state, int* old);
int pthread_setcanceltype(int type, int* old);
void pthread_testcancel(void);
int pthread_kill(pthread_t t, int sig);
int pthread_sigmask(int how, const sigset_t* set, sigset_t* old);
int pthread_once(pthread_once_t* once, void (*fn)(void));
int pthread_attr_init(pthread_attr_t* a); int pthread_attr_destroy(pthread_attr_t* a);
int pthread_attr_setdetachstate(pthread_attr_t* a, int s); int pthread_attr_getdetachstate(const pthread_attr_t* a, int* s);
int pthread_attr_setstacksize(pthread_attr_t* a, unsigned long sz); int pthread_attr_getstacksize(const pthread_attr_t* a, unsigned long* sz);
int pthread_mutexattr_init(pthread_mutexattr_t* a); int pthread_mutexattr_destroy(pthread_mutexattr_t* a);
int pthread_mutexattr_settype(pthread_mutexattr_t* a, int t); int pthread_mutexattr_gettype(const pthread_mutexattr_t* a, int* t);
int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a); int pthread_mutex_destroy(pthread_mutex_t* m);
int pthread_mutex_lock(pthread_mutex_t* m); int pthread_mutex_trylock(pthread_mutex_t* m); int pthread_mutex_unlock(pthread_mutex_t* m);
int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* abs);
int pthread_condattr_init(pthread_condattr_t* a); int pthread_condattr_destroy(pthread_condattr_t* a);
int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* a); int pthread_cond_destroy(pthread_cond_t* c);
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m);
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* abs);
int pthread_cond_signal(pthread_cond_t* c); int pthread_cond_broadcast(pthread_cond_t* c);
int pthread_rwlock_init(pthread_rwlock_t* l, const void* a); int pthread_rwlock_destroy(pthread_rwlock_t* l);
int pthread_rwlock_rdlock(pthread_rwlock_t* l); int pthread_rwlock_wrlock(pthread_rwlock_t* l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t* l); int pthread_rwlock_trywrlock(pthread_rwlock_t* l); int pthread_rwlock_unlock(pthread_rwlock_t* l);
int pthread_barrier_init(pthread_barrier_t* b, const void* a, unsigned count); int pthread_barrier_destroy(pthread_barrier_t* b);
int pthread_barrier_wait(pthread_barrier_t* b);
int pthread_spin_init(pthread_spinlock_t* s, int shared); int pthread_spin_destroy(pthread_spinlock_t* s);
int pthread_spin_lock(pthread_spinlock_t* s); int pthread_spin_trylock(pthread_spinlock_t* s); int pthread_spin_unlock(pthread_spinlock_t* s);
int pthread_key_create(pthread_key_t* k, void (*destructor)(void*)); int pthread_key_delete(pthread_key_t k);
void* pthread_getspecific(pthread_key_t k); int pthread_setspecific(pthread_key_t k, const void* v);
int sem_init(sem_t* s, int pshared, unsigned value); int sem_destroy(sem_t* s);
int sem_wait(sem_t* s); int sem_trywait(sem_t* s); int sem_post(sem_t* s); int sem_getvalue(sem_t* s, int* v);
int sem_timedwait(sem_t* s, const struct timespec* abs);
int pthread_setname_np(pthread_t t, const char* name);

/* ===== sockets, poll/select, IPC (descriptor based; see docs/IPC_POSIX.md) ===== */
#ifndef ENOTSOCK
#define ENOTSOCK 88
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define EPROTOTYPE 91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETUNREACH 101
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EALREADY 114
#define EINPROGRESS 115
#define EWOULDBLOCK EAGAIN
#endif
#define O_NONBLOCK 0x800
#define F_GETFL 3
#define F_SETFL 4
#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_LOCAL 1
#define AF_INET 2
#define PF_UNIX AF_UNIX
#define PF_LOCAL AF_UNIX
#define PF_INET AF_INET
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x1000
#define SOL_SOCKET 1
#define SO_DEBUG 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_LINGER 13
#define SO_REUSEPORT 15
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define IPPROTO_IP 0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define TCP_NODELAY 1
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define SOMAXCONN 8
#define MSG_PEEK 2
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define MSG_WAITALL 0x100
#define MSG_CTRUNC 8
#define SCM_RIGHTS 1
#define INADDR_ANY 0U
#define INADDR_LOOPBACK 0x7F000001U
#define INADDR_NONE 0xFFFFFFFFU
#define INET_ADDRSTRLEN 16
typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in { sa_family_t sin_family; in_port_t sin_port; struct in_addr sin_addr; unsigned char sin_zero[8]; };
struct sockaddr_un { sa_family_t sun_family; char sun_path[108]; };
struct sockaddr_storage { sa_family_t ss_family; char __pad[126]; };
struct iovec { void* iov_base; size_t iov_len; };
struct msghdr { void* msg_name; socklen_t msg_namelen; struct iovec* msg_iov; size_t msg_iovlen; void* msg_control; size_t msg_controllen; int msg_flags; };
struct cmsghdr { size_t cmsg_len; int cmsg_level; int cmsg_type; };
#define CMSG_ALIGN(l) (((l) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_LEN(l) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (l))
#define CMSG_SPACE(l) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(l))
#define CMSG_DATA(c) ((unsigned char*)(c) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(m) ((m)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr*)(m)->msg_control : (struct cmsghdr*)0)
struct cmsghdr* CMSG_NXTHDR(struct msghdr* m, struct cmsghdr* c);
struct linger { int l_onoff; int l_linger; };
struct addrinfo { int ai_flags; int ai_family; int ai_socktype; int ai_protocol; socklen_t ai_addrlen; struct sockaddr* ai_addr; char* ai_canonname; struct addrinfo* ai_next; };
struct hostent { char* h_name; char** h_aliases; int h_addrtype; int h_length; char** h_addr_list; };
#define h_addr h_addr_list[0]
#define AI_PASSIVE 1
#define AI_CANONNAME 2
#define AI_NUMERICHOST 4
#define AI_NUMERICSERV 0x400
#define EAI_NONAME -2
#define EAI_AGAIN -3
#define EAI_FAIL -4
#define EAI_FAMILY -6
#define EAI_SOCKTYPE -7
#define EAI_SERVICE -8
#define EAI_MEMORY -10
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
uint16_t htons(uint16_t v); uint16_t ntohs(uint16_t v); uint32_t htonl(uint32_t v); uint32_t ntohl(uint32_t v);
in_addr_t inet_addr(const char* s); int inet_aton(const char* s, struct in_addr* out); char* inet_ntoa(struct in_addr a);
int inet_pton(int af, const char* src, void* dst); const char* inet_ntop(int af, const void* src, char* dst, socklen_t size);
int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int sv[2]);
int bind(int fd, const struct sockaddr* addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr* addr, socklen_t* len);
int accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags);
int connect(int fd, const struct sockaddr* addr, socklen_t len);
ssize_t send(int fd, const void* buf, size_t n, int flags);
ssize_t recv(int fd, void* buf, size_t n, int flags);
ssize_t sendto(int fd, const void* buf, size_t n, int flags, const struct sockaddr* addr, socklen_t len);
ssize_t recvfrom(int fd, void* buf, size_t n, int flags, struct sockaddr* addr, socklen_t* len);
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags);
ssize_t recvmsg(int fd, struct msghdr* msg, int flags);
int shutdown(int fd, int how);
int getsockname(int fd, struct sockaddr* addr, socklen_t* len);
int getpeername(int fd, struct sockaddr* addr, socklen_t* len);
int setsockopt(int fd, int level, int name, const void* val, socklen_t len);
int getsockopt(int fd, int level, int name, void* val, socklen_t* len);
int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res);
void freeaddrinfo(struct addrinfo* ai);
const char* gai_strerror(int code);
struct hostent* gethostbyname(const char* name);
int getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen, char* serv, socklen_t servlen, int flags);
int pipe2(int fds[2], int flags);
ssize_t readv(int fd, const struct iovec* iov, int cnt);
ssize_t writev(int fd, const struct iovec* iov, int cnt);
/* poll / select */
#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLWRNORM 0x100
struct pollfd { int fd; short events; short revents; };
typedef unsigned long nfds_t;
int poll(struct pollfd* fds, nfds_t n, int timeout_ms);
int ppoll(struct pollfd* fds, nfds_t n, const struct timespec* timeout, const void* sigmask);
#define FD_SETSIZE 64
typedef struct { unsigned long fds_bits[1]; } fd_set;
#define FD_ZERO(s) ((s)->fds_bits[0] = 0UL)
#define FD_SET(f, s) ((s)->fds_bits[0] |= (1UL << (f)))
#define FD_CLR(f, s) ((s)->fds_bits[0] &= ~(1UL << (f)))
#define FD_ISSET(f, s) (((s)->fds_bits[0] >> (f)) & 1UL)
int select(int nfds, fd_set* r, fd_set* w, fd_set* e, struct timeval* tv);
int pselect(int nfds, fd_set* r, fd_set* w, fd_set* e, const struct timespec* ts, const void* sigmask);
/* eventfd */
#define EFD_SEMAPHORE 1
#define EFD_NONBLOCK O_NONBLOCK
#define EFD_CLOEXEC O_CLOEXEC
typedef uint64_t eventfd_t;
int eventfd(unsigned initval, int flags);
int eventfd_read(int fd, eventfd_t* value);
int eventfd_write(int fd, eventfd_t value);
extern int __rio_ep_active; void __rio_epoll_rearm(int fd);   /* internal: epoll edge re-arm on I/O */
void __rio_sockopt_close(int fd);   /* internal: forget per-descriptor socket timeouts */


/* ===== POSIX layer (ulibc7.cpp / ulibc8.cpp): stat, dirent, termios, pwd/grp, epoll, timerfd, regex, getopt, ... ===== */
typedef unsigned int uid_t; typedef unsigned int gid_t; typedef unsigned int mode_t; typedef long off_t;
typedef unsigned long dev_t; typedef unsigned long ino_t; typedef unsigned int nlink_t; typedef long blksize_t; typedef long blkcnt_t;
typedef int wint_t;
#define S_IFMT 0170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 070
#define S_IRGRP 040
#define S_IWGRP 020
#define S_IXGRP 010
#define S_IRWXO 07
#define S_IROTH 04
#define S_IWOTH 02
#define S_IXOTH 01
struct stat { dev_t st_dev; ino_t st_ino; mode_t st_mode; nlink_t st_nlink; uid_t st_uid; gid_t st_gid; dev_t st_rdev; off_t st_size;
              blksize_t st_blksize; blkcnt_t st_blocks; struct timespec st_atim, st_mtim, st_ctim; };
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
int stat(const char* path, struct stat* st); int lstat(const char* path, struct stat* st); int fstat(int fd, struct stat* st);
int fchmod(int fd, mode_t mode); int chown(const char* path, uid_t u, gid_t g); int lchown(const char* path, uid_t u, gid_t g); int fchown(int fd, uid_t u, gid_t g);
mode_t umask(mode_t mask);
struct utimbuf { time_t actime, modtime; };
int utime(const char* path, const struct utimbuf* t); int utimes(const char* path, const struct timeval tv[2]);
#define UTIME_NOW ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)
#define AT_FDCWD (-100)
int utimensat(int dirfd, const char* path, const struct timespec ts[2], int flags); int futimens(int fd, const struct timespec ts[2]);
int truncate(const char* path, off_t len); int ftruncate(int fd, off_t len);
int fsync(int fd); int fdatasync(int fd); void sync(void);
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int access(const char* path, int mode);
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
int flock(int fd, int op);
char* getcwd(char* buf, size_t size); int chdir(const char* path);
int dup3(int oldfd, int newfd, int flags);
int getpagesize(void); int getdtablesize(void);
/* dirent */
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
struct dirent { ino_t d_ino; unsigned char d_type; char d_name[64]; };
typedef struct RioDir DIR;
DIR* opendir(const char* path); struct dirent* readdir(DIR* d); int closedir(DIR* d); void rewinddir(DIR* d); int dirfd(DIR* d);
int scandir(const char* path, struct dirent*** list, int (*filter)(const struct dirent*), int (*cmp)(const struct dirent**, const struct dirent**));
int alphasort(const struct dirent** a, const struct dirent** b);
#define FNM_NOMATCH 1
#define FNM_PATHNAME 1
#define FNM_NOESCAPE 2
#define FNM_PERIOD 4
#define FNM_CASEFOLD 16
int fnmatch(const char* pattern, const char* string, int flags);
typedef struct { size_t gl_pathc; char** gl_pathv; size_t gl_offs; } glob_t;
#define GLOB_ERR 1
#define GLOB_MARK 2
#define GLOB_NOSORT 4
#define GLOB_NOCHECK 16
#define GLOB_NOMATCH (-3)
#define GLOB_NOSPACE (-1)
#define GLOB_ABORTED (-2)
int glob(const char* pattern, int flags, int (*errfn)(const char*, int), glob_t* g); void globfree(glob_t* g);
int mkstemp(char* tmpl); char* mkdtemp(char* tmpl);
char* basename(char* path); char* dirname(char* path);
/* termios / tty */
#define NCCS 19
typedef unsigned int tcflag_t; typedef unsigned char cc_t; typedef unsigned int speed_t;
struct termios { tcflag_t c_iflag, c_oflag, c_cflag, c_lflag; cc_t c_line; cc_t c_cc[NCCS]; unsigned char __pad[4]; speed_t c_ispeed, c_ospeed; };
#define ISIG 1
#define ICANON 2
#define ECHO 8
#define ECHOE 0x10
#define ECHOK 0x20
#define ICRNL 0x100
#define INLCR 0x40
#define IXON 0x400
#define OPOST 1
#define ONLCR 4
#define CS8 0x30
#define CREAD 0x80
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define TCIFLUSH 0
#define TCIOFLUSH 2
#define B9600 13
#define B38400 15
#define B115200 4098
int tcgetattr(int fd, struct termios* t); int tcsetattr(int fd, int action, const struct termios* t);
void cfmakeraw(struct termios* t); speed_t cfgetispeed(const struct termios* t); speed_t cfgetospeed(const struct termios* t);
int cfsetispeed(struct termios* t, speed_t s); int cfsetospeed(struct termios* t, speed_t s);
int tcflush(int fd, int q); int tcdrain(int fd); int tcflow(int fd, int a); int isatty(int fd); char* ttyname(int fd);
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#define TIOCGWINSZ 0x5413
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define FIONBIO 0x5421
int ioctl(int fd, unsigned long req, ...);
/* users and groups */
struct passwd { char* pw_name; char* pw_passwd; uid_t pw_uid; gid_t pw_gid; char* pw_gecos; char* pw_dir; char* pw_shell; };
struct group { char* gr_name; char* gr_passwd; gid_t gr_gid; char** gr_mem; };
struct passwd* getpwnam(const char* n); struct passwd* getpwuid(uid_t u);
int getpwnam_r(const char* n, struct passwd* pw, char* buf, size_t cap, struct passwd** res);
int getpwuid_r(uid_t u, struct passwd* pw, char* buf, size_t cap, struct passwd** res);
struct passwd* getpwent(void); void setpwent(void); void endpwent(void);
struct group* getgrnam(const char* n); struct group* getgrgid(gid_t g);
struct group* getgrent(void); void setgrent(void); void endgrent(void);
char* getlogin(void); int getgroups(int size, gid_t list[]);
/* system information and limits */
struct utsname { char sysname[32], nodename[64], release[32], version[32], machine[32]; };
int uname(struct utsname* u); int gethostname(char* name, size_t len); int sethostname(const char* name, size_t len);
typedef unsigned long rlim_t;
struct rlimit { rlim_t rlim_cur, rlim_max; };
struct rusage { struct timeval ru_utime, ru_stime; long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap, ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw; };
#define RLIM_INFINITY (~0UL)
#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_RSS 5
#define RLIMIT_NPROC 6
#define RLIMIT_NOFILE 7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS 9
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)
int getrlimit(int res, struct rlimit* r); int setrlimit(int res, const struct rlimit* r); int getrusage(int who, struct rusage* ru);
#define _SC_ARG_MAX 0
#define _SC_CLK_TCK 2
#define _SC_OPEN_MAX 4
#define _SC_PAGESIZE 30
#define _SC_PHYS_PAGES 85
#define _SC_AVPHYS_PAGES 86
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_LINE_MAX 43
#define _SC_HOST_NAME_MAX 180
#define _SC_NGROUPS_MAX 3
#define _PC_NAME_MAX 3
#define _PC_PATH_MAX 4
#define _PC_PIPE_BUF 5
long sysconf(int name); long pathconf(const char* path, int name); long fpathconf(int fd, int name);
#define PATH_MAX 128
#define NAME_MAX 63
#define PIPE_BUF 4096
pid_t wait4(pid_t pid, int* status, int options, struct rusage* ru); pid_t wait3(int* status, int options, struct rusage* ru);
void _Exit(int code) __attribute__((noreturn)); int on_exit(void (*fn)(int, void*), void* arg);
int mlock(const void* a, size_t n); int munlock(const void* a, size_t n); int mlockall(int f); int munlockall(void);
#define MADV_NORMAL 0
#define MADV_DONTNEED 4
int madvise(void* a, size_t n, int advice); int posix_madvise(void* a, size_t n, int advice);
ssize_t getrandom(void* buf, size_t n, unsigned flags); int getentropy(void* buf, size_t n);
uint32_t arc4random(void); void arc4random_buf(void* buf, size_t n); uint32_t arc4random_uniform(uint32_t bound);
long random(void); void srandom(unsigned seed); int rand_r(unsigned* seed);
double drand48(void); long lrand48(void); long mrand48(void); void srand48(long seed); double erand48(unsigned short x[3]);
/* futex / process-shared semaphores / shared memory */
int futex_wait(volatile uint32_t* addr, uint32_t expected, long timeout_ms);   /* 0 woken, -1 EAGAIN/ETIMEDOUT/EINTR */
int futex_wake(volatile uint32_t* addr, int n);
int shm_open(const char* name, int oflag, mode_t mode); int shm_unlink(const char* name);
typedef struct { unsigned long bits[1]; } cpu_set_t;
#define CPU_ZERO(s) ((s)->bits[0] = 0UL)
#define CPU_SET(c, s) ((s)->bits[0] |= (1UL << (c)))
#define CPU_ISSET(c, s) (((s)->bits[0] >> (c)) & 1UL)
#define CPU_COUNT(s) __builtin_popcountl((s)->bits[0])
int sched_getcpu(void);
int sched_getaffinity(pid_t pid, size_t size, cpu_set_t* set);   /* mask of online CPUs; pinning (sched_setaffinity) is not supported */
#define SEM_FAILED ((sem_t*)0)
sem_t* sem_open(const char* name, int oflag, ...); int sem_close(sem_t* s); int sem_unlink(const char* name);
/* epoll (level- and edge-triggered over poll) */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLLIN 0x001
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010
#define EPOLLRDHUP 0x2000
#define EPOLLONESHOT (1u << 30)
#define EPOLLET (1u << 31)
#define EPOLL_CLOEXEC O_CLOEXEC
typedef union epoll_data { void* ptr; int fd; uint32_t u32; uint64_t u64; } epoll_data_t;
struct epoll_event { uint32_t events; epoll_data_t data; };
int epoll_create(int size); int epoll_create1(int flags); int epoll_ctl(int ep, int op, int fd, struct epoll_event* ev);
int epoll_wait(int ep, struct epoll_event* evs, int maxevents, int timeout_ms);
/* timerfd */
#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC O_CLOEXEC
#define TFD_TIMER_ABSTIME 1
struct itimerspec { struct timespec it_interval, it_value; };
int timerfd_create(int clockid, int flags); int timerfd_settime(int fd, int flags, const struct itimerspec* nv, struct itimerspec* old);
int timerfd_gettime(int fd, struct itimerspec* cur);
int clock_nanosleep(int clk, int flags, const struct timespec* req, struct timespec* rem);
#define TIMER_ABSTIME 1
/* getopt */
extern char* optarg; extern int optind, opterr, optopt;
int getopt(int argc, char* const argv[], const char* optstring);
struct option { const char* name; int has_arg; int* flag; int val; };
#define no_argument 0
#define required_argument 1
#define optional_argument 2
int getopt_long(int argc, char* const argv[], const char* optstring, const struct option* longopts, int* longindex);
int getopt_long_only(int argc, char* const argv[], const char* optstring, const struct option* longopts, int* longindex);
/* regex (POSIX BRE/ERE, leftmost-longest, back-references) */
typedef struct { void* __prog; size_t re_nsub; } regex_t;
typedef int regoff_t;
typedef struct { regoff_t rm_so, rm_eo; } regmatch_t;
#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NEWLINE 4
#define REG_NOSUB 8
#define REG_NOTBOL 1
#define REG_NOTEOL 2
#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ECOLLATE 3
#define REG_ECTYPE 4
#define REG_EESCAPE 5
#define REG_ESUBREG 6
#define REG_EBRACK 7
#define REG_EPAREN 8
#define REG_EBRACE 9
#define REG_BADBR 10
#define REG_ERANGE 11
#define REG_ESPACE 12
#define REG_BADRPT 13
int regcomp(regex_t* re, const char* pattern, int cflags); int regexec(const regex_t* re, const char* s, size_t nmatch, regmatch_t pm[], int eflags);
size_t regerror(int code, const regex_t* re, char* buf, size_t cap); void regfree(regex_t* re);
/* misc string / wide / locale helpers */
char* stpcpy(char* d, const char* s); char* stpncpy(char* d, const char* s, size_t n);
int strverscmp(const char* a, const char* b); int strcoll(const char* a, const char* b); size_t strxfrm(char* d, const char* s, size_t n);
int strerror_r(int err, char* buf, size_t cap);
#define CODESET 14
char* nl_langinfo(int item);
size_t wcslen(const wchar_t* s); wchar_t* wcscpy(wchar_t* d, const wchar_t* s); wchar_t* wcsncpy(wchar_t* d, const wchar_t* s, size_t n);
int wcscmp(const wchar_t* a, const wchar_t* b); wchar_t* wcscat(wchar_t* d, const wchar_t* s); wchar_t* wcschr(const wchar_t* s, wchar_t c);
int mblen(const char* s, size_t n); int mbtowc(wchar_t* w, const char* s, size_t n); int wctomb(char* s, wchar_t w);
size_t mbstowcs(wchar_t* d, const char* s, size_t n); size_t wcstombs(char* d, const wchar_t* s, size_t n);
char* strptime(const char* s, const char* fmt, struct tm* tm);
#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7
#define LOG_PID 1
#define LOG_USER 8
#define LOG_DAEMON 24
void openlog(const char* ident, int opt, int facility); void syslog(int prio, const char* fmt, ...); void closelog(void);

/* internal hooks between the thread scheduler and sleep/yield/fork */
extern int __rio_threads_started;
unsigned __rio_thread_sleep(unsigned ms);
int __rio_thread_yield(void);
void __rio_thread_after_fork(void);

#ifdef __cplusplus
}
#endif
#endif
