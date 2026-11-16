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

/* virtio-9p mounts for fs.cpp. This file is #included in the middle of fs.cpp, inside the region where the implementation
   names are macro-renamed (fs_resolve is fs_resolve_impl, `nodes` is cur->nodes, ...), so it works on the node table of
   the current mount (`cur`) and uses the same helpers as the rest of the implementation.

   Design: a 9p mount is an ordinary in-RAM filesystem (the "cache") whose tree and file contents are a *mirror* of the
   share. The server is the single source of truth:
     - directory listings are read lazily (readdir + getattr per entry) the first time a directory is walked and again
       when the copy is older than P9_TTL_MS; entries that vanished are dropped, new ones appear, attributes follow;
     - file data is fetched on demand (whole file, <= FS_MAX_FILE_SIZE) and re-fetched whenever the server's
       version/size/mtime(+ns) differ; every read does one getattr, so changes made on the host are seen immediately;
     - every mutation is applied to the cache first (that gives the usual permission / existence checks and the same
       error codes as any other mount) and then pushed to the server synchronously; when the server refuses, the cache
       is brought back in line with the server and the error is returned. Nothing is ever dirty, so unmounting and
       power loss cannot lose data that a call already reported as written.
   fs.cpp keeps the whole-file API, so the cache never needs partial writes. When the 8 MiB cache is full, data of other
   files is simply dropped (it is re-fetched when needed). */

static const NinepOps* p9ops = 0;
#define P9_TTL_MS 1000ULL
#define P9_REL_MAX 256

static int p9_on() { return p9ops != 0 && cur->p9share1 != 0; }
static int p9_share() { return cur->p9share1 - 1; }

/* path of node `id` relative to the share root ("" for the root) */
static int p9_node_rel(int id, char* out, int cap)
{
    int idx[32]; int n = 0;
    for (int i = id; i >= 0 && i != root_id; i = nodes[i].parent)
    {
        if (n >= 32 || !nodes[i].used) return -1;
        idx[n++] = i;
    }
    int p = 0;
    for (int k = n - 1; k >= 0; --k)
    {
        if (p) { if (p >= cap - 1) return -1; out[p++] = '/'; }
        for (const char* q = nodes[idx[k]].name; *q; ++q) { if (p >= cap - 1) return -1; out[p++] = *q; }
    }
    out[p] = 0;
    return 0;
}

static int p9_join(char* out, int cap, const char* rel, const char* name)
{
    int p = 0;
    for (const char* q = rel; *q; ++q) { if (p >= cap - 1) return -1; out[p++] = *q; }
    if (p) { if (p >= cap - 1) return -1; out[p++] = '/'; }
    for (const char* q = name; *q; ++q) { if (p >= cap - 1) return -1; out[p++] = *q; }
    out[p] = 0;
    return 0;
}

/* server errno -> the error codes the rest of fs.cpp uses; kind 0 = create/write/attributes, 1 = remove, 2 = rename */
static int p9_map(int rc, int kind)
{
    if (rc >= 0) return 0;
    int e = -rc;
    if (e == 13 || e == 1) return -13;
    if (e == 30) return -30;
    if (kind == 1)
    {
        if (e == 2) return 0;                                  /* already gone on the server: the goal is reached */
        if (e == 39 || e == 17) return -2;                     /* directory not empty */
        if (e == 16) return -16;
        return -4;
    }
    if (kind == 2)
    {
        if (e == 2) return -1;
        if (e == 17 || e == 39 || e == 21 || e == 20 || e == 16) return -2;
        if (e == 22) return -5;
        return -6;
    }
    if (e == 17) return -17;
    if (e == 28 || e == 27 || e == 122) return -3;
    if (e == 2) return -1;
    if (e == 20 || e == 21) return -2;
    return -4;
}

/* ---- cache bookkeeping ---- */
static void p9_drop_data(int id)
{
    FsNode* n = &nodes[id];
    if (n->type == FS_FILE && n->sector_count)
    {
        free_sectors(n->first_sector, n->sector_count);
        n->first_sector = 0; n->sector_count = 0;
    }
    cur->p9n[id].data_ok = 0;
}

/* remove a node (and everything below it) from the cache only; the server is not touched. Nodes that carry a
   mounted filesystem are kept (and so are their parents). */
