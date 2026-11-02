#include "usb.h"
#include "pci.h"
#include "uart.h"

/* ---- minimal polling xHCI driver --------------------------------------------------------
   One command ring, one event ring (single segment), up to 4 devices attached to root-hub
   ports. Each device gets EP0 and (for HID boot keyboards/mice) one interrupt-IN endpoint. */

#define RING_TRBS 16
#define MAX_DEV 4

struct Trb { volatile uint32_t d0, d1, d2, d3; };
struct Ring { Trb* t; uint32_t idx; uint32_t cycle; };

static uint8_t cmd_ring_mem[RING_TRBS * 16] __attribute__((aligned(64)));
static uint8_t evt_ring_mem[RING_TRBS * 16] __attribute__((aligned(64)));
static uint64_t erst[2] __attribute__((aligned(64)));
static uint64_t dcbaa[16] __attribute__((aligned(64)));
static uint8_t scratch_arr[64] __attribute__((aligned(64)));
static uint8_t scratch_pages[4][4096] __attribute__((aligned(4096)));
static uint8_t out_ctx[MAX_DEV][2048] __attribute__((aligned(64)));
static uint8_t in_ctx[2304] __attribute__((aligned(64)));
static uint8_t ep0_ring_mem[MAX_DEV][RING_TRBS * 16] __attribute__((aligned(64)));
static uint8_t int_ring_mem[MAX_DEV][RING_TRBS * 16] __attribute__((aligned(64)));
static uint8_t xfer_buf[256] __attribute__((aligned(64)));
static uint8_t hid_buf[MAX_DEV][8] __attribute__((aligned(64)));

struct UsbDev
{
    int used, slot, port, kind;         /* kind: 1 keyboard, 2 mouse */
    uint32_t speed;
    Ring ep0, intr;
    uint32_t dci;
    uint8_t last[8];
    uint16_t vid, pid;
};

static uint64_t base = 0, op = 0, rt = 0, db = 0;
static uint32_t ctx_sz = 32, max_ports = 0, max_slots = 0;
static int ready = 0;
static Ring cmd_ring, evt_ring;
static UsbDev devs[MAX_DEV];
static int ndev = 0;
static char keyq[64]; static unsigned kq_h = 0, kq_t = 0;
static int mouse_x = 0, mouse_y = 0, mouse_btn = 0;
static uint32_t key_events = 0;

static volatile uint32_t* r32(uint64_t a) { return (volatile uint32_t*)a; }
static void dsb() { asm volatile("dsb sy" ::: "memory"); }
static void zero(void* p, uint32_t n) { for (uint32_t i = 0; i < n; ++i) ((volatile uint8_t*)p)[i] = 0; }
static void put_dec(uint64_t v) { char t[24]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) uart_putc(t[--n]); }

static void ring_init(Ring* r, void* mem)
{
    zero(mem, RING_TRBS * 16);
    r->t = (Trb*)mem; r->idx = 0; r->cycle = 1;
    Trb* l = &r->t[RING_TRBS - 1];            /* link back to start, toggle cycle */
    uint64_t p = (uint64_t)mem;
    l->d0 = (uint32_t)p; l->d1 = (uint32_t)(p >> 32); l->d2 = 0;
    l->d3 = (6U << 10) | 2U;                  /* LINK, TC; cycle bit set when it is reached */
}

static void ring_push(Ring* r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t ctrl)
{
    Trb* t = &r->t[r->idx];
    t->d0 = d0; t->d1 = d1; t->d2 = d2;
    dsb();
    t->d3 = ctrl | r->cycle;
    if (++r->idx == RING_TRBS - 1)
    {
        Trb* l = &r->t[RING_TRBS - 1];
        l->d3 = (6U << 10) | 2U | r->cycle;
        dsb();
        r->idx = 0; r->cycle ^= 1U;
    }
}


struct Event { uint32_t d0, d1, d2, d3; };

static int event_get(Event* e)
{
    Trb* t = &evt_ring.t[evt_ring.idx];
    if ((t->d3 & 1U) != evt_ring.cycle) return 0;
    dsb();
    e->d0 = t->d0; e->d1 = t->d1; e->d2 = t->d2; e->d3 = t->d3;
    if (++evt_ring.idx == RING_TRBS) { evt_ring.idx = 0; evt_ring.cycle ^= 1U; }
    uint64_t erdp = (uint64_t)evt_ring.t + (uint64_t)evt_ring.idx * 16U;
    *r32(rt + 0x38) = (uint32_t)erdp | 8U;
    *r32(rt + 0x3C) = (uint32_t)(erdp >> 32);
    return 1;
}

