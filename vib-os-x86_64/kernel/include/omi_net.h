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

/* Per-connection API: each socket owns one of these, so several programs can
 * be talking to different servers at the same time. */
struct tcp_conn;
struct tcp_conn *tcp_conn_open(const net_cfg_t *cfg, const uint8_t ip[4], uint16_t port);
int  tcp_conn_send(struct tcp_conn *c, const void *buf, int len);
int  tcp_conn_recv(struct tcp_conn *c, void *buf, int max, int timeout_ms);
void tcp_conn_close(struct tcp_conn *c);
int  tcp_conn_count(void);

/* Hand an inbound frame to whichever connection owns it. Every place that
 * polls the NIC but does not own a frame must call this instead of dropping
 * it: the driver has one receive queue, so a raw waiter can otherwise eat
 * another program's data. */
void tcp_input(const uint8_t *f, int n);

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