static void p9_drop_subtree(int id, int depth)
{
    if (id <= 0 || id >= FS_MAX_NODES || !nodes[id].used || depth > 40) return;
    if (nodes[id].type == FS_DIR)
    {
        for (int i = 1; i < FS_MAX_NODES; ++i)
            if (i != id && nodes[i].used && nodes[i].parent == id) p9_drop_subtree(i, depth + 1);
        for (int i = 1; i < FS_MAX_NODES; ++i)
            if (i != id && nodes[i].used && nodes[i].parent == id) return;
    }
    if (nodes[id].mount_ctx > 0) return;
    if (nodes[id].type != FS_DIR) free_sectors(nodes[id].first_sector, nodes[id].sector_count);
    clear_bytes(&nodes[id], sizeof(nodes[id]));
    clear_bytes(&cur->p9n[id], sizeof(cur->p9n[id]));
    (void)save_inode(id);
}

/* make room: drop the cached data of every file except `keep` */
static void p9_evict(int keep)
{
    for (int i = 1; i < FS_MAX_NODES; ++i)
    {
        if (i == keep || !nodes[i].used || nodes[i].type != FS_FILE || nodes[i].sector_count == 0) continue;
        p9_drop_data(i);
        (void)save_inode(i);
    }
    (void)write_bitmap();
}

static int p9_alloc(uint32_t sectors, uint64_t* first, int keep)
{
    if (alloc_contiguous(sectors, first) == 0) return 0;
    p9_evict(keep);
    return alloc_contiguous(sectors, first);
}

/* free data sectors in the cache */
static uint64_t p9_free_sectors()
{
    uint64_t total = disk_sectors > FS_DATA_START ? disk_sectors - FS_DATA_START : 0;
    uint64_t used = 0;
    for (int i = 0; i < FS_MAX_NODES; ++i)
        if (nodes[i].used && nodes[i].type != FS_DIR) used += nodes[i].sector_count;
    return used < total ? total - used : 0;
}

static void p9_make_room(uint64_t bytes, int keep)
{
    uint64_t need = (bytes + FS_SECTOR_SIZE - 1ULL) / FS_SECTOR_SIZE + 8ULL;
    if (p9_free_sectors() < need) p9_evict(keep);
}

/* copy the server's attributes into the node; cached data that no longer matches the server is dropped. have_data: the
   cache holds the content the server has now (right after we wrote it). */
static void p9_set_attr(int id, const NinepAttr* a, int have_data)
{
    FsNode* n = &nodes[id];
    P9Node* pn = &cur->p9n[id];
    n->mode = a->mode & 0777U; n->uid = a->uid; n->gid = a->gid; n->mtime = a->mtime;
    if (n->type == FS_FILE)
    {
        int same = pn->data_ok && n->size == a->size && pn->ver == a->version && pn->srv_mtime == a->mtime && pn->srv_mtime_ns == a->mtime_ns;
        if (have_data && n->size == a->size)
        {
            pn->data_ok = 1; pn->ver = a->version; pn->srv_mtime = a->mtime; pn->srv_mtime_ns = a->mtime_ns;
        }
        else if (!same)
        {
            p9_drop_data(id);
            n->size = a->size;
        }
    }
    (void)save_inode(id);
}

static int p9_sync_node(int id, int have_data)
{
    char rel[P9_REL_MAX];
    if (p9_node_rel(id, rel, sizeof(rel)) != 0) return -36;
    NinepAttr a;
    int rc = p9ops->lookup(p9_share(), rel, &a);
    if (rc != 0) return rc;
    p9_set_attr(id, &a, have_data);
    (void)write_bitmap();
    return 0;
}

static void p9_invalidate_all()
{
    for (int i = 0; i < FS_MAX_NODES; ++i) cur->p9n[i].listed = 0;
}

static uint8_t p9_sec[FS_SECTOR_SIZE] __attribute__((aligned(16)));

