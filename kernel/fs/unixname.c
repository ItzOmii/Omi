/*
 * The AF_UNIX name table.
 *
 * Eight bytes of this file would be a hash table if there were many names; there
 * are sixteen, so it is a linear scan and the code stays readable. Everything
 * here is about the three states a name can be in: claimed (bound), accepting
 * (listening), and gone (closed, or never existed).
 */

#include "../include/unixname.h"
#include "../include/socket.h"
#include "../include/net_backend.h"   /* struct socket_ops: the table a listener carries */
#include "../include/unixpair.h"     /* u_set_peer_cred: who is on the other end */
#include "../include/readiness.h"
#include "../include/errno.h"
#include "../include/string.h"

typedef struct {
  int  used;
  int  listening;
  int  backlog;                 /* how many unanswered connections are allowed */
  int  count;                   /* how many are waiting now */
  char path[UNIX_PATH_MAX];
  struct socket *pending[UNIX_BACKLOG_MAX];
  void (*cb)(void *, unsigned); /* where readiness news goes; set by bind() */
  void *ctx;
  /* Who claimed the name. A client that connects learns this, the same way it
   * would learn the listening process's pid on Linux. */
  int pid, uid, gid;
  void *space;                  /* the space that will accept what arrives */
} unix_listener_t;

static unix_listener_t names[UNIX_NAME_MAX];

static unix_listener_t *find(const char *path) {
  if (!path || !path[0]) return 0;
  for (int i = 0; i < UNIX_NAME_MAX; i++)
    if (names[i].used && !strcmp(names[i].path, path)) return &names[i];
  return 0;
}

static void notify(unix_listener_t *l, unsigned mask) {
  if (l->cb) l->cb(l->ctx, mask);
}

/* Drop a listener and every connection it was holding. Each pending endpoint is
 * closed, so a client that connected to a name that then went away sees EOF
 * rather than a socket that can never be accepted. */
static void listener_release(unix_listener_t *l) {
  for (int i = 0; i < l->count; i++) {
    if (l->pending[i]) sock_close(l->pending[i]);
    l->pending[i] = 0;
  }
  l->count = 0;
  l->used = 0;
  l->listening = 0;
  l->backlog = 0;
  l->cb = 0;
  l->ctx = 0;
  l->pid = l->uid = l->gid = 0;
  l->path[0] = 0;
}

int unix_name_bind(const char *path, void **out_impl) {
  if (!out_impl) return -EINVAL;
  *out_impl = 0;
  if (!path || !path[0]) return -EINVAL;
  uint64_t n = strlen(path);
  if (n >= UNIX_PATH_MAX) return -ENAMETOOLONG;
  if (find(path)) return -EADDRINUSE;

  for (int i = 0; i < UNIX_NAME_MAX; i++) {
    if (names[i].used) continue;
    unix_listener_t *l = &names[i];
    l->used = 1;
    l->listening = 0;
    l->backlog = 1;
    l->count = 0;
    l->cb = 0;
    l->ctx = 0;
    sock_cred_now(&l->pid, &l->uid, &l->gid);
    l->space = sock_owner_now();
    memcpy(l->path, path, (size_t)n);
    l->path[n] = 0;
    *out_impl = l;
    return 0;
  }
  return -ENOSPC;              /* all sixteen names taken: said out loud */
}

int unix_name_listen(void *impl, int backlog) {
  unix_listener_t *l = (unix_listener_t *)impl;
  if (!l || !l->used) return -EINVAL;
  if (backlog < 1) backlog = 1;
  if (backlog > UNIX_BACKLOG_MAX) backlog = UNIX_BACKLOG_MAX;
  l->backlog = backlog;
  l->listening = 1;
  return 0;
}

int unix_name_accept(void *impl, struct socket **out) {
  unix_listener_t *l = (unix_listener_t *)impl;
  if (!out) return -EINVAL;
  *out = 0;
  if (!l || !l->used) return -EINVAL;
  if (!l->listening) return -EINVAL;   /* bound but never told to listen */
  if (l->count == 0) return -EAGAIN;
  *out = l->pending[0];
  for (int i = 1; i < l->count; i++) l->pending[i - 1] = l->pending[i];
  l->pending[--l->count] = 0;
  return 0;
}

int unix_name_connect(const char *path, struct socket **out_client) {
  if (!out_client) return -EINVAL;
  *out_client = 0;
  unix_listener_t *l = find(path);
  if (!l) return -ENOENT;              /* nothing at that path */
  if (!l->listening) return -ECONNREFUSED;
  if (l->count >= l->backlog) return -ECONNREFUSED;  /* nobody accepting */

  struct socket *a = 0, *b = 0;
  int r = sock_pair(&a, &b);
  if (r != 0) return r;
  /* Both directions: the server learns which process connected, the client learns
   * which process owns the name. Neither is a live query - the connection records
   * who made it, exactly as a socketpair records who created it. */
  int cpid = 0, cuid = 0, cgid = 0;
  sock_cred_now(&cpid, &cuid, &cgid);
  /* Through the socket, not the endpoint: the endpoint handle is private to the
   * socket layer, and handing it a struct socket * writes the pid over the
   * socket's own ops pointer. */
  sock_set_peer_cred(a, l->pid, l->uid, l->gid);
  sock_set_peer_cred(b, cpid, cuid, cgid);
  /* The waiting end is the listener's now: the client may exit before anybody
   * accepts, and its teardown must not claim a connection it already gave away. */
  sock_set_owner(b, l->space);
  l->pending[l->count++] = b;
  *out_client = a;
  notify(l, OMI_POLLIN);               /* wakes a blocked accept() */
  return 0;
}

/* ---- the socket table entry a listener carries -------------------------- */

static int l_send(void *h, const void *buf, int len) {
  (void)h; (void)buf; (void)len;
  return -ENOTCONN;        /* a listener carries no data of its own */
}

static int l_recv(void *h, void *buf, int max, int timeout_ms) {
  (void)h; (void)buf; (void)max; (void)timeout_ms;
  return -ENOTCONN;
}

static void l_close(void *h) {
  unix_listener_t *l = (unix_listener_t *)h;
  if (l && l->used) listener_release(l);
}

static void l_shutdown(void *h) { (void)h; }

static unsigned l_poll(void *h) {
  unix_listener_t *l = (unix_listener_t *)h;
  if (!l || !l->used) return OMI_POLLERR;
  return l->count ? OMI_POLLIN : 0;
}

static void l_bind(void *h, void (*cb)(void *, unsigned), void *ctx) {
  unix_listener_t *l = (unix_listener_t *)h;
  if (!l) return;
  l->cb = cb;
  l->ctx = ctx;
}

static const struct socket_ops listener_ops = {
  "unixname", 0, l_send, l_recv, l_close, l_shutdown, l_poll, l_bind,
  0, 0, 0
};

const struct socket_ops *unix_listener_ops(void) { return &listener_ops; }
