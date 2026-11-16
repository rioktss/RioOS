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

#include "virtio_dev.h"
#include "virtio_mmio.h"
#include "timer.h"

#define V_MAGIC 0x000U
#define V_VERSION 0x004U
#define V_DEVICE_ID 0x008U
#define V_DEVICE_FEATURES 0x010U
#define V_DEVICE_FEATURES_SEL 0x014U
#define V_DRIVER_FEATURES 0x020U
#define V_DRIVER_FEATURES_SEL 0x024U
#define V_QUEUE_SEL 0x030U
#define V_QUEUE_NUM_MAX 0x034U
#define V_QUEUE_NUM 0x038U
#define V_QUEUE_READY 0x044U
#define V_QUEUE_NOTIFY 0x050U
#define V_STATUS 0x070U
#define V_QUEUE_DESC_LOW 0x080U
#define V_QUEUE_DESC_HIGH 0x084U
#define V_QUEUE_DRIVER_LOW 0x090U
#define V_QUEUE_DRIVER_HIGH 0x094U
#define V_QUEUE_DEVICE_LOW 0x0A0U
#define V_QUEUE_DEVICE_HIGH 0x0A4U
#define V_CONFIG_GENERATION 0x0FCU
#define V_CONFIG 0x100U
#define ST_ACK 1U
#define ST_DRIVER 2U
#define ST_DRIVER_OK 4U
#define ST_FEATURES_OK 8U
#define ST_FAILED 128U
#define VQ_DESC_F_NEXT 1U
#define VQ_DESC_F_WRITE 2U

static inline void bar() { asm volatile("dmb osh\n dsb sy" ::: "memory"); }
static inline void wr(const VirtioDev* d, uint32_t o, uint32_t v) { d->mmio[o / 4U] = v; }
static inline uint32_t rd(const VirtioDev* d, uint32_t o) { return d->mmio[o / 4U]; }

void virtio_dev_reset(VirtioDev* d)
{
    if (!d || !d->mmio) return;
    wr(d, V_STATUS, 0);
    bar();
    d->ok = 0;
}

int virtio_dev_open(VirtioDev* d, uint32_t id, uint32_t instance, uint32_t want_lo)
{
    if (!d) return 0;
    d->mmio = 0; d->id = id; d->features_lo = 0; d->irq = -1; d->ok = 0;
    uint64_t base = 0, size = 0; int irq = -1;
    if (!virtio_mmio_find_device_instance(id, instance, &base, &size, &irq)) return 0;
    d->mmio = (volatile uint32_t*)base;
    d->irq = irq;
    if (rd(d, V_MAGIC) != 0x74726976U || rd(d, V_VERSION) != 2U || rd(d, V_DEVICE_ID) != id) { d->mmio = 0; return 0; }
    wr(d, V_STATUS, 0); bar();
    wr(d, V_STATUS, ST_ACK); wr(d, V_STATUS, ST_ACK | ST_DRIVER);
    wr(d, V_DEVICE_FEATURES_SEL, 1);
    if ((rd(d, V_DEVICE_FEATURES) & 1U) == 0U) { wr(d, V_STATUS, ST_FAILED); d->mmio = 0; return 0; }   /* needs VERSION_1 */
    wr(d, V_DEVICE_FEATURES_SEL, 0);
    uint32_t offered = rd(d, V_DEVICE_FEATURES);
    d->features_lo = offered & want_lo;
    wr(d, V_DRIVER_FEATURES_SEL, 0); wr(d, V_DRIVER_FEATURES, d->features_lo);
    wr(d, V_DRIVER_FEATURES_SEL, 1); wr(d, V_DRIVER_FEATURES, 1U);
    wr(d, V_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK); bar();
    if ((rd(d, V_STATUS) & ST_FEATURES_OK) == 0U) { wr(d, V_STATUS, ST_FAILED); d->mmio = 0; return 0; }
    return 1;
}

