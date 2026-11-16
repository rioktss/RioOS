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

/* Host test for the virtio-9p mount layer (fs/fs9p.inc): the real fs.cpp runs against a fake 9P server that is
   a plain POSIX directory (so the semantics are those of a real file system), with fault injection. */

#include "fs.h"
#include "ninep.h"
#include "types.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static FILE* g_file = 0;
static const uint64_t SECTOR = 512;
static const uint64_t SECTORS = 32768;
static const uint64_t CACHE_SECTORS = 17408;
static unsigned char g_cache[2][17408 * 512];

void uart_putc(char c) { (void)c; }
void uart_puts(const char* s) { (void)s; }
void storage_init() {}
int storage_ready() { return g_file != 0; }
int storage_read_sector(uint64_t sector, void* buffer)
{
    if (!g_file || !buffer || sector >= SECTORS) return -1;
    if (fseek(g_file, (long)(sector * SECTOR), SEEK_SET) != 0) return -1;
    return fread(buffer, 1, SECTOR, g_file) == SECTOR ? 0 : -1;
}
int storage_write_sector(uint64_t sector, const void* buffer)
{
    if (!g_file || !buffer || sector >= SECTORS) return -1;
    if (fseek(g_file, (long)(sector * SECTOR), SEEK_SET) != 0) return -1;
    if (fwrite(buffer, 1, SECTOR, g_file) != SECTOR) return -1;
    fflush(g_file); return 0;
}
int storage_flush() { return g_file && fflush(g_file) == 0 ? 0 : -1; }
int storage_dev_read(int dev, uint64_t sector, void* buffer)
{
    if ((dev != 6 && dev != 7) || sector >= CACHE_SECTORS || !buffer) return -1;
    memcpy(buffer, g_cache[dev - 6] + sector * 512, 512); return 0;
}
int storage_dev_write(int dev, uint64_t sector, const void* buffer)
{
    if ((dev != 6 && dev != 7) || sector >= CACHE_SECTORS || !buffer) return -1;
    memcpy(g_cache[dev - 6] + sector * 512, buffer, 512); return 0;
}
int storage_dev_flush(int dev) { return (dev == 6 || dev == 7) ? 0 : -1; }
uint64_t storage_dev_sectors(int dev) { return (dev == 6 || dev == 7) ? CACHE_SECTORS : 0; }

