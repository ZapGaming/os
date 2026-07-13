#include <kernel/ping_task.h>
#include <kernel/pit.h>
#include <net/net.h>
#include <net/icmp.h>

#define PING_ID 0xBEEF

/* Runs as a regular kernel task under the scheduler: pings the gateway
 * once a second forever. ip_send() resolves ARP as needed. */
void ping_task_entry(void) {
    uint32_t gateway = net_get_gateway_ip();
    uint16_t seq = 0;
    for (;;) {
        icmp_send_echo_request(gateway, PING_ID, seq++);
        pit_sleep(1000);
    }
}
