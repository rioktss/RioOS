#ifndef STORAGE_H
#define STORAGE_H

#include "types.h"

void storage_init();
int storage_ready();
int storage_read_sector(uint64_t sector, void* buffer);
int storage_write_sector(uint64_t sector, const void* buffer);
int storage_flush();
void storage_status();
int storage_backend();   /* 0 none, 1 virtio-blk, 2 NVMe, 3 AHCI */

#endif
