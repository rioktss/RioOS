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
/* RioOS libc part 7: POSIX system layer. stat family, directories (dirent/glob/fnmatch), file attributes, termios, users/groups,
   system information and limits, random numbers, futex, shared memory and named semaphores, epoll, timerfd. */

#define K_FS_STAT 23
#define K_FS_TRUNCATE 24
#define K_FS_LSTAT 63
#define K_FUTEX 96
#define K_TTY 97
#define K_FSTAT 98
#define K_CHOWN 99
#define K_UTIME 100
#define K_FTRUNCATE 101
#define K_FLOCK 102
#define K_FSYNC 103
#define K_GETRANDOM 104
#define K_TIMERFD 105
#define K_GETPW 106
#define K_CHDIR 107
#define K_GETCWD 108
#define K_UMASK 109
#define K_SYSINFO 110
#define K_FDATTR 111
#define K_FDGEN 113
#define K_RLIMIT 114
#define K_AFFINITY 115
#define K_SIGNALFD 116
#define K_INOTIFY 117
#define K_RUSAGE 118

static long kret(uint64_t v) { long r = (long)v; if (r < 0 && r > -4096) { errno = (int)-r; return -1; } return r; }
static void copy_str(char* d, const char* s, size_t cap) { size_t i = 0; if (!cap) return; while (s[i] && i + 1 < cap) { d[i] = s[i]; ++i; } d[i] = 0; }

/* ---- futex -------------------------------------------------------------------------------------------------------- */
int futex_wait(volatile uint32_t* addr, uint32_t expected, long timeout_ms)
{
    uint64_t t = timeout_ms < 0 ? ~0ULL : (uint64_t)timeout_ms;
    long r = (long)user_syscall4(K_FUTEX, (uint64_t)addr, 0, expected, t);
    if (r == 0) return 0;
    if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
    return 0;
}
int futex_wake(volatile uint32_t* addr, int n)
{
    long r = (long)user_syscall3(K_FUTEX, (uint64_t)addr, 1, (uint64_t)(long)n);
    if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
    return (int)r;
}

/* ---- stat family ---------------------------------------------------------------------------------------------------- */
static void fill_stat(struct stat* st, const UserFsStat* u)
{
    memset(st, 0, sizeof(*st));
    unsigned fmt;
    switch (u->type) { case 1: fmt = S_IFDIR; break; case 2: fmt = S_IFLNK; break; case 3: fmt = S_IFIFO; break; case 4: fmt = S_IFSOCK; break; case 5: fmt = S_IFCHR; break; default: fmt = S_IFREG; break; }
    st->st_dev = 1; st->st_ino = u->inode; st->st_mode = fmt | (u->mode & 07777U);
    st->st_nlink = u->type == 1 ? 2 : 1; st->st_uid = u->uid; st->st_gid = u->gid; st->st_size = (off_t)u->size;
    st->st_blksize = 512; st->st_blocks = (blkcnt_t)u->sector_count;
    st->st_mtim.tv_sec = (long)u->mtime; st->st_atim.tv_sec = (long)u->mtime; st->st_ctim.tv_sec = (long)u->mtime;
}
int stat(const char* path, struct stat* st)
{
    UserFsStat u; if (kret(user_syscall2(K_FS_STAT, (uint64_t)path, (uint64_t)&u)) < 0) return -1;
    fill_stat(st, &u); return 0;
}
int lstat(const char* path, struct stat* st)
{
    UserFsStat u; if (kret(user_syscall2(K_FS_LSTAT, (uint64_t)path, (uint64_t)&u)) < 0) return -1;
    fill_stat(st, &u); return 0;
}
int fstat(int fd, struct stat* st)
{
    UserFsStat u; if (fd < 0) { errno = EBADF; return -1; }
    if (kret(user_syscall2(K_FSTAT, (uint64_t)(long)fd, (uint64_t)&u)) < 0) return -1;
    fill_stat(st, &u); return 0;
}
int fchmod(int fd, mode_t mode) { return (int)kret(user_syscall3(K_FDATTR, (uint64_t)(long)fd, 0, mode & 0777)); }
int chown(const char* path, uid_t u, gid_t g) { return (int)kret(user_syscall3(K_CHOWN, (uint64_t)path, u, g)); }
int fchown(int fd, uid_t u, gid_t g) { return (int)kret(user_syscall4(K_FDATTR, (uint64_t)(long)fd, 1, u, g)); }
int lchown(const char* path, uid_t u, gid_t g)
{
    struct stat st; if (lstat(path, &st) < 0) return -1;
    if (S_ISLNK(st.st_mode)) { errno = ENOSYS; return -1; }       /* ownership of the link itself cannot be changed */
    return chown(path, u, g);
}
mode_t umask(mode_t mask) { return (mode_t)user_syscall1(K_UMASK, mask & 0777U); }
int utime(const char* path, const struct utimbuf* t)
{
    long sec = t ? t->modtime : time_epoch();
    return (int)kret(user_syscall2(K_UTIME, (uint64_t)path, (uint64_t)sec));
}
int utimes(const char* path, const struct timeval tv[2])
{
    long sec = tv ? tv[1].tv_sec : time_epoch();
    return (int)kret(user_syscall2(K_UTIME, (uint64_t)path, (uint64_t)sec));
}
static int ts_valid(const struct timespec* t) { return t->tv_nsec == UTIME_NOW || t->tv_nsec == UTIME_OMIT || (t->tv_nsec >= 0 && t->tv_nsec < 1000000000L); }
int utimensat(int dirfd, const char* path, const struct timespec ts[2], int flags)
{
    (void)flags;
    if (!path) { errno = EFAULT; return -1; }
    if (dirfd != AT_FDCWD && path[0] != '/') { errno = ENOSYS; return -1; }
    long sec = time_epoch();
    if (ts)
    {
        if (!ts_valid(&ts[0]) || !ts_valid(&ts[1])) { errno = EINVAL; return -1; }
        if (ts[1].tv_nsec == UTIME_OMIT) return 0;
        if (ts[1].tv_nsec != UTIME_NOW) sec = ts[1].tv_sec;
    }
    return (int)kret(user_syscall2(K_UTIME, (uint64_t)path, (uint64_t)sec));
}
int futimens(int fd, const struct timespec ts[2])
{
    long sec = time_epoch();
    if (ts)
    {
        if (!ts_valid(&ts[0]) || !ts_valid(&ts[1])) { errno = EINVAL; return -1; }
        if (ts[1].tv_nsec == UTIME_OMIT) { struct stat st; return fstat(fd, &st); }
        if (ts[1].tv_nsec != UTIME_NOW) sec = ts[1].tv_sec;
    }
    return (int)kret(user_syscall3(K_FDATTR, (uint64_t)(long)fd, 2, (uint64_t)sec));
}
int truncate(const char* path, off_t len) { if (len < 0) { errno = EINVAL; return -1; } return (int)kret(user_syscall2(K_FS_TRUNCATE, (uint64_t)path, (uint64_t)len)); }
int ftruncate(int fd, off_t len) { if (len < 0) { errno = EINVAL; return -1; } return (int)kret(user_syscall2(K_FTRUNCATE, (uint64_t)(long)fd, (uint64_t)len)); }
int fsync(int fd) { if (fd < 0) { errno = EBADF; return -1; } return (int)kret(user_syscall1(K_FSYNC, (uint64_t)(long)fd)); }
int fdatasync(int fd) { return fsync(fd); }
void sync(void) { (void)user_syscall1(K_FSYNC, ~0ULL); }
int access(const char* path, int mode)
{
    struct stat st; if (stat(path, &st) < 0) return -1;
    if (mode == F_OK) return 0;
    unsigned uid = getuid(), gid = getgid(), m = st.st_mode & 0777U, bits;
    if (uid == 0) bits = (m & 0111U) || S_ISDIR(st.st_mode) ? 7U : 6U;
    else if (st.st_uid == uid) bits = (m >> 6) & 7U;
    else if (st.st_gid == gid) bits = (m >> 3) & 7U;
    else bits = m & 7U;
    if (((unsigned)mode & ~7U) != 0) { errno = EINVAL; return -1; }
    if ((((unsigned)mode & R_OK) && !(bits & 4U)) || (((unsigned)mode & W_OK) && !(bits & 2U)) || (((unsigned)mode & X_OK) && !(bits & 1U))) { errno = EACCES; return -1; }
    return 0;
}
int flock(int fd, int op) { return (int)kret(user_syscall2(K_FLOCK, (uint64_t)(long)fd, (uint64_t)(long)op)); }
char* getcwd(char* buf, size_t size)
{
    char tmp[256];
    if (buf && size == 0) { errno = EINVAL; return 0; }
    long r = kret(user_syscall2(K_GETCWD, (uint64_t)tmp, sizeof(tmp)));
    if (r < 0) return 0;
    if (!buf)
    {
        size_t need = (size_t)r + 1; if (size && size < need) { errno = ERANGE; return 0; }
        buf = (char*)malloc(need); if (!buf) { errno = ENOMEM; return 0; }
        memcpy(buf, tmp, need); return buf;
    }
    if ((size_t)r + 1 > size) { errno = ERANGE; return 0; }
    memcpy(buf, tmp, (size_t)r + 1);
    return buf;
}
int chdir(const char* path) { return (int)kret(user_syscall1(K_CHDIR, (uint64_t)path)); }
int dup3(int oldfd, int newfd, int flags)
{
    if (oldfd == newfd || (flags & ~O_CLOEXEC)) { errno = EINVAL; return -1; }
    int r = dup2(oldfd, newfd); if (r < 0) return -1;
    if (flags & O_CLOEXEC) (void)fcntl(newfd, F_SETFD, FD_CLOEXEC);
    return r;
}
int getpagesize(void) { return 4096; }
int getdtablesize(void) { return USER_FD_MAX_DESCRIPTORS; }

