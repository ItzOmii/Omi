/*
 * The socket object, and the only place the VFS meets a transport backend.
 *
 * This file is the whole extraction: fd.c asks for a socket, a socket asks the
 * selected backend, and the backend answers in void* handles. Nothing here
 * names TCP. Nothing in fd.h includes this file's headers.
 *
 * A static pool, like ofiles and tasks, because an endpoint is a fixed resource
 * and a lifetime-critical path should not be the first place a kernel allocation
 * can fail. SOCKET_MAX is above TCP_MAX_CONN on purpose: a socket that cannot
 * get a transport should fail at connect() with an honest error, not at socket()
 * for a reason the program cannot act on.
 *
 * Readiness lives here rather than on a descriptor, because it belongs to the
 * endpoint: dup() gives one description two descriptors, and a wake must reach
 * the reader whichever descriptor it polls through.
 */

#include "socket.h"
#include "net_backend.h"
#include "readiness.h"
#include "task.h"
#include "errno.h"
#include "gui.h"        /* serial_puts */
#include "unixpair.h"
#include "../include/unixname.h"   /* AF_UNIX socketpair */

/* 24 sockets was sized for "a browser, a few connections and the desktop's own".
 * A socketpair spends two per helper process and a multi-process program makes
 * one per child, so the pool has to be an order of magnitude past that. 96 is
 * ~40 KB of BSS. */
/* 96 sockets, 256 pipes. Raising this to 256 was tried: the socket end held, but
 * the *transmit* path did not - with that many connections live the e1000 ring
 * runs dry during concurrent TLS handshakes ("seg tx failed", "short write"), and
 * the page load fails with no reply. The NIC's descriptor ring is the next wall,
 * and it is a driver fix, not a table size. */
#define SOCKET_MAX 96

struct sock_waiter {
  int      task;      /* task id that registered */
  uint32_t tgen;      /* that task's generation at registration */
  uint32_t gen;       /* socket generation at registration */
  int      used;
};

struct socket {
  const struct socket_ops *ops;   /* the backend table, chosen once */
  void *impl;                     /* backend-owned transport, NULL until connect */
  int   used;
  int   closing;                  /* last descriptor gone: no new work */
  int   shut_wr;                  /* our FIN is out: reads still work */
  int   shut_rd;                  /* the receive side was dropped: reads say EOF */

  /* Bumped on every readiness change, BEFORE anyone is woken. A waiter
   * snapshots this, registers, then compares: a transition that landed in
   * between shows up here, which is what makes "publish then block" safe
   * without an atomic block primitive. */
  uint32_t seq;

  /* Bumped when this pool slot changes identity (release and allocation).
   * sockets[i] is reused, so a registration from the previous occupant must
   * never be woken by, or wake anyone onto, the next one. */
  uint32_t gen;

  /* The address space that created this endpoint, so the teardown audit can ask
   * "did anything belonging to that space survive it" without comparing global
   * counts - which is wrong the moment a second process connects while the first
   * one exits. */
  void *owner;

  struct sock_waiter waiters[SOCK_WAITERS];

  /* Exactly one blocking read may be in flight on a socket. Two would both park,
   * both wake on the same byte count, and split a byte stream between them in
   * whatever order the scheduler happened to pick - which is not a race a caller
   * can reason about.
   *
   * Owner, not a bare flag: a task can be killed while parked inside the read,
   * and a flag left set would make the socket unreadable for ever with nothing
   * to explain it. The task-exit hook already sweeps this socket's waiters, so
   * it clears the owner in the same pass. */
  int      read_owner;            /* task id, 0 = free */
};

static struct socket sockets[SOCKET_MAX];

/* Who is creating endpoints right now. Set by the ring-3 layer, which is the only
 * thing that knows what an address space is; NULL means "not owned by anyone",
 * which is what kernel-internal sockets get. */
static void *(*sock_owner_hook)(void);

void sock_set_owner_hook(void *(*fn)(void)) { sock_owner_hook = fn; }

