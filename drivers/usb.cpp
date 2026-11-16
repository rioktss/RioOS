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


#include "usb.h"
#include "pci.h"
#include "uart.h"
#include "timer.h"
#include "virtio_input.h"
#include "console.h"

/* ---- polling xHCI driver -----------------------------------------------------------------------------
   One command ring, one event ring (single segment), up to MAX_DEV devices anywhere in the tree below the
   root-hub ports (hubs are supported, several tiers deep). Each device gets EP0 and one interrupt-IN endpoint
   per HID interface (composite keyboards/mice work). Supported:
     - keyboards (boot protocol; Caps/Num Lock with LEDs, keypad, key repeat),
     - pointing devices of any kind through their HID report descriptor: relative mice (wheel, extra buttons) and
       absolute tablets/touchscreens; reported as RioInputEvents (usb_poll_event),
     - hubs: ports are powered, reset and polled with GET_PORT_STATUS; devices behind them are enumerated with a
       route string and, for low/full-speed devices behind a high-speed hub, the transaction-translator fields,
     - hot-plug and hot-unplug on root ports (port-status-change events) and on hub ports,
     - recovery after STALL / transfer errors (Reset Endpoint + Set TR Dequeue Pointer), EP0 max-packet fix-up.
   Everything is polled from usb_poll(), which the keyboard idle path calls; no interrupts are used. */

#define RING_TRBS 16
#define EVT_TRBS 64
#define MAX_DEV 8
#define MAX_HID 2
#define HID_BUF 32
#define MAX_HUB_PORTS 15
#define PTR_Q 32

struct Trb { volatile uint32_t d0, d1, d2, d3; };
struct Ring { Trb* t; uint32_t idx; uint32_t cycle; uint32_t n; };

static uint8_t cmd_ring_mem[RING_TRBS * 16] __attribute__((aligned(64)));
static uint8_t evt_ring_mem[EVT_TRBS * 16] __attribute__((aligned(64)));
static uint64_t erst[2] __attribute__((aligned(64)));
static uint64_t dcbaa[16] __attribute__((aligned(64)));
static uint8_t scratch_arr[64] __attribute__((aligned(64)));
static uint8_t scratch_pages[4][4096] __attribute__((aligned(4096)));
static uint8_t out_ctx[MAX_DEV][2048] __attribute__((aligned(64)));
static uint8_t in_ctx[2304] __attribute__((aligned(64)));
static uint8_t ep0_ring_mem[MAX_DEV][RING_TRBS * 16] __attribute__((aligned(64)));
static uint8_t int_ring_mem[MAX_DEV][MAX_HID][RING_TRBS * 16] __attribute__((aligned(64)));
static uint8_t xfer_buf[256] __attribute__((aligned(64)));
static uint8_t hid_buf[MAX_DEV][MAX_HID][HID_BUF] __attribute__((aligned(64)));

struct HidField { uint16_t off; uint8_t bits; uint8_t rel; int32_t lmin, lmax; };
struct HidPtr
{
    int ok, abs;
    uint8_t id;                         /* report ID (0 = none) */
    uint8_t has_x, has_y, has_w;
    uint8_t btn_n;
    uint16_t btn_off;
    HidField x, y, w;
};

enum { HID_NONE = 0, HID_KBD = 1, HID_MOUSE = 2, HID_TABLET = 3 };

struct Hid
{
    int active, kind;
    uint8_t iface;
    uint32_t dci;
    uint16_t epmps, len;
    uint8_t interval;
    Ring ring;
    uint8_t last[8];
    uint8_t need_recover, led_pending;
    uint32_t errors;
    HidPtr ptr;
};

struct UsbDev
{
    int used, slot, rport, parent, pport;
    uint32_t route, speed;
    uint8_t dclass, dproto, is_hub, hub_ports, hub_mtt;
    uint8_t child[MAX_HUB_PORTS + 1];   /* device index + 1 per hub port */
    Ring ep0;
    Hid hid[MAX_HID];
    uint16_t vid, pid;
};

static uint64_t base = 0, op = 0, rt = 0, db = 0;
static uint32_t ctx_sz = 32, max_ports = 0, max_slots = 0;
static int ready = 0, in_poll = 0;
static Ring cmd_ring, evt_ring;
static UsbDev devs[MAX_DEV];
static char keyq[64]; static unsigned kq_h = 0, kq_t = 0;
static int mouse_x = 0, mouse_y = 0, mouse_btn = 0, tab_x = 0, tab_y = 0;
static uint32_t ptr_events = 0, wheel_events = 0;
static uint32_t key_events = 0;
static uint32_t pending_ports = 0;
static uint32_t n_added = 0, n_removed = 0, n_xfer_err = 0, n_recovered = 0;
static int caps_on = 0, num_on = 1;
static int rep_key = 0, rep_dev = -1, rep_hid = 0; static uint8_t rep_mod = 0; static uint64_t rep_next = 0;
static uint64_t next_hub_poll = 0;
static int force_hub_scan = 0;
static uint32_t n_ignored = 0;
static struct RioInputEvent pq[PTR_Q]; static unsigned pq_h = 0, pq_t = 0;
static int ptr_x = 16384, ptr_y = 16384;       /* virtual cursor of relative pointers, 0..32767 like the other input drivers */

static volatile uint32_t* r32(uint64_t a) { return (volatile uint32_t*)a; }
static void dsb() { asm volatile("dsb sy" ::: "memory"); }
static void zero(void* p, uint32_t n) { for (uint32_t i = 0; i < n; ++i) ((volatile uint8_t*)p)[i] = 0; }
static void put_dec(uint64_t v) { char t[24]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) uart_putc(t[--n]); }
static void delay_ms(uint32_t ms) { uint64_t end = timer_millis() + ms; while (timer_millis() < end) asm volatile("yield"); }

