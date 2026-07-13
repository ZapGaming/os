#include <net/dns.h>
#include <net/udp.h>
#include <net/net.h>
#include <kernel/pit.h>
#include <kernel/serial.h>
#include <string.h>

#define LOCAL_DNS_PORT 53000
#define DNS_SERVER_PORT 53
#define DNS_TIMEOUT_TICKS 300 /* 3s at 100Hz */

static uint16_t next_query_id = 1;
static uint16_t pending_id = 0;
static uint32_t resolved_ip = 0;
static int resolved_state = 0; /* 0=pending, 1=success, 2=failed */

static int parse_dotted_quad(const char *s, uint32_t *out_ip) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int part = 0, digits = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            parts[part] = parts[part] * 10 + (uint32_t)(*p - '0');
            if (parts[part] > 255) return 0;
            digits++;
        } else if (*p == '.') {
            if (digits == 0 || part >= 3) return 0;
            part++; digits = 0;
        } else if (*p == '\0') {
            if (digits == 0 || part != 3) return 0;
            *out_ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
            return 1;
        } else {
            return 0;
        }
    }
}

static int encode_name(const char *host, uint8_t *out) {
    int i = 0, oi = 0;
    while (host[i]) {
        int start = i;
        while (host[i] && host[i] != '.') i++;
        int label_len = i - start;
        out[oi++] = (uint8_t)label_len;
        memcpy(out + oi, host + start, (size_t)label_len);
        oi += label_len;
        if (host[i] == '.') i++;
    }
    out[oi++] = 0;
    return oi;
}

static uint16_t dns_skip_name(const uint8_t *data, uint16_t len, uint16_t pos) {
    while (pos < len) {
        uint8_t label_len = data[pos];
        if (label_len == 0) { pos++; break; }
        if ((label_len & 0xC0) == 0xC0) { pos += 2; break; }
        pos += (uint16_t)(label_len + 1);
    }
    return pos;
}

static void dns_response_handler(uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint16_t len) {
    (void)src_ip; (void)src_port;
    if (resolved_state != 0) return;
    if (len < 12) return;

    uint16_t id = (uint16_t)((data[0] << 8) | data[1]);
    if (id != pending_id) return;

    uint16_t flags = (uint16_t)((data[2] << 8) | data[3]);
    uint16_t qdcount = (uint16_t)((data[4] << 8) | data[5]);
    uint16_t ancount = (uint16_t)((data[6] << 8) | data[7]);
    if (!(flags & 0x8000)) return;          /* not a response */
    if ((flags & 0x000F) != 0) { resolved_state = 2; return; } /* rcode != 0 */

    uint16_t pos = 12;
    for (int q = 0; q < qdcount; q++) {
        pos = dns_skip_name(data, len, pos);
        pos += 4; /* qtype + qclass */
    }

    for (int a = 0; a < ancount && pos < len; a++) {
        pos = dns_skip_name(data, len, pos);
        if ((uint16_t)(pos + 10) > len) break;
        uint16_t rtype = (uint16_t)((data[pos] << 8) | data[pos + 1]);
        uint16_t rdlength = (uint16_t)((data[pos + 8] << 8) | data[pos + 9]);
        pos += 10;
        if (rtype == 1 && rdlength == 4 && (uint16_t)(pos + 4) <= len) {
            resolved_ip = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos + 1] << 16)
                        | ((uint32_t)data[pos + 2] << 8) | data[pos + 3];
            resolved_state = 1;
            return;
        }
        pos += rdlength;
    }
    if (resolved_state == 0) resolved_state = 2;
}

int dns_resolve(const char *hostname, uint32_t *out_ip) {
    if (parse_dotted_quad(hostname, out_ip)) return 1;

    uint8_t packet[300];
    uint16_t id = next_query_id++;
    packet[0] = (uint8_t)(id >> 8); packet[1] = (uint8_t)(id & 0xFF);
    packet[2] = 0x01; packet[3] = 0x00; /* RD=1 (recursion desired) */
    packet[4] = 0; packet[5] = 1;       /* QDCOUNT=1 */
    packet[6] = 0; packet[7] = 0;
    packet[8] = 0; packet[9] = 0;
    packet[10] = 0; packet[11] = 0;

    int pos = 12 + encode_name(hostname, packet + 12);
    packet[pos++] = 0; packet[pos++] = 1; /* QTYPE=A */
    packet[pos++] = 0; packet[pos++] = 1; /* QCLASS=IN */

    pending_id = id;
    resolved_state = 0;
    resolved_ip = 0;

    udp_register_handler(LOCAL_DNS_PORT, dns_response_handler);

    uint32_t dns_server = net_get_gateway_ip() & 0xFFFFFF00; /* SLIRP's DNS proxy is x.x.x.3 */
    dns_server |= 3;
    if (!udp_send(dns_server, LOCAL_DNS_PORT, DNS_SERVER_PORT, packet, (uint16_t)pos)) {
        udp_unregister_handler(LOCAL_DNS_PORT);
        return 0;
    }

    uint32_t start = pit_ticks();
    while (resolved_state == 0 && (pit_ticks() - start) < DNS_TIMEOUT_TICKS) {
        __asm__ volatile ("hlt");
    }
    udp_unregister_handler(LOCAL_DNS_PORT);

    if (resolved_state == 1) {
        serial_printf("dns: %s -> %u.%u.%u.%u\n", hostname,
                      (resolved_ip >> 24) & 0xFF, (resolved_ip >> 16) & 0xFF,
                      (resolved_ip >> 8) & 0xFF, resolved_ip & 0xFF);
        *out_ip = resolved_ip;
        return 1;
    }
    serial_printf("dns: %s resolution failed (state=%d)\n", hostname, resolved_state);
    return 0;
}
