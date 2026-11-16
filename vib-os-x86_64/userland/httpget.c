/* httpget - resolve, connect, ask, print.
 *
 * The first program to prove the pieces are actually joined: a name becomes an
 * address, an address becomes a connected socket, and what comes back is bytes
 * on stdout. Still no libc: syscall number in rax, args in rdi/rsi/rdx, result
 * in rax, errors as -errno.
 *
 *   httpget HOST [PORT] [PATH] [dup]
 */

#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_CLOSE   26
#define SYS_DUP     27
#define SYS_SOCKET  28
#define SYS_CONNECT 29
#define SYS_SEND    30
#define SYS_RECV    31
#define SYS_RESOLVE 32

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }

static char *app(char *d, const char *s) {
  while (*s) *d++ = *s++;
  *d = 0;
  return d;
}

static void err(const char *what, long r) {
  const char *m = "error";
  switch (-r) {
    case 2:   m = "name not found"; break;
    case 9:   m = "bad descriptor"; break;
    case 11:  m = "timed out"; break;
    case 32:  m = "broken pipe"; break;
    case 88:  m = "not a socket"; break;
    case 97:  m = "address family unsupported"; break;
    case 98:  m = "address in use"; break;
    case 106: m = "already connected"; break;
    case 107: m = "not connected"; break;
    case 111: m = "connection refused"; break;
  }
  sys3(SYS_WRITE, 2, (long)"httpget: ", 9);
  sys3(SYS_WRITE, 2, (long)what, slen(what));
  sys3(SYS_WRITE, 2, (long)": ", 2);
  sys3(SYS_WRITE, 2, (long)m, slen(m));
  sys3(SYS_WRITE, 2, (long)"\n", 1);
}

/* 10.0.2.2 -> 0x0A000202. Returns 0 when the string was a literal address. */
static int parse_ip(const char *s, long *out) {
  long parts[4];
  int n = 0;
  const char *p = s;
  while (n < 4) {
    long v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; digits++; }
    if (!digits || v > 255) return -1;
    parts[n++] = v;
    if (n < 4) { if (*p != '.') return -1; p++; }
  }
  if (*p) return -1;
  *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
  return 0;
}

int cmain(long argc, char **argv) {
  if (argc < 2) {
    sys3(SYS_WRITE, 2, (long)"usage: httpget HOST [PORT] [PATH]\n", 34);
    return 2;
  }
  const char *host = argv[1];
  long port = 80;
  if (argc > 2) {
    port = 0;
    for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) port = port * 10 + (*p - '0');
  }
  const char *path = argc > 3 ? argv[3] : "/";

  long ip = 0;
  if (parse_ip(host, &ip) != 0) {
    unsigned char b[4] = { 0, 0, 0, 0 };
    long r = sys3(SYS_RESOLVE, (long)host, (long)b, 0);
    if (r < 0) { err("resolve", r); return 1; }
    ip = ((long)b[0] << 24) | ((long)b[1] << 16) | ((long)b[2] << 8) | b[3];
    char line[96];
    char *d = app(line, "httpget: ");
    d = app(d, host);
    d = app(d, " is ");
    for (int i = 3; i >= 0; i--) {
      unsigned v = (unsigned)((ip >> (i * 8)) & 0xFF);
      if (v >= 100) *d++ = (char)('0' + v / 100);
      if (v >= 10) *d++ = (char)('0' + (v / 10) % 10);
      *d++ = (char)('0' + v % 10);
      if (i) *d++ = '.';
    }
    *d++ = '\n';
    *d = 0;
    sys3(SYS_WRITE, 2, (long)line, d - line);
  }

  long fd = sys3(SYS_SOCKET, 2, 1, 0);
  if (fd < 0) { err("socket", fd); return 1; }

  long r = sys3(SYS_CONNECT, fd, ip, port);
  if (r < 0) { err("connect", r); sys3(SYS_CLOSE, fd, 0, 0); return 1; }

  /* `dup` mode exercises one ABI property: the connection belongs to the open
   * file description, not to the descriptor number. Duplicating the socket and
   * closing the original must leave the connection alive - the duplicate keeps
   * talking - and only the last close may tear it down. The kernel logs exactly
   * one "conn down" for the port; two would be a double close. */
  if (argc > 4 && argv[4][0] == 'd' && argv[4][1] == 'u' && argv[4][2] == 'p') {
    long d2 = sys3(SYS_DUP, fd, 0, 0);
    if (d2 < 0) { err("dup", d2); sys3(SYS_CLOSE, fd, 0, 0); return 1; }
    sys3(SYS_CLOSE, fd, 0, 0);          /* original descriptor gone, conn alive */
    fd = d2;
    char m[48]; char *q = m;
    const char *pre = "httpget: using dup fd ";
    while (*pre) *q++ = *pre++;
    *q++ = (char)('0' + (int)(fd % 10));
    *q++ = '\n';
    sys3(SYS_WRITE, 2, (long)m, q - m);
  }

  char req[256];
  char *d = app(req, "GET ");
  d = app(d, path);
  d = app(d, " HTTP/1.0\r\nHost: ");
  d = app(d, host);
  d = app(d, "\r\nConnection: close\r\n\r\n");
  long reqlen = d - req;

  r = sys3(SYS_SEND, fd, (long)req, reqlen);
  if (r < 0) { err("send", r); sys3(SYS_CLOSE, fd, 0, 0); return 1; }

  char buf[512];
  long total = 0;
  for (;;) {
    long n = sys3(SYS_RECV, fd, (long)buf, sizeof buf);
    if (n > 0) {
      if (sys3(SYS_WRITE, 1, (long)buf, n) != n) break;
      total += n;
      continue;
    }
    if (n == 0) break;                      /* peer closed: done */
    err("recv", n);
    break;
  }
  sys3(SYS_CLOSE, fd, 0, 0);

  char line[96];
  d = app(line, "\nhttpget: ");
  char digits[16]; int k = 0;
  long t = total;
  if (!t) digits[k++] = '0';
  while (t) { digits[k++] = (char)('0' + t % 10); t /= 10; }
  while (k) *d++ = digits[--k];
  d = app(d, " bytes\n");
  sys3(SYS_WRITE, 1, (long)line, d - line);
  return total > 0 ? 0 : 1;
}