/* Who is asking right now. Separate from the owner hook because it is asked at a
 * different moment for a different reason: ownership is recorded when a socket is
 * created, credentials when a connection is made. */
static void (*sock_cred_hook)(int *, int *, int *);

void sock_set_cred_hook(void (*fn)(int *, int *, int *)) { sock_cred_hook = fn; }

void sock_cred_now(int *pid, int *uid, int *gid) {
  *pid = 0; *uid = 0; *gid = 0;
  if (sock_cred_hook) sock_cred_hook(pid, uid, gid);
}

int sock_set_peer_cred(struct socket *s, int pid, int uid, int gid) {
  if (!s || !s->used) return -EBADF;
  if (s->ops != sock_unix_ops() || !s->impl) return -ENOTSUP;
  u_set_peer_cred(s->impl, pid, uid, gid);
  return 0;
}

int sock_peer_cred(struct socket *s, int *pid, int *uid, int *gid) {
  if (!s || !s->used) return -EBADF;
  if (!s->ops || !s->ops->peer_cred || !s->impl) return -ENOTSUP;
  return s->ops->peer_cred(s->impl, pid, uid, gid);
}


static uint32_t socket_gens[SOCKET_MAX];   /* survives release: identity is per slot */

/* Test instrumentation. Compiled in but inert: only the readiness self-test
 * touches these. */
static struct sock_stats stats;
static int skip_recheck;                   /* deliberately breaks the re-check */
static struct { struct socket *s; unsigned mask; int times; } inject;

struct sock_stats sock_stats_get(void) { return stats; }
void sock_test_skip_recheck(int on) { skip_recheck = on; }
void sock_test_inject_after_register(struct socket *s, unsigned mask, int times) {
  inject.s = s; inject.mask = mask; inject.times = times;
}
uint32_t sock_gen(struct socket *s) { return s ? s->gen : 0; }

/* ---- waiter table ------------------------------------------------------- */

static void sock_bump_gen(struct socket *s) {
  int i = (int)(s - sockets);
  if (i < 0 || i >= SOCKET_MAX) return;
  s->gen = ++socket_gens[i];
}

static void sock_wake_all(struct socket *s) {
  for (int i = 0; i < SOCK_WAITERS; i++) {
    struct sock_waiter *w = &s->waiters[i];
    if (!w->used) continue;

    /* Object reuse: this registration belongs to a previous occupant of the
     * pool slot. Drop it rather than waking whoever sits there now. */
    if (w->gen != s->gen) { w->used = 0; stats.drops++; continue; }

    if (w->task <= 0 || w->task >= task_count()) { w->used = 0; stats.drops++; continue; }
    task_t *t = task_at(w->task);

    /* Two-sided validation: the task must still name this socket, and must
     * still be the generation that registered. One side alone proves nothing. */
    if (!t || t->tgen != w->tgen || t->wait_obj != (void *)s) {
      w->used = 0;
      stats.drops++;
      continue;
    }
    if (t->state != TASK_BLOCKED) continue;   /* still running: its re-check decides */
    w->used = 0;
    stats.wakes++;
    task_wake(w->task);
  }
}

/* Plant a registration by hand, so the test can exercise the two guards that
 * reject a stale one without needing a live race to produce it. */
void sock_test_plant(struct socket *s, uint32_t gen, int task, uint32_t tgen) {
  if (!s) return;
  for (int i = 0; i < SOCK_WAITERS; i++) {
    if (s->waiters[i].used) continue;
    s->waiters[i].used = 1;
    s->waiters[i].gen = gen;
    s->waiters[i].task = task;
    s->waiters[i].tgen = tgen;
    return;
  }
}

