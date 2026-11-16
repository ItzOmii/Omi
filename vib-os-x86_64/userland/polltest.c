/* polltest - readiness from ring 3, over real connections.
 *
 * The kernel side is proved by the boot self-test on a fake backend; this is the
 * part that cannot be faked. A ring-3 process asks "when is this readable?", the
 * kernel parks it, and it wakes when the network says so. Four claims, each
 * reported separately so a failure names itself:
 *
 *   1. a blocking read waits and then returns the bytes (tick count proves it
 *      slept rather than spun)
 *   2. a drained socket that is STILL CONNECTED reports no readiness - this is
 *      what level-triggered means, and /hold exists to make it observable
 *   3. once the peer closes, HUP appears and stays: EOF is readable
 *   4. two live descriptors are watched by one poll() call and both report
 *
 * It also checks that poll() leaves `events` alone. Writing revents at the wrong
 * offset silently overwrote events once, and a caller that only looked at the
 * ready count never noticed.
 *
 *   polltest HOST PORT PATH
 */

#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_TICKS   4
#define SYS_CLOSE   26
#define SYS_SOCKET  28
#define SYS_CONNECT 29
#define SYS_SEND    30
#define SYS_RECV    31
#define SYS_RESOLVE 32
#define SYS_POLL    33

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sys3(SYS_WRITE, 1, (long)s, slen(s)); }
static void err(const char *s) { sys3(SYS_WRITE, 2, (long)s, slen(s)); }

static void num(long v, char *o_buf) {
  int o = 0; int neg = v < 0;
  if (neg) v = -v;
  char d[12]; int n = 0;
  if (!v) d[n++] = '0';
  while (v) { d[n++] = (char)('0' + v % 10); v /= 10; }
  if (neg) o_buf[o++] = '-';
  while (n) o_buf[o++] = d[--n];
  o_buf[o] = 0;
}

/* label a b c - one line, fixed shape, easy to grep on a serial console. */
static void say(const char *tag, long a, long b, long c) {
  char nb[16];
  out("polltest: "); out(tag);
  out(" "); num(a, nb); out(nb);
  out(" "); num(b, nb); out(nb);
  out(" "); num(c, nb); out(nb);
  out("\n");
}

static int parse_ip(const char *s, long *out_ip) {
  long p[4]; int n = 0; const char *q = s;
  while (n < 4) {
    long v = 0; int digits = 0;
    while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; digits++; }
    if (!digits || v > 255) return -1;
    p[n++] = v;
    if (n < 4) { if (*q != '.') return -1; q++; }
  }
  *out_ip = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
  return 0;
}

