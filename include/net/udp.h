#ifndef NET_UDP_H
#define NET_UDP_H

#include <stdint.h>

struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

typedef void (*udp_handler_t)(uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint16_t len);

int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const void *data, uint16_t len);

/* Registers a callback for datagrams arriving on local port `port`
 * (there's no real socket table -- one handler per port, last wins). */
void udp_register_handler(uint16_t port, udp_handler_t handler);
void udp_unregister_handler(uint16_t port);

void udp_handle_packet(const uint8_t *data, uint16_t len, uint32_t src_ip);

#endif
