/* pipetest - pipes from ring 3, through descriptors rather than the kernel.
 *
 * The kernel self-test proves the ring buffer. This proves the part that only
 * exists once a program owns the ends: SYS_PIPE hands out two descriptors, the
 * read end reports readiness only when there is something to read, EOF arrives
 * when the last write end closes (including one closed out from under a dup),
 * and a write with no reader gets -EPIPE instead of waiting for ever.
 *
 * Everything here is done with the ordinary read/write/poll/close calls a real
 * program would use, so a failure names the syscall and not the harness.
 */

#define SYS_WRITE  1
#define SYS_EXIT   2
#define SYS_READ   24
#define SYS_CLOSE  26
#define SYS_DUP    27
#define SYS_POLL   33
#define SYS_PIPE   34

#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010

#define EBADF 9
#define EPIPE 32

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sys3(SYS_WRITE, 1, (long)s, slen(s)); }

typedef struct { int fd; unsigned short events; unsigned short revents; } pfd_t;

static int ok, n;
static void chk(int cond, const char *what) {
  n++;
  if (cond) { ok++; return; }
  out("  FAIL ");
  out(what);
  out("\n");
}

static int poll_one(int fd, unsigned short events) {
  pfd_t p;
  p.fd = fd;
  p.events = events;
  p.revents = 0;
  if (sys3(SYS_POLL, (long)&p, 1, 0) < 0) return -1;
  return p.revents;
}

static void num(char *m, int *o, long v) {
  char d[12]; int k = 0;
  if (v < 0) { m[(*o)++] = '-'; v = -v; }
  if (!v) d[k++] = '0';
  while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
  while (k) m[(*o)++] = d[--k];
}

int cmain(int argc, char **argv) {
  (void)argc; (void)argv;
  int fds[2];

  chk(sys3(SYS_PIPE, (long)fds, 0, 0) == 0, "pipe() creates a pair");
  chk(fds[0] >= 3 && fds[1] >= 3 && fds[0] != fds[1], "two usable, distinct fds");

  /* Nothing written yet, writer alive: not readable, and poll must not pretend. */
  chk((poll_one(fds[0], POLLIN) & POLLIN) == 0, "empty pipe is not readable");
  chk((poll_one(fds[1], POLLOUT) & POLLOUT) != 0, "empty pipe is writable");

  /* Write end is write-only, read end is read-only. */
  chk(sys3(SYS_READ, fds[1], (long)"xx", 2) == -EBADF, "read on write end -> EBADF");
  chk(sys3(SYS_WRITE, fds[0], (long)"xx", 2) == -EBADF, "write on read end -> EBADF");

  /* Round trip through descriptors. */
  chk(sys3(SYS_WRITE, fds[1], (long)"plumb", 5) == 5, "write 5 bytes");
  chk((poll_one(fds[0], POLLIN) & POLLIN) != 0, "written pipe is readable");
  {
    char b[16];
    chk(sys3(SYS_READ, fds[0], (long)b, 8) == 5, "read returns what was written");
    chk(b[0] == 'p' && b[4] == 'b', "bytes arrive in order");
  }

  /* Partial reads: bounded by what is there, in FIFO order. */
  {
    char b[16];
    chk(sys3(SYS_WRITE, fds[1], (long)"0123456789", 10) == 10, "write 10 bytes");
    chk(sys3(SYS_READ, fds[0], (long)b, 4) == 4, "short read takes 4");
    chk(b[0] == '0' && b[3] == '3', "first four are the first four");
    chk(sys3(SYS_READ, fds[0], (long)b, 16) == 6, "the rest comes next");
    chk(b[0] == '4' && b[5] == '9', "and in order");
  }

  /* A dup'd write end is the same end: closing the original must not produce
   * EOF while the dup is still open, and closing the dup last must. */
  {
    char b[8];
    int w2 = (int)sys3(SYS_DUP, fds[1], 0, 0);
    chk(w2 >= 3, "dup(write end)");
    chk(sys3(SYS_CLOSE, fds[1], 0, 0) == 0, "close the original write end");
    chk(sys3(SYS_WRITE, w2, (long)"z", 1) == 1, "the dup still writes");
    chk(sys3(SYS_READ, fds[0], (long)b, 8) == 1, "and its byte reads back");
    chk((poll_one(fds[0], POLLIN) & POLLHUP) == 0, "no HUP while a writer lives");
    chk(sys3(SYS_CLOSE, w2, 0, 0) == 0, "close the dup");
    chk((poll_one(fds[0], POLLIN) & POLLHUP) != 0, "closed writers -> HUP");
    chk(sys3(SYS_READ, fds[0], (long)b, 8) == 0, "read after close -> EOF, not -EAGAIN");
    chk(sys3(SYS_CLOSE, fds[0], 0, 0) == 0, "close the read end");
  }

  /* Writing into a pipe nobody will read: an error now, not a block for ever. */
  {
    int q[2];
    chk(sys3(SYS_PIPE, (long)q, 0, 0) == 0, "second pipe");
    chk(sys3(SYS_CLOSE, q[0], 0, 0) == 0, "close its read end");
    chk(sys3(SYS_WRITE, q[1], (long)"x", 1) == -EPIPE, "write with no reader -> EPIPE");
    chk((poll_one(q[1], POLLOUT) & POLLERR) != 0, "write end reports POLLERR");
    chk(sys3(SYS_CLOSE, q[1], 0, 0) == 0, "close its write end");
  }

  /* Closed descriptors stay closed. */
  chk(sys3(SYS_READ, fds[0], (long)"xx", 2) == -EBADF, "closed fd rejects read");

  {
    char m[40]; int o = 0;
    const char *pre = "[pipe3] ";
    while (*pre) m[o++] = *pre++;
    num(m, &o, ok);
    m[o++] = '/';
    num(m, &o, n);
    const char *post = " ok\n";
    while (*post) m[o++] = *post++;
    m[o] = 0;
    out(m);
    out(ok == n ? "[pipe3] VERDICT: pipes work through descriptors\n"
                : "[pipe3] VERDICT: something above failed\n");
  }

  sys3(SYS_EXIT, ok == n ? 0 : 1, 0, 0);
  return ok == n ? 0 : 1;
}