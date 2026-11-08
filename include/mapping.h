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

#ifndef MAPPING_H
#define MAPPING_H
#include "types.h"

/* MAP_SHARED support: anonymous shared pages (inherited across fork) and file-backed shared
   pages backed by a global page cache, so every process mapping the same file page sees the
   same memory. File data is written back on msync/munmap/exit/exec. */
#define MAP_SHARED_FLAG  1U
#define MAP_PRIVATE_FLAG 2U
#define MAP_ANON_FLAG    0x20U
#define MAP_FIXED_FLAG   0x10U

/* fd < 0 && anon: anonymous. Returns the address, or a negative errno. */
int64_t mapping_mmap_shared(int pid, uint64_t length, uint32_t prot, int fd, uint64_t offset);
int mapping_msync(int pid, uint64_t addr, uint64_t length);
int mapping_munmap(int pid, uint64_t addr, uint64_t length);   /* flush + unmap */
void mapping_clone(int parent, int child);                      /* fork: child shares the same pages */
void mapping_release_pid(int pid);                              /* exit/exec: flush and drop records */
void mapping_gc(void);                                          /* drops cache pages nobody maps */
#endif
