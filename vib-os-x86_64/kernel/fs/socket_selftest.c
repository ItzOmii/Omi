/*
 * Backend substitution test.
 *
 * The claim the extraction makes is that nothing above the boundary knows what a
 * transport is. A compile-time guard proves fd.h does not *include* TCP; this
 * proves the layer does not *depend* on it, by running the same descriptor-level
 * sequence through a deterministic backend with a canned payload. The real
 * stack, driven from ring 3 by the boot clients, runs the same operations.
 *
 * Lifetime is asserted rather than inferred. Three separate questions, because a
 * single total answers none of them:
 *   - was every handle the backend allocated released exactly once?
 *   - was any handle released twice?
 *   - is the outstanding count actually zero, rather than merely back where it
 *     started (two leaks and a double free cancel out in a total)?
 *
 * A total is reported as well, and it is the least interesting of the four.
 */

#include "fd.h"
#include "socket.h"
#include "net_backend.h"
#include "errno.h"
#include "string.h"
#include "gui.h"        /* serial_puts, serial_puthex */

/* ---- a transport with no network in it --------------------------------- */

#define TB_LEN 17
static const char TB_DATA[TB_LEN] = "deterministic-42\n";

#define TB_CONNS 4

typedef struct {
  int alloc;        /* handed out by open() */
  int destroys;     /* close() calls landing on this handle */
} tb_conn_t;

static tb_conn_t tb_conns[TB_CONNS];
static int tb_alloc, tb_destroy_calls, tb_refused, tb_off, tb_bytes_out;

static void tb_reset(void) {
  for (int i = 0; i < TB_CONNS; i++) { tb_conns[i].alloc = 0; tb_conns[i].destroys = 0; }
  tb_alloc = tb_destroy_calls = tb_refused = tb_off = tb_bytes_out = 0;
}

static void *tb_open(const net_cfg_t *cfg, const uint8_t ip[4], uint16_t port) {
  (void)cfg; (void)ip; (void)port;
  if (tb_refused) return 0;
  for (int i = 0; i < TB_CONNS; i++) {
    if (tb_conns[i].alloc) continue;
    tb_conns[i].alloc = 1;
    tb_conns[i].destroys = 0;
    tb_alloc++;
    tb_off = 0;
    return &tb_conns[i];
  }
  return 0;
}

static int tb_send(void *h, const void *buf, int len) {
  (void)h; (void)buf;
  tb_bytes_out += len;
  return len;
}

static int tb_recv(void *h, void *buf, int max, int timeout_ms) {
  (void)h; (void)timeout_ms;
  int n = TB_LEN - tb_off;
  if (n > max) n = max;
  if (n <= 0) return -1;              /* peer closed: the layer turns this into EOF */
  memcpy(buf, TB_DATA + tb_off, (size_t)n);
  tb_off += n;
  return n;
}

/* Destruction is counted per handle, not in a global tally: a double free of one
 * connection and a missed free of another look identical in a total. */
static void tb_close(void *h) {
  tb_conn_t *c = (tb_conn_t *)h;
  if (c) c->destroys++;
  tb_destroy_calls++;
}

static unsigned tb_poll(void *h) {
  (void)h;
  if (tb_off >= TB_LEN) return OMI_POLLIN | OMI_POLLHUP;   /* drained: EOF */
  return OMI_POLLIN | OMI_POLLOUT;
}

static const struct socket_ops tb_ops = {
  .name = "test-deterministic",
  .open = tb_open, .send = tb_send, .recv = tb_recv,
  .close = tb_close, .poll = tb_poll,
};

/* ---- checks ------------------------------------------------------------ */

static int sock_ok, sock_total;

static void schk(const char *what, int cond) {
  sock_total++;
  if (cond) { sock_ok++; return; }
  serial_puts("[sock] FAILED: ");
  serial_puts(what);
  serial_puts("\n");
}

