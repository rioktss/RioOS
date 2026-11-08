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

#ifndef STORAGE_H
#define STORAGE_H

#include "types.h"

void storage_init();
int storage_ready();
int storage_read_sector(uint64_t sector, void* buffer);
int storage_write_sector(uint64_t sector, const void* buffer);
int storage_flush();
void storage_status();
int storage_backend();   /* 0 none, 1 virtio-blk, 2 NVMe, 3 AHCI */

/* secondary devices: 1/5 = RAM disks, 3 = NVMe, 4 = AHCI (non-root); capacity 0 = absent */
uint64_t storage_dev_sectors(int dev);
int storage_dev_read(int dev, uint64_t sector, void* buffer);
int storage_dev_write(int dev, uint64_t sector, const void* buffer);
int storage_dev_flush(int dev);

#endif
