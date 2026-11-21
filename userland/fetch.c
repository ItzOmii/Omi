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

/* This libc has no strcasestr, and an implicit declaration makes the compiler
 * assume it returns int - which truncates a 64-bit pointer. Header detection
 * silently pointed into nowhere. Roll the search here instead. */
static char *omi_strcasestr(const char *hay, const char *needle) {
  if (!*needle) return (char *)hay;
  for (; *hay; hay++) {
    const char *h = hay, *n = needle;
    while (*h && *n) {
      char a = *h >= 'A' && *h <= 'Z' ? *h + 32 : *h;
      char b = *n >= 'A' && *n <= 'Z' ? *n + 32 : *n;
      if (a != b) break;
      h++; n++;
    }
    if (!*n) return (char *)hay;
  }
  return 0;
}
#define strcasestr omi_strcasestr
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
static unsigned char               stage[16384];   /* socket read-ahead */
static int                         stage_len, stage_off;
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
/* Names already looked up. A page asks for its document, its stylesheet and its
 * images on one host, and each fetch was resolving it again - the resolver's retry
 * ladder is seconds long when the first query is dropped, which is why a load
 * could sit for five seconds before a byte moved. The cache is small and expires:
 * it exists to stop asking the same question four times in one page, not to be a
 * resolver of its own. */
#define DNS_CACHE_MAX 8
#define DNS_TTL_TICKS 6000        /* ~60 s at the 100 Hz tick */
static struct { char host[128]; char ip[16]; unsigned long at; } dns_cache[DNS_CACHE_MAX];
static int dns_cache_n;

static int resolve_host(const char *host, char ip[16]) {
  unsigned long now = (unsigned long)sys3(SYS_TICKS, 0, 0, 0);
  for (int i = 0; i < dns_cache_n; i++) {
    if (strcmp(dns_cache[i].host, host)) continue;
    if (now - dns_cache[i].at > DNS_TTL_TICKS) break;
    snprintf(ip, 16, "%s", dns_cache[i].ip);
    return 0;
  }
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
  if (dns_cache_n < DNS_CACHE_MAX && strlen(host) < sizeof dns_cache[0].host) {
    snprintf(dns_cache[dns_cache_n].host, sizeof dns_cache[0].host, "%s", host);
    snprintf(dns_cache[dns_cache_n].ip, sizeof dns_cache[0].ip, "%s", ip);
    dns_cache[dns_cache_n].at = (unsigned long)sys3(SYS_TICKS, 0, 0, 0);
    dns_cache_n++;
  }
  return 0;
}