static void ring_init(Ring* r, void* mem, uint32_t n)
{
    zero(mem, n * 16);
    r->t = (Trb*)mem; r->idx = 0; r->cycle = 1; r->n = n;
    Trb* l = &r->t[n - 1];                    /* link back to start, toggle cycle */
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
    if (++r->idx == r->n - 1)
    {
        Trb* l = &r->t[r->n - 1];
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
    if (++evt_ring.idx == evt_ring.n) { evt_ring.idx = 0; evt_ring.cycle ^= 1U; }
    uint64_t erdp = (uint64_t)evt_ring.t + (uint64_t)evt_ring.idx * 16U;
    *r32(rt + 0x38) = (uint32_t)erdp | 8U;
    *r32(rt + 0x3C) = (uint32_t)(erdp >> 32);
    return 1;
}

static void handle_transfer_event(const Event* e);

/* an event nobody was waiting for */
static void handle_other_event(const Event* e)
{
    uint32_t type = (e->d3 >> 10) & 0x3FU;
    if (type == 32) handle_transfer_event(e);
    else if (type == 34)                                   /* port status change: handled from usb_poll, never nested */
    {
        uint32_t pid = e->d0 >> 24;
        if (pid >= 1 && pid <= 31) pending_ports |= 1U << (pid - 1U);
    }
}

/* wait for a command-completion (type 33); everything else is dispatched meanwhile */
static int wait_cmd(uint32_t* slot_out)
{
    for (int spin = 0; spin < 4000000; ++spin)
    {
        Event e;
        if (!event_get(&e)) { asm volatile("yield"); continue; }
        uint32_t type = (e.d3 >> 10) & 0x3FU;
        if (type == 33) { if (slot_out) *slot_out = e.d3 >> 24; return (int)(e.d2 >> 24); }
        handle_other_event(&e);
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

/* Endpoint halted (stall / transaction error): Reset Endpoint, then move the dequeue pointer to the enqueue position so
   the rest of the failed transfer is skipped. */
static void ep_recover(UsbDev* d, uint32_t dci, Ring* r)
{
    ++n_xfer_err;
    uint32_t slot = (uint32_t)d->slot;
    (void)command(0, 0, 0, (14U << 10) | (dci << 16) | (slot << 24), 0);
    uint64_t p = (uint64_t)&r->t[r->idx] | (uint64_t)r->cycle;
    (void)command((uint32_t)p, (uint32_t)(p >> 32), 0, (16U << 10) | (dci << 16) | (slot << 24), 0);
    ++n_recovered;
}

/* control transfer on EP0; returns 0, -(completion code) or -1000 on timeout */
static int ctrl_xfer(UsbDev* d, uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void* data)
{
    if (len > sizeof(xfer_buf)) return -1001;
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
                if (cc != 1 && cc != 13) { ep_recover(d, 1, &d->ep0); return -(int)cc; }
                if (len && in) for (uint16_t i = 0; i < len; ++i) ((uint8_t*)data)[i] = xfer_buf[i];
                return 0;
            }
        }
        handle_other_event(&e);
    }
    return -1000;
}

/* ---- keyboard ---- */
static const char hid_plain[] = "\0\0\0\0abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\\0;'`,./";
static const char hid_shift[] = "\0\0\0\0ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|\0:\"~<>?";

static void kq_put(char c) { unsigned n = (kq_h + 1U) & 63U; if (n != kq_t) { keyq[kq_h] = c; kq_h = n; } }

/* HID usage -> the byte the rest of the system expects (the same codes virtio-input produces: 0x80..0x85 = Up, Down,
   Left, Right, PgUp, PgDn). 0 = no character. */
static char kbd_char(uint8_t k, uint8_t mod)
{
    int shift = (mod & 0x22) != 0, ctrl = (mod & 0x11) != 0;
    char c = 0;
    if (k >= 4 && k <= 0x38)
    {
        int alpha = k <= 0x1D;
        int up = alpha ? (shift != (caps_on != 0)) : shift;
        c = up ? hid_shift[k] : hid_plain[k];
    }
    else if (k == 0x4C) c = 0x7F;                                  /* Delete */
    else if (k == 0x52) c = (char)0x80;
    else if (k == 0x51) c = (char)0x81;
    else if (k == 0x50) c = (char)0x82;
    else if (k == 0x4F) c = (char)0x83;
    else if (k == 0x4B) c = (char)0x84;
    else if (k == 0x4E) c = (char)0x85;
    else if (k >= 0x54 && k <= 0x63)                               /* keypad */
    {
        if (k <= 0x58) c = "/*-+\n"[k - 0x54];
        else if (num_on) c = k == 0x62 ? '0' : k == 0x63 ? '.' : (char)('1' + (k - 0x59));
        else
        {
            switch (k)
            {
                case 0x5A: c = (char)0x81; break;  case 0x5B: c = (char)0x85; break;
                case 0x5C: c = (char)0x82; break;  case 0x5E: c = (char)0x83; break;
                case 0x60: c = (char)0x80; break;  case 0x61: c = (char)0x84; break;
                case 0x63: c = 0x7F; break;        default: break;
            }
        }
    }
    if (c && ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c = (char)((c | 0x20) - 'a' + 1);
    return c;
}

static void kbd_report(int di, int hi, const uint8_t* r)
{
    Hid* h = &devs[di].hid[hi];
    if (r[2] == 1) return;                                          /* phantom state (rollover error): ignore */
    uint8_t mod = r[0];
    int still = 0;
    for (int i = 2; i < 8; ++i)
    {
        uint8_t k = r[i];
        if (k < 4) continue;
        if (k == rep_key && rep_dev == di && rep_hid == hi) still = 1;
        int was = 0;
        for (int j = 2; j < 8; ++j) if (h->last[j] == k) was = 1;
        if (was) continue;
        if (k == 0x39) { caps_on = !caps_on; for (int a = 0; a < MAX_DEV; ++a) for (int b = 0; b < MAX_HID; ++b) devs[a].hid[b].led_pending = devs[a].used && devs[a].hid[b].kind == HID_KBD; continue; }
        if (k == 0x53) { num_on = !num_on; for (int a = 0; a < MAX_DEV; ++a) for (int b = 0; b < MAX_HID; ++b) devs[a].hid[b].led_pending = devs[a].used && devs[a].hid[b].kind == HID_KBD; continue; }
        char c = kbd_char(k, mod);
        if (c)
        {
            kq_put(c); ++key_events;
            rep_key = k; rep_dev = di; rep_hid = hi; rep_mod = mod; rep_next = timer_millis() + 500ULL;
            still = 1;
        }
    }
    if (rep_key && rep_dev == di && rep_hid == hi && !still) rep_key = 0;
    for (int i = 0; i < 8; ++i) h->last[i] = r[i];
}

static void kbd_leds(UsbDev* d, Hid* h)
{
    uint8_t leds = (uint8_t)((num_on ? 1 : 0) | (caps_on ? 2 : 0));
    (void)ctrl_xfer(d, 0x21, 0x09, 0x0200, h->iface, 1, &leds);    /* SET_REPORT(Output): may legitimately STALL */
}

/* ---- HID report descriptor (only what pointing devices need) ---- */
static uint32_t get_bits(const uint8_t* b, uint32_t len, uint32_t off, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n && i < 32; ++i)
    {
        uint32_t bit = off + i;
        if ((bit >> 3) < len && ((b[bit >> 3] >> (bit & 7U)) & 1U)) v |= 1U << i;
    }
    return v;
}