int sock_waiter_add(struct socket *s) {
  if (!s) return -EBADF;
  for (int i = 0; i < SOCK_WAITERS; i++) {
    struct sock_waiter *w = &s->waiters[i];
    if (w->used && w->gen == s->gen) continue;   /* live registration */
    w->used = 1;
    w->gen = s->gen;
    w->task = task_current();
    task_t *t = task_at(w->task);
    w->tgen = t ? t->tgen : 0;
    stats.registered++;
    return i;
  }
  stats.overflows++;
  return -ENOBUFS;      /* explicit: the caller must not block unregistered */
}

void sock_waiter_clear(struct socket *s, int slot) {
  if (!s || slot < 0 || slot >= SOCK_WAITERS) return;
  s->waiters[slot].used = 0;
}

int sock_waiters_live(struct socket *s) {
  int n = 0;
  if (!s) return 0;
  for (int i = 0; i < SOCK_WAITERS; i++)
    if (s->waiters[i].used && s->waiters[i].gen == s->gen) n++;
  return n;
}

void sock_drop_waiters(int task_id) {
  for (int i = 0; i < SOCKET_MAX; i++) {
    /* Ownership of the read dies with its task, in the same sweep as the waiters:
     * both are registrations a dead task cannot withdraw itself. */
    if (sockets[i].used && sockets[i].read_owner == task_id) {
      sockets[i].read_owner = 0;
      stats.drops++;
    }
    for (int k = 0; k < SOCK_WAITERS; k++)
      if (sockets[i].waiters[k].used && sockets[i].waiters[k].task == task_id) {
        sockets[i].waiters[k].used = 0;
        stats.drops++;
      }
  }
}

/* Take the read side for this task. Free is free; taken by a task that no longer
 * exists is free too, because a slot can be reused only after a release. */
int sock_read_begin(struct socket *s) {
  if (!s || !s->used) return -EBADF;
  int me = task_current();
  if (s->read_owner && s->read_owner != me) { stats.busy++; return -EBUSY; }
  s->read_owner = me;
  return 0;
}

void sock_read_end(struct socket *s) {
  if (!s) return;
  int me = task_current();
  if (s->read_owner == me) s->read_owner = 0;
}

/* ---- object lifetime ---------------------------------------------------- */

struct socket *sock_new(void) {
  for (int i = 0; i < SOCKET_MAX; i++) {
    if (sockets[i].used) continue;
    sockets[i].used = 1;
    sockets[i].impl = 0;
    sockets[i].owner = sock_owner_hook ? sock_owner_hook() : 0;
    sockets[i].ops = socket_backend();
    sockets[i].closing = 0;
    sockets[i].read_owner = 0;
    sockets[i].seq = 0;
    for (int k = 0; k < SOCK_WAITERS; k++) sockets[i].waiters[k].used = 0;
    sock_bump_gen(&sockets[i]);          /* new identity in this slot */
    return &sockets[i];
  }
  return 0;
}

/* AF_UNIX socketpair. Both endpoints exist from the start, so unlike connect()
 * there is nothing to wait for: the sockets are given the pair backend and then
 * tell that backend where to send readiness news, which is the only thing that
 * makes a blocked reader wake on a write instead of on its deadline. */
static void sock_ready_cb(void *ctx, unsigned mask);

int sock_pair(struct socket **out_a, struct socket **out_b) {
  if (!out_a || !out_b) return -EINVAL;
  struct socket *a = sock_new();
  struct socket *b = sock_new();
  if (!a || !b) {
    if (a) sock_close(a);
    if (b) sock_close(b);
    return -EMFILE;
  }
  void *ea = 0, *eb = 0;
  int r = sock_unix_new_pair(&ea, &eb);
  if (r != 0) { sock_close(a); sock_close(b); return r; }

  a->ops = sock_unix_ops();
  a->impl = ea;
  a->seq++;
  b->ops = sock_unix_ops();
  b->impl = eb;
  b->seq++;
  a->ops->bind(a->impl, sock_ready_cb, a);
  b->ops->bind(b->impl, sock_ready_cb, b);
  /* A socketpair's two ends belong to whoever made it, so each one's peer is that
   * process - the same answer Linux gives, and the reason a forked child still
   * sees its parent's pid rather than its own. */
  int cpid = 0, cuid = 0, cgid = 0;
  sock_cred_now(&cpid, &cuid, &cgid);
  u_set_peer_cred(ea, cpid, cuid, cgid);
  u_set_peer_cred(eb, cpid, cuid, cgid);

  *out_a = a;
  *out_b = b;
  return 0;
}

