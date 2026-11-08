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
#include "storage.h"
#include "virtio_blk.h"
#include "pci.h"
#include "nvme.h"
#include "ahci.h"
#include "uart.h"

/* Backends: virtio-blk first (existing behaviour); if no virtio disk exists,
   fall back to NVMe, then AHCI/SATA found through PCIe. */
enum Backend { BK_NONE = 0, BK_VIRTIO, BK_NVME, BK_AHCI };
static int backend = BK_NONE;

void storage_init()
{
    virtio_blk_init();
    pci_init();
    nvme_init();
    usb_init();
    ahci_init();
    if (virtio_blk_ready()) backend = BK_VIRTIO;
    else if (nvme_ready()) { backend = BK_NVME; uart_puts("Storage: root disk on NVMe.\r\n"); }
    else if (ahci_ready()) { backend = BK_AHCI; uart_puts("Storage: root disk on AHCI/SATA.\r\n"); }
    else backend = BK_NONE;
}

int storage_ready()
{
    switch (backend)
    {
        case BK_VIRTIO: return virtio_blk_ready();
        case BK_NVME: return nvme_ready();
        case BK_AHCI: return ahci_ready();
        default: return virtio_blk_ready();
    }
}

int storage_read_sector(uint64_t sector, void* buffer)
{
    switch (backend)
    {
        case BK_NVME: return nvme_read(sector, buffer);
        case BK_AHCI: return ahci_read(sector, buffer);
        default: return virtio_blk_read(sector, buffer);
    }
}

int storage_write_sector(uint64_t sector, const void* buffer)
{
    switch (backend)
    {
        case BK_NVME: return nvme_write(sector, buffer);
        case BK_AHCI: return ahci_write(sector, buffer);
        default: return virtio_blk_write(sector, buffer);
    }
}

int storage_flush()
{
    switch (backend)
    {
        case BK_NVME: return nvme_flush();
        case BK_AHCI: return ahci_flush();
        default: return virtio_blk_flush();
    }
}

int storage_backend() { return backend; }

void storage_status()
{
    switch (backend)
    {
        case BK_NVME: nvme_status(); break;
        case BK_AHCI: ahci_status(); break;
        default: virtio_blk_status(); break;
    }
}

/* ---- secondary devices for the multi-mount VFS ----
   1 = RAM disk 0, 5 = RAM disk 1 (tmpfs), 3 = NVMe, 4 = AHCI/SATA (the last two only when they are not the root disk). */
#define RAMDISK_SECTORS 4096
static uint8_t ramdisk[2][RAMDISK_SECTORS * 512] __attribute__((aligned(16)));
static int ram_index(int dev) { return dev == 1 ? 0 : dev == 5 ? 1 : -1; }

uint64_t storage_dev_sectors(int dev)
{
    if (ram_index(dev) >= 0) return RAMDISK_SECTORS;
    if (dev == 3) return (backend != BK_NVME && nvme_ready()) ? nvme_capacity() : 0;
    if (dev == 4) return (backend != BK_AHCI && ahci_ready()) ? ahci_capacity() : 0;
    return 0;
}

int storage_dev_read(int dev, uint64_t sector, void* buffer)
{
    int r = ram_index(dev);
    if (r >= 0)
    {
        if (sector >= RAMDISK_SECTORS) return -1;
        const uint8_t* src = &ramdisk[r][sector * 512]; uint8_t* d = (uint8_t*)buffer;
        for (int i = 0; i < 512; ++i) d[i] = src[i];
        return 0;
    }
    if (!storage_dev_sectors(dev)) return -1;
    return dev == 3 ? nvme_read(sector, buffer) : ahci_read(sector, buffer);
}

int storage_dev_write(int dev, uint64_t sector, const void* buffer)
{
    int r = ram_index(dev);
    if (r >= 0)
    {
        if (sector >= RAMDISK_SECTORS) return -1;
        uint8_t* dst = &ramdisk[r][sector * 512]; const uint8_t* s = (const uint8_t*)buffer;
        for (int i = 0; i < 512; ++i) dst[i] = s[i];
        return 0;
    }
    if (!storage_dev_sectors(dev)) return -1;
    return dev == 3 ? nvme_write(sector, buffer) : ahci_write(sector, buffer);
}

int storage_dev_flush(int dev)
{
    if (ram_index(dev) >= 0) return 0;
    if (!storage_dev_sectors(dev)) return -1;
    return dev == 3 ? nvme_flush() : ahci_flush();
}