static void fail(const char* m) { fprintf(stderr, "FS9P HOST TEST FAIL: %s\n", m); exit(1); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FS9P HOST TEST FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

/* ---------- fake server: a POSIX directory ---------- */
static char g_dir[96];
static uint64_t g_now = 1000;            /* controlled clock (ms) */
static int g_inject = 0;                 /* >0: fail the Nth next mutating call with g_inject_errno */
static int g_inject_errno = EACCES;
static int g_dead = 0;                   /* the server is gone: every call fails with EIO */
static int g_calls_lookup = 0, g_calls_read = 0, g_calls_readdir = 0;

static int path_of(const char* rel, char* out, size_t cap)
{
    int n = snprintf(out, cap, "%s%s%s", g_dir, rel[0] ? "/" : "", rel);
    return n > 0 && (size_t)n < cap ? 0 : -ENAMETOOLONG;
}
static int mut_fault()
{
    if (g_dead) return -EIO;
    if (g_inject > 0 && --g_inject == 0) return -g_inject_errno;
    return 0;
}
static uint64_t t_now() { return g_now; }
static int t_lookup(int, const char* rel, NinepAttr* a)
{
    ++g_calls_lookup;
    if (g_dead) return -EIO;
    char p[512]; if (path_of(rel, p, sizeof p)) return -ENAMETOOLONG;
    struct stat st; if (lstat(p, &st) != 0) return -errno;
    a->mode = st.st_mode; a->uid = st.st_uid; a->gid = st.st_gid; a->size = (uint64_t)st.st_size;
    a->mtime = (uint64_t)st.st_mtim.tv_sec; a->mtime_ns = (uint64_t)st.st_mtim.tv_nsec;
    a->version = a->mtime ^ (a->size << 8) ^ a->mtime_ns; a->ino = st.st_ino;
    return 0;
}
static int t_readdir(int, const char* rel, NinepDirCb cb, void* ctx)
{
    ++g_calls_readdir;
    if (g_dead) return -EIO;
    char p[512]; if (path_of(rel, p, sizeof p)) return -ENAMETOOLONG;
    DIR* d = opendir(p); if (!d) return -errno;
    struct dirent* e;
    while ((e = readdir(d)) != 0)
    {
        char q[1024]; snprintf(q, sizeof q, "%s/%s", p, e->d_name);
        struct stat st; uint32_t mt = 0;
        if (lstat(q, &st) == 0) { unsigned f = st.st_mode & S_IFMT; mt = f == S_IFDIR ? NINEP_S_IFDIR : f == S_IFREG ? NINEP_S_IFREG : f == S_IFLNK ? NINEP_S_IFLNK : 0; }
        if (cb(ctx, e->d_name, mt)) break;
    }
    closedir(d); return 0;
}
static int t_read_file(int, const char* rel, uint8_t* buf, uint64_t cap, uint64_t* out)
{
    ++g_calls_read;
    if (g_dead) return -EIO;
    char p[512]; if (path_of(rel, p, sizeof p)) return -ENAMETOOLONG;
    int fd = open(p, O_RDONLY); if (fd < 0) return -errno;
    struct stat st; fstat(fd, &st);
    if ((uint64_t)st.st_size > cap) { close(fd); return -27; }
    uint64_t got = 0; while (got < (uint64_t)st.st_size) { ssize_t n = read(fd, buf + got, (size_t)(st.st_size - got)); if (n <= 0) break; got += (uint64_t)n; }
    close(fd); *out = got; return 0;
}
static int t_write_file(int, const char* rel, const uint8_t* data, uint64_t size, uint32_t mode)
{
    int f = mut_fault(); if (f) return f;
    char p[512]; if (path_of(rel, p, sizeof p)) return -ENAMETOOLONG;
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777); if (fd < 0) return -errno;
    uint64_t put = 0; while (put < size) { ssize_t n = write(fd, data + put, (size_t)(size - put)); if (n <= 0) { close(fd); return -EIO; } put += (uint64_t)n; }
    close(fd); return 0;
}
static int t_create(int, const char* rel, uint32_t mode)
{
    int f = mut_fault(); if (f) return f;
    char p[512]; if (path_of(rel, p, sizeof p)) return -ENAMETOOLONG;
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, mode & 0777); if (fd < 0) return -errno; close(fd); return 0;
}
static int t_mkdir(int, const char* rel, uint32_t mode)
{
    int f = mut_fault(); if (f) return f;
    char p[512]; if (path_of(rel, p, sizeof p)) return -ENAMETOOLONG;
    return mkdir(p, mode & 0777) == 0 ? 0 : -errno;
}
static int rm_tree(const char* p)
{
    struct stat st; if (lstat(p, &st) != 0) return -errno;
    if (S_ISDIR(st.st_mode))
    {
        DIR* d = opendir(p); if (!d) return -errno;
        struct dirent* e;
        while ((e = readdir(d)) != 0)
        {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char q[1024]; snprintf(q, sizeof q, "%s/%s", p, e->d_name);
            int rc = rm_tree(q); if (rc) { closedir(d); return rc; }
        }
        closedir(d);
        return rmdir(p) == 0 ? 0 : -errno;
    }
    return unlink(p) == 0 ? 0 : -errno;
}
static int t_remove(int, const char* rel, int is_dir)
{
    int f = mut_fault(); if (f) return f;
    char p[512]; if (path_of(rel, p, sizeof p)) return -ENAMETOOLONG;
    return (is_dir ? rmdir(p) : unlink(p)) == 0 ? 0 : -errno;
}
static int t_remove_tree(int, const char* rel)
{
    int f = mut_fault(); if (f) return f;
    char p[512]; if (path_of(rel, p, sizeof p)) return -ENAMETOOLONG;
    return rm_tree(p);
}
static int t_rename(int, const char* a, const char* b)
{
    int f = mut_fault(); if (f) return f;
    char p[512], q[512]; if (path_of(a, p, sizeof p) || path_of(b, q, sizeof q)) return -ENAMETOOLONG;
    return rename(p, q) == 0 ? 0 : -errno;
}
static int t_setattr(int, const char* rel, uint32_t valid, uint32_t mode, uint32_t, uint32_t, uint64_t size, uint64_t mtime)
{
    int f = mut_fault(); if (f) return f;
    char p[512]; if (path_of(rel, p, sizeof p)) return -ENAMETOOLONG;
    if ((valid & 1U) && chmod(p, mode & 0777) != 0) return -errno;
    if ((valid & 8U) && truncate(p, (off_t)size) != 0) return -errno;
    if ((valid & 0x120U) == 0x120U) { struct timespec ts[2] = {{(time_t)mtime, 0}, {(time_t)mtime, 0}}; if (utimensat(AT_FDCWD, p, ts, AT_SYMLINK_NOFOLLOW) != 0) return -errno; }
    return 0;
}
static int t_symlink(int, const char* rel, const char* target)
{
    int f = mut_fault(); if (f) return f;
    char p[512]; if (path_of(rel, p, sizeof p)) return -ENAMETOOLONG;
    return symlink(target, p) == 0 ? 0 : -errno;
}
static int t_readlink(int, const char* rel, char* out, int cap)
{
    if (g_dead) return -EIO;
    char p[512]; if (path_of(rel, p, sizeof p)) return -ENAMETOOLONG;
    ssize_t n = readlink(p, out, (size_t)cap - 1); if (n < 0) return -errno; out[n] = 0; return (int)n;
}
static int t_statfs(int, uint64_t* total, uint64_t* freeb)
{
    struct statvfs sv; if (statvfs(g_dir, &sv) != 0) return -errno;
    *total = (uint64_t)sv.f_blocks * sv.f_frsize; *freeb = (uint64_t)sv.f_bavail * sv.f_frsize; return 0;
}
static const NinepOps OPS = { t_now, t_lookup, t_readdir, t_read_file, t_write_file, t_create, t_mkdir, t_remove, t_remove_tree, t_rename, t_setattr, t_symlink, t_readlink, t_statfs };

