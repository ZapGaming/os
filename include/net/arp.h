#ifndef NET_ARP_H
#define NET_ARP_H

#include <stdint.h>

struct arp_packet {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed));

void arp_send_request(uint32_t target_ip);
void arp_handle_packet(const uint8_t *data, uint16_t len);

/* Looks up `ip` in the ARP cache; returns 1 and fills mac_out on hit. */
int arp_resolve(uint32_t ip, uint8_t mac_out[6]);

#endif
