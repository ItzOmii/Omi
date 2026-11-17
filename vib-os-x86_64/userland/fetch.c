/* fetch.c - one HTTPS GET, in ring 3.
 *
 * tlsget proved the hard part (a real chain checked against pinned roots, SNI
 * sent, the date taken from the kernel's clock) but it stops reading as soon as
 * it has the headers, because what it was testing was the handshake. A browser
 * needs the body. Same recipe, different stopping rule.
 *
 * The three things in here that are easy to get wrong, each of which cost a
 * debugging session when they were wrong in tlsget:
 *
 *   1. SENDAPP is tested before RECVREC. Once the handshake completes the engine
 *      reports both at once; taking RECVREC first waits for a record that never
 *      comes, because the server is waiting for the request.
 *   2. A fresh connection gets a fresh context, zeroed. br_ssl_client_reset() on
 *      a context that already carried a handshake leaves "ready, nothing to
 *      send", which reads as success.
 *   3. The trust anchor's DN sink must be initialised {buffer, used=0, cap}. The
 *      other field order decodes every root to an empty DN, and an anchor with no
 *      name matches no chain - presenting as "not trusted" on a good chain.
 */
#include <bearssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/random.h>

#include "omi_roots.h"
#include "fetch.h"

#define SYS_RESOLVE 32   /* (host, ip_out[4]) -> 0 | -ENOENT */
#define SYS_YIELD    3
#define SYS_TICKS    4

#define MAX_ROOTS 32
#define DN_MAX    256
#define EPOCH_YEAR0_DAYS 719528

static br_ssl_client_context       cc;
static br_x509_minimal_context     xc;
static unsigned char               iobuf[BR_SSL_BUFSIZE_BIDI];
static br_x509_trust_anchor        ta[MAX_ROOTS];
static br_x509_decoder_context     dec[MAX_ROOTS];
static unsigned char               dnbuf[MAX_ROOTS][DN_MAX];
static br_name_element             cn_el[1];
static char                        peer_cn[96];

typedef struct { unsigned char *p; size_t used, cap; } dn_box_t;

static void dn_sink(void *ctx, const void *buf, size_t len) {
  dn_box_t *b = (dn_box_t *)ctx;
  if (b->used + len > b->cap) return;
  memcpy(b->p + b->used, buf, len);
  b->used += len;
}

static int load_roots(void) {
  int n = 0;
  for (int i = 0; i < omi_root_count && i < MAX_ROOTS; i++) {
    br_x509_decoder_context *d = &dec[i];
    dn_box_t box = { dnbuf[i], 0, DN_MAX };
    br_x509_decoder_init(d, dn_sink, &box);
    br_x509_decoder_push(d, omi_roots[i].der, omi_roots[i].len);
    br_x509_pkey *pk = br_x509_decoder_get_pkey(d);
    if (!pk || br_x509_decoder_last_error(d) != 0) continue;
    ta[n].dn.data = box.p;
    ta[n].dn.len = box.used;
    ta[n].flags = BR_X509_TA_CA;
    ta[n].pkey = *pk;
    n++;
  }
  return n;
}

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

/* Name resolution is the kernel's: a process asks, the kernel answers with four
 * bytes. Falling back to a pinned address would hide a broken resolver, so a
 * failure here is a failure. */