static int32_t sbits(uint32_t v, uint32_t n)
{
    if (n > 0 && n < 32 && ((v >> (n - 1U)) & 1U)) v |= ~0U << n;
    return (int32_t)v;
}

static void hid_parse(const uint8_t* d, uint32_t n, HidPtr* out)
{
    zero(out, sizeof(*out));
    uint32_t page = 0, rsize = 0, rcount = 0, rid = 0, pos = 0;
    int32_t lmin = 0, lmax = 0;
    uint32_t usages[16]; uint32_t nus = 0, umin = 0, umax = 0; int has_range = 0;
    for (uint32_t i = 0; i < n;)
    {
        uint8_t b = d[i++];
        if (b == 0xFE) { if (i >= n) break; i += 2U + d[i]; continue; }          /* long item: skipped */
        uint32_t sz = b & 3U; if (sz == 3) sz = 4;
        uint32_t type = (b >> 2) & 3U, tag = b >> 4;
        if (i + sz > n) break;
        uint32_t u = 0;
        for (uint32_t k = 0; k < sz; ++k) u |= (uint32_t)d[i + k] << (8U * k);
        int32_t s = sz == 1 ? (int32_t)(signed char)u : sz == 2 ? (int32_t)(short)u : (int32_t)u;
        i += sz;
        if (type == 1)
        {
            if (tag == 0) page = u;
            else if (tag == 1) lmin = s;
            else if (tag == 2) lmax = lmin >= 0 ? (int32_t)u : s;
            else if (tag == 7) rsize = u;
            else if (tag == 9) rcount = u;
            else if (tag == 8)
            {
                if (u != rid)
                {
                    if (out->has_x && out->has_y) break;                        /* the pointer report is complete */
                    zero(out, sizeof(*out)); pos = 0;
                }
                rid = u;
            }
        }
        else if (type == 2)
        {
            if (tag == 0 && nus < 16) usages[nus++] = u & 0xFFFFU;
            else if (tag == 1) { umin = u & 0xFFFFU; has_range = 1; }
            else if (tag == 2) umax = u & 0xFFFFU;
        }
        else if (type == 0)
        {
            if (tag == 8)                                                         /* Input */
            {
                if (!(u & 1U) && (u & 2U))                                        /* data + variable */
                    for (uint32_t k = 0; k < rcount && k < 64; ++k)
                    {
                        uint32_t usage = 0xFFFFFFFFU;
                        if (nus) usage = usages[k < nus ? k : nus - 1U];
                        else if (has_range && umin + k <= umax) usage = umin + k;
                        uint32_t off = pos + k * rsize + (rid ? 8U : 0U);
                        if (page == 9 && usage >= 1 && usage <= 8 && rsize == 1)
                        {
                            if (!out->btn_n) out->btn_off = (uint16_t)(off - (usage - 1U));
                            if (usage > out->btn_n) out->btn_n = (uint8_t)usage;
                        }
                        else if (page == 1 && rsize >= 1 && rsize <= 32 && (usage == 0x30 || usage == 0x31 || usage == 0x38))
                        {
                            HidField* f = usage == 0x30 ? &out->x : usage == 0x31 ? &out->y : &out->w;
                            f->off = (uint16_t)off; f->bits = (uint8_t)rsize; f->rel = (u & 4U) ? 1 : 0; f->lmin = lmin; f->lmax = lmax;
                            if (usage == 0x30) out->has_x = 1; else if (usage == 0x31) out->has_y = 1; else out->has_w = 1;
                        }
                    }
                pos += rsize * rcount;
            }
            nus = 0; has_range = 0; umin = umax = 0;                              /* locals die at every main item */
        }
    }
    out->id = (uint8_t)rid;
    out->ok = out->has_x && out->has_y;
    out->abs = out->ok && !out->x.rel;
}

static int scale_abs(const HidField* f, int32_t v)
{
    int64_t span = (int64_t)f->lmax - (int64_t)f->lmin;
    if (span <= 0) return 0;
    int64_t r = ((int64_t)v - (int64_t)f->lmin) * 32767LL / span;
    return r < 0 ? 0 : r > 32767 ? 32767 : (int)r;
}

static void ptr_push(uint32_t kind, int x, int y, uint32_t pressed, int scroll)
{
    unsigned n = (pq_h + 1U) % PTR_Q;
    if (n == pq_t) return;                                                        /* queue full: drop */
    struct RioInputEvent* e = &pq[pq_h];
    e->kind = kind; e->key = 0; e->x = x; e->y = y; e->pressed = pressed; e->scroll = scroll;
    pq_h = n;
}

static void ptr_report(Hid* h, const uint8_t* r, uint32_t len)
{
    const HidPtr* p = &h->ptr;
    if (p->id && (len < 1 || r[0] != p->id)) return;
    uint32_t btn = 0;
    for (uint32_t i = 0; i < p->btn_n; ++i) if (get_bits(r, len, p->btn_off + i, 1)) btn |= 1U << i;
    int moved = 0;
    int32_t wheel = 0;
    if (p->has_w) { wheel = sbits(get_bits(r, len, p->w.off, p->w.bits), p->w.bits); }
    if (p->abs)
    {
        int32_t rx = p->x.lmin < 0 ? sbits(get_bits(r, len, p->x.off, p->x.bits), p->x.bits) : (int32_t)get_bits(r, len, p->x.off, p->x.bits);
        int32_t ry = p->y.lmin < 0 ? sbits(get_bits(r, len, p->y.off, p->y.bits), p->y.bits) : (int32_t)get_bits(r, len, p->y.off, p->y.bits);
        int nx = scale_abs(&p->x, rx), ny = scale_abs(&p->y, ry);
        if (nx != ptr_x || ny != ptr_y) moved = 1;
        ptr_x = nx; ptr_y = ny;
        tab_x = nx; tab_y = ny;
    }
    else
    {
        int32_t dx = sbits(get_bits(r, len, p->x.off, p->x.bits), p->x.bits);
        int32_t dy = sbits(get_bits(r, len, p->y.off, p->y.bits), p->y.bits);
        if (dx || dy) moved = 1;
        mouse_x += dx; mouse_y += dy;
        int64_t nx = (int64_t)ptr_x + (int64_t)dx * 32767LL / 1023LL, ny = (int64_t)ptr_y + (int64_t)dy * 32767LL / 767LL;
        ptr_x = nx < 0 ? 0 : nx > 32767 ? 32767 : (int)nx;
        ptr_y = ny < 0 ? 0 : ny > 32767 ? 32767 : (int)ny;
    }
    int changed_btn = ((uint32_t)mouse_btn & 0xFFU) != (btn & 0xFFU);
    mouse_btn = (int)(btn & 7U);
    if (moved || changed_btn) { ptr_push(RIO_INPUT_POINTER, ptr_x, ptr_y, btn, 0); ++ptr_events; }
    if (wheel) { ptr_push(RIO_INPUT_SCROLL, ptr_x, ptr_y, btn, wheel > 0 ? 1 : -1); ++wheel_events; }
    (void)h;
}

