/* ustar archive reader/writer shared by tar and pkg (header-only). Needs "../libc/ulibc.h". */
#ifndef RIO_TARLIB_H
#define RIO_TARLIB_H
#include "../libc/ulibc.h"

struct TarBuf { unsigned char* p; unsigned long len, cap; };
static int tb_put(TarBuf* b, const void* d, unsigned long n)
{
    if (b->len + n > b->cap)
    {
        unsigned long nc = b->cap ? b->cap : 8192; while (nc < b->len + n) nc *= 2;
        unsigned char* np = (unsigned char*)realloc(b->p, nc); if (!np) return -1;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, d, n); b->len += n; return 0;
}
static void tar_octal(char* f, int n, unsigned long long v)
{
    f[n - 1] = 0; for (int i = n - 2; i >= 0; --i) { f[i] = (char)('0' + (v & 7)); v >>= 3; }
}
/* typeflag: '0' file, '5' directory, '2' symbolic link (data = link target, size 0). Returns 0 or -1 (name too long / out of memory). */
static int tar_add(TarBuf* b, const char* name, char type, unsigned mode, unsigned long mtime, unsigned uid, unsigned gid, const void* data, unsigned long size)
{
    char h[512]; memset(h, 0, sizeof(h));
    size_t nl = strlen(name); char nm[256]; strncpy(nm, name, 255); nm[255] = 0;
    if (type == '5' && nl && nm[nl - 1] != '/' && nl < 254) { nm[nl++] = '/'; nm[nl] = 0; }
    if (nl > 100)
    {
        int split = -1; for (int i = (int)nl - 1; i > 0; --i) if (nm[i] == '/' && (int)nl - i - 1 <= 100 && i <= 155) { split = i; break; }
        if (split < 0) return -1;
        memcpy(h + 345, nm, (size_t)split); memcpy(h, nm + split + 1, nl - (size_t)split - 1);
    }
    else memcpy(h, nm, nl);
    tar_octal(h + 100, 8, mode & 07777); tar_octal(h + 108, 8, uid); tar_octal(h + 116, 8, gid);
    tar_octal(h + 124, 12, (type == '5' || type == '2') ? 0 : size); tar_octal(h + 136, 12, mtime);
    memset(h + 148, ' ', 8); h[156] = type;
    if (type == '2' && data) { strncpy(h + 157, (const char*)data, 99); }
    memcpy(h + 257, "ustar", 6); h[263] = '0'; h[264] = '0';
    strcpy(h + 265, "root"); strcpy(h + 297, "root");
    unsigned sum = 0; for (int i = 0; i < 512; ++i) sum += (unsigned char)h[i];
    tar_octal(h + 148, 7, sum); h[155] = ' ';
    if (tb_put(b, h, 512)) return -1;
    if (type != '5' && type != '2' && size)
    {
        if (tb_put(b, data, size)) return -1;
        unsigned long pad = (512 - (size & 511)) & 511; char z[512]; memset(z, 0, sizeof(z));
        if (pad && tb_put(b, z, pad)) return -1;
    }
    return 0;
}
static int tar_finish(TarBuf* b) { char z[1024]; memset(z, 0, sizeof(z)); return tb_put(b, z, 1024); }

