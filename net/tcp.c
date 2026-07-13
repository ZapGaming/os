#include <net/tcp.h>
#include <net/ip.h>
#include <net/net.h>
#include <kernel/pit.h>
#include <kernel/serial.h>
#include <string.h>

#define TCP_RECV_BUF_SIZE 32768
#define TCP_MAX_SEGMENT    1400
#define TCP_RETRY_TICKS    100  /* 1s per retry, at 100Hz */
#define TCP_MAX_RETRIES    6

enum tcp_state {
    TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED,
    TCP_FIN_WAIT1, TCP_FIN_WAIT2, TCP_LAST_ACK
};

/* One connection at a time -- this OS only ever needs one HTTP request
 * in flight, so a real per-connection table would be unused generality. */
static struct {
    enum tcp_state state;
    uint32_t remote_ip;
    uint16_t remote_port, local_port;

    uint32_t send_seq;   /* seq of the next byte we will send */
    int      ack_pending;
    uint32_t wait_ack_target;

    uint32_t recv_seq;   /* next seq we expect from the remote */
    int      fin_received;
    int      reset_received;

    uint8_t  recv_buf[TCP_RECV_BUF_SIZE];
    uint32_t recv_len;
} conn;

static uint16_t next_local_port = 44000;

static void tcp_send_segment(uint8_t flags, const void *data, uint16_t data_len) {
    uint8_t buf[TCP_MAX_SEGMENT + sizeof(struct tcp_header)];
    struct tcp_header *hdr = (struct tcp_header *)buf;

    hdr->src_port = net_htons(conn.local_port);
    hdr->dst_port = net_htons(conn.remote_port);
    hdr->seq = net_htonl(conn.send_seq);
    hdr->ack = net_htonl((flags & TCP_FLAG_ACK) ? conn.recv_seq : 0);
    hdr->data_offset = (uint8_t)(5 << 4); /* 20-byte header, no options */
    hdr->flags = flags;
    hdr->window = net_htons(8192);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;
    if (data_len) memcpy(buf + sizeof(struct tcp_header), data, data_len);

    uint16_t total = (uint16_t)(sizeof(struct tcp_header) + data_len);

    uint8_t csum_buf[12 + sizeof(buf)];
    uint32_t src_be = net_htonl(net_get_ip());
    uint32_t dst_be = net_htonl(conn.remote_ip);
    memcpy(csum_buf, &src_be, 4);
    memcpy(csum_buf + 4, &dst_be, 4);
    csum_buf[8] = 0;
    csum_buf[9] = IP_PROTO_TCP;
    uint16_t len_be = net_htons(total);
    memcpy(csum_buf + 10, &len_be, 2);
    memcpy(csum_buf + 12, buf, total);

    uint16_t checksum = net_checksum(csum_buf, 12 + total);
    hdr->checksum = net_htons(checksum == 0 ? 0xFFFF : checksum);

    ip_send(conn.remote_ip, IP_PROTO_TCP, buf, total);
}

/* Sends one segment and blocks (cooperatively) until it's ACKed,
 * retransmitting on timeout. `flags` should include TCP_FLAG_ACK once
 * past the handshake; FIN/data consume sequence numbers as usual. */
static int tcp_send_and_wait(uint8_t flags, const uint8_t *data, uint16_t len) {
    uint32_t consumed = len + ((flags & TCP_FLAG_FIN) ? 1u : 0u);
    conn.wait_ack_target = conn.send_seq + consumed;
    conn.ack_pending = 1;

    for (int attempt = 0; attempt < TCP_MAX_RETRIES; attempt++) {
        tcp_send_segment(flags, data, len);
        uint32_t start = pit_ticks();
        while (conn.ack_pending && conn.state != TCP_CLOSED &&
               (pit_ticks() - start) < TCP_RETRY_TICKS) {
            __asm__ volatile ("hlt");
        }
        if (!conn.ack_pending) {
            conn.send_seq += consumed;
            return 1;
        }
        if (conn.state == TCP_CLOSED) return 0;
    }
    return 0;
}

int tcp_connect(uint32_t remote_ip, uint16_t remote_port) {
    memset(&conn, 0, sizeof(conn));
    conn.remote_ip = remote_ip;
    conn.remote_port = remote_port;
    conn.local_port = next_local_port++;
    conn.send_seq = 1000 + pit_ticks() * 997;
    conn.state = TCP_SYN_SENT;

    for (int attempt = 0; attempt < TCP_MAX_RETRIES; attempt++) {
        tcp_send_segment(TCP_FLAG_SYN, NULL, 0);
        uint32_t start = pit_ticks();
        while (conn.state == TCP_SYN_SENT && (pit_ticks() - start) < TCP_RETRY_TICKS) {
            __asm__ volatile ("hlt");
        }
        if (conn.state == TCP_ESTABLISHED) {
            serial_printf("tcp: connected to %u.%u.%u.%u:%u\n",
                          (remote_ip >> 24) & 0xFF, (remote_ip >> 16) & 0xFF,
                          (remote_ip >> 8) & 0xFF, remote_ip & 0xFF, remote_port);
            return 1;
        }
        if (conn.reset_received) {
            serial_printf("tcp: connection refused\n");
            return 0;
        }
    }
    serial_printf("tcp: connect timed out\n");
    return 0;
}

