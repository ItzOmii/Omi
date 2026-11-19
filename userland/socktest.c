/* socktest - the BSD socket ABI, exercised through musl's own calls.
 *
 * What is under test is not that syscalls exist but that a program written
 * against sockets works: `socket(AF_INET, SOCK_STREAM, 0)` gets a descriptor,
 * `connect` takes a `struct sockaddr_in` whose bytes are in network order,
 * `send`/`recv` are the calls a library actually makes (musl implements both as
 * `sendto`/`recvfrom` with a null address), and `recvmsg` scatters what arrives
 * across iovecs.
 *
 * The address is the interesting part. The native numbers took a bare u32, so a
 * translation that loads `sin_addr` as one would reverse the octets - and a
 * connect to 1.1.1.1 would still work, because all four are the same. The host
 * here is one.one.one.one (1.1.1.1) for the same reason anyone tests with it:
 * it answers on port 80 from anywhere.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define HOST "1.1.1.1"
#define PORT 80

static int checks, fails, skipped;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) { fails++; printf("[socktest] FAIL %s (got %ld want %ld, errno %d)\n",
                             what, got, want, errno); }
  else printf("[socktest] ok   %s\n", what);
}

/* Dial, and hand back a connected descriptor or -1. */
static int dial(struct sockaddr_in *sa) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)sa, sizeof *sa) != 0) { close(fd); return -1; }
  return fd;
}

