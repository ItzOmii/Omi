/* See kernel/include/unixpair.h. Two pipes, two ends, one shared pair object. */

#include "../include/fd.h"      /* fd_drop_ofile: the queue owns references */
#include "../include/unixpair.h"
#include "../include/pipe.h"
#include "../include/errno.h"
#include "../include/kmalloc.h"
#include "../include/types.h"

/* Descriptors in flight. A pipe carries bytes and nothing else, so the
 * descriptors a sendmsg hands over travel in a small queue beside it: one entry
 * per sendmsg, drained one per recvmsg. Eight messages deep is enough for a
 * handshake exchange, and a program that runs past it is told -ENOBUFS rather
 * than having its descriptors dropped. */
#define UNIX_CTL_SLOTS 8
#define UNIX_CTL_FDS   8

typedef struct {
  int   nfds;
  void *fds[UNIX_CTL_FDS];      /* ofile_t*, opaque here: fd.c owns that type */
} unix_ctlmsg_t;

typedef struct {
  unix_ctlmsg_t slot[UNIX_CTL_SLOTS];
  int head, tail;
} unix_ctlq_t;

typedef struct {
  struct pipe *ab;      /* A writes, B reads */
  struct pipe *ba;      /* B writes, A reads */
  int          refs;    /* endpoints still holding it */
  unix_ctlq_t  to_a;    /* descriptors B has sent to A */
  unix_ctlq_t  to_b;    /* descriptors A has sent to B */
} unixpair_t;

typedef struct unix_end_s {
  unixpair_t *pair;
  int         side;     /* 0 = A, 1 = B */
  struct unix_end_s *peer;
  int         wr_closed;
  /* The process on the other side, captured when the connection was made. Linux
   * reports the creator of a socketpair and the client of a named socket, and so
   * does this: credentials are a fact about the connection, not a live query. */
  int         peer_pid, peer_uid, peer_gid;
  void      (*notify)(void *, unsigned);
  void       *notify_ctx;
} unix_end_t;

static void end_wake(unix_end_t *e, unsigned mask) {
  if (e && e->notify) e->notify(e->notify_ctx, mask);
}

static struct pipe *rd_pipe(unix_end_t *e) {
  return e->side == 0 ? e->pair->ba : e->pair->ab;
}
static struct pipe *wr_pipe(unix_end_t *e) {
  return e->side == 0 ? e->pair->ab : e->pair->ba;
}

static unix_ctlq_t *ctl_out(unixpair_t *p, int side) {
  return side == 0 ? &p->to_b : &p->to_a;    /* what this end sends */
}
static unix_ctlq_t *ctl_in(unixpair_t *p, int side) {
  return side == 0 ? &p->to_a : &p->to_b;    /* what this end receives */
}

static int ctl_push(unix_ctlq_t *q, void **ofiles, int n) {
  if (n <= 0 || n > UNIX_CTL_FDS) return -EINVAL;
  int next = (q->tail + 1) % UNIX_CTL_SLOTS;
  if (next == q->head) return -ENOBUFS;
  unix_ctlmsg_t *m = &q->slot[q->tail];
  m->nfds = n;
  for (int i = 0; i < n; i++) m->fds[i] = ofiles[i];
  q->tail = next;
  return 0;
}

/* Pop one message. A message that does not fit is dropped whole and its
 * descriptors released - that is what MSG_CTRUNC means, and keeping them would
 * leak them into a process that was never told their numbers. */
static int ctl_pop(unix_ctlq_t *q, void **out, int max, int *truncated) {
  *truncated = 0;
  if (q->head == q->tail) return 0;
  unix_ctlmsg_t *m = &q->slot[q->head];
  int n = m->nfds;
  if (n <= max) {
    for (int i = 0; i < n; i++) out[i] = m->fds[i];
  } else {
    for (int i = 0; i < n; i++) fd_drop_ofile((ofile_t *)m->fds[i]);
    *truncated = 1;
    n = 0;
  }
  m->nfds = 0;
  q->head = (q->head + 1) % UNIX_CTL_SLOTS;
  return n;
}

/* Undo the message at the tail: the bytes it rode on were never written. */
static int ctl_unpush(unix_ctlq_t *q) {
  if (q->head == q->tail) return 0;
  int last = (q->tail + UNIX_CTL_SLOTS - 1) % UNIX_CTL_SLOTS;
  unix_ctlmsg_t *m = &q->slot[last];
  for (int i = 0; i < m->nfds; i++) fd_drop_ofile((ofile_t *)m->fds[i]);
  m->nfds = 0;
  q->tail = last;
  return 0;
}

static void ctl_drain(unix_ctlq_t *q) {
  while (q->head != q->tail) {
    unix_ctlmsg_t *m = &q->slot[q->head];
    for (int i = 0; i < m->nfds; i++) fd_drop_ofile((ofile_t *)m->fds[i]);
    m->nfds = 0;
    q->head = (q->head + 1) % UNIX_CTL_SLOTS;
  }
}

int sock_unix_new_pair(void **out_a, void **out_b) {
  if (!out_a || !out_b) return -EINVAL;
  unixpair_t *p = (unixpair_t *)kzalloc(sizeof *p);
  if (!p) return -ENOMEM;
  unix_end_t *a = (unix_end_t *)kzalloc(sizeof *a);
  unix_end_t *b = (unix_end_t *)kzalloc(sizeof *b);
  if (!a || !b) { if (a) kfree(a); if (b) kfree(b); kfree(p); return -ENOMEM; }

  p->ab = pipe_new();
  p->ba = pipe_new();
  if (!p->ab || !p->ba) {
    /* A pipe slot is the scarce thing here, and running out of one must not
     * leave half a pair behind: release whichever arrived. */
    if (p->ab) { pipe_unref(p->ab, 0); pipe_unref(p->ab, 1); }
    if (p->ba) { pipe_unref(p->ba, 0); pipe_unref(p->ba, 1); }
    kfree(a); kfree(b); kfree(p);
    return -ENFILE;
  }
  p->refs = 2;

  a->pair = p; a->side = 0; a->peer = b;
  b->pair = p; b->side = 1; b->peer = a;
  *out_a = a;
  *out_b = b;
  return 0;
}

