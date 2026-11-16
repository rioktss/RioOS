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

#ifndef NINEP_H
#define NINEP_H

#include "types.h"

/* 9P2000.L client over virtio-9p (device id 9). The operations are path based (paths relative to the share
   root, '/'-separated, "" = the root), synchronous, and return 0 / a byte count or a negative Linux errno.
   fs.cpp reaches the server only through NinepOps, so host tests can plug in a fake server. */

#define NINEP_MAX_SHARES 4

struct NinepAttr
{
    uint32_t mode;        /* Linux st_mode: S_IFMT bits + permissions */
    uint32_t uid, gid;
    uint64_t size;
    uint64_t mtime;       /* seconds */
    uint64_t mtime_ns;    /* nanosecond part of mtime (lets the cache notice two changes within one second) */
    uint64_t version;     /* qid.version (changes when the server thinks the file changed) */
    uint64_t ino;         /* qid.path */
};

#define NINEP_S_IFMT 0170000U
#define NINEP_S_IFDIR 0040000U
#define NINEP_S_IFREG 0100000U
#define NINEP_S_IFLNK 0120000U

/* setattr valid mask (same bits as the 9P2000.L protocol) */
#define NINEP_SET_MODE 0x01U
#define NINEP_SET_UID 0x02U
#define NINEP_SET_GID 0x04U
#define NINEP_SET_SIZE 0x08U
#define NINEP_SET_MTIME 0x120U        /* MTIME | MTIME_SET: set the modification time to the value given */

/* directory entry callback: mode_type is NINEP_S_IFDIR / IFREG / IFLNK (or 0 for other kinds); return non-zero to stop */
typedef int (*NinepDirCb)(void* ctx, const char* name, uint32_t mode_type);

struct NinepOps
{
    uint64_t (*now_ms)();
    int (*lookup)(int share, const char* rel, NinepAttr* out);
    int (*readdir)(int share, const char* rel, NinepDirCb cb, void* ctx);
    int (*read_file)(int share, const char* rel, uint8_t* buf, uint64_t cap, uint64_t* out_size);   /* -27 when larger than cap */
    int (*write_file)(int share, const char* rel, const uint8_t* data, uint64_t size, uint32_t mode); /* create or truncate, then write */
    int (*create)(int share, const char* rel, uint32_t mode);                                       /* new empty file; -17 if present */
    int (*mkdir)(int share, const char* rel, uint32_t mode);
    int (*remove)(int share, const char* rel, int is_dir);
    int (*remove_tree)(int share, const char* rel);
    int (*rename)(int share, const char* from, const char* to);
    int (*setattr)(int share, const char* rel, uint32_t valid, uint32_t mode, uint32_t uid, uint32_t gid, uint64_t size, uint64_t mtime);
    int (*symlink)(int share, const char* rel, const char* target);
    int (*readlink)(int share, const char* rel, char* out, int cap);
    int (*statfs)(int share, uint64_t* total_bytes, uint64_t* free_bytes);
};

/* Probe every virtio-9p device (version + attach). Returns the number of usable shares. */
int ninep_init();
int ninep_share_count();                       /* usable shares, indexes 0..count-1 */
const char* ninep_share_tag(int share);        /* mount tag given on the QEMU command line, NULL if unusable */
int ninep_share_find(const char* tag);         /* index or -1 */
uint32_t ninep_share_msize(int share);
int ninep_share_alive(int share);              /* 0 after a timeout or protocol failure (device is then ignored) */
const NinepOps* ninep_ops();
uint32_t ninep_request_count();

#endif