/* ---- interrupt endpoints ---- */
static void queue_int(int di, int hi)
{
    Hid* h = &devs[di].hid[hi];
    uint64_t p = (uint64_t)hid_buf[di][hi];
    for (int i = 0; i < HID_BUF; ++i) hid_buf[di][hi][i] = 0;
    ring_push(&h->ring, (uint32_t)p, (uint32_t)(p >> 32), h->len, (1U << 10) | (1U << 5));   /* Normal, IOC */
    dsb();
    *r32(db + 4ULL * (uint64_t)devs[di].slot) = h->dci;
}

static void handle_transfer_event(const Event* e)
{
    uint32_t slot = e->d3 >> 24, ep = (e->d3 >> 16) & 0x1FU;
    for (int i = 0; i < MAX_DEV; ++i)
    {
        if (!devs[i].used || (uint32_t)devs[i].slot != slot) continue;
        for (int k = 0; k < MAX_HID; ++k)
        {
            Hid* h = &devs[i].hid[k];
            if (!h->active || ep != h->dci) continue;
            uint32_t cc = e->d2 >> 24;
            if (cc == 1 || cc == 13)
            {
                uint32_t left = e->d2 & 0xFFFFFFU;
                uint32_t got = left <= h->len ? h->len - left : 0;
                const uint8_t* r = hid_buf[i][k];
                if (h->kind == HID_KBD) { if (got >= 8) kbd_report(i, k, r); }
                else if (got) ptr_report(h, r, got);
                queue_int(i, k);
            }
            else h->need_recover = 1;                                            /* recovered from usb_poll, not from here */
            return;
        }
        return;
    }
}

/* ---- devices ---- */
static uint32_t* ctx_at(uint8_t* ctx, int index) { return (uint32_t*)(ctx + (uint32_t)index * ctx_sz); }

static int alloc_dev()
{
    for (int i = 0; i < MAX_DEV; ++i) if (!devs[i].used) return i;
    return -1;
}

static void remove_dev(int di)
{
    UsbDev* d = &devs[di];
    if (!d->used) return;
    if (d->is_hub) for (int p = 1; p <= MAX_HUB_PORTS; ++p) if (d->child[p]) remove_dev(d->child[p] - 1);
    if (rep_dev == di) rep_key = 0;
    (void)command(0, 0, 0, (10U << 10) | ((uint32_t)d->slot << 24), 0);             /* Disable Slot */
    if (d->slot > 0 && d->slot < 16) dcbaa[d->slot] = 0;
    if (d->parent >= 0 && devs[d->parent].used) devs[d->parent].child[d->pport] = 0;
    zero(d, sizeof(*d));
    ++n_removed;
}

/* speed IDs (xHCI): 1 full, 2 low, 3 high, 4 super */
static uint32_t ep0_guess(uint32_t speed) { return speed == 4 ? 512U : speed == 2 ? 8U : 64U; }

static void slot_ctx_fill(UsbDev* d, uint32_t* sctx, uint32_t entries)
{
    sctx[0] = (entries << 27) | (d->speed << 20) | (d->route & 0xFFFFFU) | (d->is_hub ? (1U << 26) : 0U) | (d->hub_mtt ? (1U << 25) : 0U);
    sctx[1] = ((uint32_t)d->rport << 16) | (d->is_hub ? ((uint32_t)d->hub_ports << 24) : 0U);
    sctx[2] = 0;
    if (d->parent >= 0 && devs[d->parent].speed == 3 && d->speed < 3)            /* low/full speed behind a high-speed hub: needs its TT */
        sctx[2] = (uint32_t)devs[d->parent].slot | ((uint32_t)d->pport << 8);
}

