#ifndef VIRTIO_MMIO_H
#define VIRTIO_MMIO_H

#include "types.h"

/* Locate a virtio-mmio transport by its device ID in QEMU's DTB. */
int virtio_mmio_find_device(uint32_t wanted_device_id,
                            uint64_t* base,
                            uint64_t* size,
                            int* irq);

#endif