static int dial(const char *addr, int port) {
  long d0 = sys3(SYS_TICKS, 0, 0, 0);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  long d1 = sys3(SYS_TICKS, 0, 0, 0);
  if (fd < 0) return -1;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((unsigned short)port);
  inet_pton(AF_INET, addr, &sa.sin_addr);
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
  (void)d0; (void)d1;
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
      if (fetch_pump) fetch_pump();      /* the handshake can take 200ms; the window stays alive */
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
/* A connection worth keeping: the response said how long it is and did not ask
 * for the socket back. Without a length there is no way to know the body ended
 * except EOF, and EOF means the connection is gone by definition. */
int fetch_clipped;
int https_no_reuse;

/* Called while this module is waiting on the network. A fetch here is
 * synchronous and blocking, which is fine for a program with nothing else to do
 * and wrong for a browser: a window that cannot repaint until a body lands looks
 * dead, and a maximise during that time is a resize nobody acts on. The caller
 * installs something that drains its event queue; this module only has to call it
 * where it blocks. */
void (*fetch_pump)(void);

/* Measured, not guessed: how the body arrived. Printed with the phase timings so
 * a slow load can be told apart from a slow connection. */
long fetch_reads, fetch_bytes;
/* -1 when the response did not say how long it was, which is the case that ends
 * only at EOF: the browser then waits for the server to hang up. */
long fetch_want;

static long exchange_all(int fd, const char *req, char *out, size_t cap,
                         int *keep_out) {
  size_t rlen = strlen(req), total = 0;
  fetch_clipped = 0;
  fetch_reads = 0;
  stage_len = stage_off = 0;     /* a new connection is a new byte stream */
  fetch_bytes = 0;
  fetch_want = -1;

  size_t alen = 0;
  unsigned char *ab = br_ssl_engine_sendapp_buf(&cc.eng, &alen);
  if (!ab || alen < rlen) return -1;
  memcpy(ab, req, rlen);
  br_ssl_engine_sendapp_ack(&cc.eng, rlen);
  br_ssl_engine_flush(&cc.eng, 0);

  long want = -1;           /* total bytes to stop at, once the headers are in */
  int chunked = 0;          /* the body frames itself: stop at the end marker */
  *keep_out = 0;
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
      if (!b || !len) break;
      if (stage_off == stage_len) {
        /* BearSSL asks for a record header (5 bytes) and then for its body, and
         * each ask used to be its own read that blocked until those bytes were
         * there - so a server sending one TLS record per segment cost two
         * scheduler ticks per 1385 bytes, and a 121KB stylesheet became 133
         * records and 1.6 seconds of waiting for a link that could carry it in
         * a tenth of that. Reading into a staging buffer in 16KB bites lets a
         * header and its body come out of the same syscall. */
        stage_off = stage_len = 0;
        if (fetch_pump) fetch_pump();
        long got = read(fd, stage, sizeof stage);
        fetch_reads++;
        if (got <= 0) break;
        stage_len = (int)got;
        fetch_bytes += got;
      }
      size_t avail = (size_t)(stage_len - stage_off);
      size_t take = len < avail ? len : avail;
      memcpy(b, stage + stage_off, take);
      stage_off += (int)take;
      br_ssl_engine_recvrec_ack(&cc.eng, take);
    } else if (st & BR_SSL_RECVAPP) {
      size_t len = 0;
      unsigned char *b = br_ssl_engine_recvapp_buf(&cc.eng, &len);
      if (!b || !len) break;
      size_t take = len;
      if (take > cap - 1 - total) take = cap - 1 - total;
      memcpy(out + total, b, take);
      total += take;
      br_ssl_engine_recvapp_ack(&cc.eng, len);
      /* Once the headers are complete, decide how much body to expect. */
      if (want < 0) {
        out[total] = 0;
        char *he = strstr(out, "\r\n\r\n");
        if (he) {
          long hlen = (he + 4) - out;
          long cl = -1;
          const char *p = strcasestr(out, "\r\nContent-Length:");
          if (p && p < he) cl = atol(p + 17);
          int close_asked = 0;
          const char *cx = strcasestr(out, "\r\nConnection:");
          if (cx && cx < he) {
            const char *v = cx + 13;
            while (*v == ' ') v++;
            if (!strncasecmp(v, "close", 5)) close_asked = 1;
          }
          const char *te = strcasestr(out, "\r\nTransfer-Encoding:");
          if (te && te < he) {
            const char *v = te + 20;
            while (*v == ' ') v++;
            if (!strncasecmp(v, "chunked", 7)) chunked = 1;
          }
          /* Chunked framing delimits the body itself, so the socket can be kept
           * even though there is no length - and, more importantly, the read has
           * a reason to stop instead of waiting for a peer that is under no
           * obligation to close. */
          if (chunked && !close_asked) *keep_out = 1;
          if (cl >= 0 && !close_asked) { want = hlen + cl; *keep_out = 1; }
        }
      }
      if (want >= 0 && total >= want) break;
      if (chunked && total >= 7 && !memcmp(out + total - 7, "\r\n0\r\n\r\n", 7)) break;
    } else break;
  }
  out[total] = 0;
  /* The loop's own exit condition, not the length: if it stopped because there
   * was no room, the body was longer than this buffer. */
  fetch_clipped = (total + 1 >= cap);
  return (long)total;
}

/* The Location header, when the answer carried one. A redirect is not a
 * failure and not a page: it is an instruction, and the caller has to be able
 * to read it before the headers are overwritten. */
static char last_location[256];

const char *https_get_location(void) { return last_location; }

/* One connection, kept between navigations. A TLS handshake costs most of a
 * second on this machine, and every click used to pay for a new one. */
static int  ka_fd = -1;
static char ka_host[128];
static char ka_ip[16];
static int  ka_port;

/* HTTP/1.1 lets a server frame the body as chunks. Peel the framing off in place
 * so the renderer sees a document and not a wall of hex lengths. */