void sock_selftest(void) {
  const struct socket_ops *kernel_ops = socket_backend();
  fdtable_t *t = fdtable_new();
  if (!t) { serial_puts("[sock] no descriptor table\n"); return; }

  int live_before = sock_live();
  schk("the socket pool starts empty", live_before == 0);

  socket_backend_select(&tb_ops);
  tb_reset();

  int fd = fd_socket(t);
  schk("socket() yields a descriptor", fd >= 0);
  schk("an unconnected socket refuses to read", fd_read(t, fd, (void *)0, 0) == -ENOTCONN);

  schk("connect goes through the backend", fd_connect(t, fd, 0x0A000202u, 8899) == 0);
  schk("a second connect is -EISCONN", fd_connect(t, fd, 0x0A000202u, 8899) == -EISCONN);

  schk("write reaches the backend", fd_write(t, fd, "PING", 4) == 4 && tb_bytes_out == 4);
  schk("readiness is reported before draining", (fd_poll(t, fd) & OMI_POLLIN) != 0);

  /* dup before reading: the endpoint and its position must be shared. */
  int d2 = fd_dup(t, fd);
  schk("dup yields a second descriptor", d2 >= 0);
  uint8_t a[9], b[17];
  long r1 = fd_read(t, fd, a, 8);
  long r2 = fd_read(t, d2, b, sizeof b);
  schk("both descriptors read the same stream in order",
       r1 == 8 && r2 == TB_LEN - 8 && memcmp(a, TB_DATA, 8) == 0 &&
       memcmp(b, TB_DATA + 8, (size_t)r2) == 0);
  schk("EOF after the payload", fd_read(t, d2, b, sizeof b) == 0);
  schk("readiness turns into hangup when drained", (fd_poll(t, d2) & OMI_POLLHUP) != 0);

  /* The lifetime rule: one descriptor closing is not the socket closing. */
  schk("closing one descriptor is not a teardown", fd_close(t, fd) == 0 && tb_destroy_calls == 0);
  schk("the duplicate still works after the original closes", fd_read(t, d2, b, sizeof b) == 0);
  schk("the final close tears down exactly once", fd_close(t, d2) == 0 && tb_destroy_calls == 1);
  schk("a second close is refused, not a second teardown",
       fd_close(t, d2) == -EBADF && tb_destroy_calls == 1);

  tb_refused = 1;
  int fd3 = fd_socket(t);
  schk("a refused connect is an error, not a socket",
       fd3 >= 0 && fd_connect(t, fd3, 0x0A000202u, 8899) == -ECONNREFUSED);
  if (fd3 >= 0) fd_close(t, fd3);
  tb_refused = 0;

  /* ---- lifetime, asked three different ways ---- */
  int allocated = 0, destroyed = 0, twice = 0, undestroyed = 0;
  for (int i = 0; i < TB_CONNS; i++) {
    if (tb_conns[i].alloc) allocated++;
    if (tb_conns[i].destroys == 1) destroyed++;
    if (tb_conns[i].destroys > 1) twice++;
    if (tb_conns[i].alloc && tb_conns[i].destroys == 0) undestroyed++;
  }
  schk("every handle the backend opened was destroyed exactly once",
       allocated == 1 && destroyed == 1);
  schk("no handle was destroyed twice", twice == 0);
  schk("no handle was left undestroyed", undestroyed == 0);
  schk("destruction calls equal handles allocated",
       tb_destroy_calls == tb_alloc && tb_alloc == 1);

  fdtable_free(t);
  schk("the outstanding socket count is zero, not merely unchanged", sock_live() == 0);
  socket_backend_select(kernel_ops);
  schk("the kernel backend is selected again", socket_backend() == kernel_ops);

  char m[64];
  int o = 0;
  const char *pre = "[sock] ";
  while (*pre) m[o++] = *pre++;
  m[o++] = (char)('0' + (sock_total / 10) % 10);
  m[o++] = (char)('0' + sock_total % 10);
  const char *mid = " checks, ";
  while (*mid) m[o++] = *mid++;
  m[o++] = (char)('0' + (sock_ok / 10) % 10);
  m[o++] = (char)('0' + sock_ok % 10);
  const char *post = " ok (backend substitution)\n";
  while (*post) m[o++] = *post++;
  m[o] = 0;
  serial_puts(m);
}