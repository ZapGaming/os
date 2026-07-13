#include <net/arp.h>
#include <net/ethernet.h>
#include <net/net.h>
#include <kernel/serial.h>
#include <string.h>

#define ARP_HTYPE_ETHERNET 1
#define ARP_OPER_REQUEST 1
#define ARP_OPER_REPLY   2

#define ARP_CACHE_SIZE 8

struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    int valid;
};

static struct arp_entry cache[ARP_CACHE_SIZE];

static void cache_learn(uint32_t ip, const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].ip == ip) {
            memcpy(cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!cache[i].valid) {
            cache[i].ip = ip;
            memcpy(cache[i].mac, mac, 6);
            cache[i].valid = 1;
            serial_printf("arp: learned %x -> %x:%x:%x:%x:%x:%x\n", ip,
                          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            return;
        }
    }
}

int arp_resolve(uint32_t ip, uint8_t mac_out[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].ip == ip) {
            memcpy(mac_out, cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

void arp_send_request(uint32_t target_ip) {
    struct arp_packet pkt;
    pkt.htype = net_htons(ARP_HTYPE_ETHERNET);
    pkt.ptype = net_htons(ETHERTYPE_IPV4);
    pkt.hlen = 6;
    pkt.plen = 4;
    pkt.oper = net_htons(ARP_OPER_REQUEST);
    memcpy(pkt.sha, net_get_mac(), 6);
    pkt.spa = net_htonl(net_get_ip());
    memset(pkt.tha, 0, 6);
    pkt.tpa = net_htonl(target_ip);

    eth_send(ETH_BROADCAST, ETHERTYPE_ARP, &pkt, sizeof(pkt));
}

static void arp_send_reply(const struct arp_packet *req) {
    struct arp_packet pkt;
    pkt.htype = net_htons(ARP_HTYPE_ETHERNET);
    pkt.ptype = net_htons(ETHERTYPE_IPV4);
    pkt.hlen = 6;
    pkt.plen = 4;
    pkt.oper = net_htons(ARP_OPER_REPLY);
    memcpy(pkt.sha, net_get_mac(), 6);
    pkt.spa = net_htonl(net_get_ip());
    memcpy(pkt.tha, req->sha, 6);
    pkt.tpa = req->spa;

    eth_send(req->sha, ETHERTYPE_ARP, &pkt, sizeof(pkt));
}

void arp_handle_packet(const uint8_t *data, uint16_t len) {
    if (len < sizeof(struct arp_packet)) return;
    const struct arp_packet *pkt = (const struct arp_packet *)data;

    uint32_t sender_ip = net_ntohl(pkt->spa);
    cache_learn(sender_ip, pkt->sha);

    uint16_t oper = net_ntohs(pkt->oper);
    if (oper == ARP_OPER_REQUEST && net_ntohl(pkt->tpa) == net_get_ip()) {
        arp_send_reply(pkt);
    }
}
