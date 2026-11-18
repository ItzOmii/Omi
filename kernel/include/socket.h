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

/* AF_UNIX by name. bind() claims a path and makes this socket a listener;
 * listen() starts taking connections; accept() hands back one waiting endpoint as
 * a socket object the caller installs in a descriptor; connect() looks a path up
 * and gives this socket the client end. All four keep struct socket private. */
int sock_unix_bind(struct socket *s, const char *path);
int sock_unix_listen(struct socket *s, int backlog);
int sock_unix_accept(struct socket *s, struct socket **out);
int sock_unix_connect(struct socket *s, const char *path);

/* AF_UNIX socketpair: two endpoints wired to each other, ready for read() and
 * write() with no connect and no address. -> 0, or -ENOMEM / -ENFILE when the
 * socket or pipe pool is full (said out loud, never a silent half-pair). */
int sock_pair(struct socket **out_a, struct socket **out_b);

/* Hand open-file descriptions (void* here: fd.h owns that type and cannot be
 * included from this header) to the endpoint's peer. -ENOTSUP when the transport
 * has no peer inside this kernel. On success the transport owns one reference
 * each. */
int sock_send_ctl(struct socket *s, void **ofiles, int n);
/* Take queued descriptions: the count, -E2BIG when a message was dropped because
 * it did not fit (the caller sets MSG_CTRUNC), or 0 when nothing is queued. */
int sock_recv_ctl(struct socket *s, void **out, int max);
/* Take back the last queued control message (its descriptors go with it). */
int sock_cancel_ctl(struct socket *s);

int  sock_connect(struct socket *s, const net_cfg_t *cfg, const uint8_t ip[4],
                  uint16_t port);
long sock_send(struct socket *s, const void *buf, uint64_t n);
long sock_recv(struct socket *s, void *buf, uint64_t n, int timeout_ms);

/* Readiness of this endpoint, in OMI_POLL* bits. Never blocks. */
unsigned sock_poll(struct socket *s);

/* ---- readiness and waiters ---------------------------------------------
 * Level-triggered: a waiter re-checks readiness after every wake, so waking all
 * waiters of one transition is correct - one takes the data, the rest re-block.
 * Waiter slots live in this object because a node on a blocked task's stack
 * would dangle if that task's address space were freed underneath it.
 */
/* Waiter slots per socket. Four was sized for "one reader and a writer", which
 * is true of one thread and false of any program that polls from a helper or has
 * two writers. Eight costs 24 sockets x 8 x 32 B = 6 KB of BSS, and the failure
 * it prevents is not a slow path but a refused wait (-ENOBUFS) for a program
 * that did nothing wrong. Overflow is still explicit: the ninth waiter is told
 * no, counted in stats.overflows, and never left blocked-but-unregistered. */
#define SOCK_WAITERS 8

/* Announce that readiness changed: bump the sequence, then wake waiters. Every
 * transition must call this before the state it reports is observable, so a
 * waiter that snapshotted the sequence can detect a change that landed between
 * its check and its block. */
void sock_notify(struct socket *s, unsigned mask);

/* The last descriptor is gone: no new operations, waiters leave with POLLERR,
 * and the transport stays alive until the last pin is released. Idempotent. */
void sock_shutdown(struct socket *s);

/* shutdown(SHUT_WR) and shutdown(SHUT_RD). sock_shutdown above is the
 * last-descriptor-gone path and closes in both directions at once; these are the
 * half-closes a program asks for, and they are separate because "I have finished
 * sending" is not "I have finished". */
void sock_shutdown_wr(struct socket *s);
void sock_shutdown_rd(struct socket *s);

/* Block until `want` bits are ready or something terminal happens. Returns the
 * readiness bits, -EAGAIN if `deadline_ticks` elapsed with nothing ready, or
 * -ENOBUFS when the waiter table is full - never a silent block. */
int sock_wait(struct socket *s, unsigned want, uint64_t deadline_ticks);
/* One blocking read at a time. 0 = owned by us, -EBUSY = someone else has it,
 * -EBADF = the socket is gone. sock_read_end() on every exit path. */
int  sock_read_begin(struct socket *s);
void sock_read_end(struct socket *s);

int      sock_waiters_live(struct socket *s);
void     sock_drop_waiters(int task_id);   /* task exit: drop its registrations */
uint32_t sock_gen(struct socket *s);

/* Internals the readiness self-test drives directly; not part of the VFS. */
struct sock_stats { uint64_t blocks, wakes, drops, registered, spurious, overflows, busy; };
struct sock_stats sock_stats_get(void);
void     sock_test_skip_recheck(int on);
void     sock_test_inject_after_register(struct socket *s, unsigned mask, int times);
int      sock_waiter_add(struct socket *s);
void     sock_test_plant(struct socket *s, uint32_t gen, int task, uint32_t tgen);
void     sock_waiter_clear(struct socket *s, int slot);

/* Called exactly once, by the open-file layer, when the last descriptor goes. */
void sock_close(struct socket *s);

/* Number of socket objects currently allocated (test/observability). */
int sock_live(void);
/* The process on the other end of an endpoint (SO_PEERCRED). -> 0, or -ENOTSUP
 * when the transport has no peer process. */
int sock_peer_cred(struct socket *s, int *pid, int *uid, int *gid);
/* Record who is on the other end. Takes the socket, not the transport handle:
 * the handle is private to the socket layer, and passing the wrong one writes
 * credentials over the socket's own ops and impl. */
int sock_set_peer_cred(struct socket *s, int pid, int uid, int gid);
/* Who is asking. The ring-3 layer supplies this; there is one uid, so identity
 * is the pid and the uid/gid fields are reported as 0. */
void sock_set_cred_hook(void (*fn)(int *, int *, int *));
void sock_cred_now(int *pid, int *uid, int *gid);

/* Ownership: a socket queued on a listener belongs to the space that will accept
 * it, not the one that dialled - otherwise the dialling process's teardown finds
 * a connection it handed over and calls it a leak. */
void *sock_owner_now(void);
void sock_set_owner(struct socket *s, void *space);

/* Endpoints created by one address space. The space pointer is opaque here. */
int sock_owned_live(void *space);
/* Where new endpoints record their owner; the ring-3 layer supplies this. */
void sock_set_owner_hook(void *(*fn)(void));

/* Called after an address space is freed with `had` socket descriptors in it:
 * asserts every endpoint died with the space that owned it. */
/* had: endpoints the dying space held. before: live count taken immediately
 * before its descriptors were released. Global zero is only the right answer
 * when this space was the last one holding anything; the delta is right always. */
void sock_report_after_space(void *space, int had);

/* Backend substitution test: runs the descriptor sequence against a
 * deterministic backend, then restores the kernel's own. */
void sock_selftest(void);

/* Milestone 1: the wait protocol and lifetime rules, on a fake backend. */
void sock_readiness_selftest(void);

#endif /* OMI_SOCKET_H */