/* ---- directories ----------------------------------------------------------------------------------------------- */
struct RioDir { char* text; size_t len; size_t pos; int phase; struct dirent ent; unsigned serial; };
DIR* opendir(const char* path)
{
    struct stat st; if (stat(path, &st) < 0) return 0;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return 0; }
    size_t cap = 4096; char* buf = 0; long n = 0;
    for (;;)
    {
        char* nb = (char*)realloc(buf, cap); if (!nb) { free(buf); errno = ENOMEM; return 0; }
        buf = nb;
        n = readdir_text(path, buf, cap - 1);
        if (n < 0) { free(buf); return 0; }
        if ((size_t)n < cap - 64 || cap >= (1U << 20)) break;
        cap *= 2;
    }
    buf[n] = 0;
    DIR* d = (DIR*)calloc(1, sizeof(struct RioDir)); if (!d) { free(buf); errno = ENOMEM; return 0; }
    d->text = buf; d->len = (size_t)n;
    return d;
}
struct dirent* readdir(DIR* d)
{
    if (!d) { errno = EBADF; return 0; }
    d->ent.d_ino = ++d->serial;
    if (d->phase < 2)
    {
        d->ent.d_type = DT_DIR; d->ent.d_name[0] = '.'; d->ent.d_name[1] = d->phase == 1 ? '.' : 0; d->ent.d_name[2] = 0;
        ++d->phase; return &d->ent;
    }
    while (d->pos < d->len)
    {
        const char* line = d->text + d->pos; const char* nl = line; while (*nl && *nl != '\n') ++nl;
        d->pos = (size_t)(nl - d->text) + (*nl ? 1 : 0);
        char kind = line[0]; const char* p = line + 1; while (*p == ' ') ++p; while (*p >= '0' && *p <= '9') ++p; while (*p == ' ') ++p;
        size_t l = (size_t)(nl - p); if (l == 0 || kind == 0) continue;
        if (l > sizeof(d->ent.d_name) - 1) l = sizeof(d->ent.d_name) - 1;
        memcpy(d->ent.d_name, p, l); d->ent.d_name[l] = 0;
        d->ent.d_type = kind == 'd' ? DT_DIR : kind == 'l' ? DT_LNK : DT_REG;
        return &d->ent;
    }
    return 0;
}
int closedir(DIR* d) { if (!d) { errno = EBADF; return -1; } free(d->text); free(d); return 0; }
void rewinddir(DIR* d) { if (d) { d->pos = 0; d->phase = 0; d->serial = 0; } }
int dirfd(DIR* d) { (void)d; errno = ENOTSUP; return -1; }       /* directories are not descriptors in this kernel */
int alphasort(const struct dirent** a, const struct dirent** b) { return strcmp((*a)->d_name, (*b)->d_name); }
int scandir(const char* path, struct dirent*** list, int (*filter)(const struct dirent*), int (*cmp)(const struct dirent**, const struct dirent**))
{
    DIR* d = opendir(path); if (!d) return -1;
    size_t n = 0, cap = 16; struct dirent** v = (struct dirent**)malloc(cap * sizeof(*v));
    if (!v) { closedir(d); errno = ENOMEM; return -1; }
    struct dirent* e;
    while ((e = readdir(d)) != 0)
    {
        if (filter && !filter(e)) continue;
        if (n == cap) { cap *= 2; struct dirent** nv = (struct dirent**)realloc(v, cap * sizeof(*v)); if (!nv) goto fail; v = nv; }
        v[n] = (struct dirent*)malloc(sizeof(struct dirent)); if (!v[n]) goto fail;
        *v[n] = *e; ++n;
    }
    closedir(d);
    if (cmp && n > 1) qsort(v, n, sizeof(*v), (int (*)(const void*, const void*))cmp);
    *list = v; return (int)n;
fail:
    for (size_t i = 0; i < n; ++i) free(v[i]);
    free(v); closedir(d); errno = ENOMEM; return -1;
}

