#include <kernel/ping_task.h>
#include <kernel/pit.h>
#include <kernel/serial.h>
#include <net/net.h>
#include <net/arp.h>
#include <net/icmp.h>

#define PING_ID 0xBEEF

/* Runs as a regular kernel task under the scheduler: resolves the gateway
 * via ARP (retrying every 500ms), then pings it once a second forever. */
void ping_task_entry(void) {
    uint32_t gateway = net_get_gateway_ip();
    uint8_t mac[6];

    while (!arp_resolve(gateway, mac)) {
        arp_send_request(gateway);
        pit_sleep(500);
    }
    serial_printf("ping_task: gateway resolved, starting pings\n");

    uint16_t seq = 0;
    for (;;) {
        icmp_send_echo_request(gateway, PING_ID, seq++);
        pit_sleep(1000);
    }
}