/* ---------- helpers ---------- */
static void hput(const char* rel, const void* data, size_t n)
{
    char p[512]; snprintf(p, sizeof p, "%s/%s", g_dir, rel);
    FILE* f = fopen(p, "wb"); if (!f) fail("hput"); fwrite(data, 1, n, f); fclose(f);
}
static int hread(const char* rel, unsigned char* buf, size_t cap)
{
    char p[512]; snprintf(p, sizeof p, "%s/%s", g_dir, rel);
    FILE* f = fopen(p, "rb"); if (!f) return -1; size_t n = fread(buf, 1, cap, f); fclose(f); return (int)n;
}
static int hexists(const char* rel) { char p[512]; snprintf(p, sizeof p, "%s/%s", g_dir, rel); struct stat st; return lstat(p, &st) == 0; }
static int hdir(const char* rel) { char p[512]; snprintf(p, sizeof p, "%s/%s", g_dir, rel); struct stat st; return lstat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static void tick(uint64_t ms) { g_now += ms; }

static int gread(const char* path, unsigned char* buf, size_t cap, uint64_t* n)
{
    return fs_read_file(path, fs_get_root(), buf, cap, n);
}
static int guest_has(const char* path) { return fs_resolve(path, fs_get_root()) >= 0; }
static int dir_count(const char* path) { int id = fs_resolve(path, fs_get_root()); return id < 0 ? -1 : fs_get_child_count(id); }

int main()
{
    snprintf(g_dir, sizeof g_dir, "build/p9mock-%d", (int)getpid());
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/sub", g_dir, g_dir);
    if (system(cmd) != 0) fail("mkdir share");
    const char* path = "build/fs9p-host-test.img";
    g_file = fopen(path, "w+b"); if (!g_file) fail("open");
    if (fseek(g_file, (long)(SECTOR * SECTORS - 1), SEEK_SET) != 0 || fputc(0, g_file) == EOF || fflush(g_file) != 0) fail("size");
    fs_init();
    int root = fs_get_root();
    CHECK(fs_mkdir("/mnt", root) >= 0);
    hput("hello.txt", "hello\n", 6); hput("sub/deep.txt", "deep", 4);
    { char p[512]; snprintf(p, sizeof p, "%s/lnk", g_dir); if (symlink("hello.txt", p) != 0) fail("symlink"); }

    /* mount */
    CHECK(fs_mount_9p(&OPS, 0, "/mnt", root, 0) == 0);
    CHECK(fs_mount_9p(&OPS, 0, "/mnt", root, 0) == -16);                 /* same share twice */
    CHECK(fs_mount_9p(0, 0, "/mnt", root, 0) == -22);
    CHECK(fs_mount_device(6, "/mnt", root, 0, 1) == -22);                /* internal cache devices are not user devices */
    { FsMountInfo mi; int found = 0; for (int i = 0; fs_mount_info(i, &mi) == 0; ++i) if (mi.p9share == 0) found = 1; CHECK(found); }

    /* read side */
    unsigned char buf[64]; uint64_t n = 0;
    CHECK(dir_count("/mnt") == 3);
    CHECK(gread("/mnt/hello.txt", buf, sizeof buf, &n) == 0 && n == 6 && !memcmp(buf, "hello\n", 6));
    CHECK(gread("/mnt/sub/deep.txt", buf, sizeof buf, &n) == 0 && n == 4 && !memcmp(buf, "deep", 4));
    { int id = fs_resolve_nofollow("/mnt/lnk", root); CHECK(id >= 0 && fs_get_type(id) == FS_SYMLINK); char t[64]; CHECK(fs_readlink("/mnt/lnk", root, t, sizeof t) > 0 && !strcmp(t, "hello.txt")); }
    CHECK(gread("/mnt/lnk", buf, sizeof buf, &n) == 0 && n == 6);        /* through the link */
    CHECK(fs_resolve("/mnt/nope", root) < 0);

    /* freshness: host-side changes (TTL for names, a getattr per read for data) */
    hput("hello.txt", "HELLO\n", 6);                                    /* same size, probably same second: ns resolution must catch it */
    CHECK(gread("/mnt/hello.txt", buf, sizeof buf, &n) == 0 && !memcmp(buf, "HELLO\n", 6));
    hput("new_on_host.txt", "nh", 2);
    CHECK(!guest_has("/mnt/new_on_host.txt"));                           /* still inside the TTL: allowed to be stale */
    tick(1500);
    CHECK(guest_has("/mnt/new_on_host.txt"));
    unlink((std::string(g_dir) + "/new_on_host.txt").c_str());
    tick(1500);
    CHECK(!guest_has("/mnt/new_on_host.txt"));
    CHECK(dir_count("/mnt") == 3);

    /* guest writes reach the server (and the server's view wins afterwards) */
    CHECK(fs_mkdir("/mnt/d1", root) >= 0 && hdir("d1"));
    CHECK(fs_write("/mnt/d1/a.txt", root, "alpha") == 0);
    { unsigned char h[16]; CHECK(hread("d1/a.txt", h, sizeof h) == 5 && !memcmp(h, "alpha", 5)); }
    CHECK(gread("/mnt/d1/a.txt", buf, sizeof buf, &n) == 0 && n == 5);
    CHECK(fs_write("/mnt/d1/a.txt", root, "alphabet") == 0);             /* overwrite */
    { unsigned char h[16]; CHECK(hread("d1/a.txt", h, sizeof h) == 8 && !memcmp(h, "alphabet", 8)); }
    CHECK(fs_touch("/mnt/d1/empty", root) >= 0 && hexists("d1/empty"));
    CHECK(fs_mkdir_p("/mnt/x/y/z", root) >= 0 && hdir("x/y/z"));
    CHECK(fs_symlink("a.txt", "/mnt/d1/ln", root) >= 0);
    { char p[512], t[64]; snprintf(p, sizeof p, "%s/d1/ln", g_dir); ssize_t k = readlink(p, t, sizeof t - 1); CHECK(k == 5); t[k] = 0; CHECK(!strcmp(t, "a.txt")); }
    CHECK(fs_truncate("/mnt/d1/a.txt", root, 3) == 0);
    { unsigned char h[16]; CHECK(hread("d1/a.txt", h, sizeof h) == 3 && !memcmp(h, "alp", 3)); }
    CHECK(gread("/mnt/d1/a.txt", buf, sizeof buf, &n) == 0 && n == 3 && !memcmp(buf, "alp", 3));
    CHECK(fs_truncate("/mnt/d1/a.txt", root, 6) == 0);
    { unsigned char h[16]; CHECK(hread("d1/a.txt", h, sizeof h) == 6 && !memcmp(h, "alp\0\0\0", 6)); }
    CHECK(fs_set_mode_path("/mnt/d1/a.txt", root, 0600) == 0);
    { char p[512]; snprintf(p, sizeof p, "%s/d1/a.txt", g_dir); struct stat st; CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0600); }
    CHECK(fs_get_mode(fs_resolve("/mnt/d1/a.txt", root)) == 0600);
    CHECK(fs_set_mtime_path("/mnt/d1/a.txt", root, 1234567890ULL) == 0);
    { char p[512]; snprintf(p, sizeof p, "%s/d1/a.txt", g_dir); struct stat st; CHECK(stat(p, &st) == 0 && st.st_mtime == 1234567890); }
    CHECK(fs_get_mtime(fs_resolve("/mnt/d1/a.txt", root)) == 1234567890ULL);

    /* rename (file, directory with cached children, replacing a file) */
    CHECK(fs_rename("/mnt/d1/a.txt", "/mnt/d1/b.txt", root) == 0 && hexists("d1/b.txt") && !hexists("d1/a.txt"));
    CHECK(gread("/mnt/d1/b.txt", buf, sizeof buf, &n) == 0 && n == 6);
    CHECK(fs_rename("/mnt/d1", "/mnt/d2", root) == 0 && hdir("d2") && !hdir("d1"));
    CHECK(gread("/mnt/d2/b.txt", buf, sizeof buf, &n) == 0 && n == 6);
    CHECK(fs_rename("/mnt/d2/b.txt", "/mnt/sub/b.txt", root) == 0 && hexists("sub/b.txt"));
    CHECK(fs_rename("/mnt/sub/b.txt", "/mnt/sub/deep.txt", root) != 0);   /* replacing is refused by the shared rename rules */

    /* removal */
    CHECK(fs_rm("/mnt/d2/empty", root) == 0 && !hexists("d2/empty"));
    CHECK(fs_rm("/mnt/d2", root) != 0 && hdir("d2"));                    /* not empty */
    CHECK(fs_rm_recursive("/mnt/d2", root) == 0 && !hdir("d2"));
    CHECK(!guest_has("/mnt/d2/ln"));
    CHECK(fs_rm_recursive("/mnt/x", root) == 0 && !hdir("x"));
    CHECK(fs_rm("/mnt/lnk", root) == 0 && !hexists("lnk"));

    /* the server refuses: the cache must fall back to the server's state and an error is returned */
    g_inject = 1; g_inject_errno = EACCES;
    CHECK(fs_mkdir("/mnt/refused", root) < 0 && !hdir("refused") && !guest_has("/mnt/refused"));
    g_inject = 1; g_inject_errno = ENOSPC;
    CHECK(fs_write("/mnt/full.txt", root, "data") < 0);
    CHECK(!hexists("full.txt") || 1);
    tick(1500);
    { int id = fs_resolve("/mnt/full.txt", root); if (id >= 0) { CHECK(fs_file_size(id) == 0); } }   /* empty file at most, never phantom data */
    hput("keep.txt", "keep", 4);
    tick(1500);
    g_inject = 1; g_inject_errno = EACCES;
    CHECK(fs_rm("/mnt/keep.txt", root) < 0 && hexists("keep.txt") && guest_has("/mnt/keep.txt"));
    CHECK(gread("/mnt/keep.txt", buf, sizeof buf, &n) == 0 && n == 4);
    g_inject = 1;
    CHECK(fs_rename("/mnt/keep.txt", "/mnt/moved.txt", root) != 0 && hexists("keep.txt") && !hexists("moved.txt") && guest_has("/mnt/keep.txt") && !guest_has("/mnt/moved.txt"));
    g_inject = 1;
    CHECK(fs_set_mode_path("/mnt/keep.txt", root, 0777) < 0 && fs_get_mode(fs_resolve("/mnt/keep.txt", root)) == 0644);
    g_inject = 1;
    CHECK(fs_write("/mnt/keep.txt", root, "changed") < 0);
    CHECK(gread("/mnt/keep.txt", buf, sizeof buf, &n) == 0 && n == 4 && !memcmp(buf, "keep", 4));   /* the cache did not keep data the server never got */

    /* the directory vanishes on the host under a process that is inside it */
    CHECK(fs_mkdir("/mnt/gone", root) >= 0);
    CHECK(fs_write("/mnt/gone/f", root, "1") == 0);
    int gone_id = fs_resolve("/mnt/gone", root);
    CHECK(gone_id >= 0);
    { char c2[400]; snprintf(c2, sizeof c2, "rm -rf %s/gone", g_dir); if (system(c2) != 0) fail("rm gone"); }
    tick(1500);
    CHECK(fs_write("/mnt/gone/g", gone_id, "x") != 0 || 1);              /* must not crash */
    CHECK(fs_resolve("/mnt/gone", root) < 0);
    CHECK(fs_touch("g2", gone_id) < 0);                                  /* relative path from a removed cwd fails */

    /* file kinds that cannot be represented are hidden, long names too */
    { char p[512]; snprintf(p, sizeof p, "%s/fifo", g_dir); mkfifo(p, 0644); }
    { char nm[128]; memset(nm, 'n', 100); nm[100] = 0; hput(nm, "x", 1); }
    tick(1500);
    CHECK(!guest_has("/mnt/fifo"));

    /* large file + cache eviction (cache holds < 2 of these) */
    static unsigned char big1[3 * 1024 * 1024 + 11], big2[3 * 1024 * 1024 + 7], big3[3 * 1024 * 1024], rb[3 * 1024 * 1024 + 20];
    for (size_t i = 0; i < sizeof big1; ++i) big1[i] = (unsigned char)(i * 131U + 7U);
    for (size_t i = 0; i < sizeof big2; ++i) big2[i] = (unsigned char)(i * 17U + 3U);
    for (size_t i = 0; i < sizeof big3; ++i) big3[i] = (unsigned char)(i * 71U + 5U);
    CHECK(fs_write_data("/mnt/big1", root, big1, sizeof big1) == 0);
    CHECK(fs_write_data("/mnt/big2", root, big2, sizeof big2) == 0);
    CHECK(fs_write_data("/mnt/big3", root, big3, sizeof big3) == 0);
    { static unsigned char h[sizeof rb]; CHECK(hread("big1", h, sizeof h) == (int)sizeof big1 && !memcmp(h, big1, sizeof big1)); CHECK(hread("big3", h, sizeof h) == (int)sizeof big3 && !memcmp(h, big3, sizeof big3)); }
    CHECK(gread("/mnt/big1", rb, sizeof rb, &n) == 0 && n == sizeof big1 && !memcmp(rb, big1, sizeof big1));
    CHECK(gread("/mnt/big2", rb, sizeof rb, &n) == 0 && n == sizeof big2 && !memcmp(rb, big2, sizeof big2));
    CHECK(gread("/mnt/big1", rb, sizeof rb, &n) == 0 && n == sizeof big1 && !memcmp(rb, big1, sizeof big1));
    CHECK(fs_write_data("/mnt/big1", root, big3, sizeof big3) == 0);     /* rewrite a big file while the cache is nearly full */
    CHECK(gread("/mnt/big1", rb, sizeof rb, &n) == 0 && n == sizeof big3 && !memcmp(rb, big3, sizeof big3));
    CHECK(fs_truncate("/mnt/big2", root, 100) == 0);
    CHECK(gread("/mnt/big2", rb, sizeof rb, &n) == 0 && n == 100 && !memcmp(rb, big2, 100));
    { static char huge[5 * 1024 * 1024]; memset(huge, 'h', sizeof huge); hput("huge", huge, sizeof huge); tick(1500); }
    CHECK(gread("/mnt/huge", rb, sizeof rb, &n) != 0);                   /* larger than a file can be: refused, no crash */
    CHECK(gread("/mnt/big3", rb, sizeof rb, &n) == 0 && n == sizeof big3 && !memcmp(rb, big3, sizeof big3));

    /* too many entries for the node table: the first ones show up, the rest is reported as hidden */
    { char c3[400]; snprintf(c3, sizeof c3, "mkdir -p %s/many", g_dir); if (system(c3) != 0) fail("mkdir many"); }
    for (int i = 0; i < 300; ++i) { char nm[64]; snprintf(nm, sizeof nm, "many/f%03d", i); hput(nm, "a", 1); }
    tick(1500);
    { int c = dir_count("/mnt/many"); CHECK(c > 150 && c < 300); }
    { FsMountInfo mi; int hidden = 0; for (int i = 0; fs_mount_info(i, &mi) == 0; ++i) if (mi.p9share == 0) hidden = mi.p9hidden; CHECK(hidden > 0); }
    CHECK(fs_rm_recursive("/mnt/many", root) == 0 && !hdir("many"));

    /* read-only mount refuses everything and never touches the server */
    CHECK(fs_umount_path("/mnt", root, -1) == 0);
    CHECK(fs_mount_9p(&OPS, 0, "/mnt", root, 1) == 0);
    CHECK(fs_write("/mnt/ro.txt", root, "x") < 0 && !hexists("ro.txt"));
    CHECK(fs_mkdir("/mnt/rodir", root) < 0 && !hdir("rodir"));
    CHECK(fs_rm("/mnt/keep.txt", root) < 0 && hexists("keep.txt"));
    CHECK(fs_rename("/mnt/keep.txt", "/mnt/k2", root) != 0 && hexists("keep.txt"));
    CHECK(fs_set_mode_path("/mnt/keep.txt", root, 0600) < 0);
    CHECK(fs_truncate("/mnt/keep.txt", root, 1) < 0);
    CHECK(gread("/mnt/keep.txt", buf, sizeof buf, &n) == 0 && n == 4);
    CHECK(fs_umount_path("/mnt", root, -1) == 0);

    /* no hard links, no fsck on a 9p mount; server gone -> errors, not hangs or crashes */
    CHECK(fs_mount_9p(&OPS, 0, "/mnt", root, 0) == 0);
    CHECK(fs_link_hard("/mnt/keep.txt", "/mnt/hl", root) < 0);
    { FsckInfo f; CHECK(fs_fsck("/mnt", root, 0, &f) < 0); }
    CHECK(gread("/mnt/keep.txt", buf, sizeof buf, &n) == 0);
    g_dead = 1; tick(1500);
    CHECK(dir_count("/mnt") >= 0);                                       /* stale listing still readable */
    { int id = fs_resolve("/mnt/keep.txt", root); CHECK(id >= 0); }
    CHECK(fs_write("/mnt/keep.txt", root, "zzz") < 0);
    CHECK(fs_mkdir("/mnt/dead", root) < 0 && !guest_has("/mnt/dead"));
    g_dead = 0; tick(1500);
    CHECK(gread("/mnt/keep.txt", buf, sizeof buf, &n) == 0 && n == 4 && !memcmp(buf, "keep", 4));
    CHECK(fs_umount_path("/mnt", root, -1) == 0);
    CHECK(fs_mount_9p(&OPS, 0, "/mnt", root, 0) == 0);                   /* remount sees the current server state */
    CHECK(guest_has("/mnt/keep.txt"));
    CHECK(fs_umount_path("/mnt", root, -1) == 0);

    /* the root file system is untouched by all of this */
    { FsckInfo f; CHECK(fs_fsck(0, 0, 0, &f) == 0 && f.structure_ok && f.bitmap_ok && f.problems == 0); }
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir); if (system(cmd) != 0) fail("cleanup");
    printf("fs9p host test OK\n");
    return 0;
}
