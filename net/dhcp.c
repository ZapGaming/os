#include <net/dhcp.h>
#include <net/net.h>
#include <net/ethernet.h>
#include <net/ip.h>
#include <net/udp.h>
#include <kernel/pit.h>
#include <kernel/serial.h>
#include <string.h>

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC 0x63825363u

#define DHCP_OP_BOOTREQUEST 1
#define DHCP_OP_BOOTREPLY   2
#define DHCP_HTYPE_ETHERNET 1

#define DHCP_OPT_PAD          0
#define DHCP_OPT_SUBNET_MASK  1
#define DHCP_OPT_ROUTER       3
#define DHCP_OPT_DNS_SERVER   6
#define DHCP_OPT_REQUESTED_IP 50
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_PARAM_LIST   55
#define DHCP_OPT_END          255

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6

/* Fixed-size BOOTP/DHCP header (RFC 2131) -- `options` is variable
 * length and handled separately rather than as part of this struct. */
struct dhcp_header {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
} __attribute__((packed));

#define DHCP_TIMEOUT_TICKS 300 /* 3s at 100Hz, matching dns_resolve's timeout */

/* Result of the last OFFER or ACK/NAK we've seen, filled in by the UDP
 * handler and polled by the blocking wait loops below -- same pattern
 * as net/dns.c's resolved_state. */
static uint32_t expected_xid = 0;
static int reply_state = 0; /* 0=pending, 1=got offer, 2=got ack, 3=got nak */
static uint32_t reply_yiaddr = 0, reply_server_id = 0;
static uint32_t reply_subnet_mask = 0, reply_router = 0, reply_dns_server = 0;
static int want_offer = 0; /* which message type we're currently waiting for */

static void parse_options(const uint8_t *opt, uint16_t len, uint8_t *out_msg_type) {
    uint16_t i = 0;
    while (i < len) {
        uint8_t code = opt[i++];
        if (code == DHCP_OPT_PAD) continue;
        if (code == DHCP_OPT_END) break;
        if (i >= len) break;
        uint8_t olen = opt[i++];
        if ((uint16_t)(i + olen) > len) break;

        if (code == DHCP_OPT_MSG_TYPE && olen == 1) {
            *out_msg_type = opt[i];
        } else if (code == DHCP_OPT_SUBNET_MASK && olen == 4) {
            memcpy(&reply_subnet_mask, opt + i, 4);
            reply_subnet_mask = net_ntohl(reply_subnet_mask);
        } else if (code == DHCP_OPT_ROUTER && olen >= 4) {
            memcpy(&reply_router, opt + i, 4);
            reply_router = net_ntohl(reply_router);
        } else if (code == DHCP_OPT_DNS_SERVER && olen >= 4) {
            memcpy(&reply_dns_server, opt + i, 4);
            reply_dns_server = net_ntohl(reply_dns_server);
        } else if (code == DHCP_OPT_SERVER_ID && olen == 4) {
            memcpy(&reply_server_id, opt + i, 4);
            reply_server_id = net_ntohl(reply_server_id);
        }
        i += olen;
    }
}

static void dhcp_reply_handler(uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint16_t len) {
    (void)src_ip; (void)src_port;
    if (len < sizeof(struct dhcp_header)) return;

    const struct dhcp_header *hdr = (const struct dhcp_header *)data;
    if (hdr->op != DHCP_OP_BOOTREPLY) return;
    if (net_ntohl(hdr->xid) != expected_xid) return;
    if (net_ntohl(hdr->magic) != DHCP_MAGIC) return;

    uint8_t msg_type = 0;
    parse_options(data + sizeof(struct dhcp_header), (uint16_t)(len - sizeof(struct dhcp_header)), &msg_type);

    reply_yiaddr = net_ntohl(hdr->yiaddr);

    if (msg_type == DHCP_MSG_OFFER && want_offer == DHCP_MSG_OFFER) {
        reply_state = 1;
    } else if (msg_type == DHCP_MSG_ACK && want_offer == DHCP_MSG_ACK) {
        reply_state = 2;
    } else if (msg_type == DHCP_MSG_NAK && want_offer == DHCP_MSG_ACK) {
        reply_state = 3;
    }
}

/* Builds a DHCP message (DISCOVER or REQUEST) and broadcasts it inside
 * a raw Ethernet+IP+UDP frame -- deliberately not going through
 * ip_send()/udp_send(), since both assume we already have a usable
 * src IP and a unicast destination reachable via ARP, neither of which
 * holds before a lease exists. The UDP checksum is left at 0 (valid
 * per RFC 768 for IPv4 -- "no checksum computed"), which every DHCP
 * server in practice accepts fine. */
