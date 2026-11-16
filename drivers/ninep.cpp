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

#include "ninep.h"
#include "virtio_dev.h"
#include "timer.h"
#include "memory.h"
#include "string.h"

#define P9_DEVICE_ID 9U
#define P9_MSIZE 65536U
#define P9_TIMEOUT_MS 8000U
#define P9_MAX_COMPONENTS 64
#define P9_WALK_MAX 16
#define P9_NOFID 0xFFFFFFFFU
#define P9_TAG 1U

#define T_STATFS 8
#define T_LOPEN 12
#define T_LCREATE 14
#define T_SYMLINK 16
#define T_READLINK 22
#define T_GETATTR 24
#define T_SETATTR 26
#define T_READDIR 40
#define T_MKDIR 72
#define T_RENAMEAT 74
#define T_UNLINKAT 76
#define T_VERSION 100
#define T_ATTACH 104
#define T_WALK 110
#define T_READ 116
#define T_WRITE 118
#define T_CLUNK 120
#define R_LERROR 7

#define L_O_WRONLY 1U
#define L_O_EXCL 0200U
#define L_O_TRUNC 01000U
#define L_AT_REMOVEDIR 0x200U

#define E_IO 5
#define E_NOENT 2
#define E_INVAL 22
#define E_FBIG 27
#define E_PROTO 71
#define E_NAMETOOLONG 36

struct Share
{
    VirtioDev dev;
    VQ q;
    char tag[32];
    uint32_t msize;
    uint32_t next_fid;
    int ok;
};

static Share shares[NINEP_MAX_SHARES];
static VQMem share_q[NINEP_MAX_SHARES] VQ_MEM_ATTR;
static uint8_t tbuf[P9_MSIZE] __attribute__((aligned(16), section(".bss.virtio_req")));
static uint8_t rbuf[P9_MSIZE] __attribute__((aligned(16), section(".bss.virtio_req")));
static int share_total = 0;
static uint32_t requests = 0;

/* ---- little-endian message writer / reader ---- */
struct W { uint32_t n; int err; };
struct R { const uint8_t* b; uint32_t n, pos; int err; };

static void w_begin(W* w, uint8_t type)
{
    w->n = 7; w->err = 0;
    tbuf[4] = type; tbuf[5] = (uint8_t)(P9_TAG & 0xFF); tbuf[6] = (uint8_t)(P9_TAG >> 8);
}
static void w_u8(W* w, uint32_t v) { if (w->n + 1 > P9_MSIZE) { w->err = 1; return; } tbuf[w->n++] = (uint8_t)v; }
static void w_u16(W* w, uint32_t v) { w_u8(w, v & 0xFF); w_u8(w, (v >> 8) & 0xFF); }
static void w_u32(W* w, uint32_t v) { w_u16(w, v & 0xFFFF); w_u16(w, v >> 16); }
static void w_u64(W* w, uint64_t v) { w_u32(w, (uint32_t)v); w_u32(w, (uint32_t)(v >> 32)); }
static void w_str(W* w, const char* s, uint32_t len)
{
    if (len > 0xFFFF) { w->err = 1; return; }
    w_u16(w, len);
    if (w->n + len > P9_MSIZE) { w->err = 1; return; }
    for (uint32_t i = 0; i < len; ++i) tbuf[w->n++] = (uint8_t)s[i];
}
static void w_cstr(W* w, const char* s) { w_str(w, s, (uint32_t)str_len(s)); }

