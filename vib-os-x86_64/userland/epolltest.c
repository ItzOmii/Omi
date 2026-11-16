/* epoll: the ABI an event loop is written against.
 *
 * What a browser's message pump actually needs is not a monitor-per-wait but a
 * set that outlives the call, so the checks below are about the parts poll cannot
 * do: registration, modification, removal, the value the program gets back, and a
 * wait that really blocks instead of returning "nothing yet" in a spin.
 *
 * The two things this implementation refuses are checked as refusals, not as
 * absences: edge triggering, and a second registration of the same descriptor.
 * A suite that only checked the happy path would pass while a program that asked
 * for edges got levels and burned a core.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) {
    failures++;
    printf("[epolltest] FAIL %s (got %ld want %ld)\n", what, got, want);
  } else {
    printf("[epolltest] ok   %s\n", what);
  }
}

static long ms_since(const struct timespec *t0) {
  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  return (t1.tv_sec - t0->tv_sec) * 1000L + (t1.tv_nsec - t0->tv_nsec) / 1000000L;
}

int main(void) {
  struct epoll_event ev, got[4];
  struct timespec t0;
  int n;

  int ep = epoll_create1(0);
  check(ep >= 0, "epoll_create1 returns a descriptor", ep, 0);
  if (ep < 0) return 1;

  int p[2];
  if (pipe(p) != 0) { printf("[epolltest] FATAL no pipe\n"); return 1; }

  memset(&ev, 0, sizeof ev);
  ev.events = EPOLLIN;
  ev.data.u64 = 0xDEADBEEFCAFEULL;
  check(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0, "ADD the read end", 0, 0);

  errno = 0;
  n = epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev);
  check(n == -1 && errno == EEXIST, "adding the same descriptor twice is EEXIST", n, -1);

  memset(got, 0, sizeof got);
  n = epoll_wait(ep, got, 4, 0);
  check(n == 0, "a quiet set returns 0 with a zero timeout", n, 0);

  check(write(p[1], "abc", 3) == 3, "write three bytes", 3, 3);
  n = epoll_wait(ep, got, 4, 1000);
  check(n == 1 && (got[0].events & EPOLLIN) && got[0].data.u64 == 0xDEADBEEFCAFEULL,
        "a write makes it ready and the registered value comes back", n, 1);

  n = epoll_wait(ep, got, 4, 0);
  check(n == 1, "level-triggered: still ready while the bytes are unread", n, 1);

  char buf[8];
  check(read(p[0], buf, 3) == 3, "drain it", 3, 3);
  n = epoll_wait(ep, got, 4, 0);
  check(n == 0, "and then the set is quiet again", n, 0);

  /* The other end, watched for writability - a write end is ready almost always,
   * which makes it the natural probe for EPOLLOUT and for a two-member set. */
  memset(&ev, 0, sizeof ev);
  ev.events = EPOLLOUT;
  ev.data.u64 = 2;
  check(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &ev) == 0, "ADD the write end for EPOLLOUT", 0, 0);

  check(write(p[1], "x", 1) == 1, "put a byte back for the read end", 1, 1);
  memset(got, 0, sizeof got);
  n = epoll_wait(ep, got, 4, 1000);
  int saw_in = 0, saw_out = 0;
  for (int i = 0; i < n; i++) {
    if (got[i].data.u64 == 0xDEADBEEFCAFEULL && (got[i].events & EPOLLIN)) saw_in = 1;
    if (got[i].data.u64 == 2 && (got[i].events & EPOLLOUT)) saw_out = 1;
  }
  check(n == 2 && saw_in && saw_out, "two members ready are reported separately, with their own values", n, 2);

  check(epoll_ctl(ep, EPOLL_CTL_DEL, p[1], 0) == 0, "DEL the write end", 0, 0);
  n = epoll_wait(ep, got, 4, 1000);
  check(n == 1 && got[0].data.u64 == 0xDEADBEEFCAFEULL, "and only the read end is watched now", n, 1);

  errno = 0;
  n = epoll_ctl(ep, EPOLL_CTL_MOD, p[1], &ev);
  check(n == -1 && errno == ENOENT, "MOD on a descriptor that was never added is ENOENT", n, -1);

  errno = 0;
  ev.events = EPOLLIN | EPOLLET;
  n = epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev);
  check(n == -1 && errno == EINVAL, "edge triggering is refused, not silently served as level", n, -1);

  /* A closed peer is a reason for the wait to end: the read end must report HUP
   * without having been asked for it. */
  check(read(p[0], buf, 8) == 1, "drain the last byte", 1, 1);
  close(p[1]);
  n = epoll_wait(ep, got, 4, 1000);
  check(n == 1 && (got[0].events & EPOLLHUP), "a closed write end reports EPOLLHUP unasked", n, 1);

  /* Does it block? A child writes after 200 ms; if epoll_wait polled-and-returned
   * the answer would come back empty almost immediately. */
  int q[2];
  if (pipe(q) != 0) { printf("[epolltest] FATAL no pipe\n"); return 1; }
  /* The child's sleep is itself a check: before nanosleep existed this call
   * returned at once and the timing below measured the scheduler, not the wait. */
  clock_gettime(CLOCK_MONOTONIC, &t0);
  usleep(200000);
  long slept = ms_since(&t0);
  check(slept >= 180, "usleep really sleeps (nanosleep is not a stub)", slept, 200);

  pid_t kid = fork();
  if (kid == 0) {
    usleep(200000);
    write(q[1], "p", 1);
    _exit(0);
  }
  int ep2 = epoll_create1(0);
  memset(&ev, 0, sizeof ev);
  ev.events = EPOLLIN;
  ev.data.u64 = 7;
  epoll_ctl(ep2, EPOLL_CTL_ADD, q[0], &ev);

  clock_gettime(CLOCK_MONOTONIC, &t0);
  n = epoll_wait(ep2, got, 4, 3000);
  long waited = ms_since(&t0);
  check(n == 1 && got[0].data.u64 == 7, "a blocking wait wakes when the descriptor becomes ready", n, 1);
  check(waited >= 150, "and it actually slept rather than spinning to the answer", waited, 200);

  /* The timeout is a deadline, not a hint. */
  int quiet[2];
  if (pipe(quiet) != 0) { printf("[epolltest] FATAL no pipe\n"); return 1; }
  int ep3 = epoll_create1(0);
  epoll_ctl(ep3, EPOLL_CTL_ADD, quiet[0], &ev);
  clock_gettime(CLOCK_MONOTONIC, &t0);
  n = epoll_wait(ep3, got, 4, 150);
  long spent = ms_since(&t0);
  check(n == 0 && spent >= 120, "an idle wait returns 0 when its timeout expires", n, 0);

  /* One-shot: report once, then stop watching, without the program removing it. */
  int ep4 = epoll_create1(0);
  memset(&ev, 0, sizeof ev);
  ev.events = EPOLLIN | EPOLLONESHOT;
  ev.data.u64 = 9;
  epoll_ctl(ep4, EPOLL_CTL_ADD, quiet[0], &ev);
  write(quiet[1], "z", 1);
  n = epoll_wait(ep4, got, 4, 1000);
  int first = (n == 1 && (got[0].events & EPOLLIN));
  n = epoll_wait(ep4, got, 4, 0);
  check(first && n == 0, "EPOLLONESHOT reports once and then stops watching", n, 0);

  close(ep);
  errno = 0;
  n = epoll_wait(ep, got, 4, 0);
  check(n == -1 && errno == EBADF, "a closed epoll descriptor is EBADF", n, -1);

  int st = 0;
  waitpid(kid, &st, 0);

  if (failures) {
    printf("[epolltest] %d of %d checks failed\n", failures, checks);
    return 1;
  }
  printf("[epolltest] OK %d/%d (registration, level triggering, values, HUP, one-shot, blocking wait, timeout, refusals)\n",
         checks, checks);
  return 0;
}