int sock_send_ctl(struct socket *s, void **ofiles, int n) {
  if (!s || !s->ops) return -EBADF;
  if (!s->ops->send_ctl) return -ENOTSUP;
  int r = s->ops->send_ctl(s->impl, ofiles, n);
  if (r == 0) s->seq++;
  return r;
}

int sock_recv_ctl(struct socket *s, void **out, int max) {
  if (!s || !s->ops || !s->ops->recv_ctl) return 0;
  return s->ops->recv_ctl(s->impl, out, max);
}

int sock_cancel_ctl(struct socket *s) {
  if (!s || !s->ops || !s->ops->cancel_ctl) return -ENOTSUP;
  return s->ops->cancel_ctl(s->impl);
}

/* Move an existing endpoint into a socket object that already has a descriptor.
 * connect() needs this: the caller made its socket before there was anything to
 * connect it to, so the transport it just got hold of has to take the place of
 * the one that socket started with. The donor object is released without running
 * its close op - the endpoint moved, it is not dead - and the readiness callback
 * is re-pointed at the object that survives, or a blocked reader would be woken
 * on the wrong socket. */
static int sock_adopt(struct socket *dst, struct socket *donor) {
  if (!dst || !donor || !dst->used || !donor->used) return -EINVAL;
  if (dst->impl) return -EINVAL;                  /* already connected */
  if (!donor->impl || !donor->ops) return -EINVAL;
  const struct socket_ops *ops = donor->ops;
  dst->ops = ops;
  dst->impl = donor->impl;
  dst->shut_wr = 0;
  dst->shut_rd = 0;
  dst->seq++;
  donor->impl = 0;                 /* the transport belongs to dst now */
  donor->ops = 0;
  donor->used = 0;
  sock_bump_gen(donor);
  if (ops->bind) ops->bind(dst->impl, sock_ready_cb, dst);
  return 0;
}

int sock_unix_bind(struct socket *s, const char *path) {
  if (!s || !s->used) return -EBADF;
  if (s->impl) return -EINVAL;          /* already connected or already a name */
  void *rec = 0;
  int r = unix_name_bind(path, &rec);
  if (r != 0) return r;
  s->ops = unix_listener_ops();
  s->impl = rec;
  s->seq++;
  if (s->ops->bind) s->ops->bind(s->impl, sock_ready_cb, s);
  return 0;
}

int sock_unix_listen(struct socket *s, int backlog) {
  if (!s || !s->used || !s->impl) return -EINVAL;
  if (s->ops != unix_listener_ops()) return -EINVAL;
  return unix_name_listen(s->impl, backlog);
}

int sock_unix_accept(struct socket *s, struct socket **out) {
  if (!s || !s->used || !s->impl) return -EINVAL;
  if (s->ops != unix_listener_ops()) return -EINVAL;
  return unix_name_accept(s->impl, out);
}

int sock_unix_connect(struct socket *s, const char *path) {
  if (!s || !s->used) return -EBADF;
  struct socket *end = 0;
  int r = unix_name_connect(path, &end);
  if (r != 0) return r;
  r = sock_adopt(s, end);
  if (r != 0) sock_close(end);
  return r;
}

/* The last descriptor is gone. Nothing new may start, everyone parked leaves
 * with an error - but the transport is NOT torn down here: a task may be inside
 * a backend call, and its pin keeps the object alive until it returns. */
void sock_shutdown(struct socket *s) {
  if (!s || !s->used || s->closing) return;   /* idempotent */
  s->closing = 1;
  s->seq++;
  sock_wake_all(s);
}