/* A pair is never connected, so there is no open() to implement: NULL is the
 * socket layer's "cannot be connected this way", which is the truth. */
static void *u_open(const net_cfg_t *cfg, const uint8_t ip[4], uint16_t port) {
  (void)cfg; (void)ip; (void)port;
  return 0;
}

static int u_send(void *h, const void *buf, int len) {
  unix_end_t *e = (unix_end_t *)h;
  if (!e || !e->pair) return -1;
  if (e->wr_closed) return -1;
  long r = pipe_write(wr_pipe(e), buf, (uint64_t)len, 0);
  if (r > 0) end_wake(e->peer, OMI_POLLIN);
  if (r == -EAGAIN) return 0;        /* full: the socket layer waits for POLLOUT */
  if (r < 0) return -1;              /* the peer's read side is gone: EPIPE */
  return (int)r;
}

static int u_recv(void *h, void *buf, int max, int timeout_ms) {
  (void)timeout_ms;                  /* non-blocking only: the caller waits */
  unix_end_t *e = (unix_end_t *)h;
  if (!e || !e->pair) return -1;
  long got = pipe_read(rd_pipe(e), buf, (uint64_t)max, 0);
  if (got > 0) { end_wake(e->peer, OMI_POLLOUT); return (int)got; }
  if (got == 0) return -1;           /* EOF: no writer will ever come back */
  return 0;                          /* nothing yet */
}

static unsigned u_poll(void *h) {
  unix_end_t *e = (unix_end_t *)h;
  if (!e || !e->pair) return OMI_POLLERR;
  unsigned m = 0;
  m |= pipe_poll(rd_pipe(e), 0);
  m |= pipe_poll(wr_pipe(e), 1);
  return m;
}

static void u_shutdown(void *h) {
  unix_end_t *e = (unix_end_t *)h;
  if (!e || !e->pair || e->wr_closed) return;
  e->wr_closed = 1;
  pipe_unref(wr_pipe(e), 1);         /* the peer reads EOF from here */
  end_wake(e->peer, OMI_POLLIN | OMI_POLLHUP);
}

static void u_close(void *h) {
  unix_end_t *e = (unix_end_t *)h;
  if (!e) return;
  unix_end_t *peer = e->peer;
  unixpair_t *p = e->pair;
  if (p) {
    if (!e->wr_closed) { e->wr_closed = 1; pipe_unref(wr_pipe(e), 1); }
    pipe_unref(rd_pipe(e), 0);
    /* Descriptors queued for this end and never taken belong to nobody now. */
    ctl_drain(ctl_in(p, e->side));
    if (p->refs > 0) p->refs--;
    if (p->refs == 0) kfree(p);
  }
  /* The peer must learn about this before the end it points at is gone. */
  if (peer) {
    peer->peer = 0;
    end_wake(peer, OMI_POLLIN | OMI_POLLHUP);
  }
  kfree(e);
}

void u_set_peer_cred(void *h, int pid, int uid, int gid) {
  unix_end_t *e = (unix_end_t *)h;
  if (!e) return;
  e->peer_pid = pid; e->peer_uid = uid; e->peer_gid = gid;
}

static int u_peer_cred(void *h, int *pid, int *uid, int *gid) {
  unix_end_t *e = (unix_end_t *)h;
  if (!e || !e->pair) return -ENOTCONN;
  if (pid) *pid = e->peer_pid;
  if (uid) *uid = e->peer_uid;
  if (gid) *gid = e->peer_gid;
  return 0;
}

static void u_bind(void *h, void (*cb)(void *, unsigned), void *ctx) {
  unix_end_t *e = (unix_end_t *)h;
  if (!e) return;
  e->notify = cb;
  e->notify_ctx = ctx;
}

static int u_send_ctl(void *h, void **ofiles, int n) {
  unix_end_t *e = (unix_end_t *)h;
  if (!e || !e->pair) return -ENOTCONN;
  return ctl_push(ctl_out(e->pair, e->side), ofiles, n);
}

static int u_recv_ctl(void *h, void **out, int max) {
  unix_end_t *e = (unix_end_t *)h;
  if (!e || !e->pair) return 0;
  int trunc = 0;
  int n = ctl_pop(ctl_in(e->pair, e->side), out, max, &trunc);
  /* -E2BIG is this layer's way of saying "taken, but it did not fit": the
   * descriptors are already released and the caller sets MSG_CTRUNC. */
  return trunc ? -E2BIG : n;
}

static int u_cancel_ctl(void *h) {
  unix_end_t *e = (unix_end_t *)h;
  if (!e || !e->pair) return -ENOTCONN;
  return ctl_unpush(ctl_out(e->pair, e->side));
}

static const struct socket_ops unix_ops = {
  "unixpair", u_open, u_send, u_recv, u_close, u_shutdown, u_poll, u_bind,
  u_send_ctl, u_recv_ctl, u_cancel_ctl, u_peer_cred
};

const struct socket_ops *sock_unix_ops(void) { return &unix_ops; }
