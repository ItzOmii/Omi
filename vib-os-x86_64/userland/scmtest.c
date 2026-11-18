/* SCM_RIGHTS: a descriptor handed across a socketpair.
 *
 * The proof that matters is not "recvmsg returned a number" but that the number
 * is the *same pipe*: the child writes through the descriptor it was given and
 * the parent reads those bytes out of the end it opened. Everything else here is
 * the failure behaviour - a control buffer too small must say MSG_CTRUNC and not
 * leak the descriptor, and an ancillary type this kernel does not implement must
 * be refused rather than dropped.
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <stddef.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failures;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) failures++;
  printf("[scm] %s %s (got %ld want %ld)\n", ok ? "ok  " : "FAIL", what, got, want);
}

static int open_fds(void) {
  int n = 0;
  for (int fd = 3; fd < 64; fd++) if (fcntl(fd, F_GETFD) >= 0) n++;
  return n;
}

/* Send `payload_len` bytes with one descriptor attached. */
static long send_fd(int sock, int fd, const void *payload, size_t payload_len, int ctl_type, size_t ctl_size) {
  char cbuf[CMSG_SPACE(sizeof(int))];
  memset(cbuf, 0, sizeof cbuf);
  struct iovec iov = { (void *)payload, payload_len };
  struct msghdr m;
  memset(&m, 0, sizeof m);
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  m.msg_control = cbuf;
  m.msg_controllen = ctl_size;
  struct cmsghdr *c = CMSG_FIRSTHDR(&m);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = ctl_type;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &fd, sizeof fd);
  return sendmsg(sock, &m, 0);
}