static uint32_t r_u8(R* r) { if (r->pos + 1 > r->n) { r->err = 1; return 0; } return r->b[r->pos++]; }
static uint32_t r_u16(R* r) { uint32_t a = r_u8(r); uint32_t b = r_u8(r); return a | (b << 8); }
static uint32_t r_u32(R* r) { uint32_t a = r_u16(r); uint32_t b = r_u16(r); return a | (b << 16); }
static uint64_t r_u64(R* r) { uint64_t a = r_u32(r); uint64_t b = r_u32(r); return a | (b << 32); }
static void r_skip(R* r, uint32_t n) { if (r->pos + n > r->n) { r->err = 1; r->pos = r->n; return; } r->pos += n; }
/* string into out (NUL terminated, truncated to cap-1); returns the wire length */
static uint32_t r_str(R* r, char* out, uint32_t cap)
{
    uint32_t len = r_u16(r);
    if (r->err || r->pos + len > r->n) { r->err = 1; if (cap) out[0] = 0; return 0; }
    uint32_t i = 0;
    for (; i < len && i + 1 < cap; ++i) out[i] = (char)r->b[r->pos + i];
    if (cap) out[i] = 0;
    r->pos += len;
    return len;
}

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* one request/response round trip; 0 and *r positioned after the header, or a negative errno */
static int rpc(Share* s, W* w, uint8_t rtype, R* r)
{
    if (!s || !s->ok) return -E_IO;
    if (w->err) return -E_NAMETOOLONG;
    put32(tbuf, w->n);
    rbuf[4] = 0;
    VSeg segs[2] = { { tbuf, w->n, 0 }, { rbuf, P9_MSIZE, 1 } };
    uint32_t got = 0;
    ++requests;
    if (vq_request(&s->q, segs, 2, P9_TIMEOUT_MS, &got) != 0) { s->ok = 0; return -E_IO; }   /* the device may still own the buffers */
    if (got < 7U) return -E_IO;
    uint32_t sz = get32(rbuf);
    uint8_t t = rbuf[4];
    if (sz < 7U || sz > got || sz > P9_MSIZE) return -E_IO;
    if (t == R_LERROR)
    {
        if (sz < 11U) return -E_IO;
        uint32_t e = get32(rbuf + 7);
        return (e == 0U || e > 4095U) ? -E_IO : -(int)e;
    }
    if (t != rtype) return -E_PROTO;
    r->b = rbuf; r->n = sz; r->pos = 7; r->err = 0;
    return 0;
}

static uint32_t new_fid(Share* s)
{
    uint32_t f = s->next_fid++;
    if (s->next_fid == 0U || s->next_fid >= 0x7FFFFFFFU) s->next_fid = 1;
    return f;
}

static void clunk(Share* s, uint32_t fid)
{
    W w; w_begin(&w, T_CLUNK); w_u32(&w, fid);
    R r; (void)rpc(s, &w, T_CLUNK + 1, &r);
}

/* ---- path helpers ---- */
struct Comp { const char* p; uint32_t n; };

static int split_path(const char* rel, Comp* out, int cap, int* count)
{
    int n = 0;
    const char* p = rel ? rel : "";
    while (*p)
    {
        while (*p == '/') ++p;
        if (!*p) break;
        const char* st = p;
        while (*p && *p != '/') ++p;
        uint32_t len = (uint32_t)(p - st);
        if (len == 1 && st[0] == '.') continue;
        if (len == 2 && st[0] == '.' && st[1] == '.') return -E_INVAL;
        if (len > 255U) return -E_NAMETOOLONG;
        if (n >= cap) return -E_NAMETOOLONG;
        out[n].p = st; out[n].n = len; ++n;
    }
    *count = n;
    return 0;
}

/* walk the first `count` components from the root; on success *out is a fresh fid the caller must clunk */
static int walk_comps(Share* s, const Comp* c, int count, uint32_t* out)
{
    uint32_t from = 0;     /* the attached root fid; never clunked */
    int done = 0;
    do
    {
        int n = count - done;
        if (n > P9_WALK_MAX) n = P9_WALK_MAX;
        uint32_t nf = new_fid(s);
        W w; w_begin(&w, T_WALK); w_u32(&w, from); w_u32(&w, nf); w_u16(&w, (uint32_t)n);
        for (int i = 0; i < n; ++i) w_str(&w, c[done + i].p, c[done + i].n);
        R r;
        int rc = rpc(s, &w, T_WALK + 1, &r);
        if (rc == 0)
        {
            uint32_t nq = r_u16(&r);
            if (r.err) rc = -E_IO;
            else if ((int)nq != n) rc = -E_NOENT;       /* partial walk: nf was not created */
        }
        if (from != 0U) clunk(s, from);
        if (rc != 0) return rc;
        from = nf;
        done += n;
    } while (done < count);
    *out = from;
    return 0;
}

