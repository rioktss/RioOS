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

/* RioOS libc, part 2: buffered stdio (FILE), extra string/stdlib functions, integer math. No floating point. */
#include "ulibc.h"

/* ---- stdio: buffered FILEs, thread safe ---------------------------------------------------------------------------------
   Every FILE has its own recursive lock (flockfile); the *_unlocked functions skip it. FILEs are allocated on the heap
   (any number can be open); the list of open streams is guarded by files_lock, which is always taken before a FILE lock. */
#define FILE_BUF 4096
struct FILE
{
    int fd; int err; int eof; int mode;          /* mode: 1 read, 2 write, 3 read/write */
    int rpos, rlen; int wlen; int unget; int append; int line;
    struct __rio_lock lk; int owner; int depth;  /* recursive lock: owner thread id */
    FILE* next; FILE* prev; int heap;
    unsigned char rbuf[FILE_BUF]; unsigned char wbuf[FILE_BUF];
};
static struct __rio_lock files_lock;
static FILE std_in, std_out, std_err;
static FILE* all_files = 0;
static int std_linked = 0;
FILE* stdin = &std_in;
FILE* stdout = &std_out;
FILE* stderr = &std_err;

static void stdio_init(void);
static void flk(FILE* f)
{
    int me = gettid();
    if (__atomic_load_n(&f->owner, __ATOMIC_ACQUIRE) == me) { ++f->depth; return; }
    __rio_lock_acquire(&f->lk);
    f->owner = me; f->depth = 1;
}
static void fulk(FILE* f)
{
    if (--f->depth > 0) return;
    __atomic_store_n(&f->owner, 0, __ATOMIC_RELEASE);
    __rio_lock_release(&f->lk);
}
static int ftrylk(FILE* f)
{
    int me = gettid();
    if (__atomic_load_n(&f->owner, __ATOMIC_ACQUIRE) == me) { ++f->depth; return 0; }
    int z = 0;
    if (!__atomic_compare_exchange_n(&f->lk.v, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return -1;
    f->owner = me; f->depth = 1;
    return 0;
}
void flockfile(FILE* f) { if (f) flk(f); }
void funlockfile(FILE* f) { if (f) fulk(f); }
int ftrylockfile(FILE* f) { return f ? ftrylk(f) : -1; }

static void link_std(void)                       /* the three standard streams live in the same list as the heap ones */
{
    if (std_linked) return;
    std_in.fd = 0; std_in.mode = 1; std_in.unget = -1;
    std_out.fd = 1; std_out.mode = 2; std_out.unget = -1; std_out.line = 1;
    std_err.fd = 2; std_err.mode = 2; std_err.unget = -1; std_err.line = 1;
    std_in.next = &std_out; std_out.prev = &std_in; std_out.next = &std_err; std_err.prev = &std_out;
    std_err.next = all_files; if (all_files) all_files->prev = &std_err;
    all_files = &std_in;
    __atomic_store_n(&std_linked, 1, __ATOMIC_RELEASE);
}
static void std_ready(void)                      /* cheap check on the hot path; the first call of any thread links the streams */
{
    if (__atomic_load_n(&std_linked, __ATOMIC_ACQUIRE)) return;
    __rio_lock_acquire(&files_lock); link_std(); __rio_lock_release(&files_lock);
}

static int fflush_u(FILE* f)
{
    int off = 0;
    while (off < f->wlen)
    {
        long n = write(f->fd, f->wbuf + off, (size_t)(f->wlen - off));
        if (n <= 0) { if (n < 0 && (errno == EAGAIN || errno == EINTR)) { sched_yield(); continue; } f->err = 1; f->wlen = 0; return -1; }
        off += (int)n;
    }
    f->wlen = 0;
    return 0;
}
int fflush(FILE* f)
{
    if (!f)
    {
        std_ready();
        int rc = 0;
        __rio_lock_acquire(&files_lock);
        for (FILE* p = all_files; p; p = p->next)
            if (p->mode & 2) { flk(p); if (fflush_u(p)) rc = -1; fulk(p); }
        __rio_lock_release(&files_lock);
        return rc;
    }
    flk(f);
    int rc = fflush_u(f);
    fulk(f);
    return rc;
}

static int stdio_state;
static void stdio_flush_all(void)                /* at exit: a stream whose lock is busy for long is flushed anyway */
{
    std_ready();
    __rio_lock_acquire(&files_lock);
    for (FILE* p = all_files; p; p = p->next)
    {
        if (!(p->mode & 2) || p->wlen == 0) continue;
        int tries = 0, locked = 0;
        while (tries < 100) { if (ftrylk(p) == 0) { locked = 1; break; } sched_yield(); ++tries; }
        (void)fflush_u(p);
        if (locked) fulk(p);
    }
    __rio_lock_release(&files_lock);
}
static void stdio_init(void)
{
    if (__atomic_load_n(&stdio_state, __ATOMIC_ACQUIRE) == 2) return;
    int z = 0;
    if (__atomic_compare_exchange_n(&stdio_state, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        std_ready();
        atexit(stdio_flush_all);
        __atomic_store_n(&stdio_state, 2, __ATOMIC_RELEASE);
    }
    else while (__atomic_load_n(&stdio_state, __ATOMIC_ACQUIRE) != 2) sched_yield();
}

static FILE* new_file(int fd, int m, int app)
{
    stdio_init();
    FILE* f = (FILE*)malloc(sizeof(FILE));
    if (!f) { errno = ENOMEM; return 0; }
    memset(f, 0, sizeof(*f));
    f->fd = fd; f->mode = m; f->unget = -1; f->append = app; f->heap = 1;
    __rio_lock_acquire(&files_lock);
    f->next = all_files; if (all_files) all_files->prev = f; all_files = f;
    __rio_lock_release(&files_lock);
    return f;
}
static int parse_mode(const char* mode, int* fl, int* m, int* app)
{
    int plus = 0;
    for (const char* p = mode; *p; ++p) if (*p == '+') plus = 1;
    *fl = 0; *app = 0;
    if (mode[0] == 'r') { *fl = plus ? O_RDWR : O_RDONLY; *m = plus ? 3 : 1; }
    else if (mode[0] == 'w') { *fl = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; *m = plus ? 3 : 2; }
    else if (mode[0] == 'a') { *fl = (plus ? O_RDWR : O_WRONLY) | O_CREAT; *m = plus ? 3 : 2; *app = 1; }
    else { errno = EINVAL; return -1; }
    return 0;
}
FILE* fopen(const char* path, const char* mode)
{
    int fl, m, app;
    if (parse_mode(mode, &fl, &m, &app) < 0) return 0;
    int fd = open(path, fl);
    if (fd < 0) return 0;
    FILE* f = new_file(fd, m, app);
    if (!f) { int e = errno; close(fd); errno = e; return 0; }
    if (app) lseek(fd, 0, SEEK_END);
    return f;
}
FILE* fdopen(int fd, const char* mode)
{
    if (fd < 0) { errno = EBADF; return 0; }
    int fl, m, app;
    if (parse_mode(mode, &fl, &m, &app) < 0) return 0;
    return new_file(fd, m, app);
}

int fclose(FILE* f)
{
    if (!f) return -1;
    std_ready();
    int rc = 0;
    int is_std = f == &std_in || f == &std_out || f == &std_err;
    __rio_lock_acquire(&files_lock);
    flk(f);
    if (f->mode & 2) rc = fflush_u(f);
    if (is_std) { fulk(f); __rio_lock_release(&files_lock); return rc; }
    if (close(f->fd) < 0) rc = -1;
    if (f->prev) f->prev->next = f->next; else all_files = f->next;
    if (f->next) f->next->prev = f->prev;
    f->fd = -1; f->mode = 0;
    int heap = f->heap;
    fulk(f);
    __rio_lock_release(&files_lock);
    if (heap) free(f);
    return rc;
}

static int fill(FILE* f)
{
    if (f->mode & 2) fflush_u(f);
    for (;;)
    {
        long n = read(f->fd, f->rbuf, FILE_BUF);
        if (n > 0) { f->rpos = 0; f->rlen = (int)n; return 1; }
        if (n == 0)
        {
            if (f->fd == 0) { /* console: wait for input */ char c; int ch = getchar(); if (ch < 0) { f->eof = 1; return 0; } c = (char)ch; f->rbuf[0] = (unsigned char)c; f->rpos = 0; f->rlen = 1; return 1; }
            f->eof = 1; return 0;
        }
        if (errno == EAGAIN || errno == EINTR) { sched_yield(); continue; }
        f->err = 1; return 0;
    }
}

static int fgetc_u(FILE* f)
{
    if (!f || !(f->mode & 1)) return -1;
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (f->rpos >= f->rlen && !fill(f)) return -1;
    return f->rbuf[f->rpos++];
}
int fgetc(FILE* f) { if (!f) return -1; std_ready(); flk(f); int c = fgetc_u(f); fulk(f); return c; }
int getc(FILE* f) { return fgetc(f); }
int fgetc_unlocked(FILE* f) { return fgetc_u(f); }
int getc_unlocked(FILE* f) { return fgetc_u(f); }
int ungetc(int c, FILE* f)
{
    if (!f || c < 0) return -1;
    flk(f);
    int r = -1;
    if (f->unget < 0) { f->unget = c; f->eof = 0; r = c; }
    fulk(f);
    return r;
}

static size_t fread_u(void* buf, size_t sz, size_t n, FILE* f)
{
    if (!sz || !n || !f) return 0;
    unsigned char* d = (unsigned char*)buf; size_t total = sz * n, got = 0;
    if (!(f->mode & 1)) return 0;
    while (got < total)
    {
        if (f->unget >= 0) { d[got++] = (unsigned char)f->unget; f->unget = -1; continue; }
        if (f->rpos >= f->rlen && !fill(f)) break;
        size_t k = (size_t)(f->rlen - f->rpos); if (k > total - got) k = total - got;
        memcpy(d + got, f->rbuf + f->rpos, k);
        f->rpos += (int)k; got += k;
    }
    return got / sz;
}
size_t fread(void* buf, size_t sz, size_t n, FILE* f) { if (!f) return 0; std_ready(); flk(f); size_t r = fread_u(buf, sz, n, f); fulk(f); return r; }
size_t fread_unlocked(void* buf, size_t sz, size_t n, FILE* f) { return fread_u(buf, sz, n, f); }

char* fgets(char* s, int cap, FILE* f)
{
    if (!f || cap <= 0) return 0;
    std_ready();
    flk(f);
    int k = 0;
    while (k + 1 < cap)
    {
        int c = fgetc_u(f);
        if (c < 0) break;
        s[k++] = (char)c;
        if (c == '\n') break;
    }
    s[k] = 0;
    fulk(f);
    return k ? s : 0;
}

static int fputc_u(int c, FILE* f)
{
    if (!f || !(f->mode & 2)) return -1;
    if (f->wlen >= FILE_BUF && fflush_u(f)) return -1;
    f->wbuf[f->wlen++] = (unsigned char)c;
    if (f->line && c == '\n') { if (fflush_u(f)) return -1; }
    return (unsigned char)c;
}
int fputc(int c, FILE* f) { if (!f) return -1; stdio_init(); flk(f); int r = fputc_u(c, f); fulk(f); return r; }
int putc(int c, FILE* f) { return fputc(c, f); }
int fputc_unlocked(int c, FILE* f) { stdio_init(); return fputc_u(c, f); }
int putc_unlocked(int c, FILE* f) { stdio_init(); return fputc_u(c, f); }
int getchar_unlocked(void) { return fgetc_unlocked(stdin); }
int putchar_unlocked(int c) { return fputc_unlocked(c, stdout); }

static size_t fwrite_u(const void* buf, size_t sz, size_t n, FILE* f)
{
    if (!f || !(f->mode & 2) || !sz || !n) return 0;
    const unsigned char* s = (const unsigned char*)buf; size_t total = sz * n, i = 0;
    while (i < total)
    {
        if (f->wlen >= FILE_BUF && fflush_u(f)) return i / sz;
        size_t room = (size_t)(FILE_BUF - f->wlen), k = total - i < room ? total - i : room;
        memcpy(f->wbuf + f->wlen, s + i, k);
        f->wlen += (int)k; i += k;
    }
    if (f->line && memchr(s, '\n', total) && fflush_u(f)) return n - 1;      /* line buffered: a newline in this write flushes it */
    return n;
}
size_t fwrite(const void* buf, size_t sz, size_t n, FILE* f) { if (!f) return 0; stdio_init(); flk(f); size_t r = fwrite_u(buf, sz, n, f); fulk(f); return r; }
size_t fwrite_unlocked(const void* buf, size_t sz, size_t n, FILE* f) { stdio_init(); return fwrite_u(buf, sz, n, f); }
int fputs(const char* s, FILE* f)
{
    if (!f) return -1;
    stdio_init(); flk(f);
    size_t n = strlen(s);
    int r = n == 0 ? 1 : (fwrite_u(s, 1, n, f) == n ? 1 : -1);
    fulk(f);
    return r;
}

int vfprintf(FILE* f, const char* fmt, va_list ap)
{
    char b[1024];
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    if (n < 0) { va_end(ap2); return n; }
    char* out = b;
    if (n >= (int)sizeof(b))                     /* longer than the stack buffer: format again into the heap */
    {
        out = (char*)malloc((size_t)n + 1);
        if (!out) { va_end(ap2); errno = ENOMEM; return -1; }
        (void)vsnprintf(out, (size_t)n + 1, fmt, ap2);
    }
    va_end(ap2);
    int rc = n;
    if (!f) rc = -1;
    else { stdio_init(); flk(f); if (n && fwrite_u(out, 1, (size_t)n, f) != (size_t)n) rc = -1; fulk(f); }
    if (out != b) free(out);
    return rc;
}
int fprintf(FILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(f, fmt, ap); va_end(ap); return n; }

int feof(FILE* f) { return f ? f->eof : 1; }
int ferror(FILE* f) { return f ? f->err : 1; }
void clearerr(FILE* f) { if (f) { flk(f); f->err = 0; f->eof = 0; fulk(f); } }
int fileno(FILE* f) { return f ? f->fd : -1; }

long ftell(FILE* f)
{
    if (!f) return -1;
    flk(f);
    long pos = lseek(f->fd, 0, SEEK_CUR), r = -1;
    if (pos >= 0) r = pos - (f->rlen - f->rpos) - (f->unget >= 0 ? 1 : 0) + f->wlen;
    fulk(f);
    return r;
}
int fseek(FILE* f, long off, int whence)
{
    if (!f) return -1;
    flk(f);
    if (f->mode & 2) fflush_u(f);
    if (whence == SEEK_CUR) off -= (f->rlen - f->rpos) + (f->unget >= 0 ? 1 : 0);
    f->rpos = f->rlen = 0; f->unget = -1; f->eof = 0;
    int r = lseek(f->fd, off, whence) < 0 ? -1 : 0;
    fulk(f);
    return r;
}
void rewind(FILE* f) { fseek(f, 0, SEEK_SET); clearerr(f); }

void perror(const char* s)
{
    int e = errno;
    flockfile(stderr);
    if (s && *s) { fputs(s, stderr); fputs(": ", stderr); }
    fputs(strerror(e), stderr); fputc('\n', stderr);
    funlockfile(stderr);
}

/* fork: take every stream lock so the child does not inherit one held by a thread that no longer exists */
void __rio_stdio_fork_prepare(void)
{
    std_ready();
    __rio_lock_acquire(&files_lock);
    for (FILE* p = all_files; p; p = p->next) flk(p);
}
void __rio_stdio_fork_parent(void)
{
    for (FILE* p = all_files; p; p = p->next) fulk(p);
    __rio_lock_release(&files_lock);
}
void __rio_stdio_fork_child(void)
{
    for (FILE* p = all_files; p; p = p->next) { p->lk.v = 0; p->owner = 0; p->depth = 0; }
    files_lock.v = 0;
}

/* sscanf: %d %i %u %x %s %c %n-less subset; returns number of assignments */

/* ---- string / stdlib extras ----------------------------------------------------------- */
size_t strnlen(const char* s, size_t n) { size_t k = 0; while (k < n && s[k]) ++k; return k; }
char* strncat(char* d, const char* s, size_t n) { char* r = d; d += strlen(d); while (n-- && *s) *d++ = *s++; *d = 0; return r; }
void* memchr(const void* s, int c, size_t n) { const unsigned char* p = (const unsigned char*)s; for (size_t i = 0; i < n; ++i) if (p[i] == (unsigned char)c) return (void*)(p + i); return 0; }
int strcasecmp(const char* a, const char* b) { while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; } return tolower((unsigned char)*a) - tolower((unsigned char)*b); }
int strncasecmp(const char* a, const char* b, size_t n) { while (n && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; --n; } return n ? tolower((unsigned char)*a) - tolower((unsigned char)*b) : 0; }
size_t strspn(const char* s, const char* set) { size_t n = 0; while (s[n] && strchr(set, s[n])) ++n; return n; }
size_t strcspn(const char* s, const char* set) { size_t n = 0; while (s[n] && !strchr(set, s[n])) ++n; return n; }
char* strpbrk(const char* s, const char* set) { for (; *s; ++s) if (strchr(set, *s)) return (char*)s; return 0; }
char* strtok_r(char* s, const char* delim, char** save)
{
    if (!s) s = *save;
    if (!s) return 0;
    s += strspn(s, delim);
    if (!*s) { *save = 0; return 0; }
    char* e = s + strcspn(s, delim);
    if (*e) { *e = 0; *save = e + 1; } else *save = 0;
    return s;
}
char* strtok(char* s, const char* delim) { static __thread char* sv; return strtok_r(s, delim, &sv); }
const char* strerror(int e)
{
    switch (e)
    {
        case 0: return "success"; case EPERM: return "operation not permitted"; case ENOENT: return "no such file or directory";
        case ESRCH: return "no such process"; case EINTR: return "interrupted"; case EIO: return "I/O error"; case EBADF: return "bad file descriptor";
        case ECHILD: return "no child processes"; case EAGAIN: return "try again"; case ENOMEM: return "out of memory"; case EACCES: return "permission denied";
        case EFAULT: return "bad address"; case EEXIST: return "file exists"; case EINVAL: return "invalid argument"; case EMFILE: return "too many open files";
        case EPIPE: return "broken pipe"; case ENOSYS: return "function not implemented"; default: return "unknown error";
    }
}
long atol(const char* s) { return strtol(s, 0, 10); }
long long strtoll(const char* s, char** end, int base) { return (long long)strtol(s, end, base); }
unsigned long long strtoull(const char* s, char** end, int base) { return (unsigned long long)strtoul(s, end, base); }
long long atoll(const char* s) { return strtoll(s, 0, 10); }
long labs(long v) { return v < 0 ? -v : v; }
long long llabs(long long v) { return v < 0 ? -v : v; }
void* bsearch(const void* key, const void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*))
{
    size_t lo = 0, hi = n;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2; const void* p = (const char*)base + mid * sz;
        int c = cmp(key, p);
        if (c == 0) return (void*)p;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isprint(int c) { return c >= 32 && c < 127; }
int ispunct(int c) { return isprint(c) && c != ' ' && !isalnum(c); }

/* ---- integer math (the kernel and libc are built without FPU) -------------------------- */
unsigned long isqrt_u(unsigned long v) { unsigned long r = 0, bit = 1UL << 62; while (bit > v) bit >>= 2; while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; } return r; }
long gcd_l(long a, long b) { a = labs(a); b = labs(b); while (b) { long t = a % b; a = b; b = t; } return a; }
long lcm_l(long a, long b) { long g = gcd_l(a, b); return g ? labs(a / g * b) : 0; }
long ipow_l(long base, unsigned exp) { long r = 1; while (exp) { if (exp & 1) r *= base; base *= base; exp >>= 1; } return r; }
long clamp_l(long v, long lo, long hi) { return v < lo ? lo : v > hi ? hi : v; }