/* symbolic link: the target text lives in one cache sector, like on every other mount */
static int p9_load_link(int id, const char* rel, const NinepAttr* a)
{
    FsNode* n = &nodes[id];
    P9Node* pn = &cur->p9n[id];
    if (pn->data_ok && n->sector_count == 1 && pn->ver == a->version && pn->srv_mtime == a->mtime && pn->srv_mtime_ns == a->mtime_ns) return 0;
    char tgt[260];
    int len = p9ops->readlink(p9_share(), rel, tgt, (int)sizeof(tgt));
    if (len <= 0 || len > 255) return -1;
    if (n->sector_count) { free_sectors(n->first_sector, n->sector_count); n->sector_count = 0; n->first_sector = 0; n->size = 0; pn->data_ok = 0; }
    uint64_t first = 0;
    if (p9_alloc(1, &first, -1) != 0) return -3;
    clear_bytes(p9_sec, FS_SECTOR_SIZE);
    for (int i = 0; i < len; ++i) p9_sec[i] = (uint8_t)tgt[i];
    if (storage_write_sector(first, p9_sec) != 0) { free_sectors(first, 1); return -4; }
    n->size = (uint64_t)len; n->first_sector = first; n->sector_count = 1;
    pn->data_ok = 1; pn->ver = a->version; pn->srv_mtime = a->mtime; pn->srv_mtime_ns = a->mtime_ns;
    return 0;
}

/* ---- directory listing ---- */
struct P9Ent { char name[FS_NAME_SIZE]; };
static P9Ent p9_ent[FS_NODE_CAP + 8];
static int p9_nent;
static int p9_ent_over;

static int p9_collect(void*, const char* name, uint32_t mt)
{
    if (!name[0] || (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))) return 0;
    if (mt == 0) return 0;                                   /* sockets, devices, fifos: not representable */
    if (str_len(name) >= FS_NAME_SIZE) { ++p9_ent_over; return 0; }
    if (p9_nent >= FS_NODE_CAP + 8) { ++p9_ent_over; return 1; }
    str_copy(p9_ent[p9_nent++].name, name, FS_NAME_SIZE);
    return 0;
}

/* Bring the cached copy of directory d in line with the server (only when older than the TTL unless forced). */
static int p9_refresh_dir(int d, int force)
{
    if (!p9_on()) return 0;
    if (d < 0 || d >= FS_MAX_NODES || !nodes[d].used || nodes[d].type != FS_DIR) return 0;
    P9Node* pd = &cur->p9n[d];
    if (!force && pd->listed && p9ops->now_ms() - pd->ts < P9_TTL_MS) return 0;
    char rel[P9_REL_MAX];
    if (p9_node_rel(d, rel, sizeof(rel)) != 0) return -36;
    p9_nent = 0; p9_ent_over = 0;
    int rc = p9ops->readdir(p9_share(), rel, p9_collect, 0);
    if (rc != 0)
    {
        if (rc == -2 && d != root_id) p9_drop_subtree(d, 0);   /* the directory itself is gone */
        return rc;
    }
    static uint8_t seen[FS_NODE_CAP];
    for (int i = 0; i < FS_NODE_CAP; ++i) seen[i] = 0;
    int lost = p9_ent_over;
    for (int k = 0; k < p9_nent; ++k)
    {
        const char* name = p9_ent[k].name;
        char crel[P9_REL_MAX];
        if (p9_join(crel, sizeof(crel), rel, name) != 0) { ++lost; continue; }
        NinepAttr a;
        if (p9ops->lookup(p9_share(), crel, &a) != 0) continue;            /* vanished meanwhile: dropped below */
        uint32_t ft = a.mode & NINEP_S_IFMT;
        int type = ft == NINEP_S_IFDIR ? FS_DIR : ft == NINEP_S_IFLNK ? FS_SYMLINK : ft == NINEP_S_IFREG ? FS_FILE : -1;
        if (type < 0) continue;
        int id = find_child(d, name);
        if (id >= 0 && nodes[id].type != type)
        {
            if (nodes[id].mount_ctx > 0) { seen[id] = 1; continue; }
            p9_drop_subtree(id, 0);
            id = find_child(d, name);                                      /* still there only when it kept a mount below it */
            if (id >= 0) { seen[id] = 1; continue; }
        }
        if (id < 0)
        {
            id = create_node(type, name, d);
            if (id < 0) { ++lost; continue; }                              /* node table full */
            clear_bytes(&cur->p9n[id], sizeof(cur->p9n[id]));
        }
        seen[id] = 1;
        if (type == FS_SYMLINK)
        {
            if (p9_load_link(id, crel, &a) != 0) { seen[id] = 0; p9_drop_subtree(id, 0); ++lost; continue; }
        }
        p9_set_attr(id, &a, 0);
    }
    for (int i = 1; i < FS_MAX_NODES; ++i)
        if (nodes[i].used && nodes[i].parent == d && !seen[i] && nodes[i].mount_ctx <= 0) p9_drop_subtree(i, 0);
    (void)write_bitmap();
    if (lost) cur->p9lost = cur->p9lost + lost > 1000000 ? 1000000 : cur->p9lost + lost;   /* reported by 'mount' */
    pd->listed = 1;
    pd->ts = p9ops->now_ms();
    return 0;
}