static int walk_path(Share* s, const char* rel, uint32_t* out)
{
    Comp c[P9_MAX_COMPONENTS]; int n = 0;
    int rc = split_path(rel, c, P9_MAX_COMPONENTS, &n);
    if (rc != 0) return rc;
    return walk_comps(s, c, n, out);
}

/* walk to the parent directory of rel; *name / *namelen point into rel */
static int walk_parent(Share* s, const char* rel, uint32_t* dfid, Comp* name)
{
    Comp c[P9_MAX_COMPONENTS]; int n = 0;
    int rc = split_path(rel, c, P9_MAX_COMPONENTS, &n);
    if (rc != 0) return rc;
    if (n == 0) return -E_INVAL;
    *name = c[n - 1];
    return walk_comps(s, c, n - 1, dfid);
}

static Share* share_of(int i) { return (i >= 0 && i < share_total && shares[i].ok) ? &shares[i] : 0; }

/* ---- operations ---- */
static int op_lookup(int share, const char* rel, NinepAttr* out)
{
    Share* s = share_of(share); if (!s) return -E_IO;
    uint32_t fid; int rc = walk_path(s, rel, &fid);
    if (rc != 0) return rc;
    W w; w_begin(&w, T_GETATTR); w_u32(&w, fid); w_u64(&w, 0x7FFULL);          /* P9_GETATTR_BASIC */
    R r; rc = rpc(s, &w, T_GETATTR + 1, &r);
    if (rc == 0)
    {
        (void)r_u64(&r);                                                      /* valid */
        (void)r_u8(&r); uint32_t ver = r_u32(&r); uint64_t path = r_u64(&r);  /* qid */
        out->mode = r_u32(&r); out->uid = r_u32(&r); out->gid = r_u32(&r);
        (void)r_u64(&r); (void)r_u64(&r);                                     /* nlink, rdev */
        out->size = r_u64(&r);
        (void)r_u64(&r); (void)r_u64(&r);                                     /* blksize, blocks */
        (void)r_u64(&r); (void)r_u64(&r);                                     /* atime */
        out->mtime = r_u64(&r); out->mtime_ns = r_u64(&r);
        out->version = ver; out->ino = path;
        if (r.err) rc = -E_IO;
    }
    clunk(s, fid);
    return rc;
}

static int op_readdir(int share, const char* rel, NinepDirCb cb, void* ctx)
{
    Share* s = share_of(share); if (!s) return -E_IO;
    uint32_t fid; int rc = walk_path(s, rel, &fid);
    if (rc != 0) return rc;
    {
        W w; w_begin(&w, T_LOPEN); w_u32(&w, fid); w_u32(&w, 0);
        R r; rc = rpc(s, &w, T_LOPEN + 1, &r);
    }
    uint64_t offset = 0;
    uint32_t guard = 0;
    while (rc == 0 && ++guard < 100000U)
    {
        W w; w_begin(&w, T_READDIR); w_u32(&w, fid); w_u64(&w, offset); w_u32(&w, s->msize - 24U);
        R r; rc = rpc(s, &w, T_READDIR + 1, &r);
        if (rc != 0) break;
        uint32_t cnt = r_u32(&r);
        if (r.err || r.pos + cnt > r.n) { rc = -E_IO; break; }
        if (cnt == 0U) break;
        uint32_t end = r.pos + cnt;
        int stop = 0;
        uint64_t last = offset;
        while (r.pos + 24U <= end)
        {
            r_skip(&r, 13);
            uint64_t off = r_u64(&r);
            uint32_t type = r_u8(&r);
            char name[256];
            uint32_t nl = r_str(&r, name, sizeof(name));
            if (r.err || r.pos > end) { rc = -E_IO; break; }
            last = off;
            if (nl >= sizeof(name)) continue;
            uint32_t mt = type == 4U ? NINEP_S_IFDIR : type == 8U ? NINEP_S_IFREG : type == 10U ? NINEP_S_IFLNK : 0U;
            if (cb && cb(ctx, name, mt)) { stop = 1; break; }
        }
        if (rc != 0 || stop) break;
        if (last == offset) { rc = -E_IO; break; }                              /* no progress: refuse to loop forever */
        offset = last;
    }
    clunk(s, fid);
    return rc;
}