struct TarEntry { char name[256]; char link[101]; char type; unsigned mode, uid, gid; unsigned long size, mtime; const unsigned char* data; };
static unsigned long tar_parse_octal(const char* f, int n)
{
    unsigned long v = 0; int i = 0; while (i < n && (f[i] == ' ' || f[i] == 0)) ++i;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; ++i) v = (v << 3) | (unsigned long)(f[i] - '0');
    return v;
}
/* Iterates entries: *pos starts at 0. Returns 1 entry, 0 end of archive, -1 corrupt. */
static int tar_next(const unsigned char* a, unsigned long alen, unsigned long* pos, TarEntry* e)
{
    for (;;)
    {
        if (*pos + 512 > alen) return 0;
        const char* h = (const char*)(a + *pos);
        int zero = 1; for (int i = 0; i < 512; ++i) if (h[i]) { zero = 0; break; }
        if (zero) return 0;
        unsigned stored = (unsigned)tar_parse_octal(h + 148, 8), sum = 0;
        for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : (unsigned char)h[i];
        if (stored != sum) return -1;
        char pre[156]; memcpy(pre, h + 345, 155); pre[155] = 0;
        char nm[101]; memcpy(nm, h, 100); nm[100] = 0;
        if (pre[0]) snprintf(e->name, sizeof(e->name), "%s/%s", pre, nm); else snprintf(e->name, sizeof(e->name), "%s", nm);
        e->type = h[156] ? h[156] : '0';
        memcpy(e->link, h + 157, 100); e->link[100] = 0;
        e->mode = (unsigned)tar_parse_octal(h + 100, 8); e->uid = (unsigned)tar_parse_octal(h + 108, 8); e->gid = (unsigned)tar_parse_octal(h + 116, 8);
        e->size = tar_parse_octal(h + 124, 12); e->mtime = tar_parse_octal(h + 136, 12);
        *pos += 512; e->data = a + *pos;
        unsigned long skip = (e->size + 511) & ~511UL;
        if (*pos + e->size > alen) return -1;
        *pos += skip; if (*pos > alen) *pos = alen;
        if (e->type == 'x' || e->type == 'g' || e->type == 'L' || e->type == 'K') continue;   /* extended headers are ignored */
        return 1;
    }
}
/* 1 when a parent directory component of `path` is a symbolic link (extraction through it could escape the target dir) */
static inline int tar_parent_is_link(const char* path)
{
    char t[300]; strncpy(t, path, 299); t[299] = 0;
    for (char* p = t + 1; *p; ++p)
        if (*p == '/') { *p = 0; struct RioStat st; if (lstat_path(t, &st) == 0 && st.type == 2) return 1; *p = '/'; }
    return 0;
}
/* safe relative path: strips leading '/', refuses '..' components; returns pointer into name or 0 */
static const char* tar_safe_path(const char* name)
{
    while (*name == '/') ++name;
    while (name[0] == '.' && name[1] == '/') name += 2;
    for (const char* p = name; *p;)
    {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0)) return 0;
        while (*p && *p != '/') ++p; while (*p == '/') ++p;
    }
    return *name ? name : 0;
}
static int tar_mkdirs(const char* path)       /* mkdir -p for a file path's parents (path itself excluded when file=1) */
{
    char t[256]; strncpy(t, path, 255); t[255] = 0;
    for (char* p = t + 1; *p; ++p)
        if (*p == '/') { *p = 0; struct RioStat st; if (stat_path(t, &st) != 0) { if (mkdir(t, 0755) != 0) return -1; } *p = '/'; }
    return 0;
}
static unsigned char* tar_slurp(const char* path, unsigned long* n)
{
    FILE* f = path && strcmp(path, "-") ? fopen(path, "r") : stdin; if (!f) return 0;
    TarBuf b = { 0, 0, 0 }; unsigned char tmp[4096]; size_t got;
    while ((got = fread(tmp, 1, sizeof(tmp), f)) > 0) if (tb_put(&b, tmp, got)) { free(b.p); return 0; }
    if (f != stdin) fclose(f);
    if (!b.p) { b.p = (unsigned char*)malloc(1); }
    *n = b.len; return b.p;
}
/* recursively add a path (file or directory) into the archive */
static inline int tar_add_path(TarBuf* b, const char* path, int verbose, int* errors)
{
    struct RioStat st;
    if (lstat_path(path, &st) != 0) { printf("tar: %s: cannot stat\n", path); ++*errors; return 0; }
    const char* an = path; while (*an == '/') ++an; while (an[0] == '.' && an[1] == '/') an += 2;
    if (!*an) an = path;
    if (st.type == 1)
    {
        if (verbose) printf("%s/\n", an);
        if (tar_add(b, an, '5', st.mode, (unsigned long)st.mtime, st.uid, st.gid, 0, 0)) return -1;
        char* list = (char*)malloc(65536); if (!list) return -1;
        long n = readdir_text(path, list, 65535);
        if (n < 0) { free(list); printf("tar: %s: cannot read directory\n", path); ++*errors; return 0; }
        list[n] = 0;
        for (char* ln = list; *ln;)
        {
            char* nl = ln; while (*nl && *nl != '\n') ++nl; if (*nl) *nl++ = 0;
            char* name = ln + 1; while (*name == ' ') ++name; while (*name >= '0' && *name <= '9') ++name; while (*name == ' ') ++name;
            ln = nl;
            if (!*name) continue;
            char child[256]; size_t pl = strlen(path);
            if (pl && path[pl - 1] == '/') snprintf(child, sizeof(child), "%s%s", path, name); else snprintf(child, sizeof(child), "%s/%s", path, name);
            if (tar_add_path(b, child, verbose, errors)) { free(list); return -1; }
        }
        free(list);
        return 0;
    }
    if (st.type == 2)
    {
        char tgt[256]; long tn = readlink(path, tgt, sizeof(tgt) - 1);
        if (tn < 0) { printf("tar: %s: cannot read link\n", path); ++*errors; return 0; }
        tgt[tn] = 0;
        if (tn > 99) { printf("tar: %s: link target too long\n", path); ++*errors; return 0; }
        if (verbose) printf("%s -> %s\n", an, tgt);
        if (tar_add(b, an, '2', st.mode, (unsigned long)st.mtime, st.uid, st.gid, tgt, 0)) { printf("tar: %s: name too long\n", an); ++*errors; }
        return 0;
    }
    FILE* f = fopen(path, "r"); if (!f) { printf("tar: %s: cannot open\n", path); ++*errors; return 0; }
    unsigned char* data = (unsigned char*)malloc((unsigned long)st.size + 1); if (!data) { fclose(f); return -1; }
    size_t got = st.size ? fread(data, 1, (size_t)st.size, f) : 0; fclose(f);
    if (verbose) printf("%s\n", an);
    int rc = tar_add(b, an, '0', st.mode, (unsigned long)st.mtime, st.uid, st.gid, data, got);
    free(data);
    if (rc) { printf("tar: %s: name too long\n", an); ++*errors; }
    return 0;
}
#endif
