#include <net/icmp.h>
#include <net/ip.h>
#include <net/net.h>
#include <kernel/pit.h>
#include <kernel/serial.h>
#include <string.h>

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

static int requests_sent = 0;
static int replies_received = 0;
static uint32_t last_rtt_ms = 0;

static uint16_t pending_id = 0, pending_seq = 0;
static uint32_t pending_send_tick = 0;
static int pending_outstanding = 0;

int icmp_send_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq) {
    struct {
        struct icmp_header hdr;
        uint8_t payload[32];
    } __attribute__((packed)) pkt;

    pkt.hdr.type = ICMP_ECHO_REQUEST;
    pkt.hdr.code = 0;
    pkt.hdr.checksum = 0;
    pkt.hdr.id = net_htons(id);
    pkt.hdr.seq = net_htons(seq);
    for (unsigned i = 0; i < sizeof(pkt.payload); i++) pkt.payload[i] = (uint8_t)('a' + (i % 23));
    pkt.hdr.checksum = net_htons(net_checksum(&pkt, sizeof(pkt)));

    /* Set the "reply we're expecting" state *before* actually sending:
     * the reply can come back fast enough (real hardware/network speed,
     * not tied to our 10ms scheduler tick) that an RX interrupt on some
     * other task can process it before this task would otherwise get
     * back around to setting pending_* right after ip_send(). */
    pending_id = id;
    pending_seq = seq;
    pending_send_tick = pit_ticks();
    pending_outstanding = 1;

    if (!ip_send(dst_ip, IP_PROTO_ICMP, &pkt, sizeof(pkt))) {
        pending_outstanding = 0;
        return 0;
    }

    requests_sent++;
    return 1;
}

void icmp_handle_packet(const uint8_t *data, uint16_t len, uint32_t src_ip) {
    (void)src_ip;
    if (len < sizeof(struct icmp_header)) return;
    const struct icmp_header *hdr = (const struct icmp_header *)data;
    if (hdr->type != ICMP_ECHO_REPLY) return;

    uint16_t id = net_ntohs(hdr->id);
    uint16_t seq = net_ntohs(hdr->seq);

    if (pending_outstanding && id == pending_id && seq == pending_seq) {
        last_rtt_ms = (pit_ticks() - pending_send_tick) * 10; /* PIT runs at 100Hz */
        replies_received++;
        pending_outstanding = 0;
        serial_printf("icmp: echo reply from id=%d seq=%d rtt=%ums\n", id, seq, last_rtt_ms);
    }
}

int icmp_replies_received(void) { return replies_received; }
int icmp_requests_sent(void) { return requests_sent; }
uint32_t icmp_last_rtt_ms(void) { return last_rtt_ms; }
