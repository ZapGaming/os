#ifndef NET_ETHERNET_H
#define NET_ETHERNET_H

#include <stdint.h>

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

struct eth_header {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype;
} __attribute__((packed));

extern const uint8_t ETH_BROADCAST[6];

void eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const void *payload, uint16_t len);

/* Registered with rtl8139_set_rx_handler; dispatches to arp/ip by ethertype. */
void eth_handle_frame(const uint8_t *frame, uint16_t len);

#endif
