/* RioOS libc, part 2: buffered stdio (FILE), extra string/stdlib functions, integer math. No floating point. */
#include "ulibc.h"

/* ---- stdio ---------------------------------------------------------------------------- */
#define FILE_BUF 512
struct FILE
{
    int fd; int err; int eof; int mode;          /* mode: 1 read, 2 write, 3 read/write */
    int rpos, rlen; int wlen; int unget; int append; int line;
    unsigned char rbuf[FILE_BUF]; unsigned char wbuf[FILE_BUF];
};
static FILE files[8];
static FILE std_in  = { 0, 0, 0, 1, 0, 0, 0, -1, 0, 0, {0}, {0} };
static FILE std_out = { 1, 0, 0, 2, 0, 0, 0, -1, 0, 1, {0}, {0} };
static FILE std_err = { 2, 0, 0, 2, 0, 0, 0, -1, 0, 1, {0}, {0} };
FILE* stdin = &std_in;
FILE* stdout = &std_out;
FILE* stderr = &std_err;

int fflush(FILE* f)
{
    if (!f)
    {
        int rc = 0;
        if (fflush(&std_out)) rc = -1;
        if (fflush(&std_err)) rc = -1;
        for (int i = 0; i < 8; ++i) if (files[i].fd >= 0 && files[i].mode && fflush(&files[i])) rc = -1;
        return rc;
    }
    int off = 0;
    while (off < f->wlen)
    {
        long n = write(f->fd, f->wbuf + off, (size_t)(f->wlen - off));
        if (n <= 0) { if (n < 0 && errno == EAGAIN) { sched_yield(); continue; } f->err = 1; f->wlen = 0; return -1; }
        off += (int)n;
    }
    f->wlen = 0;
    return 0;
}

static int stdio_hooked;
static void stdio_flush_all(void) { fflush(0); }
static void stdio_init(void)
{
    if (stdio_hooked) return;
    stdio_hooked = 1;
    for (int i = 0; i < 8; ++i) files[i].fd = -1;
    atexit(stdio_flush_all);
}

FILE* fopen(const char* path, const char* mode)
{
    stdio_init();
    int fl = 0, m = 0, app = 0, plus = 0;
    for (const char* p = mode; *p; ++p) if (*p == '+') plus = 1;
    if (mode[0] == 'r') { fl = plus ? O_RDWR : O_RDONLY; m = plus ? 3 : 1; }
    else if (mode[0] == 'w') { fl = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; m = plus ? 3 : 2; }
    else if (mode[0] == 'a') { fl = (plus ? O_RDWR : O_WRONLY) | O_CREAT; m = plus ? 3 : 2; app = 1; }
    else { errno = EINVAL; return 0; }
    FILE* f = 0;
    for (int i = 0; i < 8; ++i) if (files[i].fd < 0 && files[i].mode == 0) { f = &files[i]; break; }
    if (!f) { errno = EMFILE; return 0; }
    int fd = open(path, fl);
    if (fd < 0) return 0;
    f->fd = fd; f->err = f->eof = 0; f->mode = m; f->rpos = f->rlen = f->wlen = 0; f->unget = -1; f->append = app; f->line = 0;
    if (app) lseek(fd, 0, SEEK_END);
    return f;
}

FILE* fdopen(int fd, const char* mode)
{
    stdio_init();
    if (fd < 0) { errno = EBADF; return 0; }
    int m = 0, app = 0, plus = 0;
    for (const char* p = mode; *p; ++p) if (*p == '+') plus = 1;
    if (mode[0] == 'r') m = plus ? 3 : 1;
    else if (mode[0] == 'w') m = plus ? 3 : 2;
    else if (mode[0] == 'a') { m = plus ? 3 : 2; app = 1; }
    else { errno = EINVAL; return 0; }
    FILE* f = 0;
    for (int i = 0; i < 8; ++i) if (files[i].fd < 0 && files[i].mode == 0) { f = &files[i]; break; }
    if (!f) { errno = EMFILE; return 0; }
    f->fd = fd; f->err = f->eof = 0; f->mode = m; f->rpos = f->rlen = f->wlen = 0; f->unget = -1; f->append = app; f->line = 0;
    return f;
}

