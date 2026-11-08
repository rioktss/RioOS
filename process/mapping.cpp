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

#include "mapping.h"
#include "mmu.h"
#include "fd.h"
#include "fs.h"
#include "page_alloc.h"

#define MAP_PAGE 4096ULL
#define MAP_RECORDS 1024
#define MAP_CACHE 1024
#define MAP_FILE_REFS 64

struct FileRef { int used; int refs; char path[128]; int cwd; int node; };
struct PageRec { int used; int pid; uint64_t va; int ref; uint64_t foff; uint64_t frame; int writable; };
struct CacheEnt { int used; int node; uint64_t index; uint64_t frame; };

static FileRef frefs[MAP_FILE_REFS];
static PageRec recs[MAP_RECORDS];
static CacheEnt cache[MAP_CACHE];
static uint8_t io_buffer[FD_FILE_MAX];

static void zero_page(uint64_t frame) { volatile uint64_t* p = (volatile uint64_t*)frame; for (uint64_t i = 0; i < MAP_PAGE / 8; ++i) p[i] = 0; }

static void str_cpy(char* d, const char* s, int cap) { int i = 0; while (s[i] && i < cap - 1) { d[i] = s[i]; ++i; } d[i] = 0; }

static int same_str(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }

static void ref_drop(int r)
{
    if (r < 0) return;
    if (--frefs[r].refs <= 0) frefs[r].used = 0;
}

/* Write the pages of `ref` found in [lo,hi) of `pid` back to the file in one read-modify-write. */
static void flush_ref(int pid, int ref, uint64_t lo, uint64_t hi)
{
    int any = 0;
    for (int i = 0; i < MAP_RECORDS; ++i)
        if (recs[i].used && recs[i].pid == pid && recs[i].ref == ref && recs[i].writable && recs[i].va >= lo && recs[i].va < hi) { any = 1; break; }
    if (!any) return;
    uint64_t size = 0;
    if (fs_read_file_for_update(frefs[ref].path, frefs[ref].cwd, io_buffer, sizeof(io_buffer), &size) != 0) return;
    int changed = 0;
    for (int i = 0; i < MAP_RECORDS; ++i)
    {
        PageRec* r = &recs[i];
        if (!r->used || r->pid != pid || r->ref != ref || !r->writable || r->va < lo || r->va >= hi) continue;
        if (r->foff >= size) continue;                           /* beyond EOF: never extends the file */
        uint64_t n = size - r->foff; if (n > MAP_PAGE) n = MAP_PAGE;
        const uint8_t* src = (const uint8_t*)r->frame;
        for (uint64_t k = 0; k < n; ++k) io_buffer[r->foff + k] = src[k];
        changed = 1;
    }
    if (changed) (void)fs_write_data(frefs[ref].path, frefs[ref].cwd, io_buffer, size);
}

static void flush_range(int pid, uint64_t lo, uint64_t hi)
{
    for (int r = 0; r < MAP_FILE_REFS; ++r) if (frefs[r].used) flush_ref(pid, r, lo, hi);
}

static void drop_range(int pid, uint64_t lo, uint64_t hi)
{
    for (int i = 0; i < MAP_RECORDS; ++i)
        if (recs[i].used && recs[i].pid == pid && recs[i].va >= lo && recs[i].va < hi)
        { ref_drop(recs[i].ref); recs[i].used = 0; }
}

static CacheEnt* cache_find(int node, uint64_t index)
{
    for (int i = 0; i < MAP_CACHE; ++i) if (cache[i].used && cache[i].node == node && cache[i].index == index) return &cache[i];
    return 0;
}

/* Frame for file page `index`; takes a reference for the caller. 0 when out of memory. */
static uint64_t cache_get(int node, const char* path, int cwd, uint64_t index, uint64_t file_size)
{
    CacheEnt* c = cache_find(node, index);
    if (!c)
    {
        for (int i = 0; i < MAP_CACHE; ++i) if (!cache[i].used) { c = &cache[i]; break; }
        if (!c) return 0;
        void* pg = allocate_page();
        if (!pg) return 0;
        zero_page((uint64_t)pg);
        uint64_t off = index * MAP_PAGE;
        if (off < file_size)
        {
            uint64_t got = 0;
            if (fs_read_file(path, cwd, io_buffer, sizeof(io_buffer), &got) == 0 && off < got)
            {
                uint64_t n = got - off; if (n > MAP_PAGE) n = MAP_PAGE;
                uint8_t* d = (uint8_t*)pg; for (uint64_t k = 0; k < n; ++k) d[k] = io_buffer[off + k];
            }
        }
        c->used = 1; c->node = node; c->index = index; c->frame = (uint64_t)pg;   /* cache owns the first reference */
    }
    if (page_ref((void*)c->frame) != 0) return 0;
    return c->frame;
}

