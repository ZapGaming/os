#ifndef DRIVERS_E1000_H
#define DRIVERS_E1000_H

#include <stdint.h>

typedef void (*e1000_rx_handler_t)(const uint8_t *frame, uint16_t len);

/* Finds an Intel 8254x-family NIC via PCI (checked against a list of
 * common device IDs -- QEMU's "e1000" is 82540EM (0x100E); VMware's
 * "E1000"/"E1000E" virtual NICs and real hardware show up as various
 * others, e.g. 82545EM (0x100F)), resets it, sets up RX/TX descriptor
 * rings, and hooks its IRQ. Returns 1 on success, 0 if none is present.
 * Unlike rtl8139 (I/O-space BAR0), this chip is memory-mapped -- BAR0 is
 * used directly as a pointer, relying on this kernel's flat identity
 * mapping of the whole 4GB address space rather than a separate MMIO
 * remap step. */
int e1000_init(void);

void e1000_get_mac(uint8_t mac[6]);
void e1000_send(const void *data, uint16_t len);
void e1000_set_rx_handler(e1000_rx_handler_t handler);

#endif