static void hid_event(UsbDev* d);
static void handle_transfer_event(const Event* e);

/* wait for a command-completion (type 33); transfer events seen meanwhile are dispatched */
static int wait_cmd(uint32_t* slot_out)
{
    for (int spin = 0; spin < 4000000; ++spin)
    {
        Event e;
        if (!event_get(&e)) { asm volatile("yield"); continue; }
        uint32_t type = (e.d3 >> 10) & 0x3FU;
        if (type == 33) { if (slot_out) *slot_out = e.d3 >> 24; return (int)(e.d2 >> 24); }
        if (type == 32) handle_transfer_event(&e);
    }
    return -1;
}

static int command(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t ctrl, uint32_t* slot_out)
{
    ring_push(&cmd_ring, d0, d1, d2, ctrl);
    dsb();
    *r32(db + 0) = 0;
    int cc = wait_cmd(slot_out);
    return cc == 1 ? 0 : (cc < 0 ? -1 : cc);
}

/* wait for a transfer event on slot/dci that belongs to a control transfer */
static int ctrl_xfer(UsbDev* d, uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void* data)
{
    int in = (bm & 0x80) != 0;
    uint32_t setup0 = (uint32_t)bm | ((uint32_t)req << 8) | ((uint32_t)val << 16);
    uint32_t setup1 = (uint32_t)idx | ((uint32_t)len << 16);
    uint32_t trt = len == 0 ? 0 : (in ? 3U : 2U);
    ring_push(&d->ep0, setup0, setup1, 8, (2U << 10) | (1U << 6) | (trt << 16));
    if (len)
    {
        uint64_t p = (uint64_t)xfer_buf;
        if (!in) for (uint16_t i = 0; i < len; ++i) xfer_buf[i] = ((uint8_t*)data)[i];
        ring_push(&d->ep0, (uint32_t)p, (uint32_t)(p >> 32), len, (3U << 10) | (in ? (1U << 16) : 0U));
    }
    ring_push(&d->ep0, 0, 0, 0, (4U << 10) | (1U << 5) | ((len && in) ? 0U : (1U << 16)));
    dsb();
    *r32(db + 4ULL * (uint64_t)d->slot) = 1;
    for (int spin = 0; spin < 4000000; ++spin)
    {
        Event e;
        if (!event_get(&e)) { asm volatile("yield"); continue; }
        uint32_t type = (e.d3 >> 10) & 0x3FU;
        if (type == 32)
        {
            uint32_t slot = e.d3 >> 24, ep = (e.d3 >> 16) & 0x1FU;
            if (slot == (uint32_t)d->slot && ep == 1)
            {
                uint32_t cc = e.d2 >> 24;
                if (cc != 1 && cc != 13) return -(int)cc;
                if (len && in) for (uint16_t i = 0; i < len; ++i) ((uint8_t*)data)[i] = xfer_buf[i];
                return 0;
            }
            handle_transfer_event(&e);
        }
    }
    return -1000;
}

static const char hid_plain[] = "\0\0\0\0abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\\0;'`,./";
static const char hid_shift[] = "\0\0\0\0ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|\0:\"~<>?";

static void kq_put(char c) { unsigned n = (kq_h + 1U) & 63U; if (n != kq_t) { keyq[kq_h] = c; kq_h = n; } }