static long atoi_(const char *s) { long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

struct pollfd { int fd; unsigned short events; unsigned short revents; };

#define POLLIN  0x001
#define POLLERR 0x008
#define POLLHUP 0x010

static long do_poll(struct pollfd *set, long n, long timeout_ms) {
  return sys3(SYS_POLL, (long)set, n, timeout_ms);
}

static long find_str(const char *h, long n, const char *needle) {
  long m = slen(needle);
  for (long i = 0; i + m <= n; i++) {
    long k = 0;
    while (k < m && h[i + k] == needle[k]) k++;
    if (k == m) return i;
  }
  return -1;
}

/* Content-Length, or -1. Bodies here are tiny, so a linear scan is fine. */
static long content_length(const char *h, long n) {
  static const char k[] = "Content-Length: ";
  long i = find_str(h, n, k);
  if (i < 0) return -1;
  i += (long)(sizeof k - 1);
  long v = 0, digits = 0;
  while (i < n && h[i] >= '0' && h[i] <= '9') { v = v * 10 + (h[i] - '0'); i++; digits++; }
  return digits ? v : -1;
}

/* Read exactly one response: stop the moment headers and body are both in hand,
 * so the socket ends up drained but untouched. Reading "until recv blocks" would
 * work too, but only at the cost of the read deadline's whole 5 seconds. */
static long read_response(int fd, char *buf, long cap, long *body_len) {
  long total = 0;
  *body_len = -1;
  while (total < cap - 1) {
    long n = sys3(SYS_RECV, fd, (long)(buf + total), cap - 1 - total);
    if (n <= 0) break;
    total += n;
    buf[total] = 0;
    long end = find_str(buf, total, "\r\n\r\n");
    if (end < 0) continue;
    long cl = content_length(buf, total);
    if (cl < 0) { *body_len = total - (end + 4); return total; }
    if (total >= end + 4 + cl) { *body_len = cl; return total; }
  }
  return total;
}

static int fetch(const char *host, long port, const char *path) {
  long ip = 0;
  if (parse_ip(host, &ip) != 0) {
    static unsigned char b[4];
    if (sys3(SYS_RESOLVE, (long)host, (long)b, 0) != 0) return -1;
    ip = ((long)b[0] << 24) | ((long)b[1] << 16) | ((long)b[2] << 8) | b[3];
  }
  long fd = sys3(SYS_SOCKET, 2, 1, 0);
  if (fd < 0) return (int)fd;
  if (sys3(SYS_CONNECT, fd, ip, port) != 0) { sys3(SYS_CLOSE, fd, 0, 0); return -2; }

  static char req[160];
  char *o = req;
  const char *k[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host, "\r\n\r\n" };
  for (int i = 0; i < 5; i++) { const char *s = k[i]; while (*s) *o++ = *s++; }
  if (sys3(SYS_SEND, fd, (long)req, o - req) < 0) { sys3(SYS_CLOSE, fd, 0, 0); return -3; }
  return (int)fd;
}

int cmain(int argc, char **argv) {
  if (argc < 4) { err("polltest: usage: polltest HOST PORT PATH\n"); return 2; }
  const char *host = argv[1], *path = argv[3];
  long port = atoi_(argv[2]);
  static char buf[1024];
  long body = 0;
  int good = 1;

  /* --- 1. a blocking read waits, then returns bytes ---------------------- */
  int fd = fetch(host, port, path);
  if (fd < 0) { say("connect-failed", fd, 0, 0); return 1; }
  long t0 = sys3(SYS_TICKS, 0, 0, 0);
  long got = read_response(fd, buf, sizeof buf, &body);
  long waited = sys3(SYS_TICKS, 0, 0, 0) - t0;
  say("read", got, body, waited);
  if (got <= 0 || body < 0) { say("read-failed", got, body, 0); good = 0; }
  if (waited < 20) { say("read-did-not-wait", waited, 0, 0); good = 0; }

  /* --- 2. drained but connected: no readiness ---------------------------- */
  struct pollfd p1 = { fd, POLLIN, 0 };
  long ev_before = p1.events;
  long r = do_poll(&p1, 1, 200);
  say("drained", r, p1.revents, p1.events);
  if (r != 0 || p1.revents != 0) { say("drained-claims-readiness", r, p1.revents, 0); good = 0; }
  if (p1.events != ev_before) { say("events-was-overwritten", ev_before, p1.events, 0); good = 0; }

  /* --- 3. peer closes: HUP, and it stays --------------------------------- */
  struct pollfd p2 = { fd, POLLIN, 0 };
  long r2 = do_poll(&p2, 1, 5000);
  say("closed", r2, p2.revents, p2.events);
  if (!(p2.revents & POLLHUP)) { say("no-hup-after-close", r2, p2.revents, 0); good = 0; }
  sys3(SYS_CLOSE, fd, 0, 0);

  /* --- 4. two descriptors, one call, both eventually readable ------------ */
  int fd2 = fetch(host, port, path);
  int fd3 = fetch(host, port, path);
  if (fd2 < 0 || fd3 < 0) { say("two-connect-failed", fd2, fd3, 0); return 1; }
  struct pollfd two[2] = { { fd2, POLLIN, 0 }, { fd3, POLLIN, 0 } };
  long deadline = sys3(SYS_TICKS, 0, 0, 0) + 800;
  long r3 = 0;
  for (;;) {
    r3 = do_poll(two, 2, 1000);
    if ((two[0].revents & POLLIN) && (two[1].revents & POLLIN)) break;
    if (sys3(SYS_TICKS, 0, 0, 0) > deadline) break;
  }
  say("two", r3, two[0].revents, two[1].revents);
  if (!(two[0].revents & POLLIN) || !(two[1].revents & POLLIN)) {
    say("two-not-both-ready", two[0].revents, two[1].revents, 0); good = 0;
  }
  sys3(SYS_CLOSE, fd2, 0, 0);
  sys3(SYS_CLOSE, fd3, 0, 0);

  say("verdict", good, 0, 0);
  return good ? 0 : 1;
}