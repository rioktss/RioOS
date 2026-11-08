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

#include "pci.h"
#include "uart.h"

#define PCI_ECAM_BASE   0x4010000000ULL
#define PCI_BAR_BASE    0x10000000ULL
#define PCI_BAR_LIMIT   0x14000000ULL
#define PCI_MAX_DEVICES 16

static PciDevice devices[PCI_MAX_DEVICES];
static int device_total = 0;
static int present = 0;
static uint64_t next_bar = PCI_BAR_BASE;

static volatile uint32_t* cfg_ptr(int bus, int dev, int fn, int off)
{
    return (volatile uint32_t*)(PCI_ECAM_BASE + ((uint64_t)bus << 20) + ((uint64_t)dev << 15) +
                                ((uint64_t)fn << 12) + (uint64_t)(off & 0xFFC));
}

static uint32_t cfg_read(int bus, int dev, int fn, int off) { return *cfg_ptr(bus, dev, fn, off); }
static void cfg_write(int bus, int dev, int fn, int off, uint32_t v)
{
    *cfg_ptr(bus, dev, fn, off) = v;
    asm volatile("dsb sy" ::: "memory");
}

static void put_hex(uint64_t v, int digits)
{
    for (int i = digits - 1; i >= 0; --i)
    {
        int d = (int)((v >> (i * 4)) & 0xF);
        uart_putc((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
}

static void put_dec(uint64_t v)
{
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) uart_putc(t[--n]);
}

static void assign_bars(PciDevice* d)
{
    for (int i = 0; i < 6; ++i) { d->bar[i] = 0; d->bar_size[i] = 0; }
    for (int i = 0; i < 6; ++i)
    {
        int off = 0x10 + i * 4;
        uint32_t orig = cfg_read(d->bus, d->dev, d->fn, off);
        cfg_write(d->bus, d->dev, d->fn, off, 0xFFFFFFFFU);
        uint32_t probe = cfg_read(d->bus, d->dev, d->fn, off);
        if (probe == 0 || probe == 0xFFFFFFFFU) { cfg_write(d->bus, d->dev, d->fn, off, orig); continue; }
        if (probe & 1U) { cfg_write(d->bus, d->dev, d->fn, off, orig); continue; }   /* I/O BAR: not used on virt */
        int is64 = ((probe >> 1) & 3U) == 2U;
        uint64_t mask = (uint64_t)(probe & ~0xFU);
        if (is64)
        {
            uint32_t orig_hi = cfg_read(d->bus, d->dev, d->fn, off + 4);
            cfg_write(d->bus, d->dev, d->fn, off + 4, 0xFFFFFFFFU);
            uint32_t hi = cfg_read(d->bus, d->dev, d->fn, off + 4);
            mask |= (uint64_t)hi << 32;
            cfg_write(d->bus, d->dev, d->fn, off + 4, orig_hi);
        }
        else mask |= 0xFFFFFFFF00000000ULL;
        uint64_t size = ~mask + 1ULL;
        if (size == 0 || size > 0x400000ULL) { cfg_write(d->bus, d->dev, d->fn, off, orig); if (is64) ++i; continue; }
        uint64_t base = (next_bar + 0x1FFFFFULL) & ~0x1FFFFFULL;       /* 2 MB aligned: matches the pre-mapped blocks */
        if (base + size > PCI_BAR_LIMIT) { cfg_write(d->bus, d->dev, d->fn, off, orig); if (is64) ++i; continue; }
        next_bar = base + size;
        cfg_write(d->bus, d->dev, d->fn, off, (uint32_t)base | (probe & 0xFU));
        if (is64) cfg_write(d->bus, d->dev, d->fn, off + 4, 0);
        d->bar[i] = base;
        d->bar_size[i] = size;
        if (is64) ++i;
    }
    uint32_t cmd = cfg_read(d->bus, d->dev, d->fn, 4);
    cfg_write(d->bus, d->dev, d->fn, 4, (cmd & 0xFFFF) | 0x6U);      /* memory space + bus master */
}

void pci_init()
{
    device_total = 0; present = 0; next_bar = PCI_BAR_BASE;
    if ((cfg_read(0, 0, 0, 0) & 0xFFFFU) == 0xFFFFU) return;          /* no host bridge */
    present = 1;
    for (int dev = 0; dev < 32; ++dev)
    {
        for (int fn = 0; fn < 8; ++fn)
        {
            uint32_t id = cfg_read(0, dev, fn, 0);
            if ((id & 0xFFFFU) == 0xFFFFU) { if (fn == 0) break; continue; }
            if (device_total >= PCI_MAX_DEVICES) return;
            PciDevice* d = &devices[device_total];
            d->bus = 0; d->dev = (uint8_t)dev; d->fn = (uint8_t)fn;
            d->vendor = (uint16_t)(id & 0xFFFFU); d->device = (uint16_t)(id >> 16);
            uint32_t cls = cfg_read(0, dev, fn, 8);
            d->cls = (uint8_t)(cls >> 24); d->subcls = (uint8_t)(cls >> 16); d->prog_if = (uint8_t)(cls >> 8);
            uint32_t hdr = (cfg_read(0, dev, fn, 0xC) >> 16) & 0x7FU;
            int is_host_bridge = d->cls == 6 && d->subcls == 0;
            if (hdr == 0 && !is_host_bridge) assign_bars(d);
            else for (int i = 0; i < 6; ++i) { d->bar[i] = 0; d->bar_size[i] = 0; }
            ++device_total;
            if (fn == 0 && !((cfg_read(0, dev, 0, 0xC) >> 23) & 1U)) break;   /* single-function device */
        }
    }
}

int pci_present() { return present; }
int pci_device_count() { return device_total; }
const PciDevice* pci_device(int index) { return (index >= 0 && index < device_total) ? &devices[index] : 0; }

const PciDevice* pci_find_class(uint8_t cls, uint8_t subcls, uint8_t prog_if, int nth)
{
    for (int i = 0; i < device_total; ++i)
        if (devices[i].cls == cls && devices[i].subcls == subcls && (prog_if == 0xFF || devices[i].prog_if == prog_if))
            if (nth-- == 0) return &devices[i];
    return 0;
}

const char* pci_class_name(uint8_t cls, uint8_t subcls)
{
    if (cls == 1 && subcls == 8) return "NVM controller (NVMe)";
    if (cls == 1 && subcls == 6) return "SATA controller (AHCI)";
    if (cls == 1 && subcls == 1) return "IDE controller";
    if (cls == 1) return "mass storage";
    if (cls == 2) return "network controller";
    if (cls == 3) return "display controller";
    if (cls == 6 && subcls == 0) return "host bridge";
    if (cls == 6) return "bridge";
    if (cls == 12 && subcls == 3) return "USB controller";
    if (cls == 4) return "multimedia";
    return "other";
}

void pci_list()
{
    if (!present) { uart_puts("PCI: no PCIe host bridge found.\r\n"); return; }
    uart_puts("PCI devices (bus 0, ECAM 0x4010000000)\r\n");
    for (int i = 0; i < device_total; ++i)
    {
        const PciDevice* d = &devices[i];
        put_hex(d->bus, 2); uart_putc(':'); put_hex(d->dev, 2); uart_putc('.'); put_hex(d->fn, 1);
        uart_puts("  "); put_hex(d->vendor, 4); uart_putc(':'); put_hex(d->device, 4);
        uart_puts("  class "); put_hex(d->cls, 2); put_hex(d->subcls, 2); uart_puts("  ");
        uart_puts(pci_class_name(d->cls, d->subcls));
        for (int b = 0; b < 6; ++b)
            if (d->bar_size[b]) { uart_puts("\r\n        BAR"); put_dec((uint64_t)b); uart_puts(" 0x"); put_hex(d->bar[b], 8); uart_puts(" size "); put_dec(d->bar_size[b]); }
        uart_puts("\r\n");
    }
}
