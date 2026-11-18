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

  /* Half-close: send our FIN and leave the connection able to receive. NULL
   * means the backend has no half-close, and the socket layer then keeps the
   * socket open but refuses further sends - the peer is told nothing, which is
   * worse, but silently dropping bytes would be too. */
  void (*shutdown)(void *h);

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

  /* Descriptor passing (SCM_RIGHTS). Optional and NULL for a backend with no
   * peer inside this kernel: a stream over the network has no other end's
   * descriptor table to install into, and saying -ENOTSUP is honest where
   * accepting the descriptor and dropping it would not be.
   *
   * send_ctl takes one reference per description on success only; recv_ctl hands
   * back references the caller must install or release. */
  int  (*send_ctl)(void *h, void **ofiles, int n);
  int  (*recv_ctl)(void *h, void **out, int max);
  /* Undo the last send_ctl, for the case where the bytes it rode on never moved.
   * Optional: without it the message stays queued and travels with the next
   * write, which is wrong but not silent. */
  int  (*cancel_ctl)(void *h);
  /* Who is on the other end, if this transport has endpoints that belong to
   * processes at all. NULL means "no such notion here" and becomes -ENOTSUP,
   * which is the honest answer for TCP: its peer is an address, not a process. */
  int  (*peer_cred)(void *h, int *pid, int *uid, int *gid);
};

/* The backend in use. Never NULL once the kernel is up. */
const struct socket_ops *socket_backend(void);

/* Swap the backend. Called once today, with the kernel's own TCP; exists so a
 * userspace-service backend can be selected without touching a single caller. */
void socket_backend_select(const struct socket_ops *ops);

#endif /* OMI_NET_BACKEND_H */