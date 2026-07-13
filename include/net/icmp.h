#ifndef NET_ICMP_H
#define NET_ICMP_H

#include <stdint.h>

struct icmp_header {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

/* Returns 1 if sent (destination MAC already resolved), 0 otherwise. */
int icmp_send_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq);

void icmp_handle_packet(const uint8_t *data, uint16_t len, uint32_t src_ip);

/* Ping stats for the GUI's Network window. */
int icmp_replies_received(void);
int icmp_requests_sent(void);
uint32_t icmp_last_rtt_ms(void);

#endif