static int open_fid(Share* s, uint32_t fid, uint32_t flags)
{
    W w; w_begin(&w, T_LOPEN); w_u32(&w, fid); w_u32(&w, flags);
    R r; return rpc(s, &w, T_LOPEN + 1, &r);
}

static int op_read_file(int share, const char* rel, uint8_t* buf, uint64_t cap, uint64_t* out_size)
{
    Share* s = share_of(share); if (!s) return -E_IO;
    uint32_t fid; int rc = walk_path(s, rel, &fid);
    if (rc != 0) return rc;
    rc = open_fid(s, fid, 0);
    uint64_t total = 0;
    uint32_t chunk_max = s->msize - 24U;
    while (rc == 0)
    {
        uint64_t want = cap - total;
        int probe = 0;
        if (want == 0) { want = 1; probe = 1; }                                 /* buffer full: is there more? */
        uint32_t ask = want > chunk_max ? chunk_max : (uint32_t)want;
        W w; w_begin(&w, T_READ); w_u32(&w, fid); w_u64(&w, total); w_u32(&w, ask);
        R r; rc = rpc(s, &w, T_READ + 1, &r);
        if (rc != 0) break;
        uint32_t got = r_u32(&r);
        if (r.err || got > ask || r.pos + got > r.n) { rc = -E_IO; break; }
        if (got == 0U) break;
        if (probe) { rc = -E_FBIG; break; }
        for (uint32_t i = 0; i < got; ++i) buf[total + i] = r.b[r.pos + i];
        total += got;
    }
    clunk(s, fid);
    if (rc == 0 && out_size) *out_size = total;
    return rc;
}

static int write_fid(Share* s, uint32_t fid, const uint8_t* data, uint64_t size)
{
    uint64_t off = 0;
    uint32_t chunk_max = s->msize - 24U;
    while (off < size)
    {
        uint64_t left = size - off;
        uint32_t n = left > chunk_max ? chunk_max : (uint32_t)left;
        W w; w_begin(&w, T_WRITE); w_u32(&w, fid); w_u64(&w, off); w_u32(&w, n);
        if (w.n + n > P9_MSIZE) return -E_IO;
        for (uint32_t i = 0; i < n; ++i) tbuf[w.n + i] = data[off + i];
        w.n += n;
        R r; int rc = rpc(s, &w, T_WRITE + 1, &r);
        if (rc != 0) return rc;
        uint32_t wrote = r_u32(&r);
        if (r.err || wrote == 0U || wrote > n) return -E_IO;
        off += wrote;
    }
    return 0;
}

static int create_in(Share* s, uint32_t dfid, const Comp* name, uint32_t flags, uint32_t mode)
{
    W w; w_begin(&w, T_LCREATE); w_u32(&w, dfid); w_str(&w, name->p, name->n); w_u32(&w, flags); w_u32(&w, mode & 07777U); w_u32(&w, 0);
    R r; return rpc(s, &w, T_LCREATE + 1, &r);        /* on success dfid now refers to the new, opened file */
}

static int op_write_file(int share, const char* rel, const uint8_t* data, uint64_t size, uint32_t mode)
{
    Share* s = share_of(share); if (!s) return -E_IO;
    uint32_t fid; int rc = walk_path(s, rel, &fid);
    if (rc == 0)
    {
        rc = open_fid(s, fid, L_O_WRONLY | L_O_TRUNC);
        if (rc == 0 && size) rc = write_fid(s, fid, data, size);
        clunk(s, fid);
        return rc;
    }
    if (rc != -E_NOENT) return rc;
    Comp name; uint32_t dfid;
    rc = walk_parent(s, rel, &dfid, &name);
    if (rc != 0) return rc;
    rc = create_in(s, dfid, &name, L_O_WRONLY | L_O_EXCL, mode);
    if (rc == 0 && size) rc = write_fid(s, dfid, data, size);
    clunk(s, dfid);
    return rc;
}