/* Enumerate one device. parent = hub device index or -1 (root port), pport = hub port (1-based). Returns device index or -1. */
static int enumerate_device(int parent, int pport, uint32_t route, int rport, uint32_t speed)
{
    int di = alloc_dev();
    if (di < 0) { ++n_ignored; return -1; }                 /* no free device entry */
    UsbDev* d = &devs[di];
    zero(d, sizeof(*d));
    d->parent = parent; d->pport = pport; d->route = route; d->rport = rport; d->speed = speed;
    uint32_t slot = 0;
    if (command(0, 0, 0, (9U << 10), &slot) != 0 || slot == 0 || slot > 15) return -1;
    d->slot = (int)slot; d->used = 1;
    zero(out_ctx[di], 2048); zero(in_ctx, sizeof(in_ctx));
    ring_init(&d->ep0, ep0_ring_mem[di], RING_TRBS);
    dcbaa[slot] = (uint64_t)out_ctx[di];
    uint32_t* ic = (uint32_t*)in_ctx;
    ic[1] = 3U;                                                /* add: slot + EP0 */
    slot_ctx_fill(d, ctx_at(in_ctx, 1), 1);
    uint32_t* e0 = ctx_at(in_ctx, 2);
    uint32_t mps = ep0_guess(speed);
    e0[1] = (3U << 1) | (4U << 3) | (mps << 16);
    uint64_t dq = (uint64_t)d->ep0.t | 1U;
    e0[2] = (uint32_t)dq; e0[3] = (uint32_t)(dq >> 32);
    e0[4] = 8U;
    dsb();
    if (command((uint32_t)(uint64_t)in_ctx, (uint32_t)((uint64_t)in_ctx >> 32), 0, (11U << 10) | (slot << 24), 0) != 0) goto fail;
    {
        uint8_t desc[18];
        if (ctrl_xfer(d, 0x80, 6, 0x0100, 0, 8, desc) != 0) goto fail;              /* first 8 bytes: bMaxPacketSize0 */
        uint32_t real = desc[7];
        if (speed == 4) real = real >= 9 && real <= 16 ? (1U << real) : 512U;
        if (real != mps && real >= 8 && real <= 1024)                              /* the guess was wrong: tell the controller */
        {
            zero(in_ctx, sizeof(in_ctx));
            ic = (uint32_t*)in_ctx; ic[1] = 2U;
            e0 = ctx_at(in_ctx, 2);
            e0[1] = (3U << 1) | (4U << 3) | (real << 16);
            e0[4] = 8U;
            dsb();
            if (command((uint32_t)(uint64_t)in_ctx, (uint32_t)((uint64_t)in_ctx >> 32), 0, (13U << 10) | (slot << 24), 0) != 0) goto fail;
        }
        if (ctrl_xfer(d, 0x80, 6, 0x0100, 0, 18, desc) != 0) goto fail;
        d->vid = (uint16_t)(desc[8] | (desc[9] << 8)); d->pid = (uint16_t)(desc[10] | (desc[11] << 8));
        d->dclass = desc[4]; d->dproto = desc[6];
    }
    {
        uint8_t cfg[256];
        if (ctrl_xfer(d, 0x80, 6, 0x0200, 0, 9, cfg) != 0) goto fail;
        uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8)); if (total > 256) total = 256; if (total < 9) goto fail;
        if (ctrl_xfer(d, 0x80, 6, 0x0200, 0, total, cfg) != 0) goto fail;
        /* interfaces: HID candidates (alt setting 0) with their interrupt-IN endpoint and report descriptor length */
        struct Cand { uint8_t iface, sub, proto, ep, interval; uint16_t mps, rdlen; } cand[MAX_HID];
        int ncand = 0, cur = -1, is_hub_if = 0;
        for (uint32_t o = 0; o + 2 <= total && cfg[o]; o += cfg[o])
        {
            uint8_t type = cfg[o + 1];
            if (type == 4 && o + 9 <= total)
            {
                cur = -1;
                if (cfg[o + 3] != 0) continue;                                   /* alternate setting: ignore */
                if (cfg[o + 5] == 9) is_hub_if = 1;
                if (cfg[o + 5] == 3 && ncand < MAX_HID)
                {
                    cur = ncand++;
                    cand[cur].iface = cfg[o + 2]; cand[cur].sub = cfg[o + 6]; cand[cur].proto = cfg[o + 7];
                    cand[cur].ep = 0; cand[cur].mps = 8; cand[cur].interval = 10; cand[cur].rdlen = 0;
                }
            }
            else if (type == 0x21 && cur >= 0 && o + 9 <= total) cand[cur].rdlen = (uint16_t)(cfg[o + 7] | (cfg[o + 8] << 8));
            else if (type == 5 && cur >= 0 && o + 7 <= total && !cand[cur].ep && (cfg[o + 2] & 0x80) && (cfg[o + 3] & 3) == 3)
            {
                cand[cur].ep = cfg[o + 2] & 0xF; cand[cur].mps = (uint16_t)(cfg[o + 4] | (cfg[o + 5] << 8)); cand[cur].interval = cfg[o + 6];
            }
        }
        if (ctrl_xfer(d, 0x00, 9, cfg[5], 0, 0, 0) != 0) goto fail;               /* SET_CONFIGURATION */

        if (d->dclass == 9 || is_hub_if)                                          /* ---- hub ---- */
        {
            uint8_t hd[16];
            int rc = ctrl_xfer(d, 0xA0, 6, speed == 4 ? 0x2A00 : 0x2900, 0, 9, hd);
            if (rc != 0 || hd[2] == 0 || hd[2] > MAX_HUB_PORTS) goto keep;          /* unusable hub: stays as an unconfigured device */
            d->is_hub = 1; d->hub_ports = hd[2]; d->hub_mtt = d->dproto == 2;
            uint32_t ttt = ((uint32_t)(hd[3] | (hd[4] << 8)) >> 5) & 3U;
            zero(in_ctx, sizeof(in_ctx));
            ic = (uint32_t*)in_ctx; ic[1] = 1U;                                   /* slot context only */
            uint32_t* sc = ctx_at(in_ctx, 1);
            slot_ctx_fill(d, sc, 1);
            sc[2] |= ttt << 16;
            dsb();
            if (command((uint32_t)(uint64_t)in_ctx, (uint32_t)((uint64_t)in_ctx >> 32), 0, (12U << 10) | (slot << 24), 0) != 0) { d->is_hub = 0; goto keep; }
            if (speed == 4) (void)ctrl_xfer(d, 0x20, 12, (uint16_t)0, 0, 0, 0);    /* SET_HUB_DEPTH (tier 0) */
            for (uint32_t p = 1; p <= d->hub_ports; ++p) (void)ctrl_xfer(d, 0x23, 3, 8, (uint16_t)p, 0, 0);   /* PORT_POWER */
            uint32_t good = (uint32_t)hd[5] * 2U; if (good < 20) good = 20; if (good > 500) good = 500;
            delay_ms(good);
            ++n_added;
            return di;                                                            /* ports are scanned by the caller (hub_scan) */
        }

        /* ---- HID interfaces ---- */
        int nh = 0;
        for (int c = 0; c < ncand && nh < MAX_HID; ++c)
        {
            if (!cand[c].ep) continue;
            Hid* h = &d->hid[nh];
            zero(h, sizeof(*h));
            h->iface = cand[c].iface;
            h->epmps = cand[c].mps; h->interval = cand[c].interval;
            if (cand[c].sub == 1 && cand[c].proto == 1)                          /* boot keyboard */
            {
                h->kind = HID_KBD;
                (void)ctrl_xfer(d, 0x21, 0x0B, 0, h->iface, 0, 0);               /* SET_PROTOCOL boot */
                (void)ctrl_xfer(d, 0x21, 0x0A, 0, h->iface, 0, 0);               /* SET_IDLE 0 */
            }
            else
            {
                uint8_t rd[256]; uint16_t rl = cand[c].rdlen > 256 ? 256 : cand[c].rdlen;
                int got = rl ? ctrl_xfer(d, 0x81, 6, 0x2200, h->iface, rl, rd) : -1;
                if (got == 0) hid_parse(rd, rl, &h->ptr);
                if (!h->ptr.ok && cand[c].sub == 1 && cand[c].proto == 2)       /* boot mouse without a usable descriptor */
                {
                    zero(&h->ptr, sizeof(h->ptr));
                    h->ptr.ok = 1; h->ptr.has_x = h->ptr.has_y = 1; h->ptr.btn_n = 3; h->ptr.btn_off = 0;
                    h->ptr.x.off = 8; h->ptr.x.bits = 8; h->ptr.x.rel = 1; h->ptr.y.off = 16; h->ptr.y.bits = 8; h->ptr.y.rel = 1;
                    (void)ctrl_xfer(d, 0x21, 0x0B, 0, h->iface, 0, 0);
                }
                else if (h->ptr.ok && cand[c].sub == 1) (void)ctrl_xfer(d, 0x21, 0x0B, 1, h->iface, 0, 0);   /* SET_PROTOCOL report: only boot-subclass interfaces implement it */
                if (!h->ptr.ok) continue;                                         /* not a pointing device */
                h->kind = h->ptr.abs ? HID_TABLET : HID_MOUSE;
            }
            uint32_t payload = h->epmps & 0x7FFU; if (payload == 0) payload = 8;
            h->len = (uint16_t)(payload > HID_BUF ? HID_BUF : payload);
            if (h->kind == HID_KBD && h->len < 8) h->len = 8;
            h->dci = (uint32_t)cand[c].ep * 2U + 1U;
            ring_init(&h->ring, int_ring_mem[di][nh], RING_TRBS);
            h->active = 1;
            ++nh;
        }
        if (nh)
        {
            zero(in_ctx, sizeof(in_ctx));
            ic = (uint32_t*)in_ctx;
            uint32_t maxdci = 1; ic[1] = 1U;
            for (int k = 0; k < nh; ++k) { ic[1] |= 1U << d->hid[k].dci; if (d->hid[k].dci > maxdci) maxdci = d->hid[k].dci; }
            slot_ctx_fill(d, ctx_at(in_ctx, 1), maxdci);
            for (int k = 0; k < nh; ++k)
            {
                Hid* h = &d->hid[k];
                uint32_t* ep = ctx_at(in_ctx, (int)h->dci + 1);
                uint32_t iv = h->interval ? h->interval : 1U;
                if (speed < 3) { uint32_t f = iv * 8U, l = 0; while ((1U << (l + 1)) <= f && l < 15) ++l; iv = l + 1; if (iv > 10) iv = 10; if (iv < 3) iv = 3; }
                ep[0] = (iv - 1U) << 16;
                uint32_t payload = h->epmps & 0x7FFU; if (payload == 0) payload = 8;
                ep[1] = (3U << 1) | (7U << 3) | (payload << 16);
                dq = (uint64_t)h->ring.t | 1U;
                ep[2] = (uint32_t)dq; ep[3] = (uint32_t)(dq >> 32);
                ep[4] = payload | (payload << 16);
            }
            dsb();
            if (command((uint32_t)(uint64_t)in_ctx, (uint32_t)((uint64_t)in_ctx >> 32), 0, (12U << 10) | (slot << 24), 0) != 0)
                for (int k = 0; k < MAX_HID; ++k) d->hid[k].active = 0;
            else
                for (int k = 0; k < nh; ++k)
                {
                    queue_int(di, k);
                    if (d->hid[k].kind == HID_KBD) kbd_leds(d, &d->hid[k]);
                }
        }
    }
