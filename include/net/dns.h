#ifndef NET_DNS_H
#define NET_DNS_H

#include <stdint.h>

/* Blocking (cooperatively -- yields via hlt while waiting, same pattern
 * as pit_sleep) resolve of a hostname to an IPv4 address via UDP/53.
 * Returns 1 and fills *out_ip on success, 0 on timeout/NXDOMAIN. Accepts
 * a dotted-quad string too (short-circuits without a DNS round trip). */
int dns_resolve(const char *hostname, uint32_t *out_ip);

#endif
