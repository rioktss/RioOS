#ifndef AHCI_H
#define AHCI_H

#include "types.h"

void ahci_init();
int ahci_ready();
uint64_t ahci_capacity();                                 /* sectors of 512 bytes */
int ahci_read(uint64_t sector, void* buffer);
int ahci_write(uint64_t sector, const void* buffer);
int ahci_flush();
void ahci_status();
int ahci_selftest(int destructive);                       /* 0 = pass */

#endif
