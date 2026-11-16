/*
 * MeepOS - Intel 8254x (e1000) NIC driver
 *
 * Polling driver: no interrupts, no DMA scatter. One RX ring, one TX ring.
 * Step 1 of MeepNet: get frames onto the wire and back. ARP is the self test
 * because a reply proves both directions and needs no IP stack.
 */

#ifndef _E1000_H
#define _E1000_H

#include "types.h"

#define E1000_MAC_LEN 6

/* Returns 0 on success; fills mac. -1 when no NIC is present. */
int e1000_init(void);

/* 1 when initialised and link is up. */
int e1000_link(void);

/* Send one raw ethernet frame. Returns 0 on success. */
int e1000_send(const void *frame, uint16_t len);

/* Test instrumentation. Forces the outcome of the NEXT send only, and counts
 * every call, so "did the caller retry?" is answerable rather than assumed.
 * One-shot on purpose: a sticky override makes a retry indistinguishable from
 * the first attempt, and the retry is exactly what the -2 policy is about. */
void     e1000_test_send_outcome(int rc);
uint32_t e1000_test_send_calls(void);
void     e1000_test_send_calls_reset(void);

/* Poll for one frame, copied into buf. Returns length, 0 if none, -1 on error. */
int e1000_poll_recv(void *buf, int max);

void e1000_mac(uint8_t out[E1000_MAC_LEN]);

/* Boot self test: ARP for the QEMU slirp gateway. Logs to COM1. */
void e1000_selftest(void);

#endif /* _E1000_H */