#ifndef DRIVERS_PCI_H
#define DRIVERS_PCI_H

#include <stdint.h>

struct pci_device {
    uint8_t bus, slot, func;
    uint16_t vendor_id, device_id;
    uint32_t bar0, bar1;
    uint8_t interrupt_line;
};

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);

/* Scans all bus/slot/func combinations; returns 1 and fills `out` on the
 * first match for (vendor_id, device_id), else returns 0. */
int pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_device *out);

void pci_enable_bus_mastering(const struct pci_device *dev);

#endif
