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

#define SOCKET_MAX 24

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

  /* Bumped on every readiness change, BEFORE anyone is woken. A waiter
   * snapshots this, registers, then compares: a transition that landed in
   * between shows up here, which is what makes "publish then block" safe
   * without an atomic block primitive. */
  uint32_t seq;

  /* Bumped when this pool slot changes identity (release and allocation).
   * sockets[i] is reused, so a registration from the previous occupant must
   * never be woken by, or wake anyone onto, the next one. */
  uint32_t gen;

  struct sock_waiter waiters[SOCK_WAITERS];
};

static struct socket sockets[SOCKET_MAX];
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
  for (int i = 0; i < SOCKET_MAX; i++)
    for (int k = 0; k < SOCK_WAITERS; k++)
      if (sockets[i].waiters[k].used && sockets[i].waiters[k].task == task_id) {
        sockets[i].waiters[k].used = 0;
        stats.drops++;
      }
}

/* ---- object lifetime ---------------------------------------------------- */

struct socket *sock_new(void) {
  for (int i = 0; i < SOCKET_MAX; i++) {
    if (sockets[i].used) continue;
    sockets[i].used = 1;
    sockets[i].impl = 0;
    sockets[i].ops = socket_backend();
    sockets[i].closing = 0;
    sockets[i].seq = 0;
    for (int k = 0; k < SOCK_WAITERS; k++) sockets[i].waiters[k].used = 0;
    sock_bump_gen(&sockets[i]);          /* new identity in this slot */
    return &sockets[i];
  }
  return 0;
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

void sock_close(struct socket *s) {
  if (!s || !s->used) return;
  /* Transport teardown happens once, here, on the last reference - the backend
   * never sees a refcount, and the ofile layer never sees a packet. */
  if (s->impl && s->ops && s->ops->close) s->ops->close(s->impl);
  s->impl = 0;
  s->ops = 0;
  s->used = 0;
  s->closing = 0;
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
  if (!s->impl) return -ENOTCONN;
  int w = s->ops->send(s->impl, buf, (int)n);
  return w < 0 ? -EPIPE : (long)w;
}

long sock_recv(struct socket *s, void *buf, uint64_t n, int timeout_ms) {
  if (!s || !s->used) return -EBADF;
  if (s->closing) return -EBADF;
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

int sock_live(void) {
  int n = 0;
  for (int i = 0; i < SOCKET_MAX; i++) if (sockets[i].used) n++;
  return n;
}

/* A space that owned endpoints has just been torn down. Every one of them must
 * be gone: a socket outliving the process that held it is a leak with a
 * connection attached, and it would keep receiving segments for nobody. */
void sock_report_after_space(int had, int live_before) {
  if (!had) return;
  int live = sock_live();
  int want = live_before - had;
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