static long dechunk(char *b, long len) {
  long out = 0, i = 0;
  while (i < len) {
    long n = 0;
    int digits = 0;
    while (i < len && b[i] != '\r' && b[i] != '\n' && b[i] != ';') {
      char c = b[i];
      int d = (c >= '0' && c <= '9') ? c - '0'
            : (c >= 'a' && c <= 'f') ? c - 'a' + 10
            : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (d < 0) break;
      n = n * 16 + d; digits++; i++;
    }
    while (i < len && b[i] != '\n') i++;
    i++;
    if (!digits || n <= 0) break;
    if (i + n > len) n = len - i;
    if (out != i) memmove(b + out, b + i, (size_t)n);
    out += n; i += n;
    while (i < len && (b[i] == '\r' || b[i] == '\n')) i++;
  }
  b[out] = 0;
  return out;
}

long https_get(const char *host, int port, const char *path,
               char *out, size_t cap, const char **why, long *body_len,
               char *ip_used) {
  *why = "ok";
  last_location[0] = 0;
  if (body_len) *body_len = 0;
  if (ip_used) ip_used[0] = 0;

  char ip[16];
  int reused = 0;
  long t0 = sys3(SYS_TICKS, 0, 0, 0);
  if (!https_no_reuse && ka_fd >= 0 && ka_port == port && !strcmp(ka_host, host)) {
    /* Same host, live socket: no resolver, no dial, no handshake. */
    memcpy(ip, ka_ip, sizeof ip);
    reused = 1;
  } else {
    if (ka_fd >= 0) { close(ka_fd); ka_fd = -1; }
    if (resolve_host(host, ip) != 0) { *why = "resolve"; return -1; }
  }
  long t1 = sys3(SYS_TICKS, 0, 0, 0);
  if (ip_used) snprintf(ip_used, 16, "%s", ip);

  const char *sni = host;
  int fd = ka_fd;
  if (!reused) {
    /* A dial that fails is usually the network dropping the SYN, not the host
     * being gone - the same request a moment later works. One retry, after a
     * tick, turns a blank page into a page; a second failure is reported. */
    fd = dial(ip, port);
    if (fd < 0) {
      while ((long)sys3(SYS_TICKS, 0, 0, 0) == t1) { }   /* let a tick pass */
      fd = dial(ip, port);
      if (fd < 0) { *why = "connect"; return -1; }
    }
  }

  long t2 = sys3(SYS_TICKS, 0, 0, 0);
  if (!reused && !handshake(fd, sni, why)) { close(fd); return -1; }
  long t3 = sys3(SYS_TICKS, 0, 0, 0);

  char req[192];
  int n = snprintf(req, sizeof req,
                   "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n"
                   "User-Agent: omi-net\r\nAccept-Encoding: identity\r\n\r\n", path, sni);
  if (n <= 0 || (size_t)n >= sizeof req) { *why = "request"; close(fd); ka_fd = -1; return -1; }

  int keep = 0;
  long got = exchange_all(fd, req, out, cap, &keep);
  long t4 = sys3(SYS_TICKS, 0, 0, 0);
  if (keep && got > 0 && !https_no_reuse) {
    ka_fd = fd;
    snprintf(ka_host, sizeof ka_host, "%s", host);
    memcpy(ka_ip, ip, sizeof ka_ip);
    ka_port = port;
  } else {
    close(fd);
    ka_fd = -1;
  }
  /* Where the seconds go, in one line: resolve, dial, TLS handshake, body. A
   * "slow" browser is not a diagnosis, and guessing at which stage is wrong
   * about as often as it is right. */
  printf("[net] timing%s resolve=%ldms dial=%ldms tls=%ldms body=%ldms total=%ldms "
         "bytes=%ld reads=%ld%s\n",
         reused ? " (reused)" : "",
         (t1 - t0) * 10, (t2 - t1) * 10, (t3 - t2) * 10, (t4 - t3) * 10, (t4 - t0) * 10,
         fetch_bytes, fetch_reads, fetch_want < 0 ? " no-length" : "");
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
  {
    const char *te = strcasestr(out, "\r\nTransfer-Encoding:");
    if (te && te < hdr_end) {
      const char *v = te + 20;
      while (*v == ' ') v++;
      if (!strncasecmp(v, "chunked", 7)) blen = dechunk(hdr_end + 4, blen);
    }
  }
  memmove(out, hdr_end + 4, (size_t)blen + 1);   /* the body in place */
  if (body_len) *body_len = blen;
  return status;
}