int main(void) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons(PORT);
  if (inet_pton(AF_INET, HOST, &sa.sin_addr) != 1) {
    printf("[socktest] cannot parse %s\n", HOST);
    return 1;
  }

  /* ---- the descriptor ---- */
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  check(fd >= 3, "socket(AF_INET, SOCK_STREAM) is a descriptor", fd, 3);
  if (fd < 0) { printf("[socktest] cannot continue\n"); return 1; }

  int type = 0; socklen_t tl = sizeof type;
  check(getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) == 0 && type == SOCK_STREAM,
        "getsockopt(SO_TYPE) says SOCK_STREAM", type, SOCK_STREAM);

  int one = 1;
  check(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0,
        "setsockopt(TCP_NODELAY) is accepted", 0, 0);

  /* ---- connect through a real sockaddr ---- */
  check(connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0,
        "connect() to a sockaddr_in", 0, 0);

  const char *req = "GET / HTTP/1.0\r\nHost: one.one.one.one\r\n\r\n";
  long sent = (long)send(fd, req, strlen(req), 0);
  check(sent == (long)strlen(req), "send() writes the whole request", sent, (long)strlen(req));

  char buf[512];
  memset(buf, 0, sizeof buf);
  long n = (long)recv(fd, buf, sizeof buf - 1, 0);
  check(n > 0 && strncmp(buf, "HTTP/1.", 7) == 0, "recv() gets an HTTP status line", n, 1);
  printf("[socktest] peer said: %.32s\n", buf);

  /* ---- a second connection while the first is still open ----
   * The peers are external and they throttle: 1.1.1.1 and its neighbours
   * answer one connection from this machine and refuse the next, in the same
   * minute the first one worked. That is a sentence about the internet, not
   * about the socket table, so the second dial tries more than one peer and
   * says out loud when they all said no, instead of going red for somebody
   * else's rate limiter. */
  static const char *peers[] = { HOST, "1.0.0.1", "8.8.8.8" };
  int fd2 = -1;
  const char *peer = 0;
  for (unsigned i = 0; i < sizeof peers / sizeof peers[0] && fd2 < 0; i++) {
    struct sockaddr_in s2 = sa;
    inet_pton(AF_INET, peers[i], &s2.sin_addr);
    fd2 = dial(&s2);
    if (fd2 >= 0) peer = peers[i];
  }
  if (fd2 < 0) {
    skipped++;
    printf("[socktest] skip a second connection: the peers refused us (external, errno %d)\n",
           errno);
  } else {
    printf("[socktest] second connection to %s\n", peer);
    check(fd2 >= 3 && fd2 != fd, "a second socket connects while the first is open", fd2, 3);
  }
  if (fd2 >= 3) {
    /* sendmsg: the request split across two iovecs, which is how a library
     * writes a header and a body without joining them. */
    struct iovec out[2];
    out[0].iov_base = (void *)"GET / HTTP/1.0\r\n";
    out[0].iov_len  = 16;
    out[1].iov_base = (void *)"Host: one.one.one.one\r\n\r\n";
    /* 25, not 24: the second CRLF is four bytes, and the last byte is the one
     * that ends the header block. With 24 the request is incomplete and the
     * server simply waits - which looked like a receive timeout for as long as
     * sendmsg was dropping the second iovec instead of writing it. */
    out[1].iov_len  = 25;
    struct msghdr mo;
    memset(&mo, 0, sizeof mo);
    mo.msg_iov = out; mo.msg_iovlen = 2;
    long m = (long)sendmsg(fd2, &mo, 0);
    check(m == 41, "sendmsg() writes both iovecs", m, 41);

    /* recvmsg: and the answer read back across two. */
    char b1[64], b2[192];
    memset(b1, 0, sizeof b1); memset(b2, 0, sizeof b2);
    struct iovec in[2];
    in[0].iov_base = b1; in[0].iov_len = sizeof b1;
    in[1].iov_base = b2; in[1].iov_len = sizeof b2;
    struct msghdr mi;
    memset(&mi, 0, sizeof mi);
    mi.msg_iov = in; mi.msg_iovlen = 2;
    long r = (long)recvmsg(fd2, &mi, 0);
    check(r > 64 && strncmp(b1, "HTTP/1.", 7) == 0,
          "recvmsg() fills the first iovec and the second", r, 65);

    /* ---- shutdown, and the EOF behind it ---- */
    check(shutdown(fd2, SHUT_WR) == 0, "shutdown(SHUT_WR) closes the sending side", 0, 0);
    long total = 0;
    while (1) {
      long got = (long)recv(fd2, buf, sizeof buf, 0);
      if (got <= 0) break;
      total += got;
      if (total > 65536) break;
    }
    check(total > 0, "the reading side sees the rest, then EOF", total, 1);
    close(fd2);
  }
  close(fd);

  /* ---- what is refused, and why ---- */
  errno = 0;
  int dgram = socket(AF_INET, SOCK_DGRAM, 0);
  check(dgram == -1 && errno == EPROTONOSUPPORT,
        "SOCK_DGRAM is refused: no UDP in this transport", errno, EPROTONOSUPPORT);

  errno = 0;
  int v6 = socket(AF_INET6, SOCK_STREAM, 0);
  check(v6 == -1 && errno == EAFNOSUPPORT, "AF_INET6 is refused", errno, EAFNOSUPPORT);

  int raw = socket(AF_INET, SOCK_STREAM, 0);
  if (raw >= 3) {
    /* Binding is a name, and names are AF_UNIX: an IP address has no local port
     * to claim here, so the family is refused rather than reinterpreted. */
    errno = 0;
    check(bind(raw, (struct sockaddr *)&sa, sizeof sa) == -1 && errno == EAFNOSUPPORT,
          "bind() to an IP address is refused", errno, EAFNOSUPPORT);
    errno = 0;
    check(listen(raw, 4) == -1 && errno == EINVAL,
          "listen() on a socket with no name is refused", errno, EINVAL);
    /* SHUT_RD is the other half: the receive side goes, and a read says so at
     * once rather than waiting for a peer that is not coming. */
    check(shutdown(raw, SHUT_RD) == 0, "shutdown(SHUT_RD) drops the receive side", 0, 0);
    errno = 0;
    long after = (long)recv(raw, buf, sizeof buf, 0);
    check(after == 0, "and a read after it is end-of-file", after, 0);
    close(raw);
  }

  /* ---- entropy, which a TLS library needs before it can do anything ---- */
  unsigned char a[16], b[16];
  memset(a, 0, sizeof a); memset(b, 0, sizeof b);
  long g1 = (long)getrandom(a, sizeof a, 0);
  long g2 = (long)getrandom(b, sizeof b, 0);
  check(g1 == 16 && g2 == 16, "getrandom() fills the buffer", g1, 16);
  int all_zero = 1, differ = 0;
  for (int i = 0; i < 16; i++) {
    if (a[i]) all_zero = 0;
    if (a[i] != b[i]) differ = 1;
  }
  check(!all_zero, "and the bytes are not all zero", all_zero, 0);
  check(differ, "and two draws differ", differ, 1);

  printf("[socktest] %d/%d", checks - fails, checks);
  if (fails) printf(" %d FAILED", fails);
  else printf(" OK");
  /* A check that could not run is not a check that passed, and it is not a
   * failure either: it is the internet. The count says so out loud. */
  if (skipped) printf(" (%d skipped: the peer refused, not this machine)", skipped);
  printf("\n");
  return fails ? 1 : 0;
}
