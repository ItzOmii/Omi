/* AF_UNIX socketpair: two endpoints in one process, full duplex, no network.
 *
 * This is the shape a multi-process program talks over: a browser hands a
 * renderer one end of a pair and keeps the other, and every message after that
 * is read() and write() on a descriptor. There is no address, no namespace and
 * nothing to bind - which is why it is the one socket family that does not need
 * a network stack to be useful.
 *
 * The transport is two pipes, because a pipe already has exactly the semantics a
 * stream endpoint needs and already had every edge case worked out (empty with a
 * writer alive waits, empty with no writers is EOF, write with no readers is
 * EPIPE, full waits). A unix pair is therefore not a second byte-buffer
 * implementation to keep correct, only a wiring of two of them.
 */
#ifndef OMI_UNIXPAIR_H
#define OMI_UNIXPAIR_H

#include "net_backend.h"

/* Both ends of a new pair. -> 0, or -ENOMEM / -ENFILE when the pipe pool is full. */
int sock_unix_new_pair(void **out_a, void **out_b);

/* Record who is on the other end of `h` (an endpoint from the pair above). The
 * socket layer calls this when the connection is made; there is no live query
 * because a peer's identity is a fact about the connection, not about now. */
void u_set_peer_cred(void *h, int pid, int uid, int gid);

/* The ops table a socket object runs on when its endpoint is a pair. */
const struct socket_ops *sock_unix_ops(void);

#endif
