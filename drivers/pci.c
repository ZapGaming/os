#include <drivers/pci.h>
#include <kernel/io.h>
#include <kernel/serial.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return 0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func << 8)
         | (offset & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read32(bus, slot, func, offset);
    return (uint16_t)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t existing = pci_config_read32(bus, slot, func, offset);
    uint32_t shift = (offset & 2) * 8;
    uint32_t mask = 0xFFFFu << shift;
    uint32_t merged = (existing & ~mask) | ((uint32_t)value << shift);
    pci_config_write32(bus, slot, func, offset, merged);
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_device *out) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            for (uint32_t func = 0; func < 8; func++) {
                uint16_t vendor = pci_config_read16(bus, slot, func, 0x00);
                if (vendor == 0xFFFF) continue;

                uint16_t device = pci_config_read16(bus, slot, func, 0x02);
                if (vendor == vendor_id && device == device_id) {
                    out->bus = bus; out->slot = slot; out->func = func;
                    out->vendor_id = vendor;
                    out->device_id = device;
                    out->bar0 = pci_config_read32(bus, slot, func, 0x10);
                    out->bar1 = pci_config_read32(bus, slot, func, 0x14);
                    out->interrupt_line = (uint8_t)pci_config_read16(bus, slot, func, 0x3C);
                    serial_printf("pci: found %x:%x at %d:%d:%d irq=%d bar0=%x\n",
                                  vendor, device, bus, slot, func, out->interrupt_line, out->bar0);
                    return 1;
                }

                /* function 0's header type bit7 tells us if this slot is
                 * multi-function; skip functions 1-7 otherwise */
                if (func == 0) {
                    uint16_t header_type = pci_config_read16(bus, slot, func, 0x0E) & 0xFF;
                    if (!(header_type & 0x80)) break;
                }
            }
        }
    }
    return 0;
}

void pci_enable_bus_mastering(const struct pci_device *dev) {
    uint16_t command = pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
    command |= 0x04; /* bus master enable */
    pci_config_write16(dev->bus, dev->slot, dev->func, 0x04, command);
}
