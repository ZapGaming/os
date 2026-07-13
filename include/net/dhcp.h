#ifndef NET_DHCP_H
#define NET_DHCP_H

/* Runs a real DHCP handshake (DISCOVER -> OFFER -> REQUEST -> ACK,
 * RFC 2131) over raw broadcast Ethernet frames -- bypassing ip_send()
 * entirely, since that assumes an IP/gateway we don't have yet and
 * ARP-resolves the destination, neither of which apply to a broadcast
 * sent before any lease exists. Must be called with interrupts already
 * enabled (it blocks on incoming replies the same way dns_resolve()
 * does) and NIC hardware already up. On success, overwrites the
 * fallback static config via net_set_ip_config()/net_set_dns_server().
 * On failure/timeout (no DHCP server on the network), silently leaves
 * whatever fallback config net_init() already set -- this is a best-
 * effort upgrade, not a hard requirement to have a working network. */
void net_dhcp_negotiate(void);

#endif