static void kbd_report(UsbDev* d, const uint8_t* r)
{
    int shift = (r[0] & 0x22) != 0, ctrl = (r[0] & 0x11) != 0;
    for (int i = 2; i < 8; ++i)
    {
        uint8_t k = r[i];
        if (!k) continue;
        int was = 0;
        for (int j = 2; j < 8; ++j) if (d->last[j] == k) was = 1;
        if (was) continue;
        char c = 0;
        if (k >= 4 && k <= 0x38) c = shift ? hid_shift[k] : hid_plain[k];
        else if (k == 0x4C) c = 0x7F;
        else if (k == 0x50) { kq_put(0x1B); kq_put('['); c = 'D'; }
        else if (k == 0x4F) { kq_put(0x1B); kq_put('['); c = 'C'; }
        else if (k == 0x52) { kq_put(0x1B); kq_put('['); c = 'A'; }
        else if (k == 0x51) { kq_put(0x1B); kq_put('['); c = 'B'; }
        if (c && ctrl && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        if (c) { kq_put(c); ++key_events; }
    }
    for (int i = 0; i < 8; ++i) d->last[i] = r[i];
}

static void queue_int(UsbDev* d)
{
    uint64_t p = (uint64_t)hid_buf[d - devs];
    for (int i = 0; i < 8; ++i) hid_buf[d - devs][i] = 0;
    ring_push(&d->intr, (uint32_t)p, (uint32_t)(p >> 32), 8, (1U << 10) | (1U << 5));   /* Normal, IOC */
    dsb();
    *r32(db + 4ULL * (uint64_t)d->slot) = d->dci;
}

static void hid_event(UsbDev* d)
{
    uint8_t* r = hid_buf[d - devs];
    if (d->kind == 1) kbd_report(d, r);
    else
    {
        mouse_btn = r[0] & 7; mouse_x += (signed char)r[1]; mouse_y += (signed char)r[2];
    }
}

static void handle_transfer_event(const Event* e)
{
    uint32_t slot = e->d3 >> 24, ep = (e->d3 >> 16) & 0x1FU;
    for (int i = 0; i < ndev; ++i)
        if (devs[i].used && (uint32_t)devs[i].slot == slot && devs[i].kind && ep == devs[i].dci)
        {
            uint32_t cc = e->d2 >> 24;
            if (cc == 1 || cc == 13) hid_event(&devs[i]);
            queue_int(&devs[i]);
            return;
        }
}

void usb_poll()
{
    if (!ready) return;
    Event e;
    int guard = 0;
    while (guard++ < 16 && event_get(&e))
        if (((e.d3 >> 10) & 0x3FU) == 32) handle_transfer_event(&e);
}

int usb_try_get_key(char* out)
{
    usb_poll();
    if (kq_h == kq_t) return 0;
    *out = keyq[kq_t]; kq_t = (kq_t + 1U) & 63U;
    return 1;
}

int usb_present() { return ready; }
int usb_keyboard_count() { int n = 0; for (int i = 0; i < ndev; ++i) if (devs[i].used && devs[i].kind == 1) ++n; return n; }
int usb_mouse_count() { int n = 0; for (int i = 0; i < ndev; ++i) if (devs[i].used && devs[i].kind == 2) ++n; return n; }

static uint32_t* ctx_at(uint8_t* ctx, int index) { return (uint32_t*)(ctx + (uint32_t)index * ctx_sz); }

static int enumerate_port(uint32_t port)
{
    uint64_t pr = op + 0x400ULL + 0x10ULL * port;
    uint32_t sc = *r32(pr);
    if (!(sc & 1U)) return 0;
    *r32(pr) = (sc & 0x0E01C3E0U & 0) | (1U << 4);          /* PR (port reset); preserve nothing risky */
    int ok = 0;
    for (int i = 0; i < 2000000; ++i) { sc = *r32(pr); if (sc & (1U << 21)) { ok = 1; break; } asm volatile("yield"); }
    if (!ok) return 0;
    *r32(pr) = (1U << 21) | (1U << 1 & 0);                     /* clear PRC (write-1-to-clear) */
    sc = *r32(pr);
    if (!(sc & 2U)) return 0;                                  /* not enabled */
    uint32_t speed = (sc >> 10) & 0xFU;
    if (ndev >= MAX_DEV) return 0;
    UsbDev* d = &devs[ndev];
    zero(d, sizeof(*d));
    d->port = (int)port + 1; d->speed = speed;
    uint32_t slot = 0;
    if (command(0, 0, 0, (9U << 10), &slot) != 0 || slot == 0 || slot > 15) return 0;
    d->slot = (int)slot; d->used = 1;
    zero(out_ctx[ndev], 2048); zero(in_ctx, sizeof(in_ctx));
    ring_init(&d->ep0, ep0_ring_mem[ndev]);
    ring_init(&d->intr, int_ring_mem[ndev]);
    dcbaa[slot] = (uint64_t)out_ctx[ndev];
    uint32_t* ic = (uint32_t*)in_ctx;
    ic[1] = 3U;                                                /* add: slot + EP0 */
    uint32_t* sctx = ctx_at(in_ctx, 1);
    sctx[0] = (1U << 27) | (speed << 20);
    sctx[1] = (port + 1U) << 16;
    uint32_t* e0 = ctx_at(in_ctx, 2);
    uint32_t mps = speed == 4 ? 512U : (speed == 3 ? 64U : 8U);
    e0[1] = (3U << 1) | (4U << 3) | (mps << 16);
    uint64_t dq = (uint64_t)d->ep0.t | 1U;
    e0[2] = (uint32_t)dq; e0[3] = (uint32_t)(dq >> 32);
    e0[4] = 8U;
    dsb();
    if (command((uint32_t)(uint64_t)in_ctx, (uint32_t)((uint64_t)in_ctx >> 32), 0, (11U << 10) | (slot << 24), 0) != 0) return 0;
    uint8_t desc[18];
    if (ctrl_xfer(d, 0x80, 6, 0x0100, 0, 18, desc) != 0) return 0;
    d->vid = (uint16_t)(desc[8] | (desc[9] << 8)); d->pid = (uint16_t)(desc[10] | (desc[11] << 8));
    uint8_t cfg[128];
    if (ctrl_xfer(d, 0x80, 6, 0x0200, 0, 9, cfg) != 0) return 0;
    uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8)); if (total > 128) total = 128;
    if (ctrl_xfer(d, 0x80, 6, 0x0200, 0, total, cfg) != 0) return 0;
    int kind = 0, iface = 0, epaddr = 0, epmps = 8, interval = 10;
    int in_hid = 0;
    for (uint32_t o = 0; o + 2 <= total; o += cfg[o] ? cfg[o] : 1U)
    {
        if (cfg[o + 1] == 4 && o + 9 <= total) { in_hid = (cfg[o + 5] == 3 && cfg[o + 6] == 1 && (cfg[o + 7] == 1 || cfg[o + 7] == 2)); if (in_hid) { kind = cfg[o + 7]; iface = cfg[o + 2]; } }
        else if (cfg[o + 1] == 5 && in_hid && (cfg[o + 2] & 0x80) && (cfg[o + 3] & 3) == 3 && o + 7 <= total) { epaddr = cfg[o + 2] & 0xF; epmps = cfg[o + 4] | (cfg[o + 5] << 8); interval = cfg[o + 6]; break; }
    }
    if (ctrl_xfer(d, 0x00, 9, cfg[5], 0, 0, 0) != 0) return 0;     /* SET_CONFIGURATION */
    if (!kind || !epaddr) { ++ndev; return 1; }                    /* enumerated but not a HID boot device */
    (void)ctrl_xfer(d, 0x21, 0x0B, 0, (uint16_t)iface, 0, 0);      /* SET_PROTOCOL boot */
    (void)ctrl_xfer(d, 0x21, 0x0A, 0, (uint16_t)iface, 0, 0);      /* SET_IDLE */
    d->kind = kind; d->dci = (uint32_t)epaddr * 2U + 1U;
    zero(in_ctx, sizeof(in_ctx));
    ic = (uint32_t*)in_ctx;
    ic[1] = 1U | (1U << d->dci);
    sctx = ctx_at(in_ctx, 1);
    sctx[0] = (d->dci << 27) | (speed << 20);
    sctx[1] = (port + 1U) << 16;
    uint32_t* ep = ctx_at(in_ctx, (int)d->dci + 1);
    uint32_t iv = (uint32_t)interval; if (speed < 3) { uint32_t f = iv * 8U, l = 0; while ((1U << (l + 1)) <= f && l < 15) ++l; iv = l + 1; if (iv > 10) iv = 10; if (iv < 3) iv = 3; }
    ep[0] = (iv - 1U) << 16;
    ep[1] = (3U << 1) | (7U << 3) | ((uint32_t)epmps << 16);
    dq = (uint64_t)d->intr.t | 1U;
    ep[2] = (uint32_t)dq; ep[3] = (uint32_t)(dq >> 32);
    ep[4] = 8U | (8U << 16);
    dsb();
    if (command((uint32_t)(uint64_t)in_ctx, (uint32_t)((uint64_t)in_ctx >> 32), 0, (12U << 10) | (slot << 24), 0) != 0) { d->kind = 0; ++ndev; return 1; }
    ++ndev;
    queue_int(d);
    return 1;
}