keep:
    ++n_added;
    return di;
fail:
    (void)command(0, 0, 0, (10U << 10) | (slot << 24), 0);                       /* Disable Slot: give the slot back */
    if (slot < 16) dcbaa[slot] = 0;
    zero(d, sizeof(*d));
    return -1;
}

/* ---- root ports ---- */
#define PORTSC_PP (1U << 9)
#define PORTSC_CHANGES 0x00FE0000U          /* CSC PEC WRC OCC PRC PLC CEC (write 1 to clear) */

static uint64_t port_reg(uint32_t port) { return op + 0x400ULL + 0x10ULL * port; }
static uint32_t port_ack(uint32_t port)
{
    uint32_t sc = *r32(port_reg(port));
    *r32(port_reg(port)) = (sc & (PORTSC_PP | 0x0E000000U)) | (sc & PORTSC_CHANGES);     /* keep power/wake, clear the change bits, never write PED/PR */
    return sc;
}

static int root_dev(uint32_t rport)
{
    for (int i = 0; i < MAX_DEV; ++i) if (devs[i].used && devs[i].parent < 0 && devs[i].rport == (int)rport) return i;
    return -1;
}

static int enumerate_root_port(uint32_t port)                    /* port: 0-based */
{
    uint32_t sc = *r32(port_reg(port));
    if (!(sc & 1U)) return 0;
    uint32_t speed = (sc >> 10) & 0xFU;
    if (!(speed == 4 && (sc & 2U)))                              /* a SuperSpeed port that trained up is already enabled */
    {
        *r32(port_reg(port)) = (sc & PORTSC_PP) | (1U << 4);     /* PR: port reset (power bit preserved) */
        int ok = 0;
        for (int i = 0; i < 2000000; ++i) { sc = *r32(port_reg(port)); if (sc & (1U << 21)) { ok = 1; break; } asm volatile("yield"); }
        if (!ok) return 0;
    }
    (void)port_ack(port);
    sc = *r32(port_reg(port));
    if (!(sc & 1U) || !(sc & 2U)) return 0;                      /* gone or not enabled */
    speed = (sc >> 10) & 0xFU;
    int di = enumerate_device(-1, 0, 0, (int)port + 1, speed);
    (void)port_ack(port);
    return di >= 0;
}

/* ---- hubs ---- */
static int hub_status(UsbDev* h, uint32_t p, uint32_t* status, uint32_t* change)
{
    uint8_t b[4];
    int rc = ctrl_xfer(h, 0xA3, 0, 0, (uint16_t)p, 4, b);
    if (rc != 0) return rc;
    *status = (uint32_t)(b[0] | (b[1] << 8)); *change = (uint32_t)(b[2] | (b[3] << 8));
    return 0;
}

static void hub_ack(UsbDev* h, uint32_t p, uint32_t change)
{
    for (uint32_t bit = 0; bit < 5; ++bit)
        if (change & (1U << bit)) (void)ctrl_xfer(h, 0x23, 1, (uint16_t)(16U + bit), (uint16_t)p, 0, 0);   /* CLEAR_FEATURE C_PORT_* */
}

static void hub_port_check(int hi, uint32_t p)
{
    UsbDev* h = &devs[hi];
    uint32_t st = 0, ch = 0;
    if (hub_status(h, p, &st, &ch) != 0) return;
    hub_ack(h, p, ch);
    int child = h->child[p] ? h->child[p] - 1 : -1;
    if (!(st & 1U)) { if (child >= 0) remove_dev(child); return; }
    if (child >= 0 && (ch & 1U)) { remove_dev(child); child = -1; }            /* unplugged and replugged between two polls */
    if (child >= 0) return;
    /* new device: reset the port and wait for it to come up */
    (void)ctrl_xfer(h, 0x23, 3, 4, (uint16_t)p, 0, 0);                         /* SET_FEATURE PORT_RESET */
    uint64_t end = timer_millis() + 500ULL;
    int done = 0;
    while (timer_millis() < end)
    {
        if (hub_status(h, p, &st, &ch) != 0) return;
        if (ch & 0x10U) { done = 1; break; }
        delay_ms(5);
    }
    hub_ack(h, p, ch | 0x10U);
    if (!done || !(st & 1U) || !(st & 2U)) return;
    delay_ms(10);                                                               /* recovery time after reset */
    uint32_t speed = h->speed == 4 ? 4U : (st & 0x200U) ? 2U : (st & 0x400U) ? 3U : 1U;
    int depth = 0; for (uint32_t r = h->route; r; r >>= 4) ++depth;
    if (depth >= 5 || p > 15) return;
    uint32_t route = h->route | (p << (4 * depth));
    int di = enumerate_device(hi, (int)p, route, h->rport, speed);
    if (di >= 0)
    {
        h->child[p] = (uint8_t)(di + 1);
        if (devs[di].is_hub)
            for (uint32_t q = 1; q <= devs[di].hub_ports; ++q) hub_port_check(di, q);
    }
}