static int resolve_host(const char *host, char ip[16]) {
  /* The resolver answers -ENOENT until the lease has given it a server to ask,
   * and this program can be started before that. Waiting is the browser's job:
   * a client that assumes the network is up is a client that fails on the boot
   * where it is not, and looks like a broken resolver when it does. */
  unsigned char b[4] = { 0, 0, 0, 0 };
  long r = -1;
  unsigned long deadline = 0;
  for (int attempt = 0; attempt < 200; attempt++) {
    r = sys3(SYS_RESOLVE, (long)host, (long)b, 0);
    if (r == 0) break;
    if (!deadline) deadline = (unsigned long)sys3(SYS_TICKS, 0, 0, 0) + 600;  /* ~10 s */
    if ((unsigned long)sys3(SYS_TICKS, 0, 0, 0) > deadline) return -1;
    sys3(SYS_YIELD, 0, 0, 0);
    for (volatile long spin = 0; spin < 200000; spin++) { }
  }
  if (r != 0) return -1;
  snprintf(ip, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  return 0;
}

static int dial(const char *addr, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((unsigned short)port);
  inet_pton(AF_INET, addr, &sa.sin_addr);
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
  return fd;
}

static int handshake(int fd, const char *sni, const char **why) {
  int nroots = load_roots();
  if (nroots <= 0) { *why = "no roots"; return 0; }

  memset(&cc, 0, sizeof cc);
  memset(&xc, 0, sizeof xc);
  br_ssl_client_init_full(&cc, &xc, ta, (size_t)nroots);
  br_ssl_engine_set_versions(&cc.eng, BR_TLS12, BR_TLS12);
  br_ssl_engine_set_buffer(&cc.eng, iobuf, sizeof iobuf, 0);

  time_t now = time(NULL);
  br_x509_minimal_set_time(&xc, (uint32_t)((uint64_t)now / 86400) + EPOCH_YEAR0_DAYS,
                           (uint32_t)((uint64_t)now % 86400));

  memset(cn_el, 0, sizeof cn_el);
  memset(peer_cn, 0, sizeof peer_cn);
  static const unsigned char cn_oid[] = { 0x03, 0x55, 0x04, 0x03 };
  cn_el[0].oid = cn_oid;
  cn_el[0].buf = peer_cn;
  cn_el[0].len = sizeof peer_cn;
  br_x509_minimal_set_name_elements(&xc, cn_el, 1);

  unsigned char seed[32];
  if (getrandom(seed, sizeof seed, 0) != (long)sizeof seed) { *why = "no entropy"; return 0; }
  br_ssl_engine_inject_entropy(&cc.eng, seed, sizeof seed);

  br_ssl_client_reset(&cc, sni, 0);

  for (int guard = 0; guard < 400; guard++) {
    unsigned st = br_ssl_engine_current_state(&cc.eng);
    if (st & BR_SSL_CLOSED) { *why = "closed"; return 0; }
    if (st & BR_SSL_SENDREC) {
      size_t len = 0;
      unsigned char *b = br_ssl_engine_sendrec_buf(&cc.eng, &len);
      long w = write(fd, b, len);
      if (w <= 0) { *why = "write"; return 0; }
      br_ssl_engine_sendrec_ack(&cc.eng, (size_t)w);
    } else if (st & BR_SSL_SENDAPP) {
      return 1;
    } else if (st & BR_SSL_RECVREC) {
      size_t len = 0;
      unsigned char *b = br_ssl_engine_recvrec_buf(&cc.eng, &len);
      long r = read(fd, b, len);
      if (r <= 0) { *why = "eof"; return 0; }
      br_ssl_engine_recvrec_ack(&cc.eng, (size_t)r);
    } else {
      *why = "stalled";
      return 0;
    }
  }
  *why = "loop";
  return 0;
}

/* Read until the peer closes or the buffer is full. A page is not a header
 * block: there is no point at which the answer is "complete enough". */
static long exchange_all(int fd, const char *req, char *out, size_t cap) {
  size_t rlen = strlen(req), total = 0;

  size_t alen = 0;
  unsigned char *ab = br_ssl_engine_sendapp_buf(&cc.eng, &alen);
  if (!ab || alen < rlen) return -1;
  memcpy(ab, req, rlen);
  br_ssl_engine_sendapp_ack(&cc.eng, rlen);
  br_ssl_engine_flush(&cc.eng, 0);

  for (int guard = 0; guard < 20000 && total + 1 < cap; guard++) {
    unsigned st = br_ssl_engine_current_state(&cc.eng);
    if (st & BR_SSL_CLOSED) break;
    if (st & BR_SSL_SENDREC) {
      size_t len = 0;
      unsigned char *b = br_ssl_engine_sendrec_buf(&cc.eng, &len);
      long w = write(fd, b, len);
      if (w <= 0) break;
      br_ssl_engine_sendrec_ack(&cc.eng, (size_t)w);
    } else if (st & BR_SSL_RECVREC) {
      size_t len = 0;
      unsigned char *b = br_ssl_engine_recvrec_buf(&cc.eng, &len);
      long r = read(fd, b, len);
      if (r <= 0) break;
      br_ssl_engine_recvrec_ack(&cc.eng, (size_t)r);
    } else if (st & BR_SSL_RECVAPP) {
      size_t len = 0;
      unsigned char *b = br_ssl_engine_recvapp_buf(&cc.eng, &len);
      if (!b || !len) break;
      size_t take = len;
      if (take > cap - 1 - total) take = cap - 1 - total;
      memcpy(out + total, b, take);
      total += take;
      br_ssl_engine_recvapp_ack(&cc.eng, len);
    } else break;
  }
  out[total] = 0;
  return (long)total;
}

/* The Location header, when the answer carried one. A redirect is not a
 * failure and not a page: it is an instruction, and the caller has to be able
 * to read it before the headers are overwritten. */
static char last_location[256];

const char *https_get_location(void) { return last_location; }

long https_get(const char *host, int port, const char *path,
               char *out, size_t cap, const char **why, long *body_len,
               char *ip_used) {
  *why = "ok";
  last_location[0] = 0;
  if (body_len) *body_len = 0;
  if (ip_used) ip_used[0] = 0;

  char ip[16];
  if (resolve_host(host, ip) != 0) { *why = "resolve"; return -1; }
  if (ip_used) snprintf(ip_used, 16, "%s", ip);

  const char *sni = host;
  int fd = dial(ip, port);
  if (fd < 0) { *why = "connect"; return -1; }

  if (!handshake(fd, sni, why)) { close(fd); return -1; }

  char req[192];
  int n = snprintf(req, sizeof req,
                   "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n"
                   "User-Agent: omi-net\r\n\r\n", path, sni);
  if (n <= 0 || (size_t)n >= sizeof req) { *why = "request"; close(fd); return -1; }

  long got = exchange_all(fd, req, out, cap);
  close(fd);
  if (got < 0) { *why = "exchange"; return -1; }

  /* Split the answer. The status is worth reporting: a 301 is a page too, and
   * rendering its body as if it were the target is how a browser lies. */
  char *hdr_end = strstr(out, "\r\n\r\n");
  if (!hdr_end) { *why = "no-headers"; return -1; }
  long status = 0;
  if (strncmp(out, "HTTP/", 5) == 0) {
    const char *sp = strchr(out, ' ');
    if (sp) status = atoi(sp + 1);
  }
  /* Location, before the headers are overwritten by the body. */
  {
    const char *loc = strstr(out, "\r\nLocation:");
    if (loc && loc < hdr_end) {
      loc += 11;
      while (*loc == ' ') loc++;
      const char *e = loc;
      while (*e && *e != '\r' && *e != '\n' && (size_t)(e - loc) < sizeof last_location - 1) e++;
      size_t len = (size_t)(e - loc);
      memcpy(last_location, loc, len);
      last_location[len] = 0;
    }
  }
  long hlen = (hdr_end + 4) - out;
  long blen = got - hlen;
  memmove(out, hdr_end + 4, (size_t)blen + 1);   /* the body in place */
  if (body_len) *body_len = blen;
  return status;
}