void usb_init()
{
    ready = 0; ndev = 0; kq_h = kq_t = 0;
    const PciDevice* p = pci_find_class(0x0C, 0x03, 0x30, 0);
    if (!p || p->bar_size[0] == 0) return;
    base = p->bar[0];
    uint32_t caplen = *r32(base) & 0xFFU;
    uint32_t hcs1 = *r32(base + 4), hcs2 = *r32(base + 8), hcc = *r32(base + 0x10);
    max_slots = hcs1 & 0xFFU; max_ports = hcs1 >> 24;
    ctx_sz = (hcc & 4U) ? 64U : 32U;
    op = base + caplen; db = base + (*r32(base + 0x14) & ~3U); rt = base + (*r32(base + 0x18) & ~0x1FU);
    uint32_t cmd = *r32(op);
    *r32(op) = cmd & ~1U;                                   /* stop */
    for (int i = 0; i < 1000000 && !(*r32(op + 4) & 1U); ++i) asm volatile("yield");
    *r32(op) = 2U;                                          /* HCRST */
    int okr = 0;
    for (int i = 0; i < 4000000; ++i) { if (!(*r32(op) & 2U) && !(*r32(op + 4) & (1U << 11))) { okr = 1; break; } asm volatile("yield"); }
    if (!okr) { uart_puts("USB: xHCI reset timeout\r\n"); return; }
    if (max_slots > 8) max_slots = 8;
    *r32(op + 0x38) = max_slots;
    zero(dcbaa, sizeof(dcbaa));
    uint32_t nscr = ((hcs2 >> 21) & 0x1FU) << 5 | ((hcs2 >> 27) & 0x1FU);
    if (nscr > 4) { uart_puts("USB: too many scratchpad buffers\r\n"); return; }
    if (nscr) { for (uint32_t i = 0; i < nscr; ++i) ((uint64_t*)scratch_arr)[i] = (uint64_t)scratch_pages[i]; dcbaa[0] = (uint64_t)scratch_arr; }
    uint64_t a = (uint64_t)dcbaa; *r32(op + 0x30) = (uint32_t)a; *r32(op + 0x34) = (uint32_t)(a >> 32);
    ring_init(&cmd_ring, cmd_ring_mem);
    a = (uint64_t)cmd_ring.t | 1U; *r32(op + 0x18) = (uint32_t)a; *r32(op + 0x1C) = (uint32_t)(a >> 32);
    zero(evt_ring_mem, sizeof(evt_ring_mem));
    evt_ring.t = (Trb*)evt_ring_mem; evt_ring.idx = 0; evt_ring.cycle = 1;
    erst[0] = (uint64_t)evt_ring_mem; erst[1] = RING_TRBS;
    *r32(rt + 0x28) = 1;
    a = (uint64_t)evt_ring_mem; *r32(rt + 0x38) = (uint32_t)a; *r32(rt + 0x3C) = (uint32_t)(a >> 32);
    a = (uint64_t)erst; *r32(rt + 0x30) = (uint32_t)a; *r32(rt + 0x34) = (uint32_t)(a >> 32);
    dsb();
    *r32(op) = 1U;                                          /* RS */
    for (int i = 0; i < 1000000 && (*r32(op + 4) & 1U); ++i) asm volatile("yield");
    ready = 1;
    for (uint32_t pt = 0; pt < max_ports; ++pt) (void)enumerate_port(pt);
    uart_puts("USB: xHCI ready, ports "); put_dec(max_ports); uart_puts(", keyboards "); put_dec((uint64_t)usb_keyboard_count());
    uart_puts(", mice "); put_dec((uint64_t)usb_mouse_count()); uart_puts("\r\n");
}

