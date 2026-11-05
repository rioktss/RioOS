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
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define WNOHANG 1
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_FAILED ((void*)-1)

#define SIGHUP 1
#define SIGINT 2
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
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
int stat_path(const char* path, struct RioStat* st);
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

#ifdef __cplusplus
}
#endif
#endif
