#ifndef NET_NET_H
#define NET_NET_H

#include <stdint.h>

static inline uint16_t net_htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t net_htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}
#define net_ntohs net_htons
#define net_ntohl net_htonl

static inline uint32_t ip_make(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

/* Returns 1 once the NIC was found and brought up, 0 otherwise. Uses a
 * static IP configuration matching QEMU's user-mode (SLIRP) networking
 * defaults, so `make run`'s default NIC setup just works without DHCP. */
int net_init(void);
int net_is_up(void);

const uint8_t *net_get_mac(void);
uint32_t net_get_ip(void);
uint32_t net_get_gateway_ip(void);
uint32_t net_get_netmask(void);
int net_is_local(uint32_t ip);

/* Name of whichever NIC driver net_init() actually brought up (e.g.
 * "rtl8139", "e1000"), for display -- empty string if none. */
const char *net_get_driver_name(void);

/* Hands a fully-built Ethernet frame to whichever NIC driver is active.
 * eth_send() is the only caller; a NIC driver is never referenced by
 * name outside net_init() itself. */
void net_send_frame(const void *data, uint16_t len);

/* RFC 1071 internet checksum over `len` bytes (odd trailing byte handled). */
uint16_t net_checksum(const void *data, int len);

#endif