/* ---- file data ---- */
/* Make the cached data of file `id` current. 0 or a server errno (negative). */
static int p9_ensure_data(int id)
{
    if (!p9_on() || id < 0 || id >= FS_MAX_NODES || !nodes[id].used || nodes[id].type != FS_FILE) return 0;
    char rel[P9_REL_MAX];
    if (p9_node_rel(id, rel, sizeof(rel)) != 0) return -36;
    NinepAttr a;
    int rc = p9ops->lookup(p9_share(), rel, &a);
    if (rc != 0)
    {
        if (rc == -2) { int par = nodes[id].parent; (void)p9_refresh_dir(par, 1); }
        return rc;
    }
    if ((a.mode & NINEP_S_IFMT) != NINEP_S_IFREG) { int par = nodes[id].parent; (void)p9_refresh_dir(par, 1); return -2; }
    p9_set_attr(id, &a, 0);                                                /* drops the cached copy when the server's differs */
    if (cur->p9n[id].data_ok) return 0;
    if (a.size > FS_MAX_FILE_SIZE) return -27;
    uint64_t got = 0;
    rc = p9ops->read_file(p9_share(), rel, truncate_buffer, FS_MAX_FILE_SIZE, &got);
    if (rc != 0) return rc;
    if (got > FS_MAX_FILE_SIZE) return -27;
    uint32_t need = (uint32_t)((got + FS_SECTOR_SIZE - 1ULL) / FS_SECTOR_SIZE);
    p9_drop_data(id);
    uint64_t first = 0;
    if (need && p9_alloc(need, &first, id) != 0) { (void)write_bitmap(); return -28; }
    for (uint32_t s = 0; s < need; ++s)
    {
        clear_bytes(io_buffer, FS_SECTOR_SIZE);
        uint64_t off = (uint64_t)s * FS_SECTOR_SIZE;
        uint64_t left = got > off ? got - off : 0;
        uint64_t take = left > FS_SECTOR_SIZE ? FS_SECTOR_SIZE : left;
        for (uint64_t i = 0; i < take; ++i) io_buffer[i] = truncate_buffer[off + i];
        if (storage_write_sector(first + s, io_buffer) != 0) { free_sectors(first, need); (void)write_bitmap(); return -5; }
    }
    nodes[id].size = got; nodes[id].first_sector = first; nodes[id].sector_count = need;
    P9Node* pn = &cur->p9n[id];
    pn->data_ok = 1; pn->ver = a.version; pn->srv_mtime = a.mtime; pn->srv_mtime_ns = a.mtime_ns;
    if (save_inode(id) != 0 || write_bitmap() != 0) return -5;
    return 0;
}

/* before reading a file through the implementation: 0 or the error code to return */
static int p9_pre_read(const char* rest, int cwd)
{
    if (!p9_on() || rest == 0) return 0;
    int id = fs_resolve(rest, cwd);
    if (id < 0 || nodes[id].type != FS_FILE) return 0;                    /* the implementation reports that */
    int rc = p9_ensure_data(id);
    if (rc == 0) return 0;
    return rc == -2 ? -2 : rc == -13 ? -13 : (rc == -27 || rc == -28) ? -3 : -4;
}

/* ---- pushing local changes to the server ---- */
static void p9_drop_pending()
{
    for (int i = 1; i < FS_MAX_NODES; ++i)
        if (nodes[i].used && cur->p9n[i].pending) p9_drop_subtree(i, 0);
    (void)write_bitmap();
}