int usb_selftest()
{
    if (!ready) return -1;
    if (ndev == 0) return -2;
    uint8_t desc[18];
    UsbDev* d = &devs[0];
    if (ctrl_xfer(d, 0x80, 6, 0x0100, 0, 18, desc) != 0) return -3;
    if (desc[0] != 18 || desc[1] != 1) return -4;
    return 0;
}

void usb_status()
{
    if (!ready) { uart_puts("USB: no xHCI controller (start QEMU with -device qemu-xhci)\r\n"); return; }
    uart_puts("xHCI: ports "); put_dec(max_ports); uart_puts(", slots "); put_dec(max_slots); uart_puts(", devices "); put_dec((uint64_t)ndev); uart_puts("\r\n");
    for (int i = 0; i < ndev; ++i)
    {
        uart_puts("  slot "); put_dec((uint64_t)devs[i].slot); uart_puts(" port "); put_dec((uint64_t)devs[i].port);
        uart_puts(" speed "); put_dec(devs[i].speed); uart_puts(" id ");
        const char* hx = "0123456789abcdef";
        for (int s = 12; s >= 0; s -= 4) uart_putc(hx[(devs[i].vid >> s) & 15]);
        uart_putc(':');
        for (int s = 12; s >= 0; s -= 4) uart_putc(hx[(devs[i].pid >> s) & 15]);
        uart_puts(devs[i].kind == 1 ? "  HID keyboard" : devs[i].kind == 2 ? "  HID mouse" : "  (other)");
        uart_puts("\r\n");
    }
    uart_puts("  key events: "); put_dec(key_events);
    uart_puts("  mouse: x="); put_dec((uint64_t)(mouse_x < 0 ? -mouse_x : mouse_x)); if (mouse_x < 0) uart_puts("(neg)");
    uart_puts(" y="); put_dec((uint64_t)(mouse_y < 0 ? -mouse_y : mouse_y)); if (mouse_y < 0) uart_puts("(neg)");
    uart_puts(" buttons="); put_dec((uint64_t)mouse_btn); uart_puts("\r\n");
}
