/*
 * Milestone 1: the readiness protocol, proved on a fake backend.
 *
 * Nothing here touches TCP or the NIC. A deterministic backend provides the
 * readiness bits, a worker task provides the blocked reader, and the test drives
 * the transitions. The point is not that a socket can be read - it is that
 * "publish, re-check, block, wake" has no lost-wakeup window and that the
 * lifetime rules hold when a descriptor disappears underneath a call.
 *
 * Every wait carries a deadline. A broken protocol therefore prints a failed
 * check rather than wedging the boot: a test that can only pass by not hanging
 * proves nothing.
 */

#include "fd.h"
#include "socket.h"
#include "net_backend.h"
#include "readiness.h"
#include "task.h"
#include "errno.h"
#include "string.h"
#include "gui.h"

/* ---- a backend with no network in it ------------------------------------ */

typedef struct {
  int  open;
  int  rd_len;
  int  destroyed;
  int  calls;             /* poll() invocations, for "becomes ready on call N" */
  int  ready_after;       /* 0 = never; else poll() reports POLLIN from call N */
  int  eof;               /* the stream ended: recv reports the close */
  void (*cb)(void *, unsigned);   /* what the socket bound us to */
  void *ctx;
  int  binds;
  char rd[32];
} fake_t;

static fake_t fk;

static void *fk_open(const net_cfg_t *cfg, const uint8_t ip[4], uint16_t port) {
  (void)cfg; (void)ip; (void)port;
  fk.open = 1;
  return &fk;
}
static int fk_send(void *h, const void *buf, int len) { (void)h; (void)buf; return len; }
/* The three outcomes the socket layer distinguishes, kept distinct here: bytes,
 * 0 for "nothing now, ask again" (would block), and -1 for the peer closing. A
 * fake that conflates the last two cannot test a read that waits. */
static int fk_recv(void *h, void *buf, int max, int timeout_ms) {
  (void)h; (void)timeout_ms;
  if (fk.rd_len > 0) {
    int n = fk.rd_len < max ? fk.rd_len : max;
    memcpy(buf, fk.rd, (size_t)n);
    fk.rd_len -= n;
    return n;
  }
  return fk.eof ? -1 : 0;
}
static void fk_close(void *h) { (void)h; fk.destroyed++; fk.open = 0; }
static unsigned fk_poll(void *h) {
  (void)h;
  fk.calls++;
  unsigned r = OMI_POLLOUT;
  if (fk.ready_after && fk.calls >= fk.ready_after) r |= OMI_POLLIN;
  if (fk.rd_len > 0) r |= OMI_POLLIN;
  if (!fk.open) r |= OMI_POLLERR;
  return r;
}

/* A backend that never takes the callback cannot wake anybody, and no amount of
 * correct waiting helps: the reader would sit until its deadline every time. So
 * the fake remembers who bound to it, and the test reports through that. */
static void fk_bind(void *h, void (*cb)(void *, unsigned), void *ctx) {
  (void)h;
  fk.cb = cb;
  fk.ctx = ctx;
  fk.binds++;
}

static const struct socket_ops fk_ops = {
  .name = "readiness-fake",
  .open = fk_open, .send = fk_send, .recv = fk_recv,
  .close = fk_close, .poll = fk_poll, .bind = fk_bind,
};

/* ---- one worker, driven by commands -------------------------------------- */

enum { C_IDLE = 0, C_WAIT, C_REGISTER_THEN_DIE, C_READ };

static volatile int  w_cmd, w_done, w_bits, w_bytes, w_timed_out;
static struct socket *w_sock;
static struct socket *first_sock;   /* test 1's socket, kept for cleanup */
static unsigned      w_want;
static uint64_t      w_deadline;

static const uint8_t noip[4] = { 0, 0, 0, 0 };

/* For the read-path checks: the worker needs a descriptor table and a buffer. */
static fdtable_t *wt;
static int        wfd;
static uint64_t   wlen;
static char       wbuf[64];

static void rd_worker(void) {
  for (;;) {
    if (!w_cmd) { task_yield(); continue; }
    int c = w_cmd;
    if (c == C_WAIT) {
      w_bits = sock_wait(w_sock, w_want, w_deadline);
      w_timed_out = task_at(task_current())->timed_out;
      if (w_bits & OMI_POLLIN) {
        char b[32];
        long n = sock_recv(w_sock, b, sizeof b, 50);
        w_bytes = (int)n;
      }
    } else if (c == C_READ) {
      w_timed_out = 0;
      w_bits = (int)fd_read_ticks(wt, wfd, wbuf, wlen, 40);
    } else if (c == C_REGISTER_THEN_DIE) {
      /* Register, then die without clearing: the exit path must unlink it. */
      sock_waiter_add(w_sock);
      task_kill_current(7);
    }
    w_cmd = 0;
    w_done = 1;
  }
}

