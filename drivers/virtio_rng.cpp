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

/* virtio-rng (device id 4): one 32-byte request at a time, polled. */
#include "random.h"
#include "virtio_mmio.h"
#include "timer.h"

#define V_MAGIC 0x000U
#define V_VERSION 0x004U
#define V_DEVICE_ID 0x008U
#define V_DEVICE_FEATURES_SEL 0x014U
#define V_DEVICE_FEATURES 0x010U
#define V_DRIVER_FEATURES_SEL 0x024U
#define V_DRIVER_FEATURES 0x020U
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
#define ST_ACK 1U
#define ST_DRIVER 2U
#define ST_OK 4U
#define ST_FEAT 8U
#define QN 4U

struct RDesc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct RAvail { uint16_t flags; uint16_t idx; uint16_t ring[QN]; uint16_t used_event; };
struct RUsedElem { uint32_t id; uint32_t len; };
struct RUsed { uint16_t flags; volatile uint16_t idx; RUsedElem ring[QN]; uint16_t avail_event; };

static volatile uint32_t* mmio = 0;
static RDesc desc[QN] __attribute__((aligned(16), section(".bss.virtio_queue")));
static RAvail avail __attribute__((aligned(16), section(".bss.virtio_queue")));
static RUsed used __attribute__((aligned(16), section(".bss.virtio_queue")));
static uint8_t rbuf[32] __attribute__((aligned(16), section(".bss.virtio_req")));
static uint16_t avail_idx = 0, last_used = 0;
static int ok = 0;

static void wr(uint32_t o, uint32_t v) { mmio[o / 4U] = v; }
static uint32_t rd(uint32_t o) { return mmio[o / 4U]; }
static void bar() { asm volatile("dmb osh\n dsb sy" ::: "memory"); }

int virtio_rng_init()
{
    uint64_t base = 0, size = 0; int irq = -1;
    if (!virtio_mmio_find_device(4U, &base, &size, &irq)) return 0;
    mmio = (volatile uint32_t*)base;
    if (rd(V_MAGIC) != 0x74726976U || rd(V_VERSION) != 2U || rd(V_DEVICE_ID) != 4U) return 0;
    wr(V_STATUS, 0); bar(); wr(V_STATUS, ST_ACK); wr(V_STATUS, ST_ACK | ST_DRIVER);
    wr(V_DEVICE_FEATURES_SEL, 1);
    if ((rd(V_DEVICE_FEATURES) & 1U) == 0) return 0;                 /* VERSION_1 */
    wr(V_DRIVER_FEATURES_SEL, 0); wr(V_DRIVER_FEATURES, 0);
    wr(V_DRIVER_FEATURES_SEL, 1); wr(V_DRIVER_FEATURES, 1U);
    wr(V_STATUS, ST_ACK | ST_DRIVER | ST_FEAT); bar();
    if ((rd(V_STATUS) & ST_FEAT) == 0) return 0;
    wr(V_QUEUE_SEL, 0);
    if (rd(V_QUEUE_NUM_MAX) < QN || rd(V_QUEUE_READY) != 0) return 0;
    wr(V_QUEUE_NUM, QN);
    uint64_t d = (uint64_t)desc, a = (uint64_t)&avail, u = (uint64_t)&used;
    wr(V_QUEUE_DESC_LOW, (uint32_t)d); wr(V_QUEUE_DESC_HIGH, (uint32_t)(d >> 32));
    wr(V_QUEUE_DRIVER_LOW, (uint32_t)a); wr(V_QUEUE_DRIVER_HIGH, (uint32_t)(a >> 32));
    wr(V_QUEUE_DEVICE_LOW, (uint32_t)u); wr(V_QUEUE_DEVICE_HIGH, (uint32_t)(u >> 32));
    bar(); wr(V_QUEUE_READY, 1);
    wr(V_STATUS, ST_ACK | ST_DRIVER | ST_FEAT | ST_OK); bar();
    ok = (rd(V_STATUS) & ST_OK) != 0;
    return ok;
}

int virtio_rng_read(uint8_t* out, uint32_t n)
{
    if (!ok || n == 0) return 0;
    if (n > sizeof(rbuf)) n = sizeof(rbuf);
    desc[0].addr = (uint64_t)rbuf; desc[0].len = n; desc[0].flags = 2U; desc[0].next = 0;      /* device-writable */
    avail.ring[avail_idx % QN] = 0; bar();
    avail.idx = (uint16_t)(avail_idx + 1U); ++avail_idx; bar();
    wr(V_QUEUE_NOTIFY, 0);
    uint64_t t0 = timer_millis();
    while (used.idx == last_used) { bar(); if (timer_millis() - t0 > 200ULL) return 0; }
    uint32_t got = used.ring[last_used % QN].len; ++last_used;
    if (got > n) got = n;
    for (uint32_t i = 0; i < got; ++i) out[i] = rbuf[i];
    return (int)got;
}
