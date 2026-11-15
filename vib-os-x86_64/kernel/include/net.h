/*
 * MeepOS - IPv4 configuration and a DHCP client
 *
 * Step 2 of MeepNet. The e1000 driver (step 1) proved ARP and ICMP against a
 * hardcoded 10.0.2.15. This asks the network what address we actually have, so
 * nothing downstream has to assume a slirp default.
 *
 * Scope: IP + mask + router + DNS over one DHCP DISCOVER/REQUEST exchange.
 * No lease renewal, no rebinding - a boot-time configuration only.
 */

#ifndef _NET_H
#define _NET_H

#include "types.h"

typedef struct {
  uint8_t ip[4];
  uint8_t mask[4];
  uint8_t gw[4];
  uint8_t dns[4];
  uint8_t gw_mac[6];  /* resolved by ARP, needed to send anything off-link */
  int valid;          /* 1 once a DHCP ACK landed */
} net_cfg_t;

/* Boot-time configuration. Returns 0 and fills cfg on success. */
int dhcp_configure(net_cfg_t *cfg);

/* The live configuration (all zeros until dhcp_configure succeeds). */
const net_cfg_t *net_get(void);

/* Resolve an on-link IPv4 address to a MAC via ARP. Returns 1 on success. */
int net_arp_resolve(const net_cfg_t *cfg, const uint8_t ip[4], uint8_t mac_out[6]);

/* One UDP datagram to dst_ip. Returns 1 when the frame was sent. */
int net_udp_send(const net_cfg_t *cfg, const uint8_t dst_ip[4], uint16_t sport,
                 uint16_t dport, const void *payload, int len);

/* One ICMP echo. Returns 1 when the reply came back. */
int net_ping(const net_cfg_t *cfg, const uint8_t dst_ip[4], uint16_t seq);

/* Boot self test: DHCP, then ARP and ping of the router. Logs to COM1. */
void net_selftest(void);

/* IPv4 header + UDP checksum helper, shared by DHCP/ICMP/TCP. */
uint16_t net_csum(const void *data, int len);

#endif /* _NET_H */