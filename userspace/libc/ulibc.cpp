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

#include "ulibc.h"
#include "../../include/user_api.h"

extern "C" {

int errno = 0;

#define SYS_FORK 37
#define SYS_EXEC 38
#define SYS_WAITPID 39
#define SYS_GETPPID 40
#define SYS_KILL 41
#define SYS_YIELD 42
#define SYS_BRK 43
#define SYS_MMAP 44
#define SYS_MMAPX 72
#define SYS_MUNMAP 45
#define SYS_SPAWN 46
#define SYS_SPAWN_FD 49
#define SYS_MPROTECT 50
#define SYS_GETUID 51
#define SYS_SETUID 52
#define SYS_FS_MKDIR 55
#define SYS_FS_UNLINK 56
#define SYS_FS_RENAME 57
#define SYS_FS_SYMLINK 60
#define SYS_FS_READLINK 61
#define SYS_FS_LINK 62
#define SYS_FS_LSTAT 63
#define SYS_FS_REALPATH 64
#define SYS_FS_READDIR 58
#define SYS_TIME_EPOCH 59

static long ret(uint64_t v)
{
    long r = (long)v;
    if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
    return r;
}

/* ---- string ------------------------------------------------------------ */
void* memcpy(void* d, const void* s, size_t n)
{
    unsigned char* a = (unsigned char*)d; const unsigned char* b = (const unsigned char*)s;
    while (n--) *a++ = *b++;
    return d;
}
void* memmove(void* d, const void* s, size_t n)
{
    unsigned char* a = (unsigned char*)d; const unsigned char* b = (const unsigned char*)s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void* memset(void* d, int c, size_t n) { unsigned char* a = (unsigned char*)d; while (n--) *a++ = (unsigned char)c; return d; }
int memcmp(const void* x, const void* y, size_t n)
{
    const unsigned char* a = (const unsigned char*)x; const unsigned char* b = (const unsigned char*)y;
    for (; n; --n, ++a, ++b) if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}
size_t strlen(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
int strcmp(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char* a, const char* b, size_t n)
{
    for (; n; --n, ++a, ++b) { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; if (!*a) return 0; }
    return 0;
}
char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }
char* strncpy(char* d, const char* s, size_t n) { size_t i = 0; for (; i < n && s[i]; ++i) d[i] = s[i]; for (; i < n; ++i) d[i] = 0; return d; }
char* strcat(char* d, const char* s) { strcpy(d + strlen(d), s); return d; }
char* strchr(const char* s, int c) { for (;; ++s) { if (*s == (char)c) return (char*)s; if (!*s) return 0; } }
char* strrchr(const char* s, int c) { const char* r = 0; for (;; ++s) { if (*s == (char)c) r = s; if (!*s) return (char*)r; } }
char* strstr(const char* h, const char* n)
{
    size_t l = strlen(n);
    if (!l) return (char*)h;
    for (; *h; ++h) if (!strncmp(h, n, l)) return (char*)h;
    return 0;
}
char* strdup(const char* s) { size_t l = strlen(s) + 1; char* p = (char*)malloc(l); if (p) memcpy(p, s, l); return p; }

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

/* ---- syscalls ---------------------------------------------------------- */
pid_t fork(void) { pid_t r = (pid_t)ret(user_syscall0(SYS_FORK)); if (r == 0 && __rio_threads_started) __rio_thread_after_fork(); return r; }
int execv_args(const char* path, const char* args) { return (int)ret(user_syscall2(SYS_EXEC, (uint64_t)path, (uint64_t)args)); }
pid_t waitpid(pid_t pid, int* st, int opt)
{
    for (;;)
    {
        long r = ret(user_syscall3(SYS_WAITPID, (uint64_t)(long)pid, (uint64_t)st, (uint64_t)opt));
        if (r == -1 && errno == EINTR) continue;
        return (pid_t)r;
    }
}
pid_t wait(int* st) { return waitpid(-1, st, 0); }
pid_t getpid(void) { return (pid_t)user_syscall0(USER_SYS_GETPID); }
pid_t getppid(void) { return (pid_t)ret(user_syscall0(SYS_GETPPID)); }
int kill(pid_t pid, int sig) { return (int)ret(user_syscall2(SYS_KILL, (uint64_t)(long)pid, (uint64_t)sig)); }
unsigned getuid(void) { return (unsigned)(user_syscall0(SYS_GETUID) & 0xffffffffU); }
unsigned getgid(void) { return (unsigned)(user_syscall0(SYS_GETUID) >> 32); }
int mkdir(const char* path, int mode) { (void)mode; return (int)ret(user_syscall1(SYS_FS_MKDIR, (uint64_t)path)); }
int unlink(const char* path) { return (int)ret(user_syscall1(SYS_FS_UNLINK, (uint64_t)path)); }
int rmdir(const char* path) { return (int)ret(user_syscall1(SYS_FS_UNLINK, (uint64_t)path)); }
int rename(const char* a, const char* b) { return (int)ret(user_syscall2(SYS_FS_RENAME, (uint64_t)a, (uint64_t)b)); }
long readdir_text(const char* path, char* buf, size_t cap) { return ret(user_syscall3(SYS_FS_READDIR, (uint64_t)path, (uint64_t)buf, cap)); }
int symlink(const char* target, const char* linkpath) { long r = ret(user_syscall2(SYS_FS_SYMLINK, (uint64_t)target, (uint64_t)linkpath)); return r < 0 ? -1 : 0; }
int link(const char* target, const char* linkpath) { long r = ret(user_syscall2(SYS_FS_LINK, (uint64_t)target, (uint64_t)linkpath)); return r < 0 ? -1 : 0; }
long readlink(const char* path, char* buf, size_t cap) { long r = ret(user_syscall3(SYS_FS_READLINK, (uint64_t)path, (uint64_t)buf, cap)); return r < 0 ? -1 : r; }
char* realpath(const char* path, char* out)
{
    static char sbuf[256]; char* dst = out ? out : sbuf;
    long r = ret(user_syscall3(SYS_FS_REALPATH, (uint64_t)path, (uint64_t)dst, 256));
    return r < 0 ? 0 : dst;
}
long time_epoch(void) { return (long)user_syscall0(SYS_TIME_EPOCH); }
int stat_path(const char* path, struct RioStat* st)
{
    UserFsStat u; long r = ret((uint64_t)user_fs_stat(path, &u));
    if (r < 0) return -1;
    st->inode = u.inode; st->type = u.type; st->mode = u.mode; st->uid = u.uid; st->gid = u.gid; st->sector_count = u.sector_count; st->size = u.size; st->mtime = u.mtime;
    return 0;
}
int lstat_path(const char* path, struct RioStat* st)
{
    UserFsStat u; long r = ret(user_syscall2(SYS_FS_LSTAT, (uint64_t)path, (uint64_t)&u));
    if (r < 0) return -1;
    st->inode = u.inode; st->type = u.type; st->mode = u.mode; st->uid = u.uid; st->gid = u.gid; st->sector_count = u.sector_count; st->size = u.size; st->mtime = u.mtime;
    return 0;
}
int chmod(const char* path, unsigned mode) { return (int)ret((uint64_t)user_fs_chmod(path, mode)); }
int setuid(unsigned uid, unsigned gid) { return (int)ret(user_syscall2(SYS_SETUID, uid, gid)); }
pid_t spawn(const char* path, const char* args, int bg) { return (pid_t)ret(user_syscall3(SYS_SPAWN, (uint64_t)path, (uint64_t)args, (uint64_t)bg)); }
pid_t spawn_fd(const char* path, const char* args, int in_fd, int out_fd)
{ return (pid_t)ret(user_syscall4(SYS_SPAWN_FD, (uint64_t)path, (uint64_t)args, (uint64_t)(long)in_fd, (uint64_t)(long)out_fd)); }
void sched_yield(void) { if (__rio_threads_started && __rio_thread_yield()) return; (void)user_syscall0(SYS_YIELD); }
unsigned sleep_ms(unsigned ms)   /* returns the milliseconds left when a signal interrupted the sleep */
{
    if (__rio_threads_started) { unsigned r = __rio_thread_sleep(ms); if (r != 0xFFFFFFFFU) return r; }
    while (ms > 10000) { long left = (long)user_sleep_ms(10000); if (left > 0) return (unsigned)left + (ms - 10000); ms -= 10000; }
    long left = (long)user_sleep_ms(ms);
    return left > 0 ? (unsigned)left : 0;
}
unsigned long clock_ms(void) { return (unsigned long)user_time_ms(); }
int open(const char* p, int f) { return (int)ret((uint64_t)user_fd_open(p, (uint64_t)f)); }
int close(int fd) { return (int)ret((uint64_t)user_fd_close(fd)); }
int __rio_sa_restart = 0;   /* set by the signal dispatcher when a SA_RESTART handler ran */
ssize_t read(int fd, void* b, size_t n)
{
    for (;;) { ssize_t r = ret((uint64_t)user_fd_read(fd, b, n)); if (r < 0 && errno == EINTR && __rio_sa_restart) { __rio_sa_restart = 0; continue; } return r; }
}
ssize_t write(int fd, const void* b, size_t n)
{
    for (;;) { ssize_t r = ret((uint64_t)user_fd_write(fd, b, n)); if (r < 0 && errno == EINTR && __rio_sa_restart) { __rio_sa_restart = 0; continue; } return r; }
}
long lseek(int fd, long o, int w) { return ret((uint64_t)user_fd_seek(fd, o, w)); }
int dup(int fd) { return (int)ret((uint64_t)user_fd_dup(fd)); }
int dup2(int a, int b) { return (int)ret((uint64_t)user_fd_dup2(a, b)); }
int pipe(int fds[2]) { return (int)ret((uint64_t)user_fd_pipe(fds)); }
const char* getargs(void)
{
    static char buf[1100];
    buf[0] = 0;
    (void)user_syscall2(USER_SYS_GETARG, (uint64_t)buf, sizeof(buf));
    for (char* p = buf; *p; ++p) if (*p == 1) { *p = 0; break; }     /* environment follows after \x01 */
    return buf;
}

static unsigned long brk_cur = 0;
int brk(void* addr)
{
    unsigned long r = (unsigned long)user_syscall1(SYS_BRK, (unsigned long)addr);
    if ((long)r < 0 && (long)r > -4096) { errno = ENOMEM; return -1; }
    brk_cur = r;
    return 0;
}
void* sbrk(long inc)
{
    if (!brk_cur) { unsigned long r = (unsigned long)user_syscall1(SYS_BRK, 0); if ((long)r < 0) { errno = ENOMEM; return (void*)-1; } brk_cur = r; }
    unsigned long old = brk_cur;
    if (inc == 0) return (void*)old;
    unsigned long r = (unsigned long)user_syscall1(SYS_BRK, old + (unsigned long)inc);
    if ((long)r < 0 && (long)r > -4096) { errno = ENOMEM; return (void*)-1; }
    brk_cur = r;
    return (void*)old;
}
void* mmap(void* addr, size_t len, int prot, int flags, int fd, long off)
{
    (void)addr;
    unsigned long r = (unsigned long)user_syscall4(SYS_MMAPX, len, (uint64_t)prot, (uint64_t)(long)fd, ((uint64_t)(unsigned)flags << 32) | ((uint64_t)off & 0xFFFFFFFFULL));
    if ((long)r < 0 && (long)r > -4096) { errno = (int)-(long)r; return MAP_FAILED; }
    return (void*)r;
}
int munmap(void* a, size_t l) { return (int)ret(user_syscall2(SYS_MUNMAP, (uint64_t)a, l)); }
int mprotect(void* a, size_t l, int p) { return (int)ret(user_syscall3(SYS_MPROTECT, (uint64_t)a, l, (uint64_t)p)); }

/* ---- malloc: first-fit free list over brk, big blocks via mmap --------- */
struct MBlock { size_t size; size_t flags; MBlock* next; MBlock* prev; };   /* 32 bytes */
#define MB_FREE 1
#define MB_MMAP 2
static MBlock* mhead = 0;
static MBlock* mtail = 0;

void* malloc(size_t n)
{
    if (!n) return 0;
    n = (n + 15) & ~(size_t)15;
    if (n >= 65536)
    {
        size_t total = n + sizeof(MBlock);
        void* p = mmap(0, total, PROT_READ | PROT_WRITE, 0, -1, 0);
        if (p == MAP_FAILED) return 0;
        MBlock* b = (MBlock*)p; b->size = total - sizeof(MBlock); b->flags = MB_MMAP; b->next = b->prev = 0;
        return b + 1;
    }
    for (MBlock* b = mhead; b; b = b->next)
        if ((b->flags & MB_FREE) && b->size >= n)
        {
            if (b->size >= n + sizeof(MBlock) + 16)
            {
                MBlock* r = (MBlock*)((char*)(b + 1) + n);
                r->size = b->size - n - sizeof(MBlock); r->flags = MB_FREE; r->next = b->next; r->prev = b;
                if (b->next) b->next->prev = r; else mtail = r;
                b->next = r; b->size = n;
            }
            b->flags = 0;
            return b + 1;
        }
    MBlock* b = (MBlock*)sbrk((long)(n + sizeof(MBlock)));
    if (b == (MBlock*)-1) return 0;
    b->size = n; b->flags = 0; b->next = 0; b->prev = mtail;
    if (mtail) mtail->next = b; else mhead = b;
    mtail = b;
    return b + 1;
}
void free(void* p)
{
    if (!p) return;
    MBlock* b = (MBlock*)p - 1;
    if (b->flags & 4) { free(b->next); return; }          /* aligned_alloc wrapper header */
    if (b->flags & MB_MMAP) { munmap(b, b->size + sizeof(MBlock)); return; }
    b->flags = MB_FREE;
    if (b->next && (b->next->flags & MB_FREE) && (char*)(b + 1) + b->size == (char*)b->next)
    {
        b->size += sizeof(MBlock) + b->next->size; b->next = b->next->next;
        if (b->next) b->next->prev = b; else mtail = b;
    }
    if (b->prev && (b->prev->flags & MB_FREE) && (char*)(b->prev + 1) + b->prev->size == (char*)b)
    {
        MBlock* a = b->prev; a->size += sizeof(MBlock) + b->size; a->next = b->next;
        if (b->next) b->next->prev = a; else mtail = a;
    }
}
void* calloc(size_t c, size_t n) { size_t t = c * n; void* p = malloc(t); if (p) memset(p, 0, t); return p; }
size_t malloc_usable_size(void* p) { if (!p) return 0; MBlock* b = (MBlock*)p - 1; if (b->flags & 4) return 0; return b->size; }
void* realloc(void* p, size_t n)
{
    if (!p) return malloc(n);
    if (!n) { free(p); return 0; }
    MBlock* b = (MBlock*)p - 1;
    if (b->flags & 4) { void* q = malloc(n); if (q) { memcpy(q, p, n); free(p); } return q; }
    size_t need = (n + 15) & ~(size_t)15;
    if (!(b->flags & MB_MMAP))
    {
        if (b->size >= need)                                   /* shrink in place, give the tail back */
        {
            if (b->size >= need + sizeof(MBlock) + 16)
            {
                MBlock* r = (MBlock*)((char*)(b + 1) + need);
                r->size = b->size - need - sizeof(MBlock); r->flags = MB_FREE; r->next = b->next; r->prev = b;
                if (b->next) b->next->prev = r; else mtail = r;
                b->next = r; b->size = need;
                if (r->next && (r->next->flags & MB_FREE) && (char*)(r + 1) + r->size == (char*)r->next)
                { r->size += sizeof(MBlock) + r->next->size; r->next = r->next->next; if (r->next) r->next->prev = r; else mtail = r; }
            }
            return p;
        }
        MBlock* nx = b->next;                                  /* grow in place into a free neighbour */
        if (nx && (nx->flags & MB_FREE) && (char*)(b + 1) + b->size == (char*)nx && b->size + sizeof(MBlock) + nx->size >= need)
        {
            b->size += sizeof(MBlock) + nx->size; b->next = nx->next;
            if (b->next) b->next->prev = b; else mtail = b;
            if (b->size >= need + sizeof(MBlock) + 16)
            {
                MBlock* r = (MBlock*)((char*)(b + 1) + need);
                r->size = b->size - need - sizeof(MBlock); r->flags = MB_FREE; r->next = b->next; r->prev = b;
                if (b->next) b->next->prev = r; else mtail = r;
                b->next = r; b->size = need;
            }
            return p;
        }
    }
    else if (b->size >= need) return p;
    void* q = malloc(n);
    if (q) { memcpy(q, p, b->size < n ? b->size : n); free(p); }
    return q;
}

int abs(int v) { return v < 0 ? -v : v; }
unsigned long strtoul(const char* s, char** end, int base)
{
    while (isspace(*s)) ++s;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0) base = (s[0] == '0') ? 8 : 10;
    unsigned long v = 0;
    for (;; ++s)
    {
        int d = isdigit(*s) ? *s - '0' : (isalpha(*s) ? tolower(*s) - 'a' + 10 : 99);
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (char*)s;
    return v;
}
long strtol(const char* s, char** end, int base)
{
    while (isspace(*s)) ++s;
    int neg = 0;
    if (*s == '-') { neg = 1; ++s; } else if (*s == '+') ++s;
    long v = (long)strtoul(s, end, base);
    return neg ? -v : v;
}
int atoi(const char* s) { return (int)strtol(s, 0, 10); }

static void swap_bytes(char* a, char* b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
void qsort(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*))
{
    char* b = (char*)base;
    for (size_t i = 1; i < n; ++i)
        for (size_t j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0; --j)
            swap_bytes(b + (j - 1) * sz, b + j * sz, sz);
}
static unsigned rng = 12345;
int rand(void) { rng = rng * 1103515245u + 12345u; return (int)((rng >> 16) & 0x7FFF); }
void srand(unsigned s) { rng = s; }

static void (*exit_hooks[8])(void);
static int exit_hook_n = 0;
int atexit(void (*fn)(void)) { if (exit_hook_n >= 8) return -1; exit_hooks[exit_hook_n++] = fn; return 0; }
void _exit(int code) { (void)user_syscall1(USER_SYS_EXIT, (uint64_t)(long)code); for (;;) {} }
void exit(int code) { while (exit_hook_n > 0) exit_hooks[--exit_hook_n](); _exit(code); }
void abort(void) { _exit(134); }

/* ---- printf ------------------------------------------------------------ */
struct Out { char* buf; size_t cap; size_t len; int fd; };
static void out_ch(Out* o, char c)
{
    if (o->fd >= 0)
    {
        if (o->len == o->cap) { write(o->fd, o->buf, o->len); o->len = 0; }
        o->buf[o->len++] = c;
    }
    else { if (o->len + 1 < o->cap) o->buf[o->len] = c; ++o->len; }
}
static void out_str(Out* o, const char* s, int width, int left, int zero, int prec)
{
    int l = 0; while (s[l] && (prec < 0 || l < prec)) ++l;
    int pad = width > l ? width - l : 0;
    if (!left) while (pad--) out_ch(o, zero ? '0' : ' ');
    for (int i = 0; i < l; ++i) out_ch(o, s[i]);
    if (left) while (pad--) out_ch(o, ' ');
}
static int format(Out* o, const char* f, va_list ap)
{
    for (; *f; ++f)
    {
        if (*f != '%') { out_ch(o, *f); continue; }
        ++f;
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0, plus = 0, space = 0, alt = 0;
        for (;; ++f) { if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1; else if (*f == '#') alt = 1; else break; }
        if (*f == '*') { width = va_arg(ap, int); ++f; } else while (isdigit(*f)) width = width * 10 + (*f++ - '0');
        if (*f == '.') { ++f; prec = 0; if (*f == '*') { prec = va_arg(ap, int); ++f; } else while (isdigit(*f)) prec = prec * 10 + (*f++ - '0'); }
        while (*f == 'l' || *f == 'z' || *f == 'h') { if (*f != 'h') ++lng; ++f; }
        char tmp[72]; int t = 0;
        switch (*f)
        {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p':
            {
                int sgn = (*f == 'd' || *f == 'i');
                unsigned long long v; int neg = 0;
                if (*f == 'p') { v = (unsigned long long)va_arg(ap, void*); lng = 1; }
                else if (sgn) { long long s = lng ? (lng > 1 ? va_arg(ap, long long) : (long long)va_arg(ap, long)) : (long long)va_arg(ap, int); if (s < 0) { neg = 1; v = (unsigned long long)(-s); } else v = (unsigned long long)s; }
                else v = lng ? (lng > 1 ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned long)) : (unsigned long long)va_arg(ap, unsigned);
                unsigned base = (*f == 'x' || *f == 'X' || *f == 'p') ? 16 : (*f == 'o' ? 8 : 10);
                const char* dig = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
                do { tmp[t++] = dig[v % base]; v /= base; } while (v && t < 70);
                if (*f == 'p') { tmp[t++] = 'x'; tmp[t++] = '0'; }
                if (neg) tmp[t++] = '-'; else if (plus && sgn) tmp[t++] = '+';
                char rev[72]; for (int i = 0; i < t; ++i) rev[i] = tmp[t - 1 - i]; rev[t] = 0;
                if (zero && !left && width > t && (rev[0] == '-' || rev[0] == '+')) { out_ch(o, rev[0]); out_str(o, rev + 1, width - 1, 0, 1, -1); }
                else out_str(o, rev, width, left, zero, -1);
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            {
                double dv = va_arg_double(ap);
                static char fb[1800];
                int fl = rio_fmt_double(fb, (int)sizeof(fb), dv, *f, prec, plus, space, alt);
                if (fl >= (int)sizeof(fb)) fl = (int)sizeof(fb) - 1;
                int sgn = (fb[0] == '-' || fb[0] == '+' || fb[0] == ' ');
                int finite = isfinite_d(dv);
                if (zero && !left && finite && width > fl) { if (sgn) { out_ch(o, fb[0]); out_str(o, fb + 1, width - 1, 0, 1, -1); } else out_str(o, fb, width, 0, 1, -1); }
                else out_str(o, fb, width, left, 0, -1);
                break;
            }
            case 'c': { char c[2] = { (char)va_arg(ap, int), 0 }; out_str(o, c, width, left, 0, -1); break; }
            case 's': { const char* s = va_arg(ap, const char*); out_str(o, s ? s : "(null)", width, left, 0, prec); break; }
            case '%': out_ch(o, '%'); break;
            default: out_ch(o, '%'); if (*f) out_ch(o, *f); else --f; break;
        }
    }
    return (int)o->len;
}
int vsnprintf(char* out, size_t cap, const char* fmt, va_list ap)
{
    Out o = { out, cap, 0, -1 };
    format(&o, fmt, ap);
    if (cap) out[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}
int vdprintf(int fd, const char* fmt, va_list ap)
{
    char buf[128]; Out o = { buf, sizeof(buf), 0, fd };
    int n = format(&o, fmt, ap);
    if (o.len) write(fd, buf, o.len);
    return n;
}
int snprintf(char* out, size_t cap, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = vsnprintf(out, cap, fmt, ap); va_end(ap); return n; }
int sprintf(char* out, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = vsnprintf(out, 4096, fmt, ap); va_end(ap); return n; }
int dprintf(int fd, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = vdprintf(fd, fmt, ap); va_end(ap); return n; }
int printf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = vdprintf(1, fmt, ap); va_end(ap); return n; }
int puts(const char* s) { write(1, s, strlen(s)); write(1, "\n", 1); return 1; }
int putchar(int c) { char ch = (char)c; write(1, &ch, 1); return c; }
int getchar(void)
{
    char c;
    for (;;)
    {
        long n = read(0, &c, 1);
        if (n == 1) return (unsigned char)c;
        if (n == 0) { if (user_read(&c, 1) == 1) return (unsigned char)c; sched_yield(); continue; }
        if (errno == EAGAIN || errno == EINTR) { sched_yield(); continue; }
        return -1;
    }
}
char* fgets_fd(int fd, char* buf, int cap)
{
    int n = 0;
    while (n + 1 < cap)
    {
        char c; long r = read(fd, &c, 1);
        if (r == 0) { if (fd == 0) { sched_yield(); continue; } break; }
        if (r < 0) { if (errno == EAGAIN) { sched_yield(); continue; } break; }
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = 0;
    return n ? buf : 0;
}

/* ---- program start ----------------------------------------------------- */
void __libc_start(void)
{
    static char argbuf[1100];
    static char* argv[66];
    int argc = 0;
    argv[argc++] = (char*)"prog";
    argbuf[0] = 0;
    (void)user_syscall2(USER_SYS_GETARG, (uint64_t)argbuf, sizeof(argbuf));
    /* argument line [ \x01 \x02argv0 ] [ \x01 NAME=value ]... */
    char* envs = 0;
    for (char* e = argbuf; *e; ++e) if (*e == 1) { *e = 0; envs = e + 1; break; }
    if (envs)
    {
        char* q = envs;
        while (q)
        {
            char* nx = 0;
            for (char* e = q; *e; ++e) if (*e == 1) { *e = 0; nx = e + 1; break; }
            if (q[0] == 2) argv[0] = q + 1;
            else if (q[0]) (void)putenv(q);
            q = nx;
        }
    }
    char* p = argbuf;
    while (*p && argc < 64)
    {
        while (*p == ' ') ++p;
        if (!*p) break;
        argv[argc++] = p;
        char* w = p; char q = 0;                       /* '...' and "..." group words and are removed */
        while (*p && (q || *p != ' '))
        {
            if (q) { if (*p == q) q = 0; else *w++ = *p; }
            else if (*p == '"' || *p == '\'') q = *p;
            else *w++ = *p;
            ++p;
        }
        if (*p) ++p;
        *w = 0;
    }
    argv[argc] = 0;
    exit(main(argc, argv));
}

} /* extern "C" */
