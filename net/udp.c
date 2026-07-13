#include <net/udp.h>
#include <net/ip.h>
#include <net/net.h>
#include <string.h>

#define MAX_UDP_HANDLERS 4

struct udp_binding {
    uint16_t port;
    udp_handler_t handler;
    int used;
};

static struct udp_binding bindings[MAX_UDP_HANDLERS];

int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const void *data, uint16_t len) {
    uint8_t buf[1472];
    uint16_t total = (uint16_t)(sizeof(struct udp_header) + len);
    if (total > sizeof(buf)) return 0;

    struct udp_header *hdr = (struct udp_header *)buf;
    hdr->src_port = net_htons(src_port);
    hdr->dst_port = net_htons(dst_port);
    hdr->length = net_htons(total);
    hdr->checksum = 0;
    memcpy(buf + sizeof(struct udp_header), data, len);

    /* UDP checksum covers a 12-byte pseudo-header (src/dst IP, zero,
     * protocol, UDP length) followed by the UDP header + data. */
    uint8_t csum_buf[12 + sizeof(buf)];
    uint32_t src_be = net_htonl(net_get_ip());
    uint32_t dst_be = net_htonl(dst_ip);
    memcpy(csum_buf, &src_be, 4);
    memcpy(csum_buf + 4, &dst_be, 4);
    csum_buf[8] = 0;
    csum_buf[9] = IP_PROTO_UDP;
    uint16_t len_be = net_htons(total);
    memcpy(csum_buf + 10, &len_be, 2);
    memcpy(csum_buf + 12, buf, total);

    uint16_t checksum = net_checksum(csum_buf, 12 + total);
    hdr->checksum = net_htons(checksum == 0 ? 0xFFFF : checksum);

    return ip_send(dst_ip, IP_PROTO_UDP, buf, total);
}

void udp_register_handler(uint16_t port, udp_handler_t handler) {
    for (int i = 0; i < MAX_UDP_HANDLERS; i++) {
        if (!bindings[i].used || bindings[i].port == port) {
            bindings[i].port = port;
            bindings[i].handler = handler;
            bindings[i].used = 1;
            return;
        }
    }
}

void udp_unregister_handler(uint16_t port) {
    for (int i = 0; i < MAX_UDP_HANDLERS; i++) {
        if (bindings[i].used && bindings[i].port == port) {
            bindings[i].used = 0;
        }
    }
}

void udp_handle_packet(const uint8_t *data, uint16_t len, uint32_t src_ip) {
    if (len < sizeof(struct udp_header)) return;
    const struct udp_header *hdr = (const struct udp_header *)data;

    uint16_t dst_port = net_ntohs(hdr->dst_port);
    uint16_t src_port = net_ntohs(hdr->src_port);
    const uint8_t *payload = data + sizeof(struct udp_header);
    uint16_t payload_len = (uint16_t)(len - sizeof(struct udp_header));

    for (int i = 0; i < MAX_UDP_HANDLERS; i++) {
        if (bindings[i].used && bindings[i].port == dst_port) {
            bindings[i].handler(src_ip, src_port, payload, payload_len);
            return;
        }
    }
}