static void hub_poll_all()
{
    for (int i = 0; i < MAX_DEV; ++i)
    {
        if (!devs[i].used || !devs[i].is_hub) continue;
        for (uint32_t p = 1; p <= devs[i].hub_ports && devs[i].used; ++p)
        {
            uint32_t st = 0, ch = 0;
            if (hub_status(&devs[i], p, &st, &ch) != 0) continue;
            if ((ch & 0x1FU) || (force_hub_scan && (st & 1U) && !devs[i].child[p])) hub_port_check(i, p);   /* a scan also retries ports whose device was ignored or failed */
        }
    }
}

/* a root port changed (event or explicit rescan) */
static void root_port_changed(uint32_t port)
{
    uint32_t sc = port_ack(port);
    int di = root_dev(port + 1U);
    int connected = (sc & 1U) != 0;
    if (!connected) { if (di >= 0) remove_dev(di); return; }
    if (di >= 0 && (sc & (1U << 17))) { remove_dev(di); di = -1; }              /* replaced */
    if (di < 0 && enumerate_root_port(port))
    {
        di = root_dev(port + 1U);
        if (di >= 0 && devs[di].is_hub) for (uint32_t q = 1; q <= devs[di].hub_ports; ++q) hub_port_check(di, q);
    }
}

/* ---- public ---- */
static void recover_endpoints()
{
    for (int i = 0; i < MAX_DEV; ++i)
        for (int k = 0; k < MAX_HID; ++k)
        {
            if (!devs[i].used) continue;
            Hid* h = &devs[i].hid[k];
            if (h->active && h->need_recover)
            {
                h->need_recover = 0;
                if (++h->errors > 8) { h->active = 0; continue; }                 /* keeps failing: give up on this endpoint */
                ep_recover(&devs[i], h->dci, &h->ring);
                queue_int(i, k);
            }
            if (h->active && h->led_pending) { h->led_pending = 0; kbd_leds(&devs[i], h); }
        }
}

void usb_poll()
{
    if (!ready || in_poll) return;
    in_poll = 1;
    Event e;
    int guard = 0;
    while (guard++ < 64 && event_get(&e)) handle_other_event(&e);
    recover_endpoints();
    if (pending_ports)
    {
        for (uint32_t p = 0; p < 31; ++p)
            if ((pending_ports & (1U << p)) && p < max_ports) { pending_ports &= ~(1U << p); root_port_changed(p); }
        pending_ports = 0;
    }
    int hubs = 0;
    for (int i = 0; i < MAX_DEV; ++i) if (devs[i].used && devs[i].is_hub) hubs = 1;
    if (hubs || rep_key)
    {
        uint64_t now = timer_millis();
        if (hubs && now >= next_hub_poll) { next_hub_poll = now + 250ULL; hub_poll_all(); force_hub_scan = 0; }
        if (rep_key && now >= rep_next)
        {
            char c = kbd_char((uint8_t)rep_key, rep_mod);
            if (c) kq_put(c);
            rep_next = now + 40ULL;
        }
    }
    in_poll = 0;
}

void usb_rescan()
{
    if (!ready) return;
    for (uint32_t p = 0; p < max_ports && p < 31; ++p) pending_ports |= 1U << p;
    next_hub_poll = 0; force_hub_scan = 1;
    usb_poll();
    force_hub_scan = 0;
}

int usb_try_get_key(char* out)
{
    usb_poll();
    while (kq_h != kq_t)
    {
        char c = keyq[kq_t]; kq_t = (kq_t + 1U) & 63U;
        if ((unsigned char)c == 0x84U) { console_scroll_pages(1); continue; }       /* PgUp / PgDn scroll the console, as with virtio-input */
        if ((unsigned char)c == 0x85U) { console_scroll_pages(-1); continue; }
        console_snap_to_bottom();
        *out = c;
        return 1;
    }
    return 0;
}

int usb_poll_event(struct RioInputEvent* event)
{
    if (!event) return 0;
    usb_poll();
    if (pq_h == pq_t) return 0;
    *event = pq[pq_t]; pq_t = (pq_t + 1U) % PTR_Q;
    return 1;
}

int usb_present() { return ready; }
static int count_kind(int kind) { int n = 0; for (int i = 0; i < MAX_DEV; ++i) if (devs[i].used) for (int k = 0; k < MAX_HID; ++k) if (devs[i].hid[k].active && devs[i].hid[k].kind == kind) ++n; return n; }
int usb_keyboard_count() { return count_kind(HID_KBD); }
int usb_mouse_count() { return count_kind(HID_MOUSE); }
int usb_tablet_count() { return count_kind(HID_TABLET); }
int usb_hub_count() { int n = 0; for (int i = 0; i < MAX_DEV; ++i) if (devs[i].used && devs[i].is_hub) ++n; return n; }

