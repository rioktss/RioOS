#ifndef NVME_H
#define NVME_H

#include "types.h"

void nvme_init();
int nvme_ready();
uint64_t nvme_capacity();                                 /* sectors of 512 bytes */
int nvme_read(uint64_t sector, void* buffer);
int nvme_write(uint64_t sector, const void* buffer);
int nvme_flush();
void nvme_status();
int nvme_selftest(int destructive);                       /* 0 = pass */

#endif
