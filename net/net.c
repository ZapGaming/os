#include <net/net.h>
#include <net/ethernet.h>
#include <drivers/rtl8139.h>
#include <drivers/e1000.h>
#include <kernel/serial.h>
#include <stddef.h>

/* Static config matching QEMU's default user-mode (SLIRP) network:
 * guest 10.0.2.15, gateway/host 10.0.2.2. No DHCP client yet. */
static uint32_t our_ip = 0;
static uint32_t gateway_ip = 0;
static uint32_t netmask = 0;
static uint8_t our_mac[6];
static int is_up = 0;
static const char *driver_name = "";
static void (*active_send)(const void *data, uint16_t len) = NULL;

/* Tries every NIC driver this kernel has, in order, and uses whichever
 * one actually finds hardware -- rtl8139 is QEMU's long-standing
 * default NIC, e1000 covers VMware's virtual NICs (and QEMU's own
 * "-device e1000"), so between the two, most VM setups this project
 * gets tested under find *something*. */
int net_init(void) {
    if (rtl8139_init()) {
        rtl8139_get_mac(our_mac);
        rtl8139_set_rx_handler(eth_handle_frame);
        active_send = rtl8139_send;
        driver_name = "rtl8139";
    } else if (e1000_init()) {
        e1000_get_mac(our_mac);
        e1000_set_rx_handler(eth_handle_frame);
        active_send = e1000_send;
        driver_name = "e1000";
    } else {
        return 0;
    }

    our_ip = ip_make(10, 0, 2, 15);
    gateway_ip = ip_make(10, 0, 2, 2);
    netmask = ip_make(255, 255, 255, 0);
    is_up = 1;

    serial_printf("net: up (%s), ip=%d.%d.%d.%d gateway=%d.%d.%d.%d\n", driver_name,
                  (our_ip >> 24) & 0xFF, (our_ip >> 16) & 0xFF, (our_ip >> 8) & 0xFF, our_ip & 0xFF,
                  (gateway_ip >> 24) & 0xFF, (gateway_ip >> 16) & 0xFF, (gateway_ip >> 8) & 0xFF, gateway_ip & 0xFF);
    return 1;
}

int net_is_up(void) { return is_up; }

const uint8_t *net_get_mac(void) { return our_mac; }
uint32_t net_get_ip(void) { return our_ip; }
uint32_t net_get_gateway_ip(void) { return gateway_ip; }
uint32_t net_get_netmask(void) { return netmask; }
const char *net_get_driver_name(void) { return driver_name; }

void net_send_frame(const void *data, uint16_t len) {
    if (active_send) active_send(data, len);
}

int net_is_local(uint32_t ip) {
    return (ip & netmask) == (our_ip & netmask);
}

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
