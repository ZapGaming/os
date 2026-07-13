#include <net/ip.h>
#include <net/ethernet.h>
#include <net/arp.h>
#include <net/net.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/tcp.h>
#include <kernel/pit.h>
#include <string.h>

static uint16_t next_id = 1;

/* arp_resolve() is just a cache lookup -- it never sends anything. Every
 * caller needs the MAC actually resolved before it can send, so retry
 * with a real ARP request a few times (cooperatively, via pit_sleep)
 * before giving up. */
static int ip_resolve_next_hop(uint32_t next_hop, uint8_t mac_out[6]) {
    if (arp_resolve(next_hop, mac_out)) return 1;
    for (int attempt = 0; attempt < 5; attempt++) {
        arp_send_request(next_hop);
        pit_sleep(200);
        if (arp_resolve(next_hop, mac_out)) return 1;
    }
    return 0;
}

int ip_send(uint32_t dst_ip, uint8_t protocol, const void *payload, uint16_t len) {
    /* Off-subnet destinations (i.e. anything on the real internet) are
     * unreachable by ARP directly -- ARP is link-local. Route through the
     * gateway instead: the IP header still carries the real destination,
     * only the Ethernet frame's MAC target changes. */
    uint32_t next_hop = net_is_local(dst_ip) ? dst_ip : net_get_gateway_ip();

    uint8_t dst_mac[6];
    if (!ip_resolve_next_hop(next_hop, dst_mac)) return 0;

    uint8_t buf[1500];
    uint16_t total = (uint16_t)(sizeof(struct ip_header) + len);
    if (total > sizeof(buf)) return 0;

    struct ip_header *hdr = (struct ip_header *)buf;
    hdr->version_ihl = 0x45; /* version 4, 5 * 4 = 20 byte header, no options */
    hdr->tos = 0;
    hdr->total_length = net_htons(total);
    hdr->id = net_htons(next_id++);
    hdr->flags_frag = 0;
    hdr->ttl = 64;
    hdr->protocol = protocol;
    hdr->checksum = 0;
    hdr->src = net_htonl(net_get_ip());
    hdr->dst = net_htonl(dst_ip);
    hdr->checksum = net_htons(net_checksum(hdr, sizeof(struct ip_header)));

    memcpy(buf + sizeof(struct ip_header), payload, len);

    eth_send(dst_mac, ETHERTYPE_IPV4, buf, total);
    return 1;
}

void ip_handle_packet(const uint8_t *data, uint16_t len) {
    if (len < sizeof(struct ip_header)) return;
    const struct ip_header *hdr = (const struct ip_header *)data;

    uint8_t ihl_words = hdr->version_ihl & 0x0F;
    uint16_t header_len = (uint16_t)(ihl_words * 4);
    if (header_len < sizeof(struct ip_header) || header_len > len) return;

    const uint8_t *payload = data + header_len;
    uint16_t total_len = net_ntohs(hdr->total_length);
    uint16_t payload_len = (uint16_t)(total_len > header_len ? total_len - header_len : 0);
    if (payload_len > len - header_len) payload_len = (uint16_t)(len - header_len);

    uint32_t src_ip = net_ntohl(hdr->src);
    if (hdr->protocol == IP_PROTO_ICMP) {
        icmp_handle_packet(payload, payload_len, src_ip);
    } else if (hdr->protocol == IP_PROTO_UDP) {
        udp_handle_packet(payload, payload_len, src_ip);
    } else if (hdr->protocol == IP_PROTO_TCP) {
        tcp_handle_packet(payload, payload_len, src_ip);
    }
}