/* shutdown(SHUT_WR): the FIN goes out through the transport and the socket stays
 * readable, so the answer to what was already sent can still be collected. */
void sock_shutdown_wr(struct socket *s) {
  if (!s || !s->used || s->shut_wr) return;
  s->shut_wr = 1;
  if (s->impl && s->ops && s->ops->shutdown) s->ops->shutdown(s->impl);
  sock_notify(s, OMI_POLLOUT);        /* a blocked writer wakes to learn it may not write */
}

/* shutdown(SHUT_RD): the receive side is dropped, buffered bytes included - that
 * is what the caller asked for. Reads answer end-of-file from here on. */
void sock_shutdown_rd(struct socket *s) {
  if (!s || !s->used || s->shut_rd) return;
  s->shut_rd = 1;
  sock_notify(s, OMI_POLLIN);
}

void sock_close(struct socket *s) {
  if (!s || !s->used) return;
  /* Transport teardown happens once, here, on the last reference - the backend
   * never sees a refcount, and the ofile layer never sees a packet. */
  if (s->impl && s->ops && s->ops->close) s->ops->close(s->impl);
  s->impl = 0;
  s->owner = 0;
  s->ops = 0;
  s->used = 0;
  s->closing = 0;
  s->shut_wr = 0;
  s->shut_rd = 0;
  for (int i = 0; i < SOCK_WAITERS; i++) s->waiters[i].used = 0;
  sock_bump_gen(s);        /* any registration still naming this gen is stale */
}

/* ---- transport calls ---------------------------------------------------- */

/* Backend news becomes a waiter wake here, and nowhere else. */
static void sock_ready_cb(void *ctx, unsigned mask) {
  sock_notify((struct socket *)ctx, mask);
}

int sock_connect(struct socket *s, const net_cfg_t *cfg, const uint8_t ip[4],
                 uint16_t port) {
  if (!s || !s->used) return -EBADF;
  if (s->closing) return -EBADF;       /* shutdown: no new operations */
  if (s->impl) return -EISCONN;
  s->impl = s->ops->open(cfg, ip, port);
  if (!s->impl) return -ECONNREFUSED;
  /* Register for readiness news before returning: a connection that becomes
   * readable between open() and the first wait must not be missed. */
  if (s->ops->bind) s->ops->bind(s->impl, sock_ready_cb, s);
  s->seq++;
  return 0;
}

long sock_send(struct socket *s, const void *buf, uint64_t n) {
  if (!s || !s->used) return -EBADF;
  if (s->closing) return -EBADF;
  if (s->shut_wr) return -EPIPE;     /* the caller said it was done sending */
  if (!s->impl) return -ENOTCONN;
  int w = s->ops->send(s->impl, buf, (int)n);
  return w < 0 ? -EPIPE : (long)w;
}

long sock_recv(struct socket *s, void *buf, uint64_t n, int timeout_ms) {
  if (!s || !s->used) return -EBADF;
  if (s->closing) return -EBADF;
  if (s->shut_rd) return 0;          /* the receive side was dropped: EOF, at once */
  if (!s->impl) return -ENOTCONN;
  int r = s->ops->recv(s->impl, buf, (int)n, timeout_ms);
  if (r > 0) { s->seq++; return r; }
  return r == 0 ? -EAGAIN : 0;      /* timeout, or the peer closed: EOF */
}

unsigned sock_poll(struct socket *s) {
  if (!s || !s->used) return OMI_POLLERR | OMI_POLLHUP;
  if (s->closing) return OMI_POLLERR;
  if (!s->impl) return OMI_POLLERR;      /* not connected: nothing will arrive */
  return s->ops->poll(s->impl);
}

void sock_notify(struct socket *s, unsigned mask) {
  (void)mask;                 /* level-triggered: every waiter re-checks anyway */
  if (!s || !s->used) return;
  s->seq++;
  sock_wake_all(s);
}

