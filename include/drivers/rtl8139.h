#ifndef DRIVERS_RTL8139_H
#define DRIVERS_RTL8139_H

#include <stdint.h>

typedef void (*rtl8139_rx_handler_t)(const uint8_t *frame, uint16_t len);

/* Finds the device via PCI, resets it, sets up RX/TX buffers, and hooks
 * its IRQ. Returns 1 on success, 0 if no RTL8139 is present. */
int rtl8139_init(void);

void rtl8139_get_mac(uint8_t mac[6]);
void rtl8139_send(const void *data, uint16_t len);
void rtl8139_set_rx_handler(rtl8139_rx_handler_t handler);

#endif