/* create on the server every node the implementation just created (parents first). On failure the local nodes are removed
   again and the (negative) server errno is returned. Regular files only get created (empty): the caller writes the data
   and then calls p9_sync_node. */
static int p9_flush_pending()
{
    for (int pass = 0; pass <= FS_MAX_NODES; ++pass)
    {
        int any = 0, did = 0;
        for (int i = 1; i < FS_MAX_NODES; ++i)
        {
            if (!nodes[i].used || !cur->p9n[i].pending) continue;
            any = 1;
            int par = nodes[i].parent;
            if (par >= 0 && cur->p9n[par].pending) continue;               /* parent first */
            char rel[P9_REL_MAX];
            int rc = p9_node_rel(i, rel, sizeof(rel));
            if (rc != 0) rc = -36;
            else if (nodes[i].type == FS_DIR) rc = p9ops->mkdir(p9_share(), rel, nodes[i].mode);
            else if (nodes[i].type == FS_FILE) rc = p9ops->create(p9_share(), rel, nodes[i].mode);
            else
            {
                char tgt[260];
                int tl = read_link_target(i, tgt, (int)sizeof(tgt));
                rc = tl > 0 ? p9ops->symlink(p9_share(), rel, tgt) : -22;
            }
            if (rc != 0) { p9_drop_pending(); return rc; }
            cur->p9n[i].pending = 0;
            did = 1;
            if (nodes[i].type != FS_FILE)
            {
                NinepAttr a;
                if (p9ops->lookup(p9_share(), rel, &a) == 0)
                {
                    p9_set_attr(i, &a, 0);
                    P9Node* pn = &cur->p9n[i];
                    if (nodes[i].type == FS_DIR) { pn->listed = 1; pn->ts = p9ops->now_ms(); }
                    else { pn->data_ok = 1; pn->ver = a.version; pn->srv_mtime = a.mtime; pn->srv_mtime_ns = a.mtime_ns; }
                }
            }
        }
        if (!any) return 0;
        if (!did) break;
    }
    p9_drop_pending();
    return -4;
}

static int p9_push_attr(int id, uint32_t valid)
{
    char rel[P9_REL_MAX];
    if (p9_node_rel(id, rel, sizeof(rel)) != 0) return -36;
    int rc = p9ops->setattr(p9_share(), rel, valid, nodes[id].mode, nodes[id].uid, nodes[id].gid, 0, nodes[id].mtime);
    (void)p9_sync_node(id, 0);                                             /* success or not: the server's values win */
    return rc;
}

/* post-processing of the implementation's result for the public wrappers */
static int p9_post_create(int v)
{
    if (!p9_on()) return v;
    if (v < 0) { p9_drop_pending(); return v; }
    int rc = p9_flush_pending();
    return rc == 0 ? v : p9_map(rc, 0);
}

static int p9_pre_exists(const char* rest, int cwd) { return p9_on() && rest && fs_resolve(rest, cwd) >= 0; }

static int p9_post_touch(int existed, int v, const char* rest, int cwd)
{
    if (!p9_on()) return v;
    if (v >= 0)
    {
        int rc = p9_flush_pending();
        if (rc != 0) return p9_map(rc, 0);
        (void)p9_sync_node(v, 1);
        return v;
    }
    if (!existed || v != -2) { p9_drop_pending(); return v; }
    int id = rest ? fs_resolve(rest, cwd) : -1;
    if (id < 0) return v;
    int rc = p9_push_attr(id, NINEP_SET_MTIME);
    return rc == 0 ? v : p9_map(rc, 0);
}

static void p9_pre_write(uint64_t size) { if (p9_on()) p9_make_room(size * 2ULL, -1); }

static int p9_post_write(int rc, const char* rest, int cwd, const uint8_t* data, uint64_t size)
{
    if (!p9_on()) return rc;
    if (rc != 0) { p9_drop_pending(); return rc; }
    int id = rest ? fs_resolve(rest, cwd) : -1;
    if (id < 0 || nodes[id].type != FS_FILE) { p9_drop_pending(); return -4; }
    int e = p9_flush_pending();
    if (e != 0) return p9_map(e, 0);
    char rel[P9_REL_MAX];
    if (p9_node_rel(id, rel, sizeof(rel)) != 0) e = -36;
    else e = p9ops->write_file(p9_share(), rel, data, size, nodes[id].mode);
    if (e != 0)
    {
        p9_drop_data(id);                                                  /* the cache holds data the server does not have */
        (void)save_inode(id);
        (void)p9_sync_node(id, 0);
        return p9_map(e, 0);
    }
    (void)p9_sync_node(id, 1);
    return 0;
}

