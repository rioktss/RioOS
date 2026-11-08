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

#include "ahci.h"
#include "pci.h"
#include "uart.h"

static uint8_t cmd_list[1024] __attribute__((aligned(1024)));
static uint8_t fis_area[256] __attribute__((aligned(256)));
static uint8_t cmd_table[256] __attribute__((aligned(128)));
static uint8_t data_buf[512] __attribute__((aligned(512)));

static uint64_t hba = 0;
static uint64_t port = 0;
static int ready = 0;
static uint64_t sectors = 0;

static volatile uint32_t* hreg(uint64_t off) { return (volatile uint32_t*)(hba + off); }
static volatile uint32_t* preg(uint64_t off) { return (volatile uint32_t*)(port + off); }
static void dsb() { asm volatile("dsb sy" ::: "memory"); }

static int port_stop()
{
    *preg(0x18) &= ~1U;                              /* ST = 0 */
    for (int i = 0; i < 1000000; ++i) if (!(*preg(0x18) & (1U << 15))) break;
    *preg(0x18) &= ~(1U << 4);                       /* FRE = 0 */
    for (int i = 0; i < 1000000; ++i) if (!(*preg(0x18) & (1U << 14))) return 0;
    return -1;
}

static void port_start()
{
    for (int i = 0; i < 1000000; ++i) if (!(*preg(0x18) & (1U << 15))) break;
    *preg(0x18) |= (1U << 4);
    *preg(0x18) |= 1U;
}

/* one command in slot 0; lba48: command 0x25 read / 0x35 write / 0xEC identify / 0xEA flush */
static int issue(uint8_t command, uint64_t lba, int write, int use_data)
{
    volatile uint32_t* h = (volatile uint32_t*)cmd_list;
    h[0] = 5U | (write ? (1U << 6) : 0U) | ((use_data ? 1U : 0U) << 16);   /* CFL = 5 dwords */
    h[1] = 0;
    h[2] = (uint32_t)(uint64_t)cmd_table; h[3] = (uint32_t)((uint64_t)cmd_table >> 32);
    volatile uint8_t* t = cmd_table;
    for (int i = 0; i < 256; ++i) t[i] = 0;
    t[0] = 0x27; t[1] = 0x80; t[2] = command; t[3] = 0;
    t[4] = (uint8_t)lba; t[5] = (uint8_t)(lba >> 8); t[6] = (uint8_t)(lba >> 16);
    t[7] = (command == 0xEC) ? 0 : 0x40;
    t[8] = (uint8_t)(lba >> 24); t[9] = (uint8_t)(lba >> 32); t[10] = (uint8_t)(lba >> 40);
    t[12] = use_data && command != 0xEC ? 1 : 0; t[13] = 0;
    if (use_data)
    {
        volatile uint32_t* prd = (volatile uint32_t*)(cmd_table + 0x80);
        prd[0] = (uint32_t)(uint64_t)data_buf; prd[1] = (uint32_t)((uint64_t)data_buf >> 32);
        prd[2] = 0; prd[3] = 511U;
    }
    *preg(0x10) = 0xFFFFFFFFU;
    dsb();
    *preg(0x38) = 1U;
    for (int i = 0; i < 4000000; ++i)
    {
        if (!(*preg(0x38) & 1U)) break;
        if (*preg(0x10) & (1U << 30)) return -1;      /* task file error */
        if (i == 3999999) return -2;
        asm volatile("yield");
    }
    if (*preg(0x20) & 0x01U) return -1;
    return 0;
}

