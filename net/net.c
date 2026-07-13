#include <net/net.h>
#include <net/ethernet.h>
#include <drivers/rtl8139.h>
#include <kernel/serial.h>

/* Static config matching QEMU's default user-mode (SLIRP) network:
 * guest 10.0.2.15, gateway/host 10.0.2.2. No DHCP client yet. */
static uint32_t our_ip = 0;
static uint32_t gateway_ip = 0;
static uint8_t our_mac[6];
static int is_up = 0;

int net_init(void) {
    if (!rtl8139_init()) return 0;

    rtl8139_get_mac(our_mac);
    our_ip = ip_make(10, 0, 2, 15);
    gateway_ip = ip_make(10, 0, 2, 2);

    rtl8139_set_rx_handler(eth_handle_frame);
    is_up = 1;

    serial_printf("net: up, ip=%d.%d.%d.%d gateway=%d.%d.%d.%d\n",
                  (our_ip >> 24) & 0xFF, (our_ip >> 16) & 0xFF, (our_ip >> 8) & 0xFF, our_ip & 0xFF,
                  (gateway_ip >> 24) & 0xFF, (gateway_ip >> 16) & 0xFF, (gateway_ip >> 8) & 0xFF, gateway_ip & 0xFF);
    return 1;
}

int net_is_up(void) { return is_up; }

const uint8_t *net_get_mac(void) { return our_mac; }
uint32_t net_get_ip(void) { return our_ip; }
uint32_t net_get_gateway_ip(void) { return gateway_ip; }

uint16_t net_checksum(const void *data, int len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += ((uint32_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint32_t)bytes[0] << 8;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}
