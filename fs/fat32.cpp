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

#include "fat32.h"
#include "storage.h"
#include "rtc.h"
#include "string.h"

/* ---- device access ---- */
static int g_dev = 0;
static int rd(uint64_t s, uint8_t* b) { return storage_dev_read(g_dev, s, b); }
static int wr(uint64_t s, const uint8_t* b) { return storage_dev_write(g_dev, s, b); }

struct Bpb { uint32_t spc, reserved, nfats, fatsz, root, data_start, clusters; uint64_t total; int valid; };
static Bpb B;
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int load_bpb(int dev)
{
    g_dev = dev; B.valid = 0;
    if (storage_dev_sectors(dev) == 0) return -1;
    uint8_t s[512];
    if (rd(0, s) != 0) return -1;
    if (s[510] != 0x55 || s[511] != 0xAA || le16(s + 11) != 512) return -1;
    B.spc = s[13]; B.reserved = le16(s + 14); B.nfats = s[16];
    B.fatsz = le32(s + 36); B.root = le32(s + 44);
    uint32_t t16 = le16(s + 19), t32 = le32(s + 32);
    B.total = t16 ? t16 : t32;
    if (B.spc == 0 || (B.spc & (B.spc - 1)) || B.nfats == 0 || B.fatsz == 0 || le16(s + 17) != 0 || le16(s + 22) != 0 || B.root < 2) return -1;
    B.data_start = B.reserved + B.nfats * B.fatsz;
    if (B.total <= B.data_start) return -1;
    B.clusters = (uint32_t)((B.total - B.data_start) / B.spc);
    if (B.total > storage_dev_sectors(dev)) return -1;
    B.valid = 1;
    return 0;
}

static uint64_t csec(uint32_t c) { return (uint64_t)B.data_start + (uint64_t)(c - 2) * B.spc; }

static int fat_get(uint32_t c, uint32_t* out)
{
    uint8_t s[512];
    uint64_t off = (uint64_t)c * 4;
    if (rd(B.reserved + off / 512, s) != 0) return -1;
    *out = le32(s + off % 512) & 0x0FFFFFFFU;
    return 0;
}
static int fat_set(uint32_t c, uint32_t v)
{
    uint8_t s[512];
    uint64_t off = (uint64_t)c * 4;
    for (uint32_t f = 0; f < B.nfats; ++f)
    {
        uint64_t sec = (uint64_t)B.reserved + (uint64_t)f * B.fatsz + off / 512;
        if (rd(sec, s) != 0) return -1;
        uint32_t old = le32(s + off % 512);
        put32(s + off % 512, (old & 0xF0000000U) | (v & 0x0FFFFFFFU));
        if (wr(sec, s) != 0) return -1;
    }
    return 0;
}
static int is_eoc(uint32_t v) { return v >= 0x0FFFFFF8U; }

static int zero_cluster(uint32_t c)
{
    uint8_t z[512]; for (int i = 0; i < 512; ++i) z[i] = 0;
    for (uint32_t i = 0; i < B.spc; ++i) if (wr(csec(c) + i, z) != 0) return -1;
    return 0;
}
static int alloc_cluster(uint32_t* out)
{
    for (uint32_t c = 3; c < B.clusters + 2; ++c)
    {
        uint32_t v; if (fat_get(c, &v) != 0) return -1;
        if (v == 0) { if (fat_set(c, 0x0FFFFFFFU) != 0 || zero_cluster(c) != 0) return -1; *out = c; return 0; }
    }
    return -28;
}
static void free_chain(uint32_t c)
{
    int guard = 0;
    while (c >= 2 && c < B.clusters + 2 && guard++ < 1000000)
    {
        uint32_t nx; if (fat_get(c, &nx) != 0) return;
        (void)fat_set(c, 0);
        if (is_eoc(nx) || nx == 0) return;
        c = nx;
    }
}

/* ---- directory handling ---- */
struct DirPos { uint32_t cluster; uint64_t sector; int index; };      /* location of a 32-byte entry */
struct Found
{
    uint32_t cluster, size; uint8_t attr; int is_dir;
    DirPos pos; DirPos lfn[21]; int nlfn; char name[64];
    uint32_t dir_cluster;
};

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ci_equal(const char* a, const char* b) { while (*a && lower(*a) == lower(*b)) { ++a; ++b; } return *a == *b; }

