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

#include "virtio_gpu.h"
#include "virtio_dev.h"

#define GPU_DEVICE_ID 16U
#define GPU_CMD_GET_DISPLAY_INFO 0x0100U
#define GPU_CMD_RESOURCE_CREATE_2D 0x0101U
#define GPU_CMD_SET_SCANOUT 0x0103U
#define GPU_CMD_RESOURCE_FLUSH 0x0104U
#define GPU_CMD_TRANSFER_TO_HOST_2D 0x0105U
#define GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106U
#define GPU_RESP_OK_NODATA 0x1100U
#define GPU_RESP_OK_DISPLAY_INFO 0x1101U
#define GPU_FORMAT_B8G8R8X8_UNORM 2U          /* bytes B,G,R,X in memory == 0x00RRGGBB as a little-endian word */
#define GPU_MAX_SCANOUTS 16U
#define GPU_RESOURCE_ID 1U
#define GPU_TIMEOUT_MS 500U

struct GpuHdr { uint32_t type; uint32_t flags; uint64_t fence_id; uint32_t ctx_id; uint8_t ring_idx; uint8_t pad[3]; };
struct GpuRect { uint32_t x, y, w, h; };
struct GpuCreate2d { GpuHdr h; uint32_t resource_id, format, width, height; };
struct GpuAttach { GpuHdr h; uint32_t resource_id, nr_entries; };
struct GpuMem { uint64_t addr; uint32_t length; uint32_t pad; };
struct GpuScanout { GpuHdr h; GpuRect r; uint32_t scanout_id, resource_id; };
struct GpuTransfer { GpuHdr h; GpuRect r; uint64_t offset; uint32_t resource_id, pad; };
struct GpuFlush { GpuHdr h; GpuRect r; uint32_t resource_id, pad; };
struct GpuDisplayOne { GpuRect r; uint32_t enabled, flags; };
struct GpuDisplayInfo { GpuHdr h; GpuDisplayOne pmodes[GPU_MAX_SCANOUTS]; };

static_assert(sizeof(GpuHdr) == 24, "virtio-gpu ctrl header is 24 bytes");
static_assert(sizeof(GpuCreate2d) == 40, "create_2d size");
static_assert(sizeof(GpuScanout) == 48, "set_scanout size");
static_assert(sizeof(GpuTransfer) == 56, "transfer_to_host_2d size");
static_assert(sizeof(GpuFlush) == 48, "resource_flush size");
static_assert(sizeof(GpuDisplayInfo) == 24 + 24 * 16, "display info size");

static VirtioDev dev;
static VQ ctrlq, cursorq;
static VQMem ctrlq_mem VQ_MEM_ATTR;
static VQMem cursorq_mem VQ_MEM_ATTR;
static union
{
    GpuCreate2d create; GpuAttach attach; GpuScanout scanout; GpuTransfer transfer; GpuFlush flush; GpuHdr hdr;
} cmd __attribute__((aligned(16), section(".bss.virtio_req")));
static GpuMem mem_entry __attribute__((aligned(16), section(".bss.virtio_req")));
static GpuDisplayInfo dinfo __attribute__((aligned(16), section(".bss.virtio_req")));
static GpuHdr resp __attribute__((aligned(16), section(".bss.virtio_req")));

static int ready = 0;
static uint32_t res_w = 0, res_h = 0;
static uint32_t disp_w = 0, disp_h = 0;
static int disp_enabled = 0;
static uint32_t flushes = 0;
static int busy = 0;
static int pend0 = 0x7FFFFFFF, pend1 = 0;

static void zero(void* p, uint32_t n) { for (uint32_t i = 0; i < n; ++i) ((volatile uint8_t*)p)[i] = 0; }

static void hdr_init(GpuHdr* h, uint32_t type)
{
    zero(h, sizeof(*h));
    h->type = type;
}

/* one command (device-readable) + one response (device-writable); 1 when the device answered with `want` */
static int send(const void* c, uint32_t clen, void* r, uint32_t rlen, uint32_t want)
{
    VSeg segs[2] = { { c, clen, 0 }, { r, rlen, 1 } };
    GpuHdr* rh = (GpuHdr*)r;
    rh->type = 0;
    uint32_t got = 0;
    if (vq_request(&ctrlq, segs, 2, GPU_TIMEOUT_MS, &got) != 0) return 0;
    return rh->type == want;
}

static int send_with_entry(const void* c, uint32_t clen)
{
    VSeg segs[3] = { { c, clen, 0 }, { &mem_entry, sizeof(mem_entry), 0 }, { &resp, sizeof(resp), 1 } };
    resp.type = 0;
    uint32_t got = 0;
    if (vq_request(&ctrlq, segs, 3, GPU_TIMEOUT_MS, &got) != 0) return 0;
    return resp.type == GPU_RESP_OK_NODATA;
}

static int simple_cmd(const void* c, uint32_t clen)
{
    return send(c, clen, &resp, sizeof(resp), GPU_RESP_OK_NODATA);
}