void usb_init()
{
    ready = 0; in_poll = 0; kq_h = kq_t = 0; pq_h = pq_t = 0; pending_ports = 0;
    caps_on = 0; num_on = 1; rep_key = 0; rep_dev = -1;
    n_added = n_removed = n_xfer_err = n_recovered = n_ignored = 0; ptr_events = wheel_events = 0; mouse_x = mouse_y = mouse_btn = tab_x = tab_y = 0;
    zero(devs, sizeof(devs));
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
    if (max_slots > MAX_DEV) max_slots = MAX_DEV;
    *r32(op + 0x38) = max_slots;
    zero(dcbaa, sizeof(dcbaa));
    uint32_t nscr = ((hcs2 >> 21) & 0x1FU) << 5 | ((hcs2 >> 27) & 0x1FU);
    if (nscr > 4) { uart_puts("USB: too many scratchpad buffers\r\n"); return; }
    if (nscr) { for (uint32_t i = 0; i < nscr; ++i) ((uint64_t*)scratch_arr)[i] = (uint64_t)scratch_pages[i]; dcbaa[0] = (uint64_t)scratch_arr; }
    uint64_t a = (uint64_t)dcbaa; *r32(op + 0x30) = (uint32_t)a; *r32(op + 0x34) = (uint32_t)(a >> 32);
    ring_init(&cmd_ring, cmd_ring_mem, RING_TRBS);
    a = (uint64_t)cmd_ring.t | 1U; *r32(op + 0x18) = (uint32_t)a; *r32(op + 0x1C) = (uint32_t)(a >> 32);
    zero(evt_ring_mem, sizeof(evt_ring_mem));
    evt_ring.t = (Trb*)evt_ring_mem; evt_ring.idx = 0; evt_ring.cycle = 1; evt_ring.n = EVT_TRBS;
    erst[0] = (uint64_t)evt_ring_mem; erst[1] = EVT_TRBS;
    *r32(rt + 0x28) = 1;
    a = (uint64_t)evt_ring_mem; *r32(rt + 0x38) = (uint32_t)a; *r32(rt + 0x3C) = (uint32_t)(a >> 32);
    a = (uint64_t)erst; *r32(rt + 0x30) = (uint32_t)a; *r32(rt + 0x34) = (uint32_t)(a >> 32);
    dsb();
    *r32(op) = 1U;                                          /* RS */
    for (int i = 0; i < 1000000 && (*r32(op + 4) & 1U); ++i) asm volatile("yield");
    ready = 1;
    for (uint32_t pt = 0; pt < max_ports && pt < 31; ++pt)
    {
        if (!enumerate_root_port(pt)) continue;
        int di = root_dev(pt + 1U);
        if (di >= 0 && devs[di].is_hub) for (uint32_t q = 1; q <= devs[di].hub_ports; ++q) hub_port_check(di, q);
    }
    pending_ports = 0;                                      /* the connect events of what was just enumerated */
    uart_puts("USB: xHCI ready, ports "); put_dec(max_ports); uart_puts(", keyboards "); put_dec((uint64_t)usb_keyboard_count());
    uart_puts(", mice "); put_dec((uint64_t)usb_mouse_count());
    uart_puts(", tablets "); put_dec((uint64_t)usb_tablet_count());
    uart_puts(", hubs "); put_dec((uint64_t)usb_hub_count()); uart_puts("\r\n");
}

int usb_selftest()
{
    if (!ready) return -1;
    int first = -1;
    for (int i = 0; i < MAX_DEV; ++i) if (devs[i].used) { first = i; break; }
    if (first < 0) return -2;
    uint8_t desc[18];
    UsbDev* d = &devs[first];
    if (ctrl_xfer(d, 0x80, 6, 0x0100, 0, 18, desc) != 0) return -3;
    if (desc[0] != 18 || desc[1] != 1) return -4;
    for (int i = 0; i < MAX_DEV; ++i)                                           /* every hub must answer a port-status request */
        if (devs[i].used && devs[i].is_hub)
        {
            uint32_t st, ch;
            if (hub_status(&devs[i], 1, &st, &ch) != 0) return -5;
        }
    return 0;
}

static void put_path(int di)
{
    int chain[6]; int n = 0;
    for (int i = di; i >= 0 && n < 6; i = devs[i].parent) chain[n++] = i;
    put_dec((uint64_t)devs[chain[n - 1]].rport);
    for (int k = n - 2; k >= 0; --k) { uart_putc('.'); put_dec((uint64_t)devs[chain[k]].pport); }
}

void usb_status()
{
    if (!ready) { uart_puts("USB: no xHCI controller (start QEMU with -device qemu-xhci)\r\n"); return; }
    int nd = 0; for (int i = 0; i < MAX_DEV; ++i) if (devs[i].used) ++nd;
    uart_puts("xHCI: ports "); put_dec(max_ports); uart_puts(", slots "); put_dec(max_slots); uart_puts(", devices "); put_dec((uint64_t)nd); uart_puts("\r\n");
    for (int i = 0; i < MAX_DEV; ++i)
    {
        if (!devs[i].used) continue;
        uart_puts("  slot "); put_dec((uint64_t)devs[i].slot); uart_puts(" port "); put_path(i);
        uart_puts(" speed "); put_dec(devs[i].speed); uart_puts(" id ");
        const char* hx = "0123456789abcdef";
        for (int s = 12; s >= 0; s -= 4) uart_putc(hx[(devs[i].vid >> s) & 15]);
        uart_putc(':');
        for (int s = 12; s >= 0; s -= 4) uart_putc(hx[(devs[i].pid >> s) & 15]);
        int any = 0;
        if (devs[i].is_hub) { uart_puts("  hub ("); put_dec(devs[i].hub_ports); uart_puts(" ports)"); any = 1; }
        for (int k = 0; k < MAX_HID; ++k)
            if (devs[i].hid[k].active)
            {
                int kd = devs[i].hid[k].kind;
                uart_puts(any ? ", " : "  ");
                uart_puts(kd == HID_KBD ? "HID keyboard" : kd == HID_MOUSE ? "HID mouse" : "HID tablet");
                any = 1;
            }
        if (!any) uart_puts("  (other)");
        uart_puts("\r\n");
    }
    uart_puts("  key events: "); put_dec(key_events);
    uart_puts("  mouse: x="); put_dec((uint64_t)(mouse_x < 0 ? -mouse_x : mouse_x)); if (mouse_x < 0) uart_puts("(neg)");
    uart_puts(" y="); put_dec((uint64_t)(mouse_y < 0 ? -mouse_y : mouse_y)); if (mouse_y < 0) uart_puts("(neg)");
    uart_puts(" buttons="); put_dec((uint64_t)mouse_btn); uart_puts("\r\n");
    if (usb_tablet_count()) { uart_puts("  tablet: x="); put_dec((uint64_t)tab_x); uart_puts(" y="); put_dec((uint64_t)tab_y); uart_puts("\r\n"); }
    uart_puts("  pointer events: "); put_dec(ptr_events); uart_puts(", wheel events: "); put_dec(wheel_events); uart_puts("\r\n");
    uart_puts("  hot-plug: added "); put_dec(n_added); uart_puts(", removed "); put_dec(n_removed);
    uart_puts("; transfer errors "); put_dec(n_xfer_err); uart_puts(" (recovered "); put_dec(n_recovered); uart_puts(")");
    if (n_ignored) { uart_puts("; ignored (no free device slot) "); put_dec(n_ignored); }
    uart_puts("\r\n");
}