int virtio_dev_queue(VirtioDev* d, uint32_t qindex, VQ* q, VQMem* mem, uint16_t num)
{
    if (!d || !d->mmio || !q || !mem || num == 0 || num > VQ_MAX || (num & (num - 1U)) != 0) return 0;
    wr(d, V_QUEUE_SEL, qindex);
    bar();
    if (rd(d, V_QUEUE_READY) != 0U || rd(d, V_QUEUE_NUM_MAX) < num) return 0;
    uint8_t* z = (uint8_t*)mem;
    for (uint32_t i = 0; i < sizeof(VQMem); ++i) z[i] = 0;
    q->dev = d; q->mem = mem; q->index = qindex; q->num = num;
    q->avail_idx = 0; q->last_used = 0; q->broken = 0;
    wr(d, V_QUEUE_NUM, num);
    uint64_t dd = (uint64_t)mem->desc, a = (uint64_t)&mem->avail, u = (uint64_t)&mem->used;
    wr(d, V_QUEUE_DESC_LOW, (uint32_t)dd); wr(d, V_QUEUE_DESC_HIGH, (uint32_t)(dd >> 32));
    wr(d, V_QUEUE_DRIVER_LOW, (uint32_t)a); wr(d, V_QUEUE_DRIVER_HIGH, (uint32_t)(a >> 32));
    wr(d, V_QUEUE_DEVICE_LOW, (uint32_t)u); wr(d, V_QUEUE_DEVICE_HIGH, (uint32_t)(u >> 32));
    bar();
    wr(d, V_QUEUE_READY, 1U);
    return 1;
}

int virtio_dev_start(VirtioDev* d)
{
    if (!d || !d->mmio) return 0;
    wr(d, V_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK); bar();
    d->ok = (rd(d, V_STATUS) & ST_DRIVER_OK) != 0U;
    return d->ok;
}

uint8_t virtio_dev_cfg8(const VirtioDev* d, uint32_t off)
{
    return *(volatile const uint8_t*)((volatile const uint8_t*)d->mmio + V_CONFIG + off);
}

uint32_t virtio_dev_cfg32(const VirtioDev* d, uint32_t off)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4U; ++i) v |= (uint32_t)virtio_dev_cfg8(d, off + i) << (8U * i);
    return v;
}

int vq_post(VQ* q, uint16_t first, const VSeg* segs, int n)
{
    if (!q || !q->mem || q->broken || n <= 0 || (uint32_t)first + (uint32_t)n > q->num) return -1;
    for (int i = 0; i < n; ++i)
    {
        VDesc* e = &q->mem->desc[first + i];
        e->addr = (uint64_t)segs[i].addr;
        e->len = segs[i].len;
        e->flags = (uint16_t)((segs[i].device_writes ? VQ_DESC_F_WRITE : 0U) | (i + 1 < n ? VQ_DESC_F_NEXT : 0U));
        e->next = (uint16_t)(first + i + 1);
    }
    bar();
    q->mem->avail.ring[q->avail_idx % q->num] = first;
    bar();
    q->avail_idx = (uint16_t)(q->avail_idx + 1U);
    q->mem->avail.idx = q->avail_idx;
    bar();
    return 0;
}

void vq_kick(VQ* q)
{
    if (!q || !q->dev) return;
    bar();
    wr(q->dev, V_QUEUE_NOTIFY, q->index);
}

int vq_poll(VQ* q, uint16_t* head, uint32_t* len)
{
    if (!q || !q->mem) return 0;
    bar();
    if (q->mem->used.idx == q->last_used) return 0;
    const VUsedElem* e = &q->mem->used.ring[q->last_used % q->num];
    if (head) *head = (uint16_t)e->id;
    if (len) *len = e->len;
    q->last_used = (uint16_t)(q->last_used + 1U);
    return 1;
}

int vq_wait(VQ* q, uint16_t* head, uint32_t* len, uint32_t timeout_ms)
{
    uint64_t t0 = timer_millis();
    for (;;)
    {
        if (vq_poll(q, head, len)) return 1;
        if (timer_millis() - t0 > (uint64_t)timeout_ms) return 0;
        asm volatile("yield");
    }
}

int vq_request(VQ* q, const VSeg* segs, int n, uint32_t timeout_ms, uint32_t* written)
{
    if (!q || q->broken) return -1;
    if (vq_post(q, 0, segs, n) != 0) return -1;
    vq_kick(q);
    uint16_t head = 0; uint32_t len = 0;
    if (!vq_wait(q, &head, &len, timeout_ms)) { q->broken = 1; return -1; }
    if (written) *written = len;
    return 0;
}