static int do_flush(int y0, int y1)
{
    if (!ready) return 0;
    uint32_t rows = (uint32_t)(y1 - y0);
    hdr_init(&cmd.hdr, GPU_CMD_TRANSFER_TO_HOST_2D);
    cmd.transfer.r.x = 0; cmd.transfer.r.y = (uint32_t)y0; cmd.transfer.r.w = res_w; cmd.transfer.r.h = rows;
    cmd.transfer.offset = (uint64_t)y0 * (uint64_t)res_w * 4ULL;
    cmd.transfer.resource_id = GPU_RESOURCE_ID; cmd.transfer.pad = 0;
    if (!simple_cmd(&cmd, sizeof(GpuTransfer))) { ready = 0; return 0; }
    hdr_init(&cmd.hdr, GPU_CMD_RESOURCE_FLUSH);
    cmd.flush.r.x = 0; cmd.flush.r.y = (uint32_t)y0; cmd.flush.r.w = res_w; cmd.flush.r.h = rows;
    cmd.flush.resource_id = GPU_RESOURCE_ID; cmd.flush.pad = 0;
    if (!simple_cmd(&cmd, sizeof(GpuFlush))) { ready = 0; return 0; }
    ++flushes;
    return 1;
}

int virtio_gpu_init(void* pixels, uint32_t width, uint32_t height)
{
    ready = 0;
    if (!pixels || width < 16 || height < 16 || width > 4096 || height > 4096) return 0;
    if (!virtio_dev_open(&dev, GPU_DEVICE_ID, 0, 0)) return 0;
    if (!virtio_dev_queue(&dev, 0, &ctrlq, &ctrlq_mem, 8) || !virtio_dev_queue(&dev, 1, &cursorq, &cursorq_mem, 4) ||
        !virtio_dev_start(&dev))
    {
        virtio_dev_reset(&dev);
        return 0;
    }
    res_w = width; res_h = height;

    /* what the host display looks like (informational; the resource may be any size) */
    hdr_init(&dinfo.h, GPU_CMD_GET_DISPLAY_INFO);
    {
        GpuHdr q; hdr_init(&q, GPU_CMD_GET_DISPLAY_INFO);
        cmd.hdr = q;
        if (send(&cmd, sizeof(GpuHdr), &dinfo, sizeof(dinfo), GPU_RESP_OK_DISPLAY_INFO))
        {
            disp_w = dinfo.pmodes[0].r.w; disp_h = dinfo.pmodes[0].r.h; disp_enabled = (int)dinfo.pmodes[0].enabled;
        }
    }

    hdr_init(&cmd.hdr, GPU_CMD_RESOURCE_CREATE_2D);
    cmd.create.resource_id = GPU_RESOURCE_ID; cmd.create.format = GPU_FORMAT_B8G8R8X8_UNORM;
    cmd.create.width = width; cmd.create.height = height;
    if (!simple_cmd(&cmd, sizeof(GpuCreate2d))) { virtio_dev_reset(&dev); return 0; }

    hdr_init(&cmd.hdr, GPU_CMD_RESOURCE_ATTACH_BACKING);
    cmd.attach.resource_id = GPU_RESOURCE_ID; cmd.attach.nr_entries = 1;
    mem_entry.addr = (uint64_t)pixels; mem_entry.length = width * height * 4U; mem_entry.pad = 0;
    if (!send_with_entry(&cmd, sizeof(GpuAttach))) { virtio_dev_reset(&dev); return 0; }

    hdr_init(&cmd.hdr, GPU_CMD_SET_SCANOUT);
    cmd.scanout.r.x = 0; cmd.scanout.r.y = 0; cmd.scanout.r.w = width; cmd.scanout.r.h = height;
    cmd.scanout.scanout_id = 0; cmd.scanout.resource_id = GPU_RESOURCE_ID;
    if (!simple_cmd(&cmd, sizeof(GpuScanout))) { virtio_dev_reset(&dev); return 0; }

    ready = 1;
    if (!do_flush(0, (int)height)) { virtio_dev_reset(&dev); ready = 0; return 0; }
    return 1;
}

int virtio_gpu_ready() { return ready; }
uint32_t virtio_gpu_flush_count() { return flushes; }

void virtio_gpu_display(uint32_t* width, uint32_t* height, int* enabled)
{
    if (width) *width = disp_w;
    if (height) *height = disp_h;
    if (enabled) *enabled = disp_enabled;
}

void virtio_gpu_flush_rows(int y0, int y1)
{
    if (!ready) return;
    if (y0 < 0) y0 = 0;
    if (y1 > (int)res_h) y1 = (int)res_h;
    if (y1 <= y0) return;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
    {
        /* a flush is already running (nested from an IRQ or another CPU): the owner picks this range up */
        if (y0 < pend0) pend0 = y0;
        if (y1 > pend1) pend1 = y1;
        return;
    }
    (void)do_flush(y0, y1);
    while (pend1 > pend0 && ready)
    {
        int a = pend0, b = pend1;
        pend0 = 0x7FFFFFFF; pend1 = 0;
        (void)do_flush(a, b);
    }
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}