static int op_create(int share, const char* rel, uint32_t mode)
{
    Share* s = share_of(share); if (!s) return -E_IO;
    Comp name; uint32_t dfid;
    int rc = walk_parent(s, rel, &dfid, &name);
    if (rc != 0) return rc;
    rc = create_in(s, dfid, &name, L_O_WRONLY | L_O_EXCL, mode);
    clunk(s, dfid);
    return rc;
}

static int op_mkdir(int share, const char* rel, uint32_t mode)
{
    Share* s = share_of(share); if (!s) return -E_IO;
    Comp name; uint32_t dfid;
    int rc = walk_parent(s, rel, &dfid, &name);
    if (rc != 0) return rc;
    W w; w_begin(&w, T_MKDIR); w_u32(&w, dfid); w_str(&w, name.p, name.n); w_u32(&w, mode & 07777U); w_u32(&w, 0);
    R r; rc = rpc(s, &w, T_MKDIR + 1, &r);
    clunk(s, dfid);
    return rc;
}

static int op_remove(int share, const char* rel, int is_dir)
{
    Share* s = share_of(share); if (!s) return -E_IO;
    Comp name; uint32_t dfid;
    int rc = walk_parent(s, rel, &dfid, &name);
    if (rc != 0) return rc;
    W w; w_begin(&w, T_UNLINKAT); w_u32(&w, dfid); w_str(&w, name.p, name.n); w_u32(&w, is_dir ? L_AT_REMOVEDIR : 0U);
    R r; rc = rpc(s, &w, T_UNLINKAT + 1, &r);
    clunk(s, dfid);
    return rc;
}

/* depth-first removal; every level keeps its own copy of the names because rbuf is reused by each request */
struct TreeNames { char (*names)[64]; uint8_t* isdir; int n; int cap; };
static int tree_cb(void* ctx, const char* name, uint32_t mt)
{
    TreeNames* t = (TreeNames*)ctx;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) return 0;
    if (str_len(name) >= 64 || t->n >= t->cap) { t->n = -1; return 1; }       /* too long / too many: give up */
    str_copy(t->names[t->n], name, 64);
    t->isdir[t->n] = mt == NINEP_S_IFDIR;
    ++t->n;
    return 0;
}

static int remove_tree_at(int share, const char* rel, int depth)
{
    if (depth > 12) return -E_NAMETOOLONG;
    NinepAttr a; int rc = op_lookup(share, rel, &a);
    if (rc != 0) return rc;
    int is_dir = (a.mode & NINEP_S_IFMT) == NINEP_S_IFDIR;
    if (is_dir)
    {
        const int cap = 256;
        char (*names)[64] = (char (*)[64])kmalloc((uint64_t)cap * 64U);
        uint8_t* isdir = (uint8_t*)kmalloc((uint64_t)cap);
        if (!names || !isdir) { if (names) kfree(names); if (isdir) kfree(isdir); return -12; }
        TreeNames t; t.names = names; t.isdir = isdir; t.n = 0; t.cap = cap;
        rc = op_readdir(share, rel, tree_cb, &t);
        if (rc == 0 && t.n < 0) rc = -E_NAMETOOLONG;
        for (int i = 0; rc == 0 && i < t.n; ++i)
        {
            char child[300];
            int p = 0;
            for (const char* q = rel; *q && p < 200; ++q) child[p++] = *q;
            if (p > 0 && child[p - 1] != '/') child[p++] = '/';
            for (const char* q = names[i]; *q && p < 298; ++q) child[p++] = *q;
            child[p] = 0;
            rc = remove_tree_at(share, child, depth + 1);
        }
        kfree(names); kfree(isdir);
        if (rc != 0) return rc;
    }
    return op_remove(share, rel, is_dir);
}