/* ---- fnmatch / glob ------------------------------------------------------------------------------------------- */
static int lower_c(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int class_match(const char* name, size_t l, int c)
{
#define CL(n, cond) if (l == sizeof(n) - 1 && !memcmp(name, n, l)) return (cond)
    CL("alpha", (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')); CL("digit", c >= '0' && c <= '9');
    CL("alnum", (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')); CL("upper", c >= 'A' && c <= 'Z');
    CL("lower", c >= 'a' && c <= 'z'); CL("space", c == ' ' || (c >= 9 && c <= 13)); CL("blank", c == ' ' || c == '\t');
    CL("punct", c > 32 && c < 127 && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')));
    CL("print", c >= 32 && c < 127); CL("graph", c > 32 && c < 127); CL("cntrl", c < 32 || c == 127);
    CL("xdigit", (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
#undef CL
    return 0;
}
/* p points after '['. Returns the position after ']' (or 0 when the bracket is unterminated) and sets *ok. */
static const char* bracket_match(const char* p, int c, int flags, int* ok)
{
    int neg = 0, hit = 0, first = 1;
    if (*p == '!' || *p == '^') { neg = 1; ++p; }
    int cc = (flags & FNM_CASEFOLD) ? lower_c(c) : c;
    for (;;)
    {
        if (!*p) return 0;
        if (*p == ']' && !first) { ++p; break; }
        first = 0;
        if (p[0] == '[' && p[1] == ':')
        {
            const char* e = p + 2; while (*e && !(e[0] == ':' && e[1] == ']')) ++e;
            if (*e) { if (class_match(p + 2, (size_t)(e - (p + 2)), c)) hit = 1; p = e + 2; continue; }
        }
        int lo = (unsigned char)*p++;
        if (lo == '\\' && !(flags & FNM_NOESCAPE) && *p) lo = (unsigned char)*p++;
        int hi = lo;
        if (*p == '-' && p[1] && p[1] != ']')
        {
            ++p; hi = (unsigned char)*p++;
            if (hi == '\\' && !(flags & FNM_NOESCAPE) && *p) hi = (unsigned char)*p++;
        }
        if (flags & FNM_CASEFOLD) { lo = lower_c(lo); hi = lower_c(hi); }
        if (cc >= lo && cc <= hi) hit = 1;
    }
    *ok = neg ? !hit : hit;
    return p;
}
static int fn_rec(const char* p, const char* s, int flags, int lead)
{
    for (;;)
    {
        char pc = *p;
        if (pc == 0) return *s == 0;
        if (pc == '*')
        {
            while (*p == '*') ++p;
            if ((flags & FNM_PERIOD) && lead && *s == '.') return 0;
            if (*p == 0) return (flags & FNM_PATHNAME) ? strchr(s, '/') == 0 : 1;
            for (;;)
            {
                if (fn_rec(p, s, flags, 0)) return 1;
                if (*s == 0 || ((flags & FNM_PATHNAME) && *s == '/')) return 0;
                ++s;
            }
        }
        if (*s == 0) return 0;
        int c = (unsigned char)*s;
        if ((flags & FNM_PATHNAME) && c == '/') { if (pc != '/') return 0; }
        if (pc == '?')
        {
            if ((flags & FNM_PATHNAME) && c == '/') return 0;
            if ((flags & FNM_PERIOD) && lead && c == '.') return 0;
            ++p;
        }
        else if (pc == '[')
        {
            if ((flags & FNM_PATHNAME) && c == '/') return 0;
            if ((flags & FNM_PERIOD) && lead && c == '.') return 0;
            int ok = 0; const char* e = bracket_match(p + 1, c, flags, &ok);
            if (e) { if (!ok) return 0; p = e; }
            else { if (c != '[') return 0; ++p; }
        }
        else
        {
            if (pc == '\\' && !(flags & FNM_NOESCAPE) && p[1]) { ++p; pc = *p; }
            int a = (unsigned char)pc, b = c;
            if (flags & FNM_CASEFOLD) { a = lower_c(a); b = lower_c(b); }
            if (a != b) return 0;
            ++p;
        }
        ++s;
        lead = (flags & FNM_PATHNAME) && c == '/';
    }
}
int fnmatch(const char* pattern, const char* string, int flags) { return fn_rec(pattern, string, flags, 1) ? 0 : FNM_NOMATCH; }

struct GlobList { char** v; size_t n, cap; int oom; };
static int glob_add(GlobList* L, const char* s, int mark_dir)
{
    if (L->n >= 2048) { L->oom = 1; return -1; }
    if (L->n == L->cap)
    {
        size_t nc = L->cap ? L->cap * 2 : 16; char** nv = (char**)realloc(L->v, nc * sizeof(char*));
        if (!nv) { L->oom = 1; return -1; }
        L->v = nv; L->cap = nc;
    }
    size_t l = strlen(s); char* d = (char*)malloc(l + 2); if (!d) { L->oom = 1; return -1; }
    memcpy(d, s, l); if (mark_dir) d[l++] = '/'; d[l] = 0;
    L->v[L->n++] = d; return 0;
}
static int glob_magic(const char* s, size_t l) { for (size_t i = 0; i < l; ++i) { if (s[i] == '\\') { ++i; continue; } if (s[i] == '*' || s[i] == '?' || s[i] == '[') return 1; } return 0; }
static int str_cmp_v(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
struct GlobCtx { int flags; int (*errfn)(const char*, int); int aborted; };

static int glob_walk(const char* base, const char* pat, GlobCtx* cx, GlobList* L)
{
    while (*pat == '/') { /* collapse separators */ size_t bl = strlen(base); char nb[256]; if (bl + 2 >= sizeof(nb)) return 0; memcpy(nb, base, bl); if (bl == 0 || nb[bl - 1] != '/') nb[bl++] = '/'; nb[bl] = 0; base = nb; while (*pat == '/') ++pat; return *pat ? glob_walk(nb, pat, cx, L) : (glob_add(L, nb, 0), 0); }
    const char* sl = strchr(pat, '/'); size_t cl = sl ? (size_t)(sl - pat) : strlen(pat);
    const char* rest = sl ? sl + 1 : pat + cl; int trailing = sl != 0; while (*rest == '/') ++rest;
    char comp[128]; if (cl >= sizeof(comp)) return 0; memcpy(comp, pat, cl); comp[cl] = 0;
    size_t bl = strlen(base);
    if (!glob_magic(comp, cl))
    {
        char un[128]; size_t k = 0; for (size_t i = 0; i < cl && k + 1 < sizeof(un); ++i) { if (comp[i] == '\\' && i + 1 < cl) ++i; un[k++] = comp[i]; } un[k] = 0;
        char path[256]; if (bl + k + 2 >= sizeof(path)) return 0;
        memcpy(path, base, bl); memcpy(path + bl, un, k + 1);
        struct stat st; if (lstat(path, &st) < 0) return 0;
        if (*rest) { size_t pl = strlen(path); path[pl] = '/'; path[pl + 1] = 0; return glob_walk(path, rest, cx, L); }
        int isdir = S_ISDIR(st.st_mode);
        if (S_ISLNK(st.st_mode)) { struct stat s2; isdir = stat(path, &s2) == 0 && S_ISDIR(s2.st_mode); }
        if (trailing && !isdir) return 0;
        return glob_add(L, path, (isdir && ((cx->flags & GLOB_MARK) || trailing)) ? 1 : 0);
    }
    DIR* d = opendir(bl ? base : ".");
    if (!d)
    {
        if ((cx->flags & GLOB_ERR) && errno != ENOENT && errno != ENOTDIR) { cx->aborted = 1; }
        else if (cx->errfn && errno != ENOENT && errno != ENOTDIR && cx->errfn(base, errno)) cx->aborted = 1;
        return 0;
    }
    typedef char Name64[64]; Name64* names = (Name64*)malloc(256 * sizeof(Name64)); int nn = 0; struct dirent* e;
    if (!names) { closedir(d); L->oom = 1; return -1; }
    while ((e = readdir(d)) != 0 && nn < 256)
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) { if (comp[0] != '.') continue; }
        if (fnmatch(comp, e->d_name, FNM_PERIOD) != 0) continue;
        copy_str(names[nn++], e->d_name, 64);
    }
    closedir(d);
    for (int i = 0; i < nn && !cx->aborted && !L->oom; ++i)
    {
        char path[256]; size_t nl = strlen(names[i]); if (bl + nl + 2 >= sizeof(path)) continue;
        memcpy(path, base, bl); memcpy(path + bl, names[i], nl + 1);
        struct stat st; int have = stat(path, &st) == 0; int isdir = have && S_ISDIR(st.st_mode);
        if (*rest)
        {
            if (!isdir) continue;
            path[bl + nl] = '/'; path[bl + nl + 1] = 0; glob_walk(path, rest, cx, L);
        }
        else
        {
            if (trailing && !isdir) continue;
            glob_add(L, path, (isdir && ((cx->flags & GLOB_MARK) || trailing)) ? 1 : 0);
        }
    }
    free(names);
    return 0;
}
int glob(const char* pattern, int flags, int (*errfn)(const char*, int), glob_t* g)
{
    GlobList L = {0, 0, 0, 0}; GlobCtx cx = {flags, errfn, 0};
    g->gl_pathc = 0; g->gl_pathv = 0;
    if (!pattern || !*pattern) return GLOB_NOMATCH;
    glob_walk("", pattern, &cx, &L);
    if (L.oom) { for (size_t i = 0; i < L.n; ++i) free(L.v[i]); free(L.v); return GLOB_NOSPACE; }
    if (cx.aborted) { for (size_t i = 0; i < L.n; ++i) free(L.v[i]); free(L.v); return GLOB_ABORTED; }
    if (L.n == 0)
    {
        if (!(flags & GLOB_NOCHECK)) return GLOB_NOMATCH;
        if (glob_add(&L, pattern, 0) < 0) return GLOB_NOSPACE;
    }
    else if (!(flags & GLOB_NOSORT) && L.n > 1) qsort(L.v, L.n, sizeof(char*), str_cmp_v);
    char** v = (char**)realloc(L.v, (L.n + 1) * sizeof(char*)); if (!v) { for (size_t i = 0; i < L.n; ++i) free(L.v[i]); free(L.v); return GLOB_NOSPACE; }
    v[L.n] = 0; g->gl_pathv = v; g->gl_pathc = L.n;
    return 0;
}
void globfree(glob_t* g) { if (g->gl_pathv) { for (size_t i = 0; i < g->gl_pathc; ++i) free(g->gl_pathv[i]); free(g->gl_pathv); } g->gl_pathv = 0; g->gl_pathc = 0; }

/* ---- temporary files, basename/dirname ---------------------------------------------------------------------------- */
static void fill_x(char* tail)
{
    static const char al[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned char r[6]; getrandom(r, 6, 0);
    for (int i = 0; i < 6; ++i) tail[i] = al[r[i] % 62];
}
static char* tmpl_tail(char* t) { size_t l = strlen(t); if (l < 6 || strcmp(t + l - 6, "XXXXXX") != 0) { errno = EINVAL; return 0; } return t + l - 6; }
int mkstemp(char* tmpl)
{
    char* tail = tmpl_tail(tmpl); if (!tail) return -1;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        fill_x(tail);
        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL);
        if (fd >= 0) { (void)chmod(tmpl, 0600); return fd; }
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST; return -1;
}
char* mkdtemp(char* tmpl)
{
    char* tail = tmpl_tail(tmpl); if (!tail) return 0;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        fill_x(tail);
        if (mkdir(tmpl, 0700) == 0) { (void)chmod(tmpl, 0700); return tmpl; }
        if (errno != EEXIST) return 0;
    }
    errno = EEXIST; return 0;
}
char* basename(char* path)
{
    static char dot[] = ".", slash[] = "/";
    if (!path || !*path) return dot;
    size_t l = strlen(path);
    while (l > 1 && path[l - 1] == '/') path[--l] = 0;
    if (l == 1 && path[0] == '/') return slash;
    char* p = strrchr(path, '/');
    return p ? p + 1 : path;
}
char* dirname(char* path)
{
    static char dot[] = ".", slash[] = "/";
    if (!path || !*path) return dot;
    size_t l = strlen(path);
    while (l > 1 && path[l - 1] == '/') path[--l] = 0;
    char* p = strrchr(path, '/');
    if (!p) return dot;
    while (p > path && p[-1] == '/') --p;
    if (p == path) return slash;
    *p = 0; return path;
}

/* ---- termios / tty ------------------------------------------------------------------------------------------------- */
int isatty(int fd)
{
    struct stat st; if (fstat(fd, &st) < 0) return 0;
    if (!S_ISCHR(st.st_mode)) { errno = ENOTTY; return 0; }
    return 1;
}
int tcgetattr(int fd, struct termios* t)
{
    if (!isatty(fd)) return -1;
    return (int)kret(user_syscall2(K_TTY, 0, (uint64_t)t));
}
int tcsetattr(int fd, int action, const struct termios* t)
{
    if (action < TCSANOW || action > TCSAFLUSH) { errno = EINVAL; return -1; }
    if (!isatty(fd)) return -1;
    return (int)kret(user_syscall3(K_TTY, 1, (uint64_t)t, (uint64_t)action));
}
void cfmakeraw(struct termios* t)
{
    t->c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK | ICANON | ISIG);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1; t->c_cc[VTIME] = 0;
}
speed_t cfgetispeed(const struct termios* t) { return t->c_ispeed; }
speed_t cfgetospeed(const struct termios* t) { return t->c_ospeed; }
int cfsetispeed(struct termios* t, speed_t s) { t->c_ispeed = s; return 0; }
int cfsetospeed(struct termios* t, speed_t s) { t->c_ospeed = s; return 0; }
int tcflush(int fd, int q)
{
    if (q < TCIFLUSH || q > TCIOFLUSH) { errno = EINVAL; return -1; }
    struct termios t; if (tcgetattr(fd, &t) < 0) return -1;
    return tcsetattr(fd, TCSAFLUSH, &t);        /* TCSAFLUSH discards pending input */
}
int tcdrain(int fd) { return isatty(fd) ? 0 : -1; }
int tcflow(int fd, int a) { (void)a; return isatty(fd) ? 0 : -1; }
char* ttyname(int fd) { static char n[] = "/dev/console"; return isatty(fd) ? n : 0; }
int ioctl(int fd, unsigned long req, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, req);
    void* arg = __builtin_va_arg(ap, void*);
    __builtin_va_end(ap);
    switch (req)
    {
        case TIOCGWINSZ:
            if (!isatty(fd)) return -1;
            return (int)kret(user_syscall2(K_TTY, 2, (uint64_t)arg));
        case TCGETS: return tcgetattr(fd, (struct termios*)arg);
        case TCSETS: return tcsetattr(fd, TCSANOW, (const struct termios*)arg);
        case TCSETSW: return tcsetattr(fd, TCSADRAIN, (const struct termios*)arg);
        case TCSETSF: return tcsetattr(fd, TCSAFLUSH, (const struct termios*)arg);
        case FIONBIO:
        {
            int fl = fcntl(fd, F_GETFL); if (fl < 0) return -1;
            fl = (arg && *(int*)arg) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
            return fcntl(fd, F_SETFL, fl) < 0 ? -1 : 0;
        }
        default: errno = ENOTTY; return -1;
    }
}

/* ---- users and groups (the kernel account table) ------------------------------------------------------------------------ */
struct Account { char name[17]; uid_t uid; gid_t gid; };
static int account_at(int idx, Account* a)
{
    uint8_t raw[24];
    if (idx < 0 || (long)user_syscall2(K_GETPW, (uint64_t)idx, (uint64_t)raw) != 0) return 0;
    memcpy(a->name, raw, 16); a->name[16] = 0;
    uint32_t u, g; memcpy(&u, raw + 16, 4); memcpy(&g, raw + 20, 4); a->uid = u; a->gid = g;
    return 1;
}
static void pw_fill(struct passwd* pw, const Account* a, char* buf, size_t cap)
{
    /* layout in buf: name, dir, shell, empty string */
    size_t nl = strlen(a->name) + 1; const char* home_pre = a->uid == 0 ? "/root" : "/home/";
    char dir[48]; if (a->uid == 0) copy_str(dir, home_pre, sizeof(dir)); else snprintf(dir, sizeof(dir), "%s%s", home_pre, a->name);
    size_t dl = strlen(dir) + 1; const char* shell = "/bin/sh"; size_t sl = strlen(shell) + 1;
    (void)cap;
    char* p = buf; memcpy(p, a->name, nl); pw->pw_name = p; p += nl;
    memcpy(p, dir, dl); pw->pw_dir = p; p += dl;
    memcpy(p, shell, sl); pw->pw_shell = p; p += sl;
    *p = 0; pw->pw_passwd = p; pw->pw_gecos = p;
    pw->pw_uid = a->uid; pw->pw_gid = a->gid;
}
#define PW_NEED 160
static int pw_lookup(const char* name, int by_uid, uid_t uid, Account* out)
{
    for (int i = 0; i < 64; ++i)
    {
        Account a; if (!account_at(i, &a)) break;
        if (by_uid ? a.uid == uid : !strcmp(a.name, name)) { *out = a; return 1; }
    }
    return 0;
}
static int pw_r(const char* name, int by_uid, uid_t uid, struct passwd* pw, char* buf, size_t cap, struct passwd** res)
{
    *res = 0; Account a;
    if (!by_uid && !name) return EINVAL;
    if (!pw_lookup(name, by_uid, uid, &a)) return 0;
    if (cap < PW_NEED) return ERANGE;
    pw_fill(pw, &a, buf, cap); *res = pw; return 0;
}
int getpwnam_r(const char* n, struct passwd* pw, char* buf, size_t cap, struct passwd** res) { return pw_r(n, 0, 0, pw, buf, cap, res); }
int getpwuid_r(uid_t u, struct passwd* pw, char* buf, size_t cap, struct passwd** res) { return pw_r(0, 1, u, pw, buf, cap, res); }
static struct passwd g_pw; static char g_pwbuf[PW_NEED];
struct passwd* getpwnam(const char* n) { struct passwd* r; int e = getpwnam_r(n, &g_pw, g_pwbuf, sizeof(g_pwbuf), &r); if (e) errno = e; return r; }
struct passwd* getpwuid(uid_t u) { struct passwd* r; int e = getpwuid_r(u, &g_pw, g_pwbuf, sizeof(g_pwbuf), &r); if (e) errno = e; return r; }
static int pw_iter;
struct passwd* getpwent(void)
{
    Account a; if (!account_at(pw_iter, &a)) return 0;
    ++pw_iter; pw_fill(&g_pw, &a, g_pwbuf, sizeof(g_pwbuf)); return &g_pw;
}
void setpwent(void) { pw_iter = 0; }
void endpwent(void) { pw_iter = 0; }

static struct group g_gr; static char g_grname[20]; static char* g_grmem[1];
static struct group* gr_from(const Account* a) { copy_str(g_grname, a->name, sizeof(g_grname)); g_grmem[0] = 0; g_gr.gr_name = g_grname; g_gr.gr_passwd = g_grname + strlen(g_grname); g_gr.gr_gid = a->gid; g_gr.gr_mem = g_grmem; return &g_gr; }
struct group* getgrgid(gid_t g)
{
    for (int i = 0; i < 64; ++i) { Account a; if (!account_at(i, &a)) break; if (a.gid == g) return gr_from(&a); }
    return 0;
}
struct group* getgrnam(const char* n)
{
    for (int i = 0; i < 64; ++i) { Account a; if (!account_at(i, &a)) break; if (!strcmp(a.name, n)) return gr_from(&a); }
    return 0;
}
static int gr_iter;
struct group* getgrent(void)
{
    for (;;)
    {
        Account a; if (!account_at(gr_iter, &a)) return 0;
        ++gr_iter;
        int dup = 0; for (int j = 0; j < gr_iter - 1; ++j) { Account b; if (account_at(j, &b) && b.gid == a.gid) { dup = 1; break; } }
        if (!dup) return gr_from(&a);
    }
}
void setgrent(void) { gr_iter = 0; }
void endgrent(void) { gr_iter = 0; }
char* getlogin(void) { struct passwd* p = getpwuid(getuid()); return p ? p->pw_name : 0; }
int getgroups(int size, gid_t list[])
{
    if (size == 0) return 1;
    if (size < 1) { errno = EINVAL; return -1; }
    list[0] = getgid(); return 1;
}

/* ---- system information and limits ------------------------------------------------------------------------------------------ */
int gethostname(char* name, size_t len)
{
    char h[64] = "rioos";
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd >= 0)
    {
        long n = read(fd, h, sizeof(h) - 1); close(fd);
        if (n > 0) { h[n] = 0; for (long i = 0; i < n; ++i) if (h[i] == '\n' || h[i] == '\r') { h[i] = 0; break; } if (!h[0]) copy_str(h, "rioos", sizeof(h)); }
    }
    if (strlen(h) + 1 > len) { errno = ENAMETOOLONG; return -1; }
    strcpy(name, h); return 0;
}
int sethostname(const char* name, size_t len)
{
    if (len == 0 || len > 63) { errno = EINVAL; return -1; }
    int fd = open("/etc/hostname", O_WRONLY | O_CREAT | O_TRUNC); if (fd < 0) return -1;
    char b[65]; memcpy(b, name, len); b[len] = '\n';
    long w = write(fd, b, len + 1); close(fd);
    return w == (long)len + 1 ? 0 : -1;
}
int uname(struct utsname* u)
{
    if (!u) { errno = EFAULT; return -1; }
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "RioOS"); strcpy(u->release, "1.0"); strcpy(u->version, "RioOS AArch64"); strcpy(u->machine, "aarch64");
    gethostname(u->nodename, sizeof(u->nodename));
    return 0;
}
int getrlimit(int res, struct rlimit* r)
{
    if (res < 0 || res > 9 || !r) { errno = EINVAL; return -1; }
    uint64_t out[2] = {0, 0};
    if (kret(user_syscall5(K_RLIMIT, 0, (uint64_t)res, 0, 0, (uint64_t)out)) < 0) return -1;
    r->rlim_cur = (rlim_t)out[0]; r->rlim_max = (rlim_t)out[1]; return 0;
}
/* enforced by the kernel: NOFILE (descriptor allocation), FSIZE (SIGXFSZ/EFBIG), NPROC (fork -> EAGAIN, non-root), CPU (SIGXCPU at the
   soft limit, SIGKILL at the hard limit), AS/DATA (heap + mmap growth -> ENOMEM). Inherited by fork. STACK/CORE/RSS/MEMLOCK are only recorded. */
int setrlimit(int res, const struct rlimit* r)
{
    if (res < 0 || res > 9 || !r || r->rlim_cur > r->rlim_max) { errno = EINVAL; return -1; }
    return kret(user_syscall5(K_RLIMIT, 1, (uint64_t)res, (uint64_t)r->rlim_cur, (uint64_t)r->rlim_max, 0)) < 0 ? -1 : 0;
}
int getrusage(int who, struct rusage* ru)
{
    if ((who != RUSAGE_SELF && who != RUSAGE_CHILDREN) || !ru) { errno = EINVAL; return -1; }
    uint64_t v[4] = {0, 0, 0, 0};
    memset(ru, 0, sizeof(*ru));
    if (kret(user_syscall2(K_RUSAGE, who == RUSAGE_CHILDREN ? 1 : 0, (uint64_t)v)) < 0) return -1;
    ru->ru_utime.tv_sec = (long)(v[0] / 1000U); ru->ru_utime.tv_usec = (long)(v[0] % 1000U) * 1000L;
    ru->ru_stime.tv_sec = (long)(v[1] / 1000U); ru->ru_stime.tv_usec = (long)(v[1] % 1000U) * 1000L;
    ru->ru_nvcsw = (long)v[3];
    return 0;
}
long sysconf(int name)
{
    uint64_t si[5] = {0, 0, 1, 0, 0};
    switch (name)
    {
        case _SC_ARG_MAX: return 4096;
        case _SC_CLK_TCK: return 100;
        case _SC_OPEN_MAX: return USER_FD_MAX_DESCRIPTORS;
        case _SC_PAGESIZE: return 4096;
        case _SC_LINE_MAX: return 2048;
        case _SC_HOST_NAME_MAX: return 63;
        case _SC_NGROUPS_MAX: return 1;
        case _SC_PHYS_PAGES: case _SC_AVPHYS_PAGES: case _SC_NPROCESSORS_CONF: case _SC_NPROCESSORS_ONLN:
            if (kret(user_syscall1(K_SYSINFO, (uint64_t)si)) < 0) return -1;
            return name == _SC_PHYS_PAGES ? (long)si[0] : name == _SC_AVPHYS_PAGES ? (long)si[1] : (long)si[2];
        default: errno = EINVAL; return -1;
    }
}
long pathconf(const char* path, int name)
{
    struct stat st; if (stat(path, &st) < 0) return -1;
    switch (name) { case _PC_NAME_MAX: return NAME_MAX; case _PC_PATH_MAX: return PATH_MAX; case _PC_PIPE_BUF: return PIPE_BUF; default: errno = EINVAL; return -1; }
}
long fpathconf(int fd, int name)
{
    struct stat st; if (fstat(fd, &st) < 0) return -1;
    switch (name) { case _PC_NAME_MAX: return NAME_MAX; case _PC_PATH_MAX: return PATH_MAX; case _PC_PIPE_BUF: return PIPE_BUF; default: errno = EINVAL; return -1; }
}
pid_t wait4(pid_t pid, int* status, int options, struct rusage* ru)
{
    if (ru) memset(ru, 0, sizeof(*ru));
    return waitpid(pid, status, options);
}
pid_t wait3(int* status, int options, struct rusage* ru) { return wait4(-1, status, options, ru); }
void _Exit(int code) { _exit(code); }
extern int __rio_exit_status;
struct OnExit { void (*fn)(int, void*); void* arg; };
static OnExit on_exit_tab[32]; static int on_exit_n;
static void run_on_exit(void) { while (on_exit_n > 0) { OnExit e = on_exit_tab[--on_exit_n]; e.fn(__rio_exit_status, e.arg); } }
int on_exit(void (*fn)(int, void*), void* arg)
{
    if (!fn || on_exit_n >= 32) { errno = ENOMEM; return -1; }
    if (on_exit_n == 0 && atexit(run_on_exit) < 0) return -1;
    on_exit_tab[on_exit_n].fn = fn; on_exit_tab[on_exit_n].arg = arg; ++on_exit_n; return 0;
}
int mlock(const void* a, size_t n) { (void)a; (void)n; return 0; }          /* memory is never swapped: always resident */
int munlock(const void* a, size_t n) { (void)a; (void)n; return 0; }
int mlockall(int f) { (void)f; return 0; }
int munlockall(void) { return 0; }
int madvise(void* a, size_t n, int advice) { (void)a; (void)n; if (advice < 0) { errno = EINVAL; return -1; } return 0; }
int posix_madvise(void* a, size_t n, int advice) { return madvise(a, n, advice) < 0 ? EINVAL : 0; }

/* ---- random numbers ------------------------------------------------------------------------------------------------------ */
ssize_t getrandom(void* buf, size_t n, unsigned flags)
{
    (void)flags; size_t done = 0;
    while (done < n)
    {
        size_t c = n - done; if (c > 4096) c = 4096;
        long r = kret(user_syscall2(K_GETRANDOM, (uint64_t)((char*)buf + done), c));
        if (r < 0) return done ? (ssize_t)done : -1;
        done += (size_t)r;
    }
    return (ssize_t)done;
}
int getentropy(void* buf, size_t n) { if (n > 256) { errno = EIO; return -1; } return getrandom(buf, n, 0) == (ssize_t)n ? 0 : -1; }
uint32_t arc4random(void) { uint32_t v = 0; getrandom(&v, 4, 0); return v; }
void arc4random_buf(void* buf, size_t n) { getrandom(buf, n, 0); }
uint32_t arc4random_uniform(uint32_t bound)
{
    if (bound < 2) return 0;
    uint32_t min = (uint32_t)(-bound) % bound, r;
    do { r = arc4random(); } while (r < min);
    return r % bound;
}
static uint64_t rnd_state = 1;
void srandom(unsigned seed) { rnd_state = seed ? seed : 1; }
long random(void)
{
    uint64_t x = rnd_state; x ^= x >> 12; x ^= x << 25; x ^= x >> 27; rnd_state = x;
    return (long)((x * 0x2545F4914F6CDD1DULL) >> 33);
}
int rand_r(unsigned* seed)
{
    unsigned next = *seed; int result;
    next *= 1103515245; next += 12345; result = (int)((next / 65536) % 2048);
    next *= 1103515245; next += 12345; result <<= 10; result ^= (int)((next / 65536) % 1024);
    next *= 1103515245; next += 12345; result <<= 10; result ^= (int)((next / 65536) % 1024);
    *seed = next; return result;
}
static uint64_t x48 = 0x1234ABCD330EULL;
static uint64_t next48(uint64_t* x) { *x = (*x * 0x5DEECE66DULL + 0xBULL) & 0xFFFFFFFFFFFFULL; return *x; }
void srand48(long seed) { x48 = (((uint64_t)seed & 0xFFFFFFFFULL) << 16) | 0x330EULL; }
double drand48(void) { return (double)next48(&x48) / 281474976710656.0; }
long lrand48(void) { return (long)(next48(&x48) >> 17); }
long mrand48(void) { return (long)(int32_t)(uint32_t)(next48(&x48) >> 16); }
double erand48(unsigned short x[3])
{
    uint64_t s = (uint64_t)x[0] | ((uint64_t)x[1] << 16) | ((uint64_t)x[2] << 32);
    next48(&s); x[0] = (unsigned short)s; x[1] = (unsigned short)(s >> 16); x[2] = (unsigned short)(s >> 32);
    return (double)s / 281474976710656.0;
}

/* ---- shared memory and named semaphores (files in /tmp, shared mappings are coherent) ------------------------------------ */
static int ipc_path(char* out, size_t cap, const char* prefix, const char* name)
{
    if (!name || name[0] != '/' || name[1] == 0 || strchr(name + 1, '/')) { errno = EINVAL; return -1; }
    if (strlen(prefix) + strlen(name) >= cap || strlen(name) > 40) { errno = ENAMETOOLONG; return -1; }
    strcpy(out, prefix); strcat(out, name + 1); return 0;
}
static void ensure_tmp(void) { (void)mkdir("/tmp", 0777); }
int shm_open(const char* name, int oflag, mode_t mode)
{
    (void)mode; char path[96]; if (ipc_path(path, sizeof(path), "/tmp/shm.", name) < 0) return -1;
    ensure_tmp();
    return open(path, oflag & (O_RDWR | O_CREAT | O_EXCL | O_TRUNC | O_WRONLY | O_CLOEXEC));
}
int shm_unlink(const char* name) { char path[96]; if (ipc_path(path, sizeof(path), "/tmp/shm.", name) < 0) return -1; return unlink(path); }
sem_t* sem_open(const char* name, int oflag, ...)
{
    char path[96]; if (ipc_path(path, sizeof(path), "/tmp/sem.", name) < 0) return SEM_FAILED;
    unsigned value = 0;
    if (oflag & O_CREAT)
    {
        __builtin_va_list ap; __builtin_va_start(ap, oflag);
        (void)__builtin_va_arg(ap, unsigned); value = __builtin_va_arg(ap, unsigned);
        __builtin_va_end(ap);
        if (value > 0x7FFFFFFFU) { errno = EINVAL; return SEM_FAILED; }
    }
    ensure_tmp();
    int created = 0, fd = -1;
    if (oflag & O_CREAT)
    {
        fd = open(path, O_RDWR | O_CREAT | O_EXCL);
        if (fd >= 0) created = 1;
        else if (errno != EEXIST || (oflag & O_EXCL)) return SEM_FAILED;
    }
    if (fd < 0) { fd = open(path, O_RDWR); if (fd < 0) return SEM_FAILED; }
    if (created && ftruncate(fd, sizeof(sem_t)) < 0) { close(fd); unlink(path); return SEM_FAILED; }
    for (int i = 0; i < 200 && !created; ++i)       /* creator may still be sizing the file */
    {
        struct stat st; if (fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(sem_t)) break;
        sleep_ms(1);
    }
    void* m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { if (created) unlink(path); return SEM_FAILED; }
    sem_t* s = (sem_t*)m;
    if (created) { s->shared = 1; __atomic_store_n(&s->value, value, __ATOMIC_SEQ_CST); }
    return s;
}
int sem_close(sem_t* s) { return munmap(s, 4096); }
int sem_unlink(const char* name) { char path[96]; if (ipc_path(path, sizeof(path), "/tmp/sem.", name) < 0) return -1; return unlink(path); }

/* ---- timerfd / clock_nanosleep -------------------------------------------------------------------------------------------------- */
static unsigned char tfd_clock[USER_FD_MAX_DESCRIPTORS];
static long ts_to_ms_ceil(const struct timespec* t)
{
    if (t->tv_sec < 0 || t->tv_nsec < 0 || t->tv_nsec >= 1000000000L) return -1;
    if (t->tv_sec > 2000000000L) return 2000000000000L;
    long ms = t->tv_sec * 1000L + t->tv_nsec / 1000000L;
    if (t->tv_nsec % 1000000L) ++ms;
    return ms;
}
int timerfd_create(int clockid, int flags)
{
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC) { errno = EINVAL; return -1; }
    if (flags & ~(O_NONBLOCK | O_CLOEXEC)) { errno = EINVAL; return -1; }
    int fd = (int)kret(user_syscall3(K_TIMERFD, 0, (uint64_t)(unsigned)flags, 0));
    if (fd >= 0 && fd < USER_FD_MAX_DESCRIPTORS) tfd_clock[fd] = (unsigned char)clockid;
    return fd;
}
static void ms_to_ts(uint64_t ms, struct timespec* t) { t->tv_sec = (long)(ms / 1000); t->tv_nsec = (long)(ms % 1000) * 1000000L; }
int timerfd_settime(int fd, int flags, const struct itimerspec* nv, struct itimerspec* old)
{
    if (!nv || (flags & ~TFD_TIMER_ABSTIME)) { errno = EINVAL; return -1; }
    long first = ts_to_ms_ceil(&nv->it_value), iv = ts_to_ms_ceil(&nv->it_interval);
    if (first < 0 || iv < 0) { errno = EINVAL; return -1; }
    if (first > 0 && (flags & TFD_TIMER_ABSTIME))
    {
        struct timespec now; int clk = (fd >= 0 && fd < USER_FD_MAX_DESCRIPTORS) ? tfd_clock[fd] : CLOCK_MONOTONIC;
        clock_gettime(clk, &now);
        long nowms = ts_to_ms_ceil(&now); first -= nowms; if (first <= 0) first = 1;
    }
    uint64_t o[2] = {0, 0};
    if (kret(user_syscall5(K_TIMERFD, 1, (uint64_t)(long)fd, (uint64_t)first, (uint64_t)iv, (uint64_t)o)) < 0) return -1;
    if (old) { ms_to_ts(o[0], &old->it_value); ms_to_ts(o[1], &old->it_interval); }
    return 0;
}
int timerfd_gettime(int fd, struct itimerspec* cur)
{
    uint64_t o[2];
    if (kret(user_syscall3(K_TIMERFD, 2, (uint64_t)(long)fd, (uint64_t)o)) < 0) return -1;
    ms_to_ts(o[0], &cur->it_value); ms_to_ts(o[1], &cur->it_interval); return 0;
}
int clock_nanosleep(int clk, int flags, const struct timespec* req, struct timespec* rem)
{
    if ((clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC) || !req || (flags & ~TIMER_ABSTIME)) return EINVAL;
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) return EINVAL;
    struct timespec rel = *req;
    if (flags & TIMER_ABSTIME)
    {
        struct timespec now; clock_gettime(clk, &now);
        rel.tv_sec = req->tv_sec - now.tv_sec; rel.tv_nsec = req->tv_nsec - now.tv_nsec;
        if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000L; --rel.tv_sec; }
        if (rel.tv_sec < 0) return 0;
        rem = 0;
    }
    if (nanosleep(&rel, rem) < 0) return errno;
    return 0;
}