static PageRec* rec_alloc() { for (int i = 0; i < MAP_RECORDS; ++i) if (!recs[i].used) return &recs[i]; return 0; }

int64_t mapping_mmap_shared(int pid, uint64_t length, uint32_t prot, int fd, uint64_t offset)
{
    if (length == 0 || length > (4ULL << 20)) return -22;
    uint64_t pages = (length + MAP_PAGE - 1) / MAP_PAGE;
    char path[128]; int cwd = 0; uint32_t oflags = 0; int node = -1; uint64_t fsize = 0; int ref = -1;
    if (fd >= 0)
    {
        if (offset % MAP_PAGE) return -22;
        int rc = fd_file_info(fd, path, sizeof(path), &cwd, &oflags);
        if (rc != 0) return rc;
        if ((prot & 2U) && (oflags & 3U) != FD_O_RDWR) return -13;      /* EACCES: writable shared map needs O_RDWR */
        if ((oflags & 3U) == FD_O_WRONLY) return -13;
        node = fs_resolve(path, cwd);
        if (node < 0 || fs_get_type(node) != FS_FILE) return -19;
        fsize = fs_file_size(node);
        for (int i = 0; i < MAP_FILE_REFS; ++i) if (frefs[i].used && frefs[i].node == node && frefs[i].cwd == cwd && same_str(frefs[i].path, path)) { ref = i; break; }
        if (ref < 0)
        {
            for (int i = 0; i < MAP_FILE_REFS; ++i) if (!frefs[i].used) { ref = i; break; }
            if (ref < 0) return -12;
            frefs[ref].used = 1; frefs[ref].refs = 0; frefs[ref].cwd = cwd; frefs[ref].node = node; str_cpy(frefs[ref].path, path, sizeof(frefs[ref].path));
        }
    }
    uint64_t base = 0;
    if (mmu_user_mmap_shared(pid, length, prot, &base) != 0) return -12;
    for (uint64_t p = 0; p < pages; ++p)
    {
        uint64_t va = base + p * MAP_PAGE;
        uint64_t frame;
        if (fd >= 0) frame = cache_get(node, path, cwd, offset / MAP_PAGE + p, fsize);
        else { void* pg = allocate_page(); frame = (uint64_t)pg; if (pg) zero_page(frame); }
        PageRec* r = fd >= 0 ? rec_alloc() : 0;
        if (!frame || (fd >= 0 && !r) || mmu_user_install_frame(pid, va, frame) != 0)
        {
            if (frame) (void)free_page((void*)frame);
            drop_range(pid, base, base + p * MAP_PAGE);
            (void)mmu_user_munmap(pid, base, length);
            mapping_gc();
            return -12;
        }
        if (fd >= 0)
        {
            r->used = 1; r->pid = pid; r->va = va; r->ref = ref; r->foff = offset + p * MAP_PAGE; r->frame = frame; r->writable = (prot & 2U) ? 1 : 0;
            ++frefs[ref].refs;
        }
    }
    return (int64_t)base;
}

int mapping_msync(int pid, uint64_t addr, uint64_t length)
{
    if (addr & (MAP_PAGE - 1)) return -22;
    flush_range(pid, addr, addr + ((length + MAP_PAGE - 1) & ~(MAP_PAGE - 1)));
    return 0;
}

int mapping_munmap(int pid, uint64_t addr, uint64_t length)
{
    uint64_t hi = addr + ((length + MAP_PAGE - 1) & ~(MAP_PAGE - 1));
    flush_range(pid, addr, hi);
    drop_range(pid, addr, hi);
    return 0;
}

void mapping_clone(int parent, int child)
{
    for (int i = 0; i < MAP_RECORDS; ++i)
    {
        if (!recs[i].used || recs[i].pid != parent) continue;
        PageRec* r = rec_alloc();
        if (!r) return;
        *r = recs[i]; r->pid = child;
        ++frefs[r->ref].refs;
    }
}

void mapping_release_pid(int pid)
{
    flush_range(pid, 0, ~0ULL);
    drop_range(pid, 0, ~0ULL);
}

void mapping_gc(void)
{
    for (int i = 0; i < MAP_CACHE; ++i)
        if (cache[i].used && page_refcount((void*)cache[i].frame) <= 1)
        { (void)free_page((void*)cache[i].frame); cache[i].used = 0; }
}
