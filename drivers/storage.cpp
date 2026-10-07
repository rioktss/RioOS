#include "storage.h"
#include "virtio_blk.h"
#include "uart.h"

void storage_init()
{
    virtio_blk_init();
}

int storage_ready()
{
    return virtio_blk_ready();
}

int storage_read_sector(uint64_t sector, void* buffer)
{
    return virtio_blk_read(sector, buffer);
}

int storage_write_sector(uint64_t sector, const void* buffer)
{
    return virtio_blk_write(sector, buffer);
}

int storage_flush()
{
    return virtio_blk_flush();
}

void storage_status()
{
    virtio_blk_status();
}