/* ---- epoll over poll(); the descriptor is an eventfd that only reserves the number ----------------------------------------------- */
struct EpEntry { int fd; uint32_t events; epoll_data_t data; uint32_t last; int disabled; uint32_t gen; int has_gen; };
#define EP_MAX_WATCH 64
#define EP_MAX_INST 8
struct EpInst { int fd; int used; int n; EpEntry e[EP_MAX_WATCH]; };
static EpInst eps[EP_MAX_INST];
static EpInst* ep_find(int fd) { for (int i = 0; i < EP_MAX_INST; ++i) if (eps[i].used && eps[i].fd == fd) return &eps[i]; return 0; }
int __rio_ep_active = 0;
void __rio_epoll_close(int fd) { EpInst* e = ep_find(fd); if (e) { e->used = 0; --__rio_ep_active; } }
/* Edge detection.  The kernel keeps an arrival counter per descriptor (data, EOF, error, new connection), so an edge for
   EPOLLIN/EPOLLHUP/EPOLLERR is "the counter moved since the last report", also when the data arrived while this process
   was doing nothing.  EPOLLOUT edges are not-writable -> writable transitions; I/O on the descriptor re-arms them. */
void __rio_epoll_rearm(int fd)
{
    for (int k = 0; k < EP_MAX_INST; ++k)
        if (eps[k].used) for (int i = 0; i < eps[k].n; ++i) if (eps[k].e[i].fd == fd) eps[k].e[i].last &= ~(uint32_t)EPOLLOUT;
}
static int ep_gen(int fd, uint32_t* g)
{
    long r = (long)user_syscall1(K_FDGEN, (uint64_t)fd);
    if (r <= 0) return 0;                       /* kind without a counter (file, tty) or error */
    *g = (uint32_t)((unsigned long)r & 0xFFFFFFFFUL); return 1;
}
int epoll_create1(int flags)
{
    if (flags & ~EPOLL_CLOEXEC) { errno = EINVAL; return -1; }
    int slot = -1; for (int i = 0; i < EP_MAX_INST; ++i) if (!eps[i].used) { slot = i; break; }
    if (slot < 0) { errno = EMFILE; return -1; }
    int fd = eventfd(0, flags & EPOLL_CLOEXEC); if (fd < 0) return -1;
    eps[slot].used = 1; eps[slot].fd = fd; eps[slot].n = 0; ++__rio_ep_active; return fd;
}
int epoll_create(int size) { if (size <= 0) { errno = EINVAL; return -1; } return epoll_create1(0); }
int epoll_ctl(int ep, int op, int fd, struct epoll_event* ev)
{
    EpInst* e = ep_find(ep); if (!e) { errno = EBADF; return -1; }
    if (fd == ep) { errno = EINVAL; return -1; }
    if (fcntl(fd, F_GETFD) < 0) { errno = EBADF; return -1; }
    int idx = -1; for (int i = 0; i < e->n; ++i) if (e->e[i].fd == fd) { idx = i; break; }
    if (op == EPOLL_CTL_ADD)
    {
        if (!ev) { errno = EFAULT; return -1; }
        if (idx >= 0) { errno = EEXIST; return -1; }
        if (e->n >= EP_MAX_WATCH) { errno = ENOSPC; return -1; }
        e->e[e->n].fd = fd; e->e[e->n].events = ev->events; e->e[e->n].data = ev->data; e->e[e->n].last = 0; e->e[e->n].disabled = 0; { uint32_t g; e->e[e->n].has_gen = ep_gen(fd, &g); e->e[e->n].gen = g - 1U; } ++e->n; return 0;
    }
    if (idx < 0) { errno = ENOENT; return -1; }
    if (op == EPOLL_CTL_DEL) { e->e[idx] = e->e[--e->n]; return 0; }
    if (op == EPOLL_CTL_MOD)
    {
        if (!ev) { errno = EFAULT; return -1; }
        e->e[idx].events = ev->events; e->e[idx].data = ev->data; e->e[idx].last = 0; e->e[idx].disabled = 0; { uint32_t g; e->e[idx].has_gen = ep_gen(e->e[idx].fd, &g); e->e[idx].gen = g - 1U; } return 0;
    }
    errno = EINVAL; return -1;
}
int epoll_wait(int ep, struct epoll_event* evs, int maxevents, int timeout_ms)
{
    EpInst* e = ep_find(ep); if (!e) { errno = EBADF; return -1; }
    if (maxevents <= 0 || !evs) { errno = EINVAL; return -1; }
    unsigned long deadline = timeout_ms < 0 ? 0 : clock_ms() + (unsigned long)timeout_ms;
    for (;;)
    {
        struct pollfd pf[EP_MAX_WATCH]; int map[EP_MAX_WATCH]; int np = 0, et_armed = 0;
        for (int i = 0; i < e->n; ++i)
        {
            if (e->e[i].disabled) continue;
            short pe = 0; uint32_t ev = e->e[i].events;
            if (ev & (EPOLLIN | EPOLLRDHUP)) pe |= POLLIN;
            if (ev & EPOLLOUT) pe |= POLLOUT;
            pf[np].fd = e->e[i].fd; pf[np].events = pe; pf[np].revents = 0; map[np++] = i;
            if ((ev & EPOLLET) && !e->e[i].has_gen && e->e[i].last) et_armed = 1;
        }
        long wait = -1;
        if (timeout_ms >= 0) { unsigned long now = clock_ms(); wait = now >= deadline ? 0 : (long)(deadline - now); }
        if (et_armed) wait = 0;                      /* an edge can only be noticed by sampling */
        int rc = np ? poll(pf, (nfds_t)np, (int)wait) : 0;
        if (rc < 0) return -1;
        int out = 0;
        for (int k = 0; k < np && out < maxevents; ++k)
        {
            EpEntry* en = &e->e[map[k]]; uint32_t ready = 0;
            if (pf[k].revents & POLLNVAL) { en->fd = -1; continue; }
            if (pf[k].revents & POLLIN) ready |= EPOLLIN;
            if (pf[k].revents & POLLOUT) ready |= EPOLLOUT;
            if (pf[k].revents & POLLERR) ready |= EPOLLERR;
            if (pf[k].revents & POLLHUP) ready |= EPOLLHUP | (en->events & EPOLLRDHUP);
            ready &= (en->events | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
            uint32_t report = ready;
            if (en->events & EPOLLET)
            {
                uint32_t g = 0, outb = ready & EPOLLOUT, inb = ready & ~(uint32_t)EPOLLOUT;
                report = outb & ~(en->last & EPOLLOUT);
                if (en->has_gen && ep_gen(en->fd, &g)) { if (inb && g != en->gen) { report |= inb; en->gen = g; } }
                else report |= inb & ~(en->last & ~(uint32_t)EPOLLOUT);
                en->last = ready;
            }
            if (!report) continue;
            evs[out].events = report; evs[out].data = en->data; ++out;
            if (en->events & EPOLLONESHOT) en->disabled = 1;
        }
        for (int i = 0; i < e->n; ) { if (e->e[i].fd < 0) e->e[i] = e->e[--e->n]; else ++i; }   /* closed descriptors drop out */
        if (out > 0) return out;
        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0 && clock_ms() >= deadline) return 0;
        if (et_armed || np == 0) sleep_ms(np == 0 && timeout_ms < 0 ? 10 : 1);
    }
}

