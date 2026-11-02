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
