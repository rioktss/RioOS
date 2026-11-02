#ifndef PCI_H
#define PCI_H

#include "types.h"

struct PciDevice
{
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t cls, subcls, prog_if;
    uint64_t bar[6];
    uint64_t bar_size[6];
};

void pci_init();                       /* enumerate bus 0 via ECAM, assign BARs, enable memory + bus master */
int pci_present();
int pci_device_count();
const PciDevice* pci_device(int index);
const PciDevice* pci_find_class(uint8_t cls, uint8_t subcls, uint8_t prog_if, int nth);  /* prog_if 0xFF = any */
const char* pci_class_name(uint8_t cls, uint8_t subcls);
void pci_list();

#endif
