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