/* ---- checks ------------------------------------------------------------- */

static int ok, total;
static void chk(const char *what, int cond) {
  total++;
  if (cond) { ok++; return; }
  serial_puts("[rdy] FAILED: ");
  serial_puts(what);
  serial_puts("\n");
}

/* Numbers behind a failure: a check that just says "no" costs another boot to
 * diagnose, and the values are already in registers. */
static void diag(const char *label, int a, int b, int c) {
  int vals[3]; vals[0] = a; vals[1] = b; vals[2] = c;
  serial_puts("[rdy]   ");
  serial_puts(label);
  for (int k = 0; k < 3; k++) {
    serial_puts(" ");
    int v = vals[k], neg = v < 0;
    if (neg) v = -v;
    char d[8]; int o = 0;
    if (!v) d[o++] = '0';
    while (v) { d[o++] = (char)('0' + v % 10); v /= 10; }
    if (neg) serial_puts("-");
    while (o) { char ch = d[--o]; char s2[2] = { ch, 0 }; serial_puts(s2); }
  }
  serial_puts("\n");
}

/* Let real ticks pass. task_sleep_ticks() refuses task 0 outright (`cur <= 0`),
 * so the boot task has to yield and watch the clock itself; deadlines still fire
 * because the PIT keeps interrupting kernel code. */
static void wait_ticks(uint64_t n) {
  uint64_t want = task_total_ticks() + n;
  while (task_total_ticks() < want) task_yield();
}

/* Hand the worker a command and wait for it to finish. Bounded: if it never
 * gets there, the caller's assertions fail instead of the boot hanging. */
static void run_worker(int cmd) {
  w_done = 0;
  w_bits = -999;
  w_bytes = -1;
  w_timed_out = 0;
  w_cmd = cmd;
  for (int i = 0; i < 200 && !w_done; i++) wait_ticks(1);
}

/* Wait for a command that was issued earlier. */
static void wait_done(void) {
  for (int i = 0; i < 200 && !w_done; i++) wait_ticks(1);
}

static void fake_reset(void) {
  /* open: the transport exists, so POLLOUT and no POLLERR. Tests that need the
   * connection gone drive it through shutdown, not by lying about the backend. */
  fk.open = 1; fk.rd_len = 0; fk.destroyed = 0; fk.calls = 0; fk.ready_after = 0;
  fk.eof = 0; fk.cb = 0; fk.ctx = 0; fk.binds = 0;
}

