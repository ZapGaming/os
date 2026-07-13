#include <kernel/ping_task.h>
#include <kernel/pit.h>
#include <net/net.h>
#include <net/icmp.h>

#define PING_ID 0xBEEF

/* Runs as a regular kernel task under the scheduler: pings the gateway
 * once a second forever. ip_send() resolves ARP as needed. Reads the
 * gateway fresh every iteration rather than caching it once -- DHCP
 * negotiation (see net/dhcp.c) can still be updating it for the first
 * second or two after this task starts. */
void ping_task_entry(void) {
    uint16_t seq = 0;
    for (;;) {
        icmp_send_echo_request(net_get_gateway_ip(), PING_ID, seq++);
        pit_sleep(1000);
    }
}
