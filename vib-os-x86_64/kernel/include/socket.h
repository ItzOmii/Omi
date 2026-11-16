/*
 * The socket object - the kernel's side of one network endpoint.
 *
 * fd.h knows this type only as an opaque pointer, and this header knows nothing
 * about TCP: the transport is declared in terms of a void* handle that only the
 * selected backend interprets. That is the boundary. The VFS owns descriptors,
 * this object owns "an endpoint and its readiness", and the backend underneath
 * owns packets - today the kernel's TCP, later possibly a service over IPC.
 *
 * Lifetime is deliberately NOT the backend's problem. Open file descriptions are
 * reference counted, dup() shares one description, and the final close calls
 * sock_close() exactly once; the backend's close op is transport teardown, not
 * object destruction. Waiter state belongs here, too, when poll() arrives,
 * because this object outlives any single descriptor that refers to it.
 */

#ifndef OMI_SOCKET_H
#define OMI_SOCKET_H

#include "types.h"
#include "net.h"

struct socket;                 /* opaque everywhere above this header */
struct socket_ops;

/* No connection yet - a socket() call has no transport behind it until connect. */
struct socket *sock_new(void);

int  sock_connect(struct socket *s, const net_cfg_t *cfg, const uint8_t ip[4],
                  uint16_t port);
long sock_send(struct socket *s, const void *buf, uint64_t n);
long sock_recv(struct socket *s, void *buf, uint64_t n, int timeout_ms);

/* Readiness of this endpoint, in OMI_POLL* bits. Never blocks. */
unsigned sock_poll(struct socket *s);

/* Called exactly once, by the open-file layer, when the last descriptor goes. */
void sock_close(struct socket *s);

/* Number of socket objects currently allocated (test/observability). */
int sock_live(void);

/* Called after an address space is freed with `had` socket descriptors in it:
 * asserts every endpoint died with the space that owned it. */
void sock_report_after_space(int had);

/* Backend substitution test: runs the descriptor sequence against a
 * deterministic backend, then restores the kernel's own. */
void sock_selftest(void);

#endif /* OMI_SOCKET_H */