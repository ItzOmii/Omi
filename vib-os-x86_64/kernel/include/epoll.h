#ifndef OMI_EPOLL_H
#define OMI_EPOLL_H

/* epoll: poll with the set kept.
 *
 * There is no event queue here and no second readiness mechanism - a wait still
 * asks every member whether it is ready, through the same fd_poll() that poll()
 * uses. What registration buys is only that the set outlives the call, which is
 * what an event loop needs: build the set once, then wait on it forever.
 *
 * The consequence is worth stating instead of hiding: a large set costs a scan
 * per wait, so this is O(n) like poll. It is not the O(ready) design epoll is
 * famous for, and calling it "epoll" is about the ABI a program is written
 * against, not about a red-black tree. */

#include <stdint.h>
#include "fd.h"

#define EPOLL_MAX 16   /* registrations per instance: the same ceiling as poll's set */

/* Linux's bit values. The low four being identical to poll's POLLIN/POLLOUT/
 * POLLERR/POLLHUP is not a coincidence - epoll was defined as poll-with-
 * registration - so the translation below is a mask, never a table. */
#define EPOLLIN      0x001u
#define EPOLLPRI     0x002u
#define EPOLLOUT     0x004u
#define EPOLLERR     0x008u
#define EPOLLHUP     0x010u
#define EPOLLRDHUP   0x2000u
#define EPOLLONESHOT 0x40000000u
#define EPOLLET      0x80000000u

enum { EPOLL_CTL_ADD = 1, EPOLL_CTL_DEL = 2, EPOLL_CTL_MOD = 3 };

/* One registration: which descriptor, which bits were asked for, and the value
 * the program wants handed back so it can find its own object without a lookup. */
typedef struct {
  int      fd;
  uint32_t events;
  uint64_t data;
} epitem_t;

typedef struct epoll {
  int      n;
  epitem_t e[EPOLL_MAX];
} epoll_t;

epoll_t *epoll_new(void);
void     epoll_free(epoll_t *ep);

/* ADD refuses a second registration of the same descriptor (EEXIST), a full set
 * (ENOSPC) and a request for edge triggering (EINVAL); MOD and DEL refuse a
 * descriptor that was never registered (ENOENT). */
int epoll_ctl(epoll_t *ep, int op, int fd, uint32_t events, uint64_t data);

/* Fill `out` with the members of `ep` that are ready right now, at most `max`.
 * Returns how many, 0 when nothing is. Never blocks - the waiting belongs to the
 * caller, which is the only part that knows a timeout. */
int epoll_ready(epoll_t *ep, fdtable_t *t, epitem_t *out, int max);

/* EPOLLONESHOT means "report me once, then stop watching": the registration is
 * removed after the wait that reported it, never while the scan is running. */
void epoll_drop_oneshot(epoll_t *ep, int fd);

/* 1 when any member is ready: the instance's own readiness. */
int  epoll_any_ready(epoll_t *ep, fdtable_t *t);

#endif