static int op_remove_tree(int share, const char* rel)
{
    Comp c[P9_MAX_COMPONENTS]; int n = 0;
    int rc = split_path(rel, c, P9_MAX_COMPONENTS, &n);
    if (rc != 0) return rc;
    if (n == 0) return -E_INVAL;                    /* never delete the share root */
    return remove_tree_at(share, rel, 0);
}

static int op_rename(int share, const char* from, const char* to)
{
    Share* s = share_of(share); if (!s) return -E_IO;
    Comp fn, tn; uint32_t fd, td;
    int rc = walk_parent(s, from, &fd, &fn);
    if (rc != 0) return rc;
    rc = walk_parent(s, to, &td, &tn);
    if (rc != 0) { clunk(s, fd); return rc; }
    W w; w_begin(&w, T_RENAMEAT); w_u32(&w, fd); w_str(&w, fn.p, fn.n); w_u32(&w, td); w_str(&w, tn.p, tn.n);
    R r; rc = rpc(s, &w, T_RENAMEAT + 1, &r);
    clunk(s, td); clunk(s, fd);
    return rc;
}

static int op_setattr(int share, const char* rel, uint32_t valid, uint32_t mode, uint32_t uid, uint32_t gid, uint64_t size, uint64_t mtime)
{
    Share* s = share_of(share); if (!s) return -E_IO;
    uint32_t fid; int rc = walk_path(s, rel, &fid);
    if (rc != 0) return rc;
    W w; w_begin(&w, T_SETATTR); w_u32(&w, fid); w_u32(&w, valid); w_u32(&w, mode & 07777U); w_u32(&w, uid); w_u32(&w, gid);
    w_u64(&w, size); w_u64(&w, 0); w_u64(&w, 0); w_u64(&w, mtime); w_u64(&w, 0);
    R r; rc = rpc(s, &w, T_SETATTR + 1, &r);
    clunk(s, fid);
    return rc;
}

static int op_symlink(int share, const char* rel, const char* target)
{
    Share* s = share_of(share); if (!s) return -E_IO;
    Comp name; uint32_t dfid;
    int rc = walk_parent(s, rel, &dfid, &name);
    if (rc != 0) return rc;
    W w; w_begin(&w, T_SYMLINK); w_u32(&w, dfid); w_str(&w, name.p, name.n); w_cstr(&w, target); w_u32(&w, 0);
    R r; rc = rpc(s, &w, T_SYMLINK + 1, &r);
    clunk(s, dfid);
    return rc;
}

static int op_readlink(int share, const char* rel, char* out, int cap)
{
    Share* s = share_of(share); if (!s || cap < 2) return -E_IO;
    uint32_t fid; int rc = walk_path(s, rel, &fid);
    if (rc != 0) return rc;
    W w; w_begin(&w, T_READLINK); w_u32(&w, fid);
    R r; rc = rpc(s, &w, T_READLINK + 1, &r);
    int len = 0;
    if (rc == 0)
    {
        uint32_t wl = r_u16(&r);
        if (r.err || r.pos + wl > r.n) rc = -E_IO;
        else if (wl >= (uint32_t)cap) rc = -E_NAMETOOLONG;
        else { for (uint32_t i = 0; i < wl; ++i) out[i] = (char)r.b[r.pos + i]; out[wl] = 0; len = (int)wl; }
    }
    clunk(s, fid);
    return rc == 0 ? len : rc;
}

static int op_statfs(int share, uint64_t* total, uint64_t* freeb)
{
    Share* s = share_of(share); if (!s) return -E_IO;
    W w; w_begin(&w, T_STATFS); w_u32(&w, 0);
    R r; int rc = rpc(s, &w, T_STATFS + 1, &r);
    if (rc != 0) return rc;
    (void)r_u32(&r);
    uint64_t bsize = r_u32(&r), blocks = r_u64(&r), bfree = r_u64(&r);
    if (r.err) return -E_IO;
    if (total) *total = blocks * bsize;
    if (freeb) *freeb = bfree * bsize;
    return 0;
}