int fclose(FILE* f)
{
    if (!f) return -1;
    int rc = 0;
    if (f->mode & 2) rc = fflush(f);
    if (f == &std_in || f == &std_out || f == &std_err) return rc;
    if (close(f->fd) < 0) rc = -1;
    f->fd = -1; f->mode = 0;
    return rc;
}

static int fill(FILE* f)
{
    if (f->mode & 2) fflush(f);
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

int fgetc(FILE* f)
{
    if (!f || !(f->mode & 1)) return -1;
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (f->rpos >= f->rlen && !fill(f)) return -1;
    return f->rbuf[f->rpos++];
}
int getc(FILE* f) { return fgetc(f); }
int ungetc(int c, FILE* f) { if (!f || c < 0 || f->unget >= 0) return -1; f->unget = c; f->eof = 0; return c; }

size_t fread(void* buf, size_t sz, size_t n, FILE* f)
{
    if (!sz || !n) return 0;
    unsigned char* d = (unsigned char*)buf; size_t total = sz * n, got = 0;
    while (got < total)
    {
        int c = fgetc(f);
        if (c < 0) break;
        d[got++] = (unsigned char)c;
    }
    return got / sz;
}

char* fgets(char* s, int cap, FILE* f)
{
    int k = 0;
    while (k + 1 < cap)
    {
        int c = fgetc(f);
        if (c < 0) break;
        s[k++] = (char)c;
        if (c == '\n') break;
    }
    s[k] = 0;
    return k ? s : 0;
}

static void stdio_init(void);
int fputc(int c, FILE* f)
{
    stdio_init();
    if (!f || !(f->mode & 2)) return -1;
    if (f->wlen >= FILE_BUF && fflush(f)) return -1;
    f->wbuf[f->wlen++] = (unsigned char)c;
    if (f->line && c == '\n') { if (fflush(f)) return -1; }
    return (unsigned char)c;
}
int putc(int c, FILE* f) { return fputc(c, f); }

size_t fwrite(const void* buf, size_t sz, size_t n, FILE* f)
{
    const unsigned char* s = (const unsigned char*)buf; size_t total = sz * n;
    for (size_t i = 0; i < total; ++i) if (fputc(s[i], f) < 0) return sz ? i / sz : 0;
    return n;
}
int fputs(const char* s, FILE* f) { while (*s) if (fputc((unsigned char)*s++, f) < 0) return -1; return 1; }

int vfprintf(FILE* f, const char* fmt, va_list ap)
{
    char b[1024];
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    int w = n < (int)sizeof(b) ? n : (int)sizeof(b) - 1;
    for (int i = 0; i < w; ++i) if (fputc((unsigned char)b[i], f) < 0) return -1;
    return n;
}
int fprintf(FILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(f, fmt, ap); va_end(ap); return n; }

int feof(FILE* f) { return f ? f->eof : 1; }
int ferror(FILE* f) { return f ? f->err : 1; }
void clearerr(FILE* f) { if (f) { f->err = 0; f->eof = 0; } }
int fileno(FILE* f) { return f ? f->fd : -1; }

long ftell(FILE* f)
{
    if (!f) return -1;
    long pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    return pos - (f->rlen - f->rpos) - (f->unget >= 0 ? 1 : 0) + f->wlen;
}
int fseek(FILE* f, long off, int whence)
{
    if (!f) return -1;
    if (f->mode & 2) fflush(f);
    if (whence == SEEK_CUR) off -= (f->rlen - f->rpos) + (f->unget >= 0 ? 1 : 0);
    f->rpos = f->rlen = 0; f->unget = -1; f->eof = 0;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}
void rewind(FILE* f) { fseek(f, 0, SEEK_SET); clearerr(f); }

void perror(const char* s)
{
    if (s && *s) { fputs(s, stderr); fputs(": ", stderr); }
    fputs(strerror(errno), stderr); fputc('\n', stderr);
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
char* strtok(char* s, const char* delim) { static char* sv; return strtok_r(s, delim, &sv); }
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