static void send_dhcp_message(uint8_t msg_type, uint32_t xid, uint32_t requested_ip, uint32_t server_id) {
    uint8_t dhcp_buf[300];
    struct dhcp_header *hdr = (struct dhcp_header *)dhcp_buf;
    memset(hdr, 0, sizeof(*hdr));
    hdr->op = DHCP_OP_BOOTREQUEST;
    hdr->htype = DHCP_HTYPE_ETHERNET;
    hdr->hlen = 6;
    hdr->xid = net_htonl(xid);
    memcpy(hdr->chaddr, net_get_mac(), 6);
    hdr->magic = net_htonl(DHCP_MAGIC);

    uint8_t *opt = dhcp_buf + sizeof(struct dhcp_header);
    int oi = 0;
    opt[oi++] = DHCP_OPT_MSG_TYPE; opt[oi++] = 1; opt[oi++] = msg_type;
    if (requested_ip) {
        uint32_t be = net_htonl(requested_ip);
        opt[oi++] = DHCP_OPT_REQUESTED_IP; opt[oi++] = 4;
        memcpy(opt + oi, &be, 4); oi += 4;
    }
    if (server_id) {
        uint32_t be = net_htonl(server_id);
        opt[oi++] = DHCP_OPT_SERVER_ID; opt[oi++] = 4;
        memcpy(opt + oi, &be, 4); oi += 4;
    }
    opt[oi++] = DHCP_OPT_PARAM_LIST; opt[oi++] = 3;
    opt[oi++] = DHCP_OPT_SUBNET_MASK; opt[oi++] = DHCP_OPT_ROUTER; opt[oi++] = DHCP_OPT_DNS_SERVER;
    opt[oi++] = DHCP_OPT_END;

    uint16_t dhcp_len = (uint16_t)(sizeof(struct dhcp_header) + oi);

    uint8_t udp_buf[300 + sizeof(struct udp_header)];
    struct udp_header *udp_hdr = (struct udp_header *)udp_buf;
    udp_hdr->src_port = net_htons(DHCP_CLIENT_PORT);
    udp_hdr->dst_port = net_htons(DHCP_SERVER_PORT);
    udp_hdr->length = net_htons((uint16_t)(sizeof(struct udp_header) + dhcp_len));
    udp_hdr->checksum = 0;
    memcpy(udp_buf + sizeof(struct udp_header), dhcp_buf, dhcp_len);
    uint16_t udp_total = (uint16_t)(sizeof(struct udp_header) + dhcp_len);

    uint8_t ip_buf[300 + sizeof(struct udp_header) + sizeof(struct ip_header)];
    struct ip_header *ip_hdr = (struct ip_header *)ip_buf;
    ip_hdr->version_ihl = 0x45;
    ip_hdr->tos = 0;
    ip_hdr->total_length = net_htons((uint16_t)(sizeof(struct ip_header) + udp_total));
    ip_hdr->id = 0;
    ip_hdr->flags_frag = 0;
    ip_hdr->ttl = 64;
    ip_hdr->protocol = IP_PROTO_UDP;
    ip_hdr->checksum = 0;
    ip_hdr->src = net_htonl(ip_make(0, 0, 0, 0));
    ip_hdr->dst = net_htonl(ip_make(255, 255, 255, 255));
    ip_hdr->checksum = net_htons(net_checksum(ip_hdr, sizeof(struct ip_header)));
    memcpy(ip_buf + sizeof(struct ip_header), udp_buf, udp_total);

    eth_send(ETH_BROADCAST, ETHERTYPE_IPV4, ip_buf, (uint16_t)(sizeof(struct ip_header) + udp_total));
}

/* Sends `msg_type` and blocks (same hlt-loop pattern as dns_resolve())
 * until a reply matching `wait_for` arrives or we time out. Returns 1
 * on a matching reply, 0 on timeout. */
static int send_and_wait(uint8_t msg_type, uint32_t xid, uint32_t requested_ip, uint32_t server_id, uint8_t wait_for) {
    want_offer = wait_for;
    reply_state = 0;
    send_dhcp_message(msg_type, xid, requested_ip, server_id);

    uint32_t start = pit_ticks();
    while (reply_state == 0 && (pit_ticks() - start) < DHCP_TIMEOUT_TICKS) {
        __asm__ volatile ("hlt");
    }
    return reply_state != 0;
}

void net_dhcp_negotiate(void) {
    uint32_t xid = pit_ticks() ^ 0xA5A5A5A5u;
    expected_xid = xid;

    udp_register_handler(DHCP_CLIENT_PORT, dhcp_reply_handler);

    int ok = 0;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        /* Reset per-lease fields every attempt -- otherwise a stale
         * option from an earlier attempt's OFFER (e.g. one that never
         * made it to a successful ACK) could linger and get mixed with
         * a *different* server's lease on a later attempt. */
        reply_subnet_mask = 0; reply_router = 0; reply_dns_server = 0; reply_server_id = 0;

        if (!send_and_wait(DHCP_MSG_DISCOVER, xid, 0, 0, DHCP_MSG_OFFER)) continue;

        uint32_t offered_ip = reply_yiaddr;
        uint32_t server_id = reply_server_id;

        if (!send_and_wait(DHCP_MSG_REQUEST, xid, offered_ip, server_id, DHCP_MSG_ACK)) continue;
        if (reply_state == 3) continue; /* NAK -- retry from DISCOVER */

        uint32_t mask = reply_subnet_mask ? reply_subnet_mask : ip_make(255, 255, 255, 0);
        uint32_t router = reply_router ? reply_router : server_id;
        net_set_ip_config(reply_yiaddr, router, mask);
        if (reply_dns_server) net_set_dns_server(reply_dns_server);

        serial_printf("dhcp: leased %d.%d.%d.%d gateway=%d.%d.%d.%d mask=%d.%d.%d.%d\n",
                      (reply_yiaddr >> 24) & 0xFF, (reply_yiaddr >> 16) & 0xFF,
                      (reply_yiaddr >> 8) & 0xFF, reply_yiaddr & 0xFF,
                      (router >> 24) & 0xFF, (router >> 16) & 0xFF, (router >> 8) & 0xFF, router & 0xFF,
                      (mask >> 24) & 0xFF, (mask >> 16) & 0xFF, (mask >> 8) & 0xFF, mask & 0xFF);
        ok = 1;
    }

    udp_unregister_handler(DHCP_CLIENT_PORT);

    if (!ok) {
        serial_printf("dhcp: no server responded, keeping static fallback config\n");
    }
}
