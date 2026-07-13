#ifndef NET_IP_H
#define NET_IP_H

#include <stdint.h>

#define IP_PROTO_ICMP 1

struct ip_header {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

/* Resolves dst_ip via the ARP cache (caller is expected to have already
 * triggered/awaited resolution) and sends an IPv4 packet. Returns 1 if
 * sent, 0 if the destination MAC isn't known yet. */
int ip_send(uint32_t dst_ip, uint8_t protocol, const void *payload, uint16_t len);

void ip_handle_packet(const uint8_t *data, uint16_t len);

#endif
