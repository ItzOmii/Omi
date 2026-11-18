/* AF_UNIX by name: processes that were never forked from each other still meet.
 *
 * The important change is that the two sides are *not* related by a socketpair:
 * the listener is bound and listening before the client is forked, the client
 * looks the name up, and the parent accepts two of them. Everything else is the
 * failure behaviour - a name taken twice, a name nobody listens on, a name that
 * went away, and accept() on something that was never a listener.
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failures;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) failures++;
  printf("[unixname] %s %s (got %ld want %ld)\n", ok ? "ok  " : "FAIL", what, got, want);
}

static socklen_t addr_of(struct sockaddr_un *a, const char *path) {
  memset(a, 0, sizeof *a);
  a->sun_family = AF_UNIX;
  strncpy(a->sun_path, path, sizeof a->sun_path - 1);
  return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
}

static int listen_at(const char *path) {
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0) return -1;
  struct sockaddr_un a;
  socklen_t l = addr_of(&a, path);
  if (bind(s, (struct sockaddr *)&a, l) != 0) { close(s); return -1; }
  if (listen(s, 4) != 0) { close(s); return -1; }
  return s;
}

int main(void) {
  const char *path = "/run/omi/broker.sock";

  int ls = socket(AF_UNIX, SOCK_STREAM, 0);
  check(ls >= 3, "a socket to bind", ls, 3);
  struct sockaddr_un a;
  socklen_t alen = addr_of(&a, path);
  long r = bind(ls, (struct sockaddr *)&a, alen);
  check(r == 0, "bind() claims a name", r, 0);
  r = listen(ls, 4);
  check(r == 0, "listen() starts taking connections", r, 0);

  /* The same name, again: two listeners on one path would make connect() a coin
   * toss, so it is refused. */
  int dup = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a2;
  socklen_t alen2 = addr_of(&a2, path);
  r = bind(dup, (struct sockaddr *)&a2, alen2);
  check(r < 0 && errno == EADDRINUSE, "a second bind to the same name is refused",
        r < 0 ? errno : 0, EADDRINUSE);
  close(dup);

  /* A name that was never bound. */
  int c = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a3;
  socklen_t alen3 = addr_of(&a3, "/run/omi/nobody.sock");
  r = connect(c, (struct sockaddr *)&a3, alen3);
  check(r < 0 && errno == ENOENT, "connecting to a name nobody bound says so",
        r < 0 ? errno : 0, ENOENT);
  close(c);

  /* Bound, but never told to listen: the name exists and still refuses. */
  int bound_only = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a4;
  socklen_t alen4 = addr_of(&a4, "/run/omi/idle.sock");
  bind(bound_only, (struct sockaddr *)&a4, alen4);
  int c2 = socket(AF_UNIX, SOCK_STREAM, 0);
  r = connect(c2, (struct sockaddr *)&a4, alen4);
  check(r < 0 && errno == ECONNREFUSED, "a name that never listened refuses",
        r < 0 ? errno : 0, ECONNREFUSED);
  close(c2);
  close(bound_only);

  /* Two clients that never shared a descriptor with this process. */
  pid_t kids[2];
  for (int i = 0; i < 2; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      int s = socket(AF_UNIX, SOCK_STREAM, 0);
      struct sockaddr_un ka;
      socklen_t kl = addr_of(&ka, path);
      if (connect(s, (struct sockaddr *)&ka, kl) != 0) _exit(20 + i);
      char msg[16];
      int n = snprintf(msg, sizeof msg, "client-%d", i);
      if (write(s, msg, (size_t)n) != n) _exit(30 + i);
      char back[32];
      memset(back, 0, sizeof back);
      long got = read(s, back, sizeof back - 1);
      if (got != n || memcmp(back, msg, (size_t)n)) _exit(40 + i);
      close(s);
      _exit(0);
    }
    kids[i] = pid;
  }

  int served = 0;
  for (int i = 0; i < 2; i++) {
    int afd = accept(ls, 0, 0);
    if (afd < 3) { check(0, "accept() hands back a connection", afd, 3); break; }
    char buf[32];
    memset(buf, 0, sizeof buf);
    long n = read(afd, buf, sizeof buf - 1);
    if (n > 0 && write(afd, buf, (size_t)n) == n) served++;
    close(afd);
  }
  check(served == 2, "two clients that never shared a descriptor were served", served, 2);

  int ok = 0;
  for (int i = 0; i < 2; i++) {
    int st = 0;
    waitpid(kids[i], &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) ok++;
  }
  check(ok == 2, "and each one got its own reply back", ok, 2);

  /* The name lives exactly as long as the listener does. */
  close(ls);
  int c3 = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a5;
  socklen_t alen5 = addr_of(&a5, path);
  r = connect(c3, (struct sockaddr *)&a5, alen5);
  check(r < 0 && errno == ENOENT, "closing the listener removes the name",
        r < 0 ? errno : 0, ENOENT);
  close(c3);

  /* accept() on something that is not a listener. */
  int sp[2];
  socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
  r = accept(sp[0], 0, 0);
  check(r < 0 && errno == EINVAL, "accept() on a plain socket is refused",
        r < 0 ? errno : 0, EINVAL);
  close(sp[0]);
  close(sp[1]);

  /* And a pair still works alongside a name, so the two mechanisms coexist. */
  int ls2 = listen_at("/run/omi/second.sock");
  check(ls2 >= 3, "a second listener can exist at once", ls2, 3);
  if (ls2 >= 3) close(ls2);

  printf("[unixname] %d/%d\n", checks - failures, checks);
  return failures ? 1 : 0;
}