/* Publish, re-check, block. The re-check is the whole point: without it a
 * transition landing between the readiness test and the block is a lost wakeup,
 * and the task sleeps until its deadline. */
int sock_wait(struct socket *s, unsigned want, uint64_t deadline_ticks) {
  if (!s || !s->used) return -EBADF;
  const unsigned term = OMI_POLLERR | OMI_POLLHUP;

  for (;;) {
    uint32_t sq = s->seq;
    unsigned r = sock_poll(s);
    if (r & (want | term)) return (int)r;
    if (s->closing) return (int)OMI_POLLERR;

    int slot = sock_waiter_add(s);
    if (slot < 0) return slot;          /* -ENOBUFS, never a silent block */

    /* Test hook: an event lands exactly here - after registration, before the
     * re-check. The narrowest possible version of the race. */
    if (inject.s == s && inject.times > 0) {
      inject.times--;
      sock_notify(s, inject.mask);
    }

    if (!skip_recheck && (s->seq != sq || (sock_poll(s) & (want | term)))) {
      sock_waiter_clear(s, slot);
      stats.spurious++;
      continue;                          /* it arrived before we slept */
    }

    task_t *me = task_at(task_current());
    stats.blocks++;
    me->timed_out = 0;
    me->wake_at = task_total_ticks() + deadline_ticks;
    me->wait_obj = (void *)s;
    task_block_current();
    me->wait_obj = 0;
    me->wake_at = 0;
    int fired = me->timed_out;
    sock_waiter_clear(s, slot);

    if (fired) {
      /* The deadline is a belt, not a mechanism: re-check once in case the wake
       * and the expiry raced, then report that nothing arrived. */
      unsigned r2 = sock_poll(s);
      if (r2 & (want | term)) return (int)r2;
      return -EAGAIN;
    }
  }
}

/* ---- observability ------------------------------------------------------ */

void *sock_owner_now(void) { return sock_owner_hook ? sock_owner_hook() : 0; }

void sock_set_owner(struct socket *s, void *space) {
  if (s && s->used) s->owner = space;
}

int sock_owned_live(void *space) {
  int n = 0;
  for (int i = 0; i < SOCKET_MAX; i++)
    if (sockets[i].used && sockets[i].owner == space) n++;
  return n;
}

int sock_live(void) {
  int n = 0;
  for (int i = 0; i < SOCKET_MAX; i++) if (sockets[i].used) n++;
  return n;
}

/* A space that owned endpoints has just been torn down. Every one of them must
 * be gone: a socket outliving the process that held it is a leak with a
 * connection attached, and it would keep receiving segments for nobody.
 *
 * The count is per owner, not global. Global counting reported a leak whenever
 * another process opened a socket while this one was being reaped - which is
 * every interesting workload, and said nothing about the space being audited. */
void sock_report_after_space(void *space, int had) {
  if (!had) return;
  int live = sock_owned_live(space);
  int want = 0;
  char d[8];
  int o = 0;
  if (live == 0) d[o++] = '0';
  for (int v = live, i = 0; v > 0 && i < 6; i++) { d[o++] = (char)('0' + v % 10); v /= 10; }
  for (int i = 0; i < o / 2; i++) { char c = d[i]; d[i] = d[o - 1 - i]; d[o - 1 - i] = c; }
  d[o] = 0;

  serial_puts(live != want ? "[sock] FAILED: endpoints outlived the space that owned them: live "
                           : "[sock] ok   every endpoint died with its space (live ");
  serial_puts(d);
  if (live != want) {
    char w[8]; int n = 0;
    if (!want) w[n++] = '0';
    for (int v = want, i = 0; v > 0 && i < 6; i++) { w[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n / 2; i++) { char c = w[i]; w[i] = w[n - 1 - i]; w[n - 1 - i] = c; }
    serial_puts(", expected ");
    serial_puts(w);
  }
  serial_puts(live != want ? "\n" : ")\n");
}