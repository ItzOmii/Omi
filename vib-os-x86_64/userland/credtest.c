/* SO_PEERCRED: the kernel's answer to "who is on the other end".
 *
 * Three things matter and each is checked here. A socketpair belongs to the
 * process that made it, so both ends name that process - and still name it after
 * a fork, because the credential is a fact about the connection, not a live look
 * at whoever holds the descriptor now. A named socket names the process that
 * connected on the accepting side, and the process that owns the name on the
 * client side. And a TCP socket has no peer process at all, so it refuses the
 * question instead of inventing one.
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
  printf("[cred] %s %s (got %ld want %ld)\n", ok ? "ok  " : "FAIL", what, got, want);
}

/* The peer's pid, or -errno. */
static long peer_pid(int fd) {
  struct ucred cr;
  socklen_t l = sizeof cr;
  memset(&cr, 0, sizeof cr);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) != 0) return -errno;
  return cr.pid;
}

static socklen_t addr_of(struct sockaddr_un *a, const char *path) {
  memset(a, 0, sizeof *a);
  a->sun_family = AF_UNIX;
  strncpy(a->sun_path, path, sizeof a->sun_path - 1);
  return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
}

int main(void) {
  long me = (long)getpid();

  /* A pair names its maker, on both ends. */
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
    printf("[cred] FAIL no socketpair (%d)\n", errno);
    return 1;
  }
  check(peer_pid(sp[0]) == me, "a pair names the process that made it", peer_pid(sp[0]), me);
  check(peer_pid(sp[1]) == me, "on both ends", peer_pid(sp[1]), me);

  /* A name: the accepting side learns the client, the client learns the owner. */
  const char *path = "/run/omi/cred.sock";
  int ls = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a;
  socklen_t alen = addr_of(&a, path);
  int bad = 0;
  if (ls < 0 || bind(ls, (struct sockaddr *)&a, alen) != 0 || listen(ls, 4) != 0) bad = 1;
  check(!bad, "a listener exists to connect to", bad, 0);

  pid_t kid = bad ? -1 : fork();
  if (kid == 0) {
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ka;
    socklen_t kl = addr_of(&ka, path);
    if (connect(c, (struct sockaddr *)&ka, kl) != 0) _exit(10);
    if (peer_pid(c) != me) _exit(11);          /* the client sees the owner */
    if (peer_pid(sp[1]) != me) _exit(12);      /* and inheritance changes nothing */
    if (write(c, "x", 1) != 1) _exit(13);
    _exit(0);
  }

  long seen = -1;
  int st = 0;
  if (kid > 0) {
    int afd = accept(ls, 0, 0);
    if (afd >= 0) {
      seen = peer_pid(afd);
      char b;
      read(afd, &b, 1);
      close(afd);
    } else {
      seen = -1000 - errno;
    }
    waitpid(kid, &st, 0);
  }
  check(kid > 0 && seen == (long)kid, "the accepted side names the process that connected",
        seen, (long)kid);
  check(kid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
        "the client names the owner, and a forked descriptor still names its maker",
        kid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);

  if (ls >= 0) close(ls);
  close(sp[0]);
  close(sp[1]);

  /* A transport whose peer is an address, not a process, says so. */
  int t = socket(AF_INET, SOCK_STREAM, 0);
  if (t >= 0) {
    struct ucred cr;
    socklen_t l = sizeof cr;
    errno = 0;
    long r = getsockopt(t, SOL_SOCKET, SO_PEERCRED, &cr, &l);
    check(r == -1 && errno == ENOTSUP, "a TCP socket has no peer process to name",
          r == -1 ? errno : 0, ENOTSUP);
    close(t);
  }

  /* A listening socket has no peer either: nothing has connected to it. */
  int ls2 = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a2;
  socklen_t alen2 = addr_of(&a2, "/run/omi/idle-cred.sock");
  bind(ls2, (struct sockaddr *)&a2, alen2);
  listen(ls2, 1);
  struct ucred cr;
  socklen_t l = sizeof cr;
  errno = 0;
  long r = getsockopt(ls2, SOL_SOCKET, SO_PEERCRED, &cr, &l);
  check(r == -1 && errno == ENOTSUP, "a listener has no peer to name", r == -1 ? errno : 0, ENOTSUP);
  close(ls2);

  printf("[cred] %d/%d\n", checks - failures, checks);
  return failures ? 1 : 0;
}