static uint64_t op_now() { return timer_millis(); }

static const NinepOps ops = {
    op_now, op_lookup, op_readdir, op_read_file, op_write_file, op_create, op_mkdir, op_remove, op_remove_tree,
    op_rename, op_setattr, op_symlink, op_readlink, op_statfs
};

const NinepOps* ninep_ops() { return &ops; }

/* ---- device bring-up ---- */
static int share_open(int index)
{
    Share* s = &shares[index];
    s->ok = 0; s->tag[0] = 0; s->msize = 0; s->next_fid = 1;
    if (!virtio_dev_open(&s->dev, P9_DEVICE_ID, (uint32_t)index, 1U)) return 0;     /* feature bit 0 = MOUNT_TAG */
    if ((s->dev.features_lo & 1U) == 0U || !virtio_dev_queue(&s->dev, 0, &s->q, &share_q[index], 8) || !virtio_dev_start(&s->dev))
    {
        virtio_dev_reset(&s->dev);
        return 0;
    }
    uint32_t tl = (uint32_t)virtio_dev_cfg8(&s->dev, 0) | ((uint32_t)virtio_dev_cfg8(&s->dev, 1) << 8);
    if (tl == 0U || tl >= sizeof(s->tag)) { virtio_dev_reset(&s->dev); return 0; }
    for (uint32_t i = 0; i < tl; ++i) s->tag[i] = (char)virtio_dev_cfg8(&s->dev, 2U + i);
    s->tag[tl] = 0;
    s->ok = 1;                                                                       /* rpc() needs ok while negotiating */
    s->msize = P9_MSIZE;

    W w; w_begin(&w, T_VERSION); w_u32(&w, P9_MSIZE); w_cstr(&w, "9P2000.L");
    tbuf[5] = 0xFF; tbuf[6] = 0xFF;                                                  /* NOTAG */
    R r;
    int rc = rpc(s, &w, T_VERSION + 1, &r);
    if (rc == 0)
    {
        uint32_t ms = r_u32(&r);
        char ver[16]; (void)r_str(&r, ver, sizeof(ver));
        if (r.err || !str_equal(ver, "9P2000.L") || ms < 4096U) rc = -E_PROTO;
        else s->msize = ms < P9_MSIZE ? ms : P9_MSIZE;
    }
    if (rc == 0)
    {
        W a; w_begin(&a, T_ATTACH); w_u32(&a, 0); w_u32(&a, P9_NOFID); w_cstr(&a, "root"); w_cstr(&a, ""); w_u32(&a, 0);
        rc = rpc(s, &a, T_ATTACH + 1, &r);
    }
    if (rc != 0) { s->ok = 0; virtio_dev_reset(&s->dev); return 0; }
    return 1;
}

int ninep_init()
{
    share_total = 0;
    for (int i = 0; i < NINEP_MAX_SHARES; ++i)
    {
        shares[i].ok = 0; shares[i].tag[0] = 0;
        if (share_open(i)) share_total = i + 1;
        else if (!shares[i].dev.mmio) break;                                         /* no more devices */
    }
    int usable = 0;
    for (int i = 0; i < share_total; ++i) if (shares[i].ok) ++usable;
    return usable;
}

int ninep_share_count() { return share_total; }
const char* ninep_share_tag(int share) { return (share >= 0 && share < share_total && shares[share].ok) ? shares[share].tag : 0; }
int ninep_share_find(const char* tag)
{
    if (!tag) return -1;
    for (int i = 0; i < share_total; ++i) if (shares[i].ok && str_equal(shares[i].tag, tag)) return i;
    return -1;
}
uint32_t ninep_share_msize(int share) { return (share >= 0 && share < share_total) ? shares[share].msize : 0; }
int ninep_share_alive(int share) { return share >= 0 && share < share_total && shares[share].ok; }
uint32_t ninep_request_count() { return requests; }