int tcp_send(const void *data, uint16_t len) {
    if (conn.state != TCP_ESTABLISHED) return 0;
    const uint8_t *p = (const uint8_t *)data;
    uint16_t remaining = len;
    while (remaining > 0) {
        uint16_t chunk = remaining > TCP_MAX_SEGMENT ? TCP_MAX_SEGMENT : remaining;
        if (!tcp_send_and_wait(TCP_FLAG_ACK | TCP_FLAG_PSH, p, chunk)) return 0;
        p += chunk;
        remaining = (uint16_t)(remaining - chunk);
    }
    return 1;
}

int tcp_recv(void *buf, uint16_t max_len) {
    if (conn.recv_len == 0) {
        return (conn.fin_received || conn.state == TCP_CLOSED) ? -1 : 0;
    }
    uint32_t copy_len = conn.recv_len < max_len ? conn.recv_len : max_len;
    memcpy(buf, conn.recv_buf, copy_len);
    memmove(conn.recv_buf, conn.recv_buf + copy_len, conn.recv_len - copy_len);
    conn.recv_len -= copy_len;
    return (int)copy_len;
}

void tcp_close(void) {
    if (conn.state == TCP_ESTABLISHED) {
        conn.state = conn.fin_received ? TCP_LAST_ACK : TCP_FIN_WAIT1;
        tcp_send_and_wait(TCP_FLAG_ACK | TCP_FLAG_FIN, NULL, 0);

        uint32_t start = pit_ticks();
        while (conn.state != TCP_CLOSED && (pit_ticks() - start) < 200) {
            __asm__ volatile ("hlt");
        }
    }
    conn.state = TCP_CLOSED;
}

void tcp_handle_packet(const uint8_t *data, uint16_t len, uint32_t src_ip) {
    if (conn.state == TCP_CLOSED) return;
    if (len < sizeof(struct tcp_header)) return;
    const struct tcp_header *hdr = (const struct tcp_header *)data;

    if (src_ip != conn.remote_ip) return;
    if (net_ntohs(hdr->src_port) != conn.remote_port) return;
    if (net_ntohs(hdr->dst_port) != conn.local_port) return;

    uint8_t header_len = (uint8_t)(((hdr->data_offset >> 4) & 0x0F) * 4);
    if (header_len < sizeof(struct tcp_header) || header_len > len) return;
    const uint8_t *payload = data + header_len;
    uint16_t payload_len = (uint16_t)(len - header_len);

    uint32_t seq = net_ntohl(hdr->seq);
    uint32_t ack = net_ntohl(hdr->ack);

    if (hdr->flags & TCP_FLAG_RST) {
        conn.state = TCP_CLOSED;
        conn.reset_received = 1;
        return;
    }

    /* Clearing a pending "waiting for this ACK" is the same regardless
     * of which state we're in (established data, or the FIN handshake). */
    if ((hdr->flags & TCP_FLAG_ACK) && conn.ack_pending &&
        (int32_t)(ack - conn.wait_ack_target) >= 0) {
        conn.ack_pending = 0;
    }

    switch (conn.state) {
        case TCP_SYN_SENT:
            if ((hdr->flags & TCP_FLAG_SYN) && (hdr->flags & TCP_FLAG_ACK)) {
                conn.recv_seq = seq + 1;
                conn.send_seq += 1; /* our SYN consumed one sequence number */
                conn.state = TCP_ESTABLISHED;
                tcp_send_segment(TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_ESTABLISHED:
            if (payload_len > 0 && seq == conn.recv_seq) {
                uint32_t space = TCP_RECV_BUF_SIZE - conn.recv_len;
                uint32_t copy_len = payload_len < space ? payload_len : space;
                memcpy(conn.recv_buf + conn.recv_len, payload, copy_len);
                conn.recv_len += copy_len;
                conn.recv_seq += payload_len;
                tcp_send_segment(TCP_FLAG_ACK, NULL, 0);
            }
            if (hdr->flags & TCP_FLAG_FIN) {
                conn.recv_seq = seq + payload_len + 1;
                conn.fin_received = 1;
                tcp_send_segment(TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_FIN_WAIT1:
            if (!conn.ack_pending) conn.state = TCP_FIN_WAIT2;
            if (hdr->flags & TCP_FLAG_FIN) {
                conn.recv_seq = seq + 1;
                tcp_send_segment(TCP_FLAG_ACK, NULL, 0);
                conn.state = TCP_CLOSED;
            }
            break;

        case TCP_FIN_WAIT2:
            if (hdr->flags & TCP_FLAG_FIN) {
                conn.recv_seq = seq + 1;
                tcp_send_segment(TCP_FLAG_ACK, NULL, 0);
                conn.state = TCP_CLOSED;
            }
            break;

        case TCP_LAST_ACK:
            if (!conn.ack_pending) conn.state = TCP_CLOSED;
            break;

        default:
            break;
    }
}