/* ---- CPUs ---------------------------------------------------------------------------------------------------------------------- */
int sched_getcpu(void)
{
    uint64_t si[5]; if (kret(user_syscall1(K_SYSINFO, (uint64_t)si)) < 0) return -1;
    return (int)si[4];
}
int sched_getaffinity(pid_t pid, size_t size, cpu_set_t* set)
{
    if (!set || size < sizeof(cpu_set_t)) { errno = EINVAL; return -1; }
    long m = kret(user_syscall3(K_AFFINITY, 0, (uint64_t)pid, 0));
    if (m < 0) return -1;
    set->bits[0] = (unsigned long)m;
    return 0;
}
int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t* set)
{
    if (!set || size < sizeof(cpu_set_t)) { errno = EINVAL; return -1; }
    return kret(user_syscall3(K_AFFINITY, 1, (uint64_t)pid, (uint64_t)set->bits[0])) < 0 ? -1 : 0;
}

/* ---- signalfd / inotify ---------------------------------------------------------------------------------------------------------- */
int signalfd(int fd, const sigset_t* mask, int flags)
{
    if (!mask) { errno = EINVAL; return -1; }
    long r = kret(user_syscall3(K_SIGNALFD, (uint64_t)(int64_t)fd, (uint64_t)(*(const uint32_t*)mask), (uint64_t)flags));
    return (int)r;
}
int inotify_init1(int flags)
{
    return (int)kret(user_syscall2(K_INOTIFY, 0, (uint64_t)flags));
}
int inotify_init(void) { return inotify_init1(0); }
int inotify_add_watch(int fd, const char* path, uint32_t mask)
{
    if (!path) { errno = EFAULT; return -1; }
    return (int)kret(user_syscall4(K_INOTIFY, 1, (uint64_t)fd, (uint64_t)path, (uint64_t)mask));
}
int inotify_rm_watch(int fd, int wd)
{
    return kret(user_syscall4(K_INOTIFY, 2, (uint64_t)fd, (uint64_t)wd, 0)) < 0 ? -1 : 0;
}

