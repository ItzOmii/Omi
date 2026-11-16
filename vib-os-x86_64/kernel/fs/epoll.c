/* The epoll instance: a set of descriptors to watch, and nothing else. See
 * epoll.h for why this is a kept set rather than an event queue. */

#include "../include/epoll.h"
#include "../include/kmalloc.h"
#include "../include/errno.h"

epoll_t *epoll_new(void) {
  epoll_t *ep = kzalloc(sizeof *ep);
  if (!ep) return 0;
  ep->n = 0;
  return ep;
}

void epoll_free(epoll_t *ep) {
  /* Registration holds no reference to what it watches, so there is nothing to
   * release here: the members' own descriptors are what keep them alive, and a
   * member that closes leaves an entry that reports EPOLLERR on the next wait.
   * Linux behaves the same way, and the alternative - a reference from the set -
   * is how a program that forgets EPOLL_CTL_DEL leaks a socket forever. */
  if (ep) kfree(ep);
}

static int epoll_find(const epoll_t *ep, int fd) {
  for (int i = 0; i < ep->n; i++)
    if (ep->e[i].fd == fd) return i;
  return -1;
}

int epoll_ctl(epoll_t *ep, int op, int fd, uint32_t events, uint64_t data) {
  if (!ep) return -EINVAL;
  int at = epoll_find(ep, fd);

  if (op == EPOLL_CTL_ADD) {
    if (at >= 0) return -EEXIST;
    if (ep->n >= EPOLL_MAX) return -ENOSPC;
    /* Refused rather than quietly served: a program that asked for edges and got
     * levels re-arms nothing and spins at full speed in its own loop, and a bug
     * that looks like the program's is ours to prevent. */
    if (events & EPOLLET) return -EINVAL;
    ep->e[ep->n].fd = fd;
    ep->e[ep->n].events = events;
    ep->e[ep->n].data = data;
    ep->n++;
    return 0;
  }

  if (op == EPOLL_CTL_MOD) {
    if (at < 0) return -ENOENT;
    if (events & EPOLLET) return -EINVAL;
    ep->e[at].events = events;
    ep->e[at].data = data;      /* Linux keeps the data on MOD: a program may change it */
    return 0;
  }

  if (op == EPOLL_CTL_DEL) {
    if (at < 0) return -ENOENT;
    for (int i = at; i + 1 < ep->n; i++) ep->e[i] = ep->e[i + 1];
    ep->n--;
    return 0;
  }

  return -EINVAL;
}

int epoll_ready(epoll_t *ep, fdtable_t *t, epitem_t *out, int max) {
  if (!ep || !out || max <= 0) return 0;
  int k = 0;
  for (int i = 0; i < ep->n && k < max; i++) {
    /* ERR and HUP are reported whether or not they were asked for: they are the
     * reasons a wait ends, not optional bits. */
    unsigned r = fd_poll(t, ep->e[i].fd) & (unsigned)(ep->e[i].events | EPOLLERR | EPOLLHUP);
    if (!r) continue;
    out[k] = ep->e[i];
    out[k].events = r;          /* what is ready, not what was asked for */
    k++;
  }
  return k;
}

void epoll_drop_oneshot(epoll_t *ep, int fd) {
  if (!ep) return;
  int at = epoll_find(ep, fd);
  if (at < 0) return;
  if (!(ep->e[at].events & EPOLLONESHOT)) return;
  for (int i = at; i + 1 < ep->n; i++) ep->e[i] = ep->e[i + 1];
  ep->n--;
}

/* Any member ready makes the instance itself readable, so an epoll descriptor can
 * be handed to poll() or to another epoll - which is how a program watches two
 * event loops, and how a nested wait would work if anyone wrote one. */
int epoll_any_ready(epoll_t *ep, fdtable_t *t) {
  if (!ep) return 0;
  for (int i = 0; i < ep->n; i++)
    if (fd_poll(t, ep->e[i].fd) & (unsigned)(ep->e[i].events | EPOLLERR | EPOLLHUP)) return 1;
  return 0;
}