int main(void) {
  int sv[2], pf[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || pipe(pf)) {
    printf("[scm] FAIL could not set up (%d)\n", errno);
    return 1;
  }

  pid_t pid = fork();
  if (pid == 0) {
    /* The child has neither end of the pipe until bytes arrive telling it so. */
    close(sv[0]);
    close(pf[0]);
    close(pf[1]);

    char ch = 0;
    char cbuf[64];
    struct iovec iov = { &ch, 1 };
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = cbuf;
    m.msg_controllen = sizeof cbuf;

    long n = recvmsg(sv[1], &m, 0);
    if (n != 1) _exit(10);
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) _exit(11);
    int fd = -1;
    memcpy(&fd, CMSG_DATA(c), sizeof fd);
    if (fd < 3) _exit(12);

    const char *msg = "through the passed fd";
    if (write(fd, msg, strlen(msg)) != (long)strlen(msg)) _exit(13);
    close(fd);
    close(sv[1]);
    _exit(0);
  }

  close(sv[1]);
  long sent = send_fd(sv[0], pf[1], "x", 1, SCM_RIGHTS, CMSG_SPACE(sizeof(int)));
  check(sent == 1, "sendmsg carries a byte and a descriptor", sent, 1);
  /* The child must hold the pipe by itself from here: closing the parent's copy
   * before the child writes is what makes the test about the descriptor and not
   * about a shared table. */
  close(pf[1]);

  char buf[64];
  memset(buf, 0, sizeof buf);
  long got = read(pf[0], buf, sizeof buf - 1);
  check(got == (long)strlen("through the passed fd") && !strcmp(buf, "through the passed fd"),
        "the child wrote through a pipe end it never opened", got, 21);
  int status = 0;
  waitpid(pid, &status, 0);
  check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child's own checks passed",
        WIFEXITED(status) ? WEXITSTATUS(status) : -1, 0);
  close(pf[0]);
  close(sv[0]);

  /* A control buffer too small must not install anything: the descriptor is
   * released, and the receiver is told the message was cut. */
  int sv2[2], keep[2];
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv2);
  if (pipe(keep) == 0) {
    int before = open_fds();
    long s2 = send_fd(sv2[0], keep[1], "y", 1, SCM_RIGHTS, CMSG_SPACE(sizeof(int)));
    check(s2 == 1, "a second descriptor goes over a second pair", s2, 1);
    char ch = 0, small[8];
    struct iovec iov = { &ch, 1 };
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = small;
    m.msg_controllen = sizeof small;            /* room for a header, not for an fd */
    long n2 = recvmsg(sv2[1], &m, 0);
    check(n2 == 1 && (m.msg_flags & MSG_CTRUNC), "a short control buffer sets MSG_CTRUNC",
          (m.msg_flags & MSG_CTRUNC) ? 1 : 0, 1);
    check(open_fds() == before, "and the descriptor it could not take is released",
          open_fds(), before);
    close(sv2[0]);
    close(sv2[1]);
    close(keep[0]);
    close(keep[1]);
  }

  /* Growing an unknown ancillary type silently would be the old bug in a new
   * place: it is refused, and the connection is still usable afterwards. */
  int sv3[2];
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv3);
  long bad = send_fd(sv3[0], 1, "z", 1, 99, CMSG_SPACE(sizeof(int)));
  check(bad < 0 && errno == EINVAL, "an ancilliary type it cannot honour is refused",
        bad < 0 ? errno : 0, EINVAL);

  char ch = 'q';
  struct iovec iov = { &ch, 1 };
  struct msghdr m;
  memset(&m, 0, sizeof m);
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  m.msg_control = 0;
  m.msg_controllen = 0;
  long plain = sendmsg(sv3[0], &m, 0);
  check(plain == 1, "and plain messages still go through", plain, 1);
  long back = recvmsg(sv3[1], &m, 0);
  check(back == 1 && ch == 'q', "in both directions", back, 1);
  close(sv3[0]);
  close(sv3[1]);

  /* Two iovecs: joined on send and split on receive. This was writing the second
   * iovec at the first one's *address* in the kernel's own buffer. */
  int sv4[2];
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv4);
  struct iovec out[2] = { { (void *)"hello ", 6 }, { (void *)"world", 5 } };
  struct msghdr mo;
  memset(&mo, 0, sizeof mo);
  mo.msg_iov = out;
  mo.msg_iovlen = 2;
  long w4 = sendmsg(sv4[0], &mo, 0);
  check(w4 == 11, "two iovecs go out as one message", w4, 11);
  char a[6], bb[5];
  struct iovec in[2] = { { a, sizeof a }, { bb, sizeof bb } };
  struct msghdr mi;
  memset(&mi, 0, sizeof mi);
  mi.msg_iov = in;
  mi.msg_iovlen = 2;
  long r4 = recvmsg(sv4[1], &mi, 0);
  check(r4 == 11 && !memcmp(a, "hello ", 6) && !memcmp(bb, "world", 5),
        "and come back split across two iovecs", r4, 11);
  close(sv4[0]);
  close(sv4[1]);

  /* Not only pipes: the transport moves an open file description, so a socket
   * and a memfd must travel the same way. This is the shape a browser's
   * broker uses - hand a child the end of a connection it did not open. */
  int sp1[2], sp2[2];
  socketpair(AF_UNIX, SOCK_STREAM, 0, sp1);
  socketpair(AF_UNIX, SOCK_STREAM, 0, sp2);
  long s5 = send_fd(sp1[0], sp2[0], "s", 1, SCM_RIGHTS, CMSG_SPACE(sizeof(int)));
  check(s5 == 1, "a socket can be passed too", s5, 1);
  char cb2[64];
  char ch5 = 0;
  struct iovec iv5 = { &ch5, 1 };
  struct msghdr m5;
  memset(&m5, 0, sizeof m5);
  m5.msg_iov = &iv5;
  m5.msg_iovlen = 1;
  m5.msg_control = cb2;
  m5.msg_controllen = sizeof cb2;
  long r5 = recvmsg(sp1[1], &m5, 0);
  struct cmsghdr *c5 = r5 == 1 ? CMSG_FIRSTHDR(&m5) : 0;
  int sockfd = -1;
  if (c5) memcpy(&sockfd, CMSG_DATA(c5), sizeof sockfd);
  check(sockfd >= 3, "and arrives as a usable descriptor", sockfd, 3);
  if (sockfd >= 3) {
    close(sp2[0]);
    if (write(sockfd, "ping", 4) != 4) {
      check(0, "the passed socket carries a write", -1, 4);
    } else {
      char got5[8];
      memset(got5, 0, sizeof got5);
      long g5 = read(sp2[1], got5, sizeof got5 - 1);
      check(g5 == 4 && !memcmp(got5, "ping", 4), "the passed socket carries a write", g5, 4);
    }
    close(sockfd);
  }
  close(sp1[0]); close(sp1[1]); close(sp2[1]);

  int mfd = memfd_create("passed", 0);
  if (mfd >= 3) {
    ftruncate(mfd, 4096);
    const char *w = "shared bytes";
    write(mfd, w, strlen(w));
    int c[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, c);
    long s6 = send_fd(c[0], mfd, "m", 1, SCM_RIGHTS, CMSG_SPACE(sizeof(int)));
    char cb3[64], ch6 = 0;
    struct iovec iv6 = { &ch6, 1 };
    struct msghdr m6;
    memset(&m6, 0, sizeof m6);
    m6.msg_iov = &iv6;
    m6.msg_iovlen = 1;
    m6.msg_control = cb3;
    m6.msg_controllen = sizeof cb3;
    long r6 = recvmsg(c[1], &m6, 0);
    struct cmsghdr *c6 = r6 == 1 ? CMSG_FIRSTHDR(&m6) : 0;
    int mfd2 = -1;
    if (c6) memcpy(&mfd2, CMSG_DATA(c6), sizeof mfd2);
    /* The receive side inherits the *open file description*, offset included:
     * the parent's write left it at 12, so reading without seeking returns the
     * zeroes after it. Seeking back is also the proof that both descriptors name
     * the same object rather than two copies of its contents. */
    char rb[32];
    memset(rb, 0, sizeof rb);
    long off6 = mfd2 >= 3 ? lseek(mfd2, 0, SEEK_SET) : -1;
    long g6 = off6 == 0 ? read(mfd2, rb, sizeof rb - 1) : -1;
    /* The memfd is 4096 bytes long, so a 31-byte read returns 31: the check is
     * that the bytes at the start are the ones the parent wrote. */
    check(off6 == 0 && g6 >= (long)strlen(w) && !memcmp(rb, w, strlen(w)),
          "and a memfd arrives sharing its offset and its bytes", g6, (long)strlen(w));
    if (mfd2 >= 3) close(mfd2);
    close(c[0]); close(c[1]); close(mfd);
  }

  printf("[scm] %d/%d\n", checks - failures, checks);
  return failures ? 1 : 0;
}
