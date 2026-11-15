/*
 * Omi - TCP client, DNS resolver and HTTP/1.1 GET
 *
 * The stack below this file does DHCP + ARP + ICMP. Everything above a
 * browser needs streams, so this adds the smallest TCP that can honestly
 * carry an HTTP request: one connection at a time, blocking, no options
 * beyond MSS, no window scaling, no SACK. That is enough for a GET on a
 * local network and for QEMU's slirp, and small enough to read.
 *
 * Deliberate limits: a single in-flight connection, data is ACKed as it
 * arrives but never retransmitted (the caller writes once, and if the
 * segment is lost the request times out instead of hanging forever), and
 * out-of-order segments are dropped rather than queued.
 */

#ifndef _OMI_TCP_H
#define _OMI_TCP_H

#include "types.h"
#include "net.h"

/* Opens a connection. Returns 1 on ESTABLISHED, 0 on timeout/refusal. */
int tcp_connect(const net_cfg_t *cfg, const uint8_t ip[4], uint16_t port);

/* Sends the whole buffer. Returns bytes accepted, or -1. */
int tcp_send(const void *buf, int len);

/* Reads up to `max` bytes. Returns >0 bytes, 0 on timeout, -1 when the peer
   closed the connection. `timeout_ms` bounds the wait. */
int tcp_recv(void *buf, int max, int timeout_ms);

/* Sends FIN. Safe to call when not connected. */
void tcp_close(void);

int tcp_is_open(void);

/* Resolves a hostname to IPv4 through the DHCP-provided resolver.
   Returns 1 on success. */
int net_dns_resolve(const net_cfg_t *cfg, const char *host, uint8_t ip_out[4]);

#endif /* _OMI_TCP_H */