static int p9_post_truncate(int rc, const char* rest, int cwd, uint64_t size)
{
    if (!p9_on()) return rc;
    if (rc != 0) return rc;
    int id = rest ? fs_resolve(rest, cwd) : -1;
    if (id < 0 || nodes[id].type != FS_FILE) return -4;
    char rel[P9_REL_MAX];
    int e;
    if (p9_node_rel(id, rel, sizeof(rel)) != 0) e = -36;
    else e = p9ops->setattr(p9_share(), rel, NINEP_SET_SIZE, nodes[id].mode, nodes[id].uid, nodes[id].gid, size, 0);
    if (e != 0)
    {
        p9_drop_data(id);
        (void)save_inode(id);
        (void)p9_sync_node(id, 0);
        return p9_map(e, 0);
    }
    (void)p9_sync_node(id, 1);
    return 0;
}

static int p9_post_attr(int id, uint32_t valid)
{
    if (!p9_on()) return 0;
    int e = p9_push_attr(id, valid);
    return e == 0 ? 0 : p9_map(e, 0);
}

/* removal: everything the push needs is captured before the implementation deletes the node */
struct P9Rm { int active; int isdir; int parent; char rel[P9_REL_MAX]; };

static void p9_pre_rm(const char* rest, int cwd, P9Rm* m, int refresh_dir)
{
    m->active = 0;
    if (!p9_on() || rest == 0) return;
    int id = fs_resolve_nofollow(rest, cwd);
    if (id <= 0 || id >= FS_MAX_NODES) return;
    if (p9_node_rel(id, m->rel, sizeof(m->rel)) != 0) return;
    m->isdir = nodes[id].type == FS_DIR;
    m->parent = nodes[id].parent;
    if (m->isdir && refresh_dir) (void)p9_refresh_dir(id, 1);             /* "not empty" must be judged on the server's listing */
    m->active = 1;
}

static int p9_post_rm(int rc, const P9Rm* m, int tree)
{
    if (!m->active || !p9_on()) return rc;
    if (rc != 0)
    {
        if (tree) p9_invalidate_all();                                     /* a failed recursive delete may have removed part of it */
        return rc;
    }
    int e = tree ? p9ops->remove_tree(p9_share(), m->rel) : p9ops->remove(p9_share(), m->rel, m->isdir);
    if (e == 0 || e == -2) return 0;
    (void)p9_refresh_dir(m->parent, 1);
    p9_invalidate_all();
    return p9_map(e, 1);
}

/* rename within one share: from_rel is captured before the implementation moves the node */
struct P9Mv { int active; int src; int from_parent; char from[P9_REL_MAX]; };

static void p9_pre_rename(const char* fa, P9Mv* m)
{
    m->active = 0;
    if (!p9_on() || fa == 0) return;
    int id = fs_resolve_nofollow(fa, 0);
    if (id <= 0 || id >= FS_MAX_NODES) return;
    if (p9_node_rel(id, m->from, sizeof(m->from)) != 0) return;
    m->src = id; m->from_parent = nodes[id].parent;
    m->active = 1;
}

static int p9_post_rename(int rc, const P9Mv* m)
{
    if (!m->active || !p9_on()) return rc;
    if (rc != 0) return rc;
    if (!nodes[m->src].used) return 0;
    char to[P9_REL_MAX];
    if (p9_node_rel(m->src, to, sizeof(to)) != 0) return -6;
    int same = 1;
    for (int i = 0;; ++i) { if (to[i] != m->from[i]) { same = 0; break; } if (!to[i]) break; }
    if (same) return 0;
    int to_parent = nodes[m->src].parent;
    int e = p9ops->rename(p9_share(), m->from, to);
    if (e == 0) return 0;
    (void)p9_refresh_dir(m->from_parent, 1);
    (void)p9_refresh_dir(to_parent, 1);
    p9_invalidate_all();
    return p9_map(e, 2);
}
