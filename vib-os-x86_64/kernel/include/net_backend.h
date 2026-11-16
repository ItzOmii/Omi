/*
 * The socket backend boundary.
 *
 * Above this line: descriptors, byte streams, dup(), close(), and later poll().
 * Below it: packets, connections, sequence numbers, retransmission. The boundary
 * exists so the layer below can move into a userspace network service later
 * without the VFS, the descriptor semantics, or a browser noticing - and so the
 * socket ABI Chromium will eventually call does not become a synonym for "the
 * kernel's TCP implementation".
 *
 * Today exactly one backend is compiled in. A future one answers the same calls
 * by sending messages over an IPC channel to a network service process, and the
 * kernel keeps only what it needs to notify waiters, cancel, and get descriptor
 * lifetime right. Nothing above this header changes when that happens.
 */

#ifndef OMI_NET_BACKEND_H
#define OMI_NET_BACKEND_H

#include "types.h"
#include "net.h"
#include "readiness.h"

/* Readiness bits come from readiness.h: shared with the VFS without dragging
 * this header into it. */

/* Handles are void* on purpose: the boundary must not name a transport type, or
 * it becomes a TCP header by another route. Only the backend knows what its own
 * handle means. */
struct socket_ops {
  const char *name;

  /* Open a connection to ip:port. NULL means refused or timed out. */
  void *(*open)(const net_cfg_t *cfg, const uint8_t ip[4], uint16_t port);

  int  (*send)(void *h, const void *buf, int len);
  int  (*recv)(void *h, void *buf, int max, int timeout_ms);
  void (*close)(void *h);

  /* Readiness of this endpoint, computed and returned - never blocks. This is
   * the shape poll() needs: the same call answers "is there data", "is there
   * room", "did it hang up" and "did it fail", and a backend that answers by IPC
   * can answer it from cached endpoint state without a round trip. */
  unsigned (*poll)(void *h);

  /* Tell the backend where to send readiness news. The backend calls this
   * callback whenever its endpoint state changes; it never learns what the
   * ctx is, and the layer above never learns what the handle is. Without this,
   * a waiter can only be woken by its own deadline - which is not a wake. */
  void (*bind)(void *h, void (*cb)(void *, unsigned), void *ctx);
};

/* The backend in use. Never NULL once the kernel is up. */
const struct socket_ops *socket_backend(void);

/* Swap the backend. Called once today, with the kernel's own TCP; exists so a
 * userspace-service backend can be selected without touching a single caller. */
void socket_backend_select(const struct socket_ops *ops);

#endif /* OMI_NET_BACKEND_H */