static void short_to_name(const uint8_t* e, char* out)
{
    int k = 0;
    for (int i = 0; i < 8 && e[i] != ' '; ++i) out[k++] = (char)(e[0] == 0x05 && i == 0 ? 0xE5 : e[i]);
    if (e[8] != ' ') { out[k++] = '.'; for (int i = 8; i < 11 && e[i] != ' '; ++i) out[k++] = (char)e[i]; }
    out[k] = 0;
}

/* Iterate directory `dir`; cb returns 1 to stop. */
typedef int (*DirCb)(const uint8_t* e, const Found* f, void* ctx);
static int walk_dir(uint32_t dir, DirCb cb, void* ctx, int want_free, DirPos* free_slot, uint32_t* last_cluster)
{
    uint8_t s[512];
    char lname[64]; int lvalid = 0; DirPos lpos[21]; int ln = 0;
    uint32_t c = dir; int guard = 0;
    lname[0] = 0;
    while (c >= 2 && c < B.clusters + 2 && guard++ < 100000)
    {
        if (last_cluster) *last_cluster = c;
        for (uint32_t si = 0; si < B.spc; ++si)
        {
            uint64_t sec = csec(c) + si;
            if (rd(sec, s) != 0) return -1;
            for (int i = 0; i < 16; ++i)
            {
                uint8_t* e = s + i * 32;
                if (e[0] == 0x00)
                {
                    if (want_free && free_slot) { free_slot->cluster = c; free_slot->sector = sec; free_slot->index = i; return 1; }
                    return 0;
                }
                if (e[0] == 0xE5)
                {
                    if (want_free && free_slot) { free_slot->cluster = c; free_slot->sector = sec; free_slot->index = i; return 1; }
                    lvalid = 0; ln = 0; continue;
                }
                if (e[11] == 0x0F)
                {
                    int seq = e[0] & 0x1F;
                    if (e[0] & 0x40) { for (int k = 0; k < 63; ++k) lname[k] = 0; lvalid = 1; ln = 0; }
                    if (lvalid && seq >= 1 && seq <= 5)
                    {
                        static const int offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                        for (int k = 0; k < 13; ++k)
                        {
                            int pos = (seq - 1) * 13 + k; uint16_t ch = le16(e + offs[k]);
                            if (pos < 62) lname[pos] = (ch == 0 || ch == 0xFFFF) ? 0 : (ch < 128 ? (char)ch : '?');
                        }
                        if (ln < 20) { lpos[ln].cluster = c; lpos[ln].sector = sec; lpos[ln].index = i; ++ln; }
                    }
                    continue;
                }
                if (e[11] & 0x08) { lvalid = 0; ln = 0; continue; }     /* volume label */
                Found f; for (unsigned k = 0; k < sizeof(f); ++k) ((uint8_t*)&f)[k] = 0;
                short_to_name(e, f.name);
                if (lvalid && lname[0]) str_copy(f.name, lname, 64);
                f.attr = e[11]; f.is_dir = (e[11] & 0x10) != 0;
                f.cluster = ((uint32_t)le16(e + 20) << 16) | le16(e + 26);
                f.size = le32(e + 28);
                f.pos.cluster = c; f.pos.sector = sec; f.pos.index = i;
                f.dir_cluster = dir;
                f.nlfn = lvalid ? ln : 0;
                for (int k = 0; k < f.nlfn; ++k) f.lfn[k] = lpos[k];
                lvalid = 0; ln = 0; lname[0] = 0;
                if (cb && cb(e, &f, ctx)) return 1;
            }
        }
        uint32_t nx; if (fat_get(c, &nx) != 0) return -1;
        if (is_eoc(nx) || nx == 0) return 0;
        c = nx;
    }
    return 0;
}

struct FindCtx { const char* name; Found f; int found; };
static int find_cb(const uint8_t* e, const Found* f, void* ctx)
{
    FindCtx* fc = (FindCtx*)ctx;
    char sn[16]; short_to_name(e, sn);
    if (ci_equal(f->name, fc->name) || ci_equal(sn, fc->name)) { fc->f = *f; fc->found = 1; return 1; }
    return 0;
}

/* split "a/b/c" into parent directory cluster + last component; returns <0 on error */
static int resolve_parent(const char* path, uint32_t* dir, char* last, int cap)
{
    uint32_t cur = B.root;
    const char* p = path;
    while (*p == '/') ++p;
    if (!*p) return -22;
    for (;;)
    {
        char comp[64]; int k = 0;
        while (*p && *p != '/') { if (k < 63) comp[k++] = *p; ++p; }
        comp[k] = 0;
        while (*p == '/') ++p;
        if (!*p) { str_copy(last, comp, cap); *dir = cur; return 0; }
        FindCtx fc; fc.name = comp; fc.found = 0;
        if (walk_dir(cur, find_cb, &fc, 0, 0, 0) < 0) return -5;
        if (!fc.found) return -2;
        if (!fc.f.is_dir) return -20;
        cur = fc.f.cluster ? fc.f.cluster : B.root;
    }
}
static int lookup(const char* path, Found* out)
{
    const char* p = path; while (*p == '/') ++p;
    if (!*p) { for (unsigned k = 0; k < sizeof(*out); ++k) ((uint8_t*)out)[k] = 0; out->is_dir = 1; out->cluster = B.root; out->attr = 0x10; return 0; }
    uint32_t dir; char last[64];
    int rc = resolve_parent(path, &dir, last, 64);
    if (rc != 0) return rc;
    FindCtx fc; fc.name = last; fc.found = 0;
    if (walk_dir(dir, find_cb, &fc, 0, 0, 0) < 0) return -5;
    if (!fc.found) return -2;
    *out = fc.f; return 0;
}

/* ---- 8.3 name handling ---- */
static int make_short(const char* name, uint8_t* out11)
{
    for (int i = 0; i < 11; ++i) out11[i] = ' ';
    int base = 0, ext = 0, dot = 0;
    for (const char* p = name; *p; ++p)
    {
        char c = *p;
        if (c == '.') { if (dot || p == name) return -1; dot = 1; continue; }
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        int ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '$' || c == '~';
        if (!ok) return -1;
        if (!dot) { if (base >= 8) return -1; out11[base++] = (uint8_t)c; }
        else { if (ext >= 3) return -1; out11[8 + ext++] = (uint8_t)c; }
    }
    return base ? 0 : -1;
}
static void fat_stamp(uint8_t* e)
{
    uint64_t t = rtc_get_epoch();
    uint64_t days = t / 86400ULL, sod = t % 86400ULL;
    long z = (long)days + 719468; long era = (z >= 0 ? z : z - 146096) / 146097; long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; long y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100); long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1; long m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) ++y;
    if (y < 1980) { y = 1980; m = 1; d = 1; }
    uint32_t date = (uint32_t)(((y - 1980) << 9) | (m << 5) | d);
    uint32_t tm = (uint32_t)(((sod / 3600) << 11) | (((sod / 60) % 60) << 5) | ((sod % 60) / 2));
    put16(e + 14, tm); put16(e + 16, date); put16(e + 18, date); put16(e + 22, tm); put16(e + 24, date);
}

static int write_entry_at(const DirPos* pos, const uint8_t* e32)
{
    uint8_t s[512];
    if (rd(pos->sector, s) != 0) return -1;
    for (int i = 0; i < 32; ++i) s[pos->index * 32 + i] = e32[i];
    return wr(pos->sector, s);
}
static int update_entry(const DirPos* pos, uint32_t cluster, uint32_t size)
{
    uint8_t s[512];
    if (rd(pos->sector, s) != 0) return -1;
    uint8_t* e = s + pos->index * 32;
    put16(e + 20, cluster >> 16); put16(e + 26, cluster & 0xFFFF); put32(e + 28, size);
    fat_stamp(e);
    return wr(pos->sector, s);
}

/* make a new entry (short name only) in directory `dir` */
static int add_entry(uint32_t dir, const char* name, uint8_t attr, uint32_t cluster, uint32_t size, DirPos* where)
{
    uint8_t e[32]; for (int i = 0; i < 32; ++i) e[i] = 0;
    if (make_short(name, e) != 0) return -22;
    e[11] = attr;
    put16(e + 20, cluster >> 16); put16(e + 26, cluster & 0xFFFF); put32(e + 28, size);
    fat_stamp(e);
    DirPos fp; uint32_t lastc = dir;
    int rc = walk_dir(dir, 0, 0, 1, &fp, &lastc);
    if (rc < 0) return -5;
    if (rc == 0)
    {
        /* directory full: extend with a fresh cluster */
        uint32_t nc; int ar = alloc_cluster(&nc); if (ar != 0) return ar;
        if (fat_set(lastc, nc) != 0) return -5;
        fp.cluster = nc; fp.sector = csec(nc); fp.index = 0;
    }
    if (write_entry_at(&fp, e) != 0) return -5;
    if (where) *where = fp;
    return 0;
}

/* ---- public API ---- */
int fat_info(int dev, FatInfo* out)
{
    if (load_bpb(dev) != 0) return -1;
    out->total_sectors = B.total; out->sectors_per_cluster = B.spc; out->clusters = B.clusters; out->root_cluster = B.root;
    uint32_t freec = 0;
    for (uint32_t c = 2; c < B.clusters + 2; ++c) { uint32_t v; if (fat_get(c, &v) != 0) return -1; if (v == 0) ++freec; }
    out->free_clusters = freec;
    uint8_t s[512]; rd(0, s);
    for (int i = 0; i < 11; ++i) out->label[i] = (char)s[71 + i];
    out->label[11] = 0;
    for (int i = 10; i >= 0 && out->label[i] == ' '; --i) out->label[i] = 0;
    return 0;
}

int fat_mkfs(int dev, const char* label)
{
    g_dev = dev;
    uint64_t total = storage_dev_sectors(dev);
    if (total < 128) return -19;
    uint32_t reserved = 32, nfats = 2, spc = 1;
    if (total > 4194304ULL) spc = 8; else if (total > 524288ULL) spc = 4; else if (total > 65536ULL) spc = 2;
    /* FAT size: iterate to a fixed point */
    uint32_t fatsz = 1;
    for (int i = 0; i < 8; ++i)
    {
        uint64_t data = total - reserved - (uint64_t)nfats * fatsz;
        uint32_t clusters = (uint32_t)(data / spc);
        uint32_t need = (uint32_t)(((uint64_t)(clusters + 2) * 4 + 511) / 512);
        if (need == fatsz) break;
        fatsz = need;
    }
    uint8_t s[512]; for (int i = 0; i < 512; ++i) s[i] = 0;
    s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
    const char* oem = "RIOOS   "; for (int i = 0; i < 8; ++i) s[3 + i] = (uint8_t)oem[i];
    put16(s + 11, 512); s[13] = (uint8_t)spc; put16(s + 14, reserved); s[16] = (uint8_t)nfats;
    put16(s + 17, 0); put16(s + 19, 0); s[21] = 0xF8; put16(s + 22, 0); put16(s + 24, 32); put16(s + 26, 64); put32(s + 28, 0);
    put32(s + 32, (uint32_t)total); put32(s + 36, fatsz); put16(s + 40, 0); put16(s + 42, 0); put32(s + 44, 2);
    put16(s + 48, 1); put16(s + 50, 6); s[64] = 0x80; s[66] = 0x29; put32(s + 67, 0x52494F53U);
    for (int i = 0; i < 11; ++i) s[71 + i] = ' ';
    for (int i = 0; label && label[i] && i < 11; ++i) { char c = label[i]; if (c >= 'a' && c <= 'z') c = (char)(c - 32); s[71 + i] = (uint8_t)c; }
    const char* fst = "FAT32   "; for (int i = 0; i < 8; ++i) s[82 + i] = (uint8_t)fst[i];
    s[510] = 0x55; s[511] = 0xAA;
    if (wr(0, s) != 0) return -5;
    /* FSInfo (sector 1) */
    uint8_t fi[512]; for (int i = 0; i < 512; ++i) fi[i] = 0;
    put32(fi, 0x41615252U); put32(fi + 484, 0x61417272U); put32(fi + 488, 0xFFFFFFFFU); put32(fi + 492, 0xFFFFFFFFU); fi[510] = 0x55; fi[511] = 0xAA;
    if (wr(1, fi) != 0) return -5;
    if (wr(6, s) != 0) return -5;                      /* backup boot sector */
    uint8_t z[512]; for (int i = 0; i < 512; ++i) z[i] = 0;
    for (uint32_t f = 0; f < nfats; ++f)
        for (uint32_t i = 0; i < fatsz; ++i)
        {
            uint8_t b[512]; for (int k = 0; k < 512; ++k) b[k] = 0;
            if (i == 0) { put32(b, 0x0FFFFFF8U); put32(b + 4, 0x0FFFFFFFU); put32(b + 8, 0x0FFFFFFFU); }   /* media, EOC, root cluster EOC */
            if (wr((uint64_t)reserved + (uint64_t)f * fatsz + i, b) != 0) return -5;
        }
    uint64_t data = reserved + (uint64_t)nfats * fatsz;
    for (uint32_t i = 0; i < spc; ++i) if (wr(data + i, z) != 0) return -5;      /* root directory cluster 2 */
    return 0;
}

struct ListCtx { FatListCb cb; void* ctx; };
static int list_cb(const uint8_t* e, const Found* f, void* ctx)
{
    (void)e; ListCtx* l = (ListCtx*)ctx;
    if (f->name[0] == '.' && (f->name[1] == 0 || (f->name[1] == '.' && f->name[2] == 0))) return 0;
    l->cb(f->name, f->size, f->is_dir, l->ctx);
    return 0;
}
int fat_list(int dev, const char* path, FatListCb cb, void* ctx)
{
    if (load_bpb(dev) != 0) return -1;
    Found f; int rc = lookup(path, &f);
    if (rc != 0) return rc;
    if (!f.is_dir) return -20;
    ListCtx l = { cb, ctx };
    return walk_dir(f.cluster ? f.cluster : B.root, list_cb, &l, 0, 0, 0) < 0 ? -5 : 0;
}

int fat_read(int dev, const char* path, uint8_t* buf, uint32_t cap, uint32_t* size)
{
    if (load_bpb(dev) != 0) return -1;
    Found f; int rc = lookup(path, &f);
    if (rc != 0) return rc;
    if (f.is_dir) return -21;
    if (f.size > cap) return -27;
    uint32_t c = f.cluster, got = 0; int guard = 0;
    uint8_t s[512];
    while (got < f.size && c >= 2 && c < B.clusters + 2 && guard++ < 1000000)
    {
        for (uint32_t i = 0; i < B.spc && got < f.size; ++i)
        {
            if (rd(csec(c) + i, s) != 0) return -5;
            uint32_t take = f.size - got < 512 ? f.size - got : 512;
            for (uint32_t k = 0; k < take; ++k) buf[got + k] = s[k];
            got += take;
        }
        uint32_t nx; if (fat_get(c, &nx) != 0) return -5;
        if (is_eoc(nx) || nx == 0) break;
        c = nx;
    }
    if (got != f.size) return -5;
    *size = f.size;
    return 0;
}

int fat_write(int dev, const char* path, const uint8_t* data, uint32_t size)
{
    if (load_bpb(dev) != 0) return -1;
    uint32_t dir; char last[64];
    int rc = resolve_parent(path, &dir, last, 64);
    if (rc != 0) return rc;
    uint8_t chk[11]; if (make_short(last, chk) != 0) return -22;           /* name must be 8.3 */
    FindCtx fc; fc.name = last; fc.found = 0;
    if (walk_dir(dir, find_cb, &fc, 0, 0, 0) < 0) return -5;
    if (fc.found && fc.f.is_dir) return -21;
    /* allocate the new chain first so a full disk leaves the old file intact */
    uint32_t first = 0, prev = 0; uint32_t need = (size + B.spc * 512 - 1) / (B.spc * 512);
    for (uint32_t n = 0; n < need; ++n)
    {
        uint32_t c; int ar = alloc_cluster(&c);
        if (ar != 0) { if (first) free_chain(first); return ar; }
        if (prev) { if (fat_set(prev, c) != 0) return -5; } else first = c;
        prev = c;
    }
    uint32_t c = first; uint32_t off = 0; uint8_t s[512];
    while (c && off < size)
    {
        for (uint32_t i = 0; i < B.spc && off < size; ++i)
        {
            for (int k = 0; k < 512; ++k) s[k] = (off + (uint32_t)k < size) ? data[off + k] : 0;
            if (wr(csec(c) + i, s) != 0) { free_chain(first); return -5; }
            off += 512;
        }
        uint32_t nx; if (fat_get(c, &nx) != 0) return -5;
        c = is_eoc(nx) ? 0 : nx;
    }
    if (fc.found)
    {
        uint32_t oldc = fc.f.cluster;
        if (update_entry(&fc.f.pos, first, size) != 0) return -5;
        if (oldc) free_chain(oldc);
    }
    else
    {
        int ar = add_entry(dir, last, 0x20, first, size, 0);
        if (ar != 0) { if (first) free_chain(first); return ar; }
    }
    return storage_dev_flush(dev);
}

int fat_mkdir(int dev, const char* path)
{
    if (load_bpb(dev) != 0) return -1;
    uint32_t dir; char last[64];
    int rc = resolve_parent(path, &dir, last, 64);
    if (rc != 0) return rc;
    uint8_t chk[11]; if (make_short(last, chk) != 0) return -22;
    FindCtx fc; fc.name = last; fc.found = 0;
    if (walk_dir(dir, find_cb, &fc, 0, 0, 0) < 0) return -5;
    if (fc.found) return -17;
    uint32_t c; int ar = alloc_cluster(&c); if (ar != 0) return ar;
    uint8_t s[512]; for (int i = 0; i < 512; ++i) s[i] = 0;
    for (int i = 0; i < 11; ++i) { s[i] = ' '; s[32 + i] = ' '; }
    s[0] = '.'; s[32] = '.'; s[33] = '.';
    s[11] = 0x10; s[43] = 0x10;
    put16(s + 20, c >> 16); put16(s + 26, c & 0xFFFF);
    uint32_t parent = (dir == B.root) ? 0 : dir;
    put16(s + 52, parent >> 16); put16(s + 58, parent & 0xFFFF);
    fat_stamp(s); fat_stamp(s + 32);
    if (wr(csec(c), s) != 0) return -5;
    ar = add_entry(dir, last, 0x10, c, 0, 0);
    if (ar != 0) { free_chain(c); return ar; }
    return storage_dev_flush(dev);
}

static int nonempty_cb(const uint8_t* e, const Found* f, void* ctx)
{
    (void)e;
    if (f->name[0] == '.' && (f->name[1] == 0 || (f->name[1] == '.' && f->name[2] == 0))) return 0;
    *(int*)ctx = 1; return 1;
}
int fat_remove(int dev, const char* path)
{
    if (load_bpb(dev) != 0) return -1;
    Found f; int rc = lookup(path, &f);
    if (rc != 0) return rc;
    if (f.pos.sector == 0) return -22;                                      /* root */
    if (f.is_dir)
    {
        int nonempty = 0;
        if (walk_dir(f.cluster, nonempty_cb, &nonempty, 0, 0, 0) < 0) return -5;
        if (nonempty) return -39;
    }
    uint8_t s[512];
    for (int k = 0; k < f.nlfn; ++k)
    {
        if (rd(f.lfn[k].sector, s) != 0) return -5;
        s[f.lfn[k].index * 32] = 0xE5;
        if (wr(f.lfn[k].sector, s) != 0) return -5;
    }
    if (rd(f.pos.sector, s) != 0) return -5;
    s[f.pos.index * 32] = 0xE5;
    if (wr(f.pos.sector, s) != 0) return -5;
    if (f.cluster) free_chain(f.cluster);
    return storage_dev_flush(dev);
}
