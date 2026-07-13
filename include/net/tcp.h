#ifndef NET_TCP_H
#define NET_TCP_H

#include <stdint.h>

struct tcp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset; /* high nibble: header length in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed));

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

/* Single-connection client-only TCP: good enough for one HTTP request at
 * a time (which is all this OS's browser needs), not a general socket
 * layer. Every call here operates on that one connection. */

/* Blocking active-open (3-way handshake, with retries/timeout). Returns 1
 * on ESTABLISHED, 0 on refusal/timeout. */
int tcp_connect(uint32_t remote_ip, uint16_t remote_port);

/* Blocking send: splits into segments as needed, waits for each to be
 * ACKed (with retransmit) before sending the next. Returns 1 on success. */
int tcp_send(const void *data, uint16_t len);

/* Non-blocking: copies up to max_len buffered bytes into buf. Returns the
 * number of bytes copied (0 if none available yet), or -1 once the
 * remote has sent FIN and the buffer is drained (i.e. "no more data,
 * ever"). */
int tcp_recv(void *buf, uint16_t max_len);

/* Graceful active close (sends FIN, waits for the final ACK/FIN). */
void tcp_close(void);

void tcp_handle_packet(const uint8_t *data, uint16_t len, uint32_t src_ip);

#endif
