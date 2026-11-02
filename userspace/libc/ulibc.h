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

/* dynamic loading (ET_DYN shared objects) */
void* dlopen(const char* path, int flags);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);
const char* dlerror(void);

#ifdef __cplusplus
}
#endif
#endif
