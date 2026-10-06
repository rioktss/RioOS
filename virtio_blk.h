#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "types.h"

void virtio_blk_init();
int virtio_blk_present();
int virtio_blk_ready();
int virtio_blk_read(uint64_t sector, void* buffer);
int virtio_blk_write(uint64_t sector, const void* buffer);
int virtio_blk_flush();
uint64_t virtio_blk_capacity();
uint32_t virtio_blk_block_size();
int virtio_blk_read_only();
uint64_t virtio_blk_base();
int virtio_blk_irq();
void virtio_blk_status();

#endif
