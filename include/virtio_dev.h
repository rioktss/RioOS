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

#ifndef VIRTIO_DEV_H
#define VIRTIO_DEV_H

#include "types.h"

/* Small polled virtio-mmio (v2 / VERSION_1) helper shared by the gpu, sound and 9p drivers.
   One request at a time per queue is the common case (vq_request); streaming drivers can keep several
   chains in flight with vq_post / vq_kick / vq_poll. All ring memory is static kernel RAM (identity mapped). */

#define VQ_MAX 32U

struct VDesc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct VAvail { uint16_t flags; uint16_t idx; uint16_t ring[VQ_MAX]; uint16_t used_event; };
struct VUsedElem { uint32_t id; uint32_t len; };
struct VUsed { uint16_t flags; volatile uint16_t idx; VUsedElem ring[VQ_MAX]; uint16_t avail_event; };
struct VQMem { VDesc desc[VQ_MAX]; VAvail avail; VUsed used; };

#define VQ_MEM_ATTR __attribute__((aligned(4096), section(".bss.virtio_queue")))

struct VirtioDev
{
    volatile uint32_t* mmio;
    uint32_t id;
    uint32_t features_lo;       /* negotiated feature bits 0..31 */
    int irq;
    int ok;
};

struct VQ
{
    VirtioDev* dev;
    VQMem* mem;
    uint32_t index;
    uint16_t num;
    uint16_t avail_idx;
    uint16_t last_used;
    int broken;                 /* set after a timeout: the device may still own buffers, never reuse them */
};

struct VSeg { const void* addr; uint32_t len; int device_writes; };

/* Find the instance-th device with this VirtIO id, reset it and negotiate VERSION_1 plus (offered & want_lo).
   Leaves the device in FEATURES_OK; add queues, then call virtio_dev_start(). Returns 1 on success. */
int virtio_dev_open(VirtioDev* d, uint32_t id, uint32_t instance, uint32_t want_lo);
int virtio_dev_queue(VirtioDev* d, uint32_t qindex, VQ* q, VQMem* mem, uint16_t num);
int virtio_dev_start(VirtioDev* d);
void virtio_dev_reset(VirtioDev* d);
uint8_t virtio_dev_cfg8(const VirtioDev* d, uint32_t off);
uint32_t virtio_dev_cfg32(const VirtioDev* d, uint32_t off);

/* Chain n segments into descriptors first..first+n-1 and publish the head. 0 on success, -1 on bad args. */
int vq_post(VQ* q, uint16_t first, const VSeg* segs, int n);
void vq_kick(VQ* q);
/* Consume one completion if present: returns 1 and fills head/len. */
int vq_poll(VQ* q, uint16_t* head, uint32_t* len);
/* Poll until a completion arrives or timeout_ms passes: 1 = got one, 0 = timeout. */
int vq_wait(VQ* q, uint16_t* head, uint32_t* len, uint32_t timeout_ms);
/* post at descriptor 0, kick, wait: 0 ok (len = bytes the device wrote), -1 failure/timeout. */
int vq_request(VQ* q, const VSeg* segs, int n, uint32_t timeout_ms, uint32_t* written);

#endif
