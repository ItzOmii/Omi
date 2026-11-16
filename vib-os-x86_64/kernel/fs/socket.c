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
 */

#include "socket.h"
#include "net_backend.h"
#include "errno.h"
#include "gui.h"        /* serial_puts */

#define SOCKET_MAX 24

struct socket {
  const struct socket_ops *ops;   /* the backend table, chosen once */
  void *impl;                     /* backend-owned transport, NULL until connect */
  int   used;
};

static struct socket sockets[SOCKET_MAX];

struct socket *sock_new(void) {
  for (int i = 0; i < SOCKET_MAX; i++) {
    if (sockets[i].used) continue;
    sockets[i].used = 1;
    sockets[i].impl = 0;
    sockets[i].ops = socket_backend();
    return &sockets[i];
  }
  return 0;
}

int sock_connect(struct socket *s, const net_cfg_t *cfg, const uint8_t ip[4],
                 uint16_t port) {
  if (!s || !s->used) return -EBADF;
  if (s->impl) return -EISCONN;
  s->impl = s->ops->open(cfg, ip, port);
  if (!s->impl) return -ECONNREFUSED;
  return 0;
}

long sock_send(struct socket *s, const void *buf, uint64_t n) {
  if (!s || !s->used) return -EBADF;
  if (!s->impl) return -ENOTCONN;
  int w = s->ops->send(s->impl, buf, (int)n);
  return w < 0 ? -EPIPE : (long)w;
}

long sock_recv(struct socket *s, void *buf, uint64_t n, int timeout_ms) {
  if (!s || !s->used) return -EBADF;
  if (!s->impl) return -ENOTCONN;
  int r = s->ops->recv(s->impl, buf, (int)n, timeout_ms);
  if (r > 0) return r;
  return r == 0 ? -EAGAIN : 0;      /* timeout, or the peer closed: EOF */
}

unsigned sock_poll(struct socket *s) {
  if (!s || !s->used) return OMI_POLLERR | OMI_POLLHUP;
  if (!s->impl) return OMI_POLLERR;      /* not connected: nothing will arrive */
  return s->ops->poll(s->impl);
}

/* Live endpoints. Exists for tests: "nothing leaked" has to be a number, not an
 * absence of log lines. */
int sock_live(void) {
  int n = 0;
  for (int i = 0; i < SOCKET_MAX; i++) if (sockets[i].used) n++;
  return n;
}

/* A space that owned endpoints has just been torn down. Every one of them must
 * be gone: a socket outliving the process that held it is a leak with a
 * connection attached, and it would keep receiving segments for nobody. */
void sock_report_after_space(int had) {
  if (!had) return;
  int live = sock_live();
  char d[8];
  int o = 0;
  if (live == 0) d[o++] = '0';
  for (int v = live, i = 0; v > 0 && i < 6; i++) { d[o++] = (char)('0' + v % 10); v /= 10; }
  for (int i = 0; i < o / 2; i++) { char c = d[i]; d[i] = d[o - 1 - i]; d[o - 1 - i] = c; }
  d[o] = 0;

  serial_puts(live ? "[sock] FAILED: endpoints outlived the space that owned them: live "
                   : "[sock] ok   every endpoint died with its space (live ");
  serial_puts(d);
  serial_puts(live ? "\n" : ")\n");
}

void sock_close(struct socket *s) {
  if (!s || !s->used) return;
  /* Transport teardown happens once, here, on the last reference - the backend
   * never sees a refcount, and the ofile layer never sees a packet. */
  if (s->impl && s->ops && s->ops->close) s->ops->close(s->impl);
  s->impl = 0;
  s->ops = 0;
  s->used = 0;
}