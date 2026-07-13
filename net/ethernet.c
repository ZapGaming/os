#include <net/ethernet.h>
#include <net/net.h>
#include <net/arp.h>
#include <net/ip.h>
#include <string.h>

const uint8_t ETH_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const void *payload, uint16_t len) {
    uint8_t frame[1514];
    struct eth_header *hdr = (struct eth_header *)frame;

    memcpy(hdr->dst, dst_mac, 6);
    memcpy(hdr->src, net_get_mac(), 6);
    hdr->ethertype = net_htons(ethertype);

    uint16_t max_payload = sizeof(frame) - sizeof(struct eth_header);
    if (len > max_payload) len = max_payload;
    memcpy(frame + sizeof(struct eth_header), payload, len);

    net_send_frame(frame, (uint16_t)(sizeof(struct eth_header) + len));
}

void eth_handle_frame(const uint8_t *frame, uint16_t len) {
    if (len < sizeof(struct eth_header)) return;

    const struct eth_header *hdr = (const struct eth_header *)frame;
    uint16_t ethertype = net_ntohs(hdr->ethertype);
    const uint8_t *payload = frame + sizeof(struct eth_header);
    uint16_t paylen = (uint16_t)(len - sizeof(struct eth_header));

    if (ethertype == ETHERTYPE_ARP) {
        arp_handle_packet(payload, paylen);
    } else if (ethertype == ETHERTYPE_IPV4) {
        ip_handle_packet(payload, paylen);
    }
}