/* ---- iconv: UTF-8, UTF-16(LE/BE), UTF-32(LE/BE), WCHAR_T, ASCII, ISO-8859-1 (//TRANSLIT -> '?', //IGNORE skips) -------------------------- */
enum { CS_UTF8 = 1, CS_ASCII, CS_L1, CS_U16LE, CS_U16BE, CS_U16, CS_U32LE, CS_U32BE, CS_U32 };
struct IconvState { int from, to; int translit, ignore; int bom_out_done; int from_be; int from_known; };
static int cs_lookup(const char* name, int* translit, int* ignore)
{
    char b[40]; size_t n = 0;
    while (name[n] && n < sizeof(b) - 1) { char c = name[n]; if (c >= 'a' && c <= 'z') c = (char)(c - 32); b[n++] = c; }
    b[n] = 0;
    char* sl = strstr(b, "//");
    if (sl) { if (strstr(sl, "TRANSLIT")) *translit = 1; if (strstr(sl, "IGNORE")) *ignore = 1; *sl = 0; }
    if (!strcmp(b, "UTF-8") || !strcmp(b, "UTF8") || !strcmp(b, "") ) return CS_UTF8;
    if (!strcmp(b, "ASCII") || !strcmp(b, "US-ASCII") || !strcmp(b, "ANSI_X3.4-1968") || !strcmp(b, "646")) return CS_ASCII;
    if (!strcmp(b, "ISO-8859-1") || !strcmp(b, "ISO8859-1") || !strcmp(b, "LATIN1") || !strcmp(b, "L1") || !strcmp(b, "ISO_8859-1")) return CS_L1;
    if (!strcmp(b, "UTF-16LE") || !strcmp(b, "UCS-2LE")) return CS_U16LE;
    if (!strcmp(b, "UTF-16BE") || !strcmp(b, "UCS-2BE")) return CS_U16BE;
    if (!strcmp(b, "UTF-16") || !strcmp(b, "UCS-2")) return CS_U16;
    if (!strcmp(b, "UTF-32LE") || !strcmp(b, "UCS-4LE") || !strcmp(b, "WCHAR_T")) return CS_U32LE;
    if (!strcmp(b, "UTF-32BE") || !strcmp(b, "UCS-4BE")) return CS_U32BE;
    if (!strcmp(b, "UTF-32") || !strcmp(b, "UCS-4")) return CS_U32;
    return 0;
}
iconv_t iconv_open(const char* to, const char* from)
{
    if (!to || !from) { errno = EINVAL; return (iconv_t)-1; }
    int tr = 0, ig = 0, tr2 = 0, ig2 = 0;
    int t = cs_lookup(to, &tr, &ig), f = cs_lookup(from, &tr2, &ig2);
    if (!t || !f) { errno = EINVAL; return (iconv_t)-1; }
    IconvState* st = (IconvState*)calloc(1, sizeof(IconvState));
    if (!st) { errno = ENOMEM; return (iconv_t)-1; }
    st->from = f; st->to = t; st->translit = tr; st->ignore = ig;
    st->from_be = (f == CS_U16BE || f == CS_U32BE); st->from_known = !(f == CS_U16 || f == CS_U32);
    return (iconv_t)st;
}
int iconv_close(iconv_t cd)
{
    if (cd == (iconv_t)-1 || !cd) { errno = EBADF; return -1; }
    free(cd); return 0;
}
/* decode one code point: returns bytes used, 0 = incomplete input, -1 = invalid */
static int cs_decode(IconvState* st, const unsigned char* p, size_t n, uint32_t* cp)
{
    switch (st->from)
    {
        case CS_ASCII: if (n < 1) return 0; if (p[0] > 0x7F) return -1; *cp = p[0]; return 1;
        case CS_L1: if (n < 1) return 0; *cp = p[0]; return 1;
        case CS_UTF8:
        {
            if (n < 1) return 0;
            unsigned c = p[0];
            if (c < 0x80) { *cp = c; return 1; }
            int len; uint32_t v, min;
            if (c >= 0xC2 && c <= 0xDF) { len = 2; v = c & 0x1F; min = 0x80; }
            else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0F; min = 0x800; }
            else if (c >= 0xF0 && c <= 0xF4) { len = 4; v = c & 0x07; min = 0x10000; }
            else return -1;
            for (int i = 1; i < len; ++i)
            {
                if ((size_t)i >= n) return 0;
                if ((p[i] & 0xC0) != 0x80) return -1;
                v = (v << 6) | (p[i] & 0x3F);
            }
            if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return -1;
            *cp = v; return len;
        }
        case CS_U16LE: case CS_U16BE: case CS_U16:
        {
            if (n < 2) return 0;
            if (!st->from_known)
            {   /* BOM decides the byte order; without one UTF-16 is little endian here */
                st->from_known = 1;
                if (p[0] == 0xFE && p[1] == 0xFF) { st->from_be = 1; if (n < 4) { *cp = 0xFEFF; return 2; } }
                else if (p[0] == 0xFF && p[1] == 0xFE) st->from_be = 0;
                else st->from_be = 0;
                if (p[0] == 0xFE && p[1] == 0xFF) return -2;
                if (p[0] == 0xFF && p[1] == 0xFE) return -2;
            }
            uint32_t u = st->from_be ? (uint32_t)(p[0] << 8 | p[1]) : (uint32_t)(p[1] << 8 | p[0]);
            if (u >= 0xD800 && u <= 0xDBFF)
            {
                if (n < 4) return 0;
                uint32_t l = st->from_be ? (uint32_t)(p[2] << 8 | p[3]) : (uint32_t)(p[3] << 8 | p[2]);
                if (l < 0xDC00 || l > 0xDFFF) return -1;
                *cp = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00); return 4;
            }
            if (u >= 0xDC00 && u <= 0xDFFF) return -1;
            *cp = u; return 2;
        }
        default:
        {
            if (n < 4) return 0;
            if (!st->from_known)
            {
                st->from_known = 1;
                if (p[0] == 0 && p[1] == 0 && p[2] == 0xFE && p[3] == 0xFF) { st->from_be = 1; return -2; }
                if (p[0] == 0xFF && p[1] == 0xFE && p[2] == 0 && p[3] == 0) { st->from_be = 0; return -2; }
            }
            uint32_t u = st->from_be ? ((uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]) : ((uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]);
            if (u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) return -1;
            *cp = u; return 4;
        }
    }
}
/* encode one code point into out (capacity cap): returns bytes, 0 = no room, -1 = not representable */
static int cs_encode(IconvState* st, uint32_t cp, unsigned char* out, size_t cap)
{
    switch (st->to)
    {
        case CS_ASCII: if (cp > 0x7F) return -1; if (cap < 1) return 0; out[0] = (unsigned char)cp; return 1;
        case CS_L1: if (cp > 0xFF) return -1; if (cap < 1) return 0; out[0] = (unsigned char)cp; return 1;
        case CS_UTF8:
            if (cp < 0x80) { if (cap < 1) return 0; out[0] = (unsigned char)cp; return 1; }
            if (cp < 0x800) { if (cap < 2) return 0; out[0] = (unsigned char)(0xC0 | cp >> 6); out[1] = (unsigned char)(0x80 | (cp & 0x3F)); return 2; }
            if (cp < 0x10000) { if (cap < 3) return 0; out[0] = (unsigned char)(0xE0 | cp >> 12); out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (unsigned char)(0x80 | (cp & 0x3F)); return 3; }
            if (cap < 4) return 0;
            out[0] = (unsigned char)(0xF0 | cp >> 18); out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F)); out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (unsigned char)(0x80 | (cp & 0x3F)); return 4;
        case CS_U16LE: case CS_U16BE: case CS_U16:
        {
            int be = st->to == CS_U16BE; int need = cp >= 0x10000 ? 4 : 2; int bom = (st->to == CS_U16 && !st->bom_out_done) ? 2 : 0;
            if (cap < (size_t)(need + bom)) return 0;
            int o = 0;
            if (bom) { out[0] = 0xFF; out[1] = 0xFE; o = 2; st->bom_out_done = 1; }
            uint32_t u[2]; int nu;
            if (cp >= 0x10000) { uint32_t v = cp - 0x10000; u[0] = 0xD800 + (v >> 10); u[1] = 0xDC00 + (v & 0x3FF); nu = 2; } else { u[0] = cp; nu = 1; }
            for (int i = 0; i < nu; ++i) { if (be) { out[o++] = (unsigned char)(u[i] >> 8); out[o++] = (unsigned char)u[i]; } else { out[o++] = (unsigned char)u[i]; out[o++] = (unsigned char)(u[i] >> 8); } }
            return o;
        }
        default:
        {
            int be = st->to == CS_U32BE; int bom = (st->to == CS_U32 && !st->bom_out_done) ? 4 : 0;
            if (cap < (size_t)(4 + bom)) return 0;
            int o = 0;
            if (bom) { out[0] = 0xFF; out[1] = 0xFE; out[2] = 0; out[3] = 0; o = 4; st->bom_out_done = 1; }
            if (be) { out[o++] = (unsigned char)(cp >> 24); out[o++] = (unsigned char)(cp >> 16); out[o++] = (unsigned char)(cp >> 8); out[o++] = (unsigned char)cp; }
            else { out[o++] = (unsigned char)cp; out[o++] = (unsigned char)(cp >> 8); out[o++] = (unsigned char)(cp >> 16); out[o++] = (unsigned char)(cp >> 24); }
            return o;
        }
    }
}
size_t iconv(iconv_t cd, char** inbuf, size_t* inleft, char** outbuf, size_t* outleft)
{
    if (cd == (iconv_t)-1 || !cd) { errno = EBADF; return (size_t)-1; }
    IconvState* st = (IconvState*)cd;
    if (!inbuf || !*inbuf)
    {   /* reset the shift state; emit nothing more (the encodings here are stateless apart from the BOM) */
        st->bom_out_done = 0; st->from_known = !(st->from == CS_U16 || st->from == CS_U32); return 0;
    }
    size_t irrev = 0;
    while (*inleft > 0)
    {
        uint32_t cp = 0;
        int used = cs_decode(st, (const unsigned char*)*inbuf, *inleft, &cp);
        if (used == -2) { *inbuf += (st->from == CS_U16 ? 2 : 4); *inleft -= (st->from == CS_U16 ? 2 : 4); continue; }   /* consumed a BOM */
        if (used == 0) { errno = EINVAL; return (size_t)-1; }
        if (used < 0)
        {
            if (st->ignore) { *inbuf += 1; *inleft -= 1; ++irrev; continue; }
            errno = EILSEQ; return (size_t)-1;
        }
        unsigned char tmp[8];
        int w = cs_encode(st, cp, tmp, sizeof(tmp));
        if (w < 0)
        {
            if (st->translit) { cp = '?'; w = cs_encode(st, cp, tmp, sizeof(tmp)); ++irrev; }
            else if (st->ignore) { *inbuf += used; *inleft -= (size_t)used; ++irrev; continue; }
            else { errno = EILSEQ; return (size_t)-1; }
        }
        if (w == 0 || (size_t)w > *outleft) { errno = E2BIG; return (size_t)-1; }
        if (st->to == CS_U16 || st->to == CS_U32) { /* cs_encode already inserted the BOM into tmp when due */ }
        memcpy(*outbuf, tmp, (size_t)w);
        *outbuf += w; *outleft -= (size_t)w; *inbuf += used; *inleft -= (size_t)used;
    }
    return irrev;
}