void ahci_init()
{
    ready = 0; sectors = 0;
    const PciDevice* d = pci_find_class(1, 6, 0xFF, 0);
    if (!d || d->bar_size[5] == 0) return;
    hba = d->bar[5];
    *hreg(0x04) |= (1U << 31);                         /* AHCI enable */
    uint32_t pi = *hreg(0x0C);
    for (int p = 0; p < 32; ++p)
    {
        if (!(pi & (1U << p))) continue;
        port = hba + 0x100 + (uint64_t)p * 0x80;
        uint32_t ssts = *preg(0x28);
        if ((ssts & 0xFU) != 3 || ((ssts >> 8) & 0xFU) != 1) continue;     /* no device / no link */
        if (port_stop() != 0) continue;
        for (int i = 0; i < 1024; ++i) cmd_list[i] = 0;
        for (int i = 0; i < 256; ++i) fis_area[i] = 0;
        *preg(0x00) = (uint32_t)(uint64_t)cmd_list; *preg(0x04) = (uint32_t)((uint64_t)cmd_list >> 32);
        *preg(0x08) = (uint32_t)(uint64_t)fis_area; *preg(0x0C) = (uint32_t)((uint64_t)fis_area >> 32);
        *preg(0x30) = 0xFFFFFFFFU; *preg(0x10) = 0xFFFFFFFFU; *preg(0x14) = 0;
        dsb();
        port_start();
        for (int i = 0; i < 20000000 && *preg(0x24) == 0xFFFFFFFFU; ++i) asm volatile("yield");   /* wait for D2H signature FIS */
        if (*preg(0x24) != 0x00000101U) continue;                         /* not a SATA disk */
        for (int i = 0; i < 20000000 && (*preg(0x20) & 0x88U); ++i) asm volatile("yield");        /* BSY/DRQ clear */
        if (issue(0xEC, 0, 0, 1) != 0) { uart_puts("AHCI: identify failed\r\n"); continue; }
        volatile uint16_t* w = (volatile uint16_t*)data_buf;
        uint64_t lba48 = (uint64_t)w[100] | ((uint64_t)w[101] << 16) | ((uint64_t)w[102] << 32) | ((uint64_t)w[103] << 48);
        if (lba48 == 0) lba48 = (uint64_t)w[60] | ((uint64_t)w[61] << 16);
        sectors = lba48;
        ready = sectors != 0;
        if (ready) { uart_puts("AHCI: SATA disk ready on port "); uart_putc((char)('0' + (p % 10))); uart_puts(".\r\n"); return; }
    }
}

int ahci_ready() { return ready; }
uint64_t ahci_capacity() { return sectors; }

int ahci_read(uint64_t sector, void* buffer)
{
    if (!ready || sector >= sectors) return -1;
    if (issue(0x25, sector, 0, 1) != 0) return -1;
    for (int i = 0; i < 512; ++i) ((uint8_t*)buffer)[i] = ((volatile uint8_t*)data_buf)[i];
    return 0;
}

int ahci_write(uint64_t sector, const void* buffer)
{
    if (!ready || sector >= sectors) return -1;
    for (int i = 0; i < 512; ++i) ((volatile uint8_t*)data_buf)[i] = ((const uint8_t*)buffer)[i];
    return issue(0x35, sector, 1, 1) == 0 ? 0 : -1;
}

int ahci_flush() { return ready ? (issue(0xEA, 0, 0, 0) == 0 ? 0 : -1) : -1; }

static void put_dec(uint64_t v)
{
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) uart_putc(t[--n]);
}

void ahci_status()
{
    if (!ready) { uart_puts("AHCI: no SATA disk\r\n"); return; }
    uart_puts("AHCI: SATA disk, "); put_dec(sectors); uart_puts(" sectors of 512 B ("); put_dec(sectors / 2048); uart_puts(" MiB)\r\n");
}

int ahci_selftest(int destructive)
{
    if (!ready) return -1;
    static uint8_t a[512], b[512];
    if (ahci_read(0, a) != 0) return -2;
    if (ahci_read(0, b) != 0) return -2;
    for (int i = 0; i < 512; ++i) if (a[i] != b[i]) return -3;
    if (!destructive) return 0;
    uint64_t last = sectors - 1;
    uint8_t save[512];
    if (ahci_read(last, save) != 0) return -4;
    for (int i = 0; i < 512; ++i) a[i] = (uint8_t)(i * 5 + 1);
    if (ahci_write(last, a) != 0) return -5;
    if (ahci_flush() != 0) return -6;
    if (ahci_read(last, b) != 0) return -7;
    int bad = 0;
    for (int i = 0; i < 512; ++i) if (a[i] != b[i]) bad = 1;
    (void)ahci_write(last, save);
    return bad ? -8 : 0;
}