void sock_readiness_selftest(void) {
  const struct socket_ops *kernel_ops = socket_backend();
  fdtable_t *t = fdtable_new();
  if (!t) { serial_puts("[rdy] no descriptor table\n"); return; }

  int worker_id = task_spawn("rdy", rd_worker);
  chk("a reader task exists", worker_id > 0);
  socket_backend_select(&fk_ops);

  /* 1. the plain case: block, notify, wake, read. */
  fake_reset();
  w_sock = sock_new();
  first_sock = w_sock;
  sock_connect(w_sock, 0, noip, 0);
  struct sock_stats s0 = sock_stats_get();
  w_want = OMI_POLLIN; w_deadline = 40;
  w_done = 0; w_bits = -999; w_cmd = C_WAIT;
  wait_ticks(2);                             /* let it register and block */
  chk("the reader is parked", sock_waiters_live(w_sock) == 1);
  fk.rd_len = 5; memcpy(fk.rd, "HELLO", 5);
  sock_notify(w_sock, OMI_POLLIN);
  wait_done();
  struct sock_stats s1 = sock_stats_get();
  chk("the reader woke on the notification", w_done && (w_bits & OMI_POLLIN));
  chk("exactly one wake was delivered", s1.wakes - s0.wakes == 1);
  chk("the reader consumed the payload", w_bytes == 5);
  chk("no registration is left behind", sock_waiters_live(w_sock) == 0);

  /* 2. the race: an event lands after registration, before the block. The
   *    re-check must catch it and the task must never sleep. */
  fake_reset();
  fk.ready_after = 2;                        /* poll #1 not ready, poll #2 ready */
  w_want = OMI_POLLIN; w_deadline = 40;
  sock_test_inject_after_register(w_sock, OMI_POLLIN, 1);
  s0 = sock_stats_get();
  run_worker(C_WAIT);
  s1 = sock_stats_get();
  sock_test_inject_after_register(0, 0, 0);
  diag("race  bits/blocks/spurious", w_bits, (int)(s1.blocks - s0.blocks), (int)(s1.spurious - s0.spurious));
  chk("the race was caught before sleeping", w_done && (w_bits & OMI_POLLIN));
  chk("the reader never blocked", s1.blocks - s0.blocks == 0);
  chk("the re-check counted it as spurious", s1.spurious - s0.spurious >= 1);

  /* 3. the same race with the re-check disabled: the wake is genuinely lost,
   *    and only the deadline belt saves it. Expected outcome, printed loudly. */
  fake_reset();                              /* never becomes ready: the only
                                              * way out is the deadline */
  sock_test_inject_after_register(w_sock, OMI_POLLIN, 1);
  sock_test_skip_recheck(1);
  s0 = sock_stats_get();
  run_worker(C_WAIT);
  s1 = sock_stats_get();
  sock_test_skip_recheck(0);
  sock_test_inject_after_register(0, 0, 0);
  diag("lost  bits/blocks/fired", w_bits, (int)(s1.blocks - s0.blocks), w_timed_out);
  chk("without the re-check the reader does sleep", s1.blocks - s0.blocks == 1);
  chk("the notify was genuinely lost", s1.wakes - s0.wakes == 0);
  chk("the deadline belt rescues the lost wake", w_bits == -EAGAIN && w_timed_out);
  serial_puts("[rdy] LOST-WAKE: the re-check was disabled on purpose, the belt caught it\n");

  /* 4. last descriptor closes while a backend call is pinned: no new work, but
   *    the transport survives until the pin is released, then dies once. */
  fake_reset();
  struct socket *s = sock_new();
  sock_connect(s, 0, noip, 0);
  int fd = fd_socket(t);                     /* a real descriptor, for the pin */
  chk("descriptor exists", fd >= 0);
  ofile_t *o = t->fd[fd];
  sock_close(o->sock);                       /* reuse: point the ofile at `s` */
  o->sock = s;
  ofile_pin(o);                              /* a task is inside a call */
  chk("shutdown before destruction", fd_close(t, fd) == 0 && o->closing);
  chk("the transport is still alive while pinned", fk.destroyed == 0 && sock_waiters_live(s) == 0);
  chk("shutdown refuses new operations", sock_poll(s) == OMI_POLLERR);
  ofile_unpin(o);
  chk("the transport dies exactly once when the pin goes", fk.destroyed == 1);

  /* 5. last-descriptor close wakes a parked reader with POLLERR, not a timeout. */
  fake_reset();
  struct socket *s2 = sock_new();
  sock_connect(s2, 0, noip, 0);
  int fd2 = fd_socket(t);
  ofile_t *o2 = t->fd[fd2];
  sock_close(o2->sock);
  o2->sock = s2;
  w_sock = s2; w_want = OMI_POLLIN; w_deadline = 40;
  /* The read path pins while it is inside the backend. Without this the reader
   * would be parked on an unpinned description - the exact hazard the lifetime
   * rule forbids - and the close below would free the transport under it. */
  ofile_pin(o2);
  w_done = 0; w_bits = -999; w_cmd = C_WAIT;
  wait_ticks(2);
  chk("the reader is parked on the second socket", sock_waiters_live(s2) == 1);
  fd_close(t, fd2);                          /* refs -> 0: shutdown, not destroy */
  wait_done();
  diag("close bits/fired/destroyed", w_bits, w_timed_out, fk.destroyed);
  chk("shutdown woke the reader with POLLERR", w_done && w_bits == (int)OMI_POLLERR);
  chk("it was a wake, not the deadline", w_timed_out == 0);
  chk("the transport survived the parked reader", fk.destroyed == 0);
  ofile_unpin(o2);
  chk("and died exactly once when the pin went", fk.destroyed == 1);

  /* 6. a task that dies while registered must not leave the registration. */
  fake_reset();
  struct socket *s3 = sock_new();
  sock_connect(s3, 0, noip, 0);
  w_sock = s3;
  run_worker(C_REGISTER_THEN_DIE);
  chk("task exit unlinked its registration", sock_waiters_live(s3) == 0);

  /* 7. socket-pool reuse: a registration from the previous occupant must not be
   *    woken by the next socket in the same slot. */
  fake_reset();
  struct socket *a = sock_new();
  sock_connect(a, 0, noip, 0);
  uint32_t gen_a = sock_gen(a);
  sock_waiter_add(a);                        /* registered against A */
  sock_close(a);
  struct socket *b = sock_new();             /* same slot, new identity */
  sock_connect(b, 0, noip, 0);
  chk("the slot was reused", b == a && sock_gen(b) > gen_a);

  /* An old generation, and a current generation naming the wrong task: both
   * are rejected, for different reasons. */
  sock_test_plant(b, gen_a, task_current(), task_at(task_current())->tgen);
  sock_test_plant(b, sock_gen(b), 999, 0);
  s0 = sock_stats_get();
  sock_notify(b, OMI_POLLIN);
  s1 = sock_stats_get();
  chk("the stale generation was dropped, not woken", s1.wakes - s0.wakes == 0);
  chk("the mismatched task was dropped too", s1.drops - s0.drops == 2);
  chk("and nothing is left registered", sock_waiters_live(b) == 0);

  /* 8. waiter table exhaustion is an error, never a silent block. */
  fake_reset();
  struct socket *c = sock_new();
  sock_connect(c, 0, noip, 0);
  int got = 0;
  for (int i = 0; i < SOCK_WAITERS; i++) if (sock_waiter_add(c) >= 0) got++;
  chk("all slots fill", got == SOCK_WAITERS);
  chk("the next registration is refused", sock_waiter_add(c) == -ENOBUFS);
  chk("the refusal did not disturb the filled slots", sock_waiters_live(c) == SOCK_WAITERS);
  for (int i = 0; i < SOCK_WAITERS; i++) sock_waiter_clear(c, i);

  /* 9. the read path itself: partial data returns at once, a would-block read
   *    becomes a wait rather than an error, and EAGAIN never leaks out.
   *
   * The test above killed its worker on purpose, so this section needs one of
   * its own - and that is worth stating, because a suite that silently inherits
   * a dead helper reports "never ran" as "wrong answer". */
  worker_id = task_spawn("rdy2", rd_worker);
  chk("a second worker for the read path", worker_id >= 0);
  fake_reset();
  struct socket *r = sock_new();
  int crc = sock_connect(r, 0, noip, 0);
  chk("the socket bound its readiness callback", fk.binds == 1);
  int rfd = fd_socket(t);
  chk("a descriptor for the read path", rfd >= 0);
  /* Probe the socket itself, before any descriptor or task is involved: -EAGAIN
   * here means "open, connected to the backend, nothing buffered yet". */
  int probe = (int)sock_recv(r, wbuf, 4, 0);
  chk("a fresh connection reports would-block, not EOF", probe == -EAGAIN);
  chk("and it connected", crc == 0);
  ofile_t *ro = t->fd[rfd];
  sock_close(ro->sock);
  ro->sock = r;
  wt = t; wfd = rfd; wlen = sizeof wbuf;

  memcpy(fk.rd, "abc", 3); fk.rd_len = 3;
  s0 = sock_stats_get();
  run_worker(C_READ);
  s1 = sock_stats_get();

  chk("a partial read returns the bytes it has", w_bits == 3);
  chk("and does not wait to fill the buffer", s1.blocks - s0.blocks == 0);

  /* Nothing buffered and no EOF: the read must park, not fail. */
  fk.rd_len = 0;
  s0 = sock_stats_get();
  w_done = 0; w_bits = -999; w_cmd = C_READ;
  wait_ticks(2);
  chk("an empty stream parks the reader", sock_waiters_live(r) == 1);
  memcpy(fk.rd, "later!", 6); fk.rd_len = 6;
  /* Report through the bound callback, not by calling sock_notify directly:
   * the wire from backend to waiter is the thing that was missing. */
  fk.cb(fk.ctx, OMI_POLLIN);
  wait_done();
  s1 = sock_stats_get();
  chk("the backend's report woke the reader", w_bits == 6);
  chk("and it waited exactly once", s1.blocks - s0.blocks == 1);

  /* EOF is a result, not a wait. */
  fk.rd_len = 0; fk.eof = 1;
  run_worker(C_READ);
  chk("eof reads as zero bytes", w_bits == 0);

  /* And a genuine expiry still reports EAGAIN, because that one is the caller's. */
  fk.eof = 0; fk.rd_len = 0;
  run_worker(C_READ);
  chk("a read that runs out of patience says EAGAIN", w_bits == -EAGAIN);

  fd_close(t, rfd);

  /* Clean up: release everything the test created. */
  sock_close(first_sock);
  sock_close(b);
  sock_close(c);
  sock_close(s3);
  socket_backend_select(kernel_ops);
  fdtable_free(t);
  chk("no socket object leaked out of the test", sock_live() == 0);
  chk("the kernel backend is selected again", socket_backend() == kernel_ops);

  char m[64];
  int o3 = 0;
  const char *pre = "[rdy] ";
  while (*pre) m[o3++] = *pre++;
  m[o3++] = (char)('0' + (total / 10) % 10);
  m[o3++] = (char)('0' + total % 10);
  const char *mid = " checks, ";
  while (*mid) m[o3++] = *mid++;
  m[o3++] = (char)('0' + (ok / 10) % 10);
  m[o3++] = (char)('0' + ok % 10);
  const char *post = " ok (readiness, fake backend)\n";
  while (*post) m[o3++] = *post++;
  m[o3] = 0;
  serial_puts(m);
}