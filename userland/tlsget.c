/* tlsget - a TLS 1.2 handshake, in a program, in user mode.
 *
 * This is the thing the socket ABI was for. BearSSL - the same library the
 * kernel's own browser uses - is compiled against musl here and given nothing
 * but the public POSIX calls: socket, connect, read, write, getrandom, time.
 * If the handshake completes, a certificate chain was verified against the
 * kernel's pinned roots, the transcript was hashed, and the keys were derived,
 * all inside this process.
 *
 * The hard part is that there is no name service in user mode, so the host has
 * to be an address. 1.1.1.1 is the convenient one: it answers on 443 from
 * anywhere, its certificate carries `one.one.one.one` as a DNS name, and the
 * chain ends at SSL.com Root CA ECC, which is one of the 26 pinned roots.
 * So the connection dials an address and the certificate is checked against a
 * name - which is exactly the arrangement a browser has to handle.
 *
 * The second connection deliberately asks for the wrong name. A TLS client that
 * cannot fail is not verifying anything, and a check that only ever passes is
 * worth nothing.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bearssl.h"
#include "omi_roots.h"

#define HOST_NAME "one.one.one.one"    /* the name the certificate must cover */
#define HOST_ADDR "1.1.1.1"            /* the address actually dialled */
#define HOST_PORT 443

/* 1970-01-01 as days since year 0, which is the origin br_x509_minimal wants. */
#define EPOCH_YEAR0_DAYS 719528

#define MAX_ROOTS 32
#define DN_MAX    256

static int checks, fails;
static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) { fails++; printf("[tlsget] FAIL %s (got %ld want %ld, errno %d)\n",
                             what, got, want, errno); }
  else printf("[tlsget] ok   %s\n", what);
}

static long hs_in, hs_out;
static br_ssl_client_context     cc;
static br_x509_minimal_context   xc;
static unsigned char             iobuf[BR_SSL_BUFSIZE_BIDI];
static br_x509_trust_anchor      ta[MAX_ROOTS];
static br_x509_decoder_context   dec[MAX_ROOTS];
static unsigned char             dnbuf[MAX_ROOTS][DN_MAX];
static br_name_element           cn_el[1];
static char                      peer_cn[96];

/* Field order matters: the initializer below says {buffer, 0, capacity}, so the
 * middle field is how much is used. With the other order every root decoded to an
 * empty DN, and an anchor with no name matches no chain - which is exactly how it
 * presented: "not trusted", on a chain that was fine. */
typedef struct { unsigned char *p; size_t used, cap; } dn_box_t;

static void dn_sink(void *ctx, const void *buf, size_t len) {
  dn_box_t *b = (dn_box_t *)ctx;
  if (b->used + len > b->cap) return;
  memcpy(b->p + b->used, buf, len);
  b->used += len;
}

/* Turn the pinned DER blobs into trust anchors: decode each one far enough to
 * get its DN and public key, which is all the X.509 engine compares against. */
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

/* BearSSL gives an error code, not a sentence. The codes worth being able to
 * read are the ones that mean "this chain did not check out", because that is the
 * difference between verifying and pretending to. */
static const char *err_name(int e) {
  switch (e) {
    case 0:                          return "ok";
    case BR_ERR_BAD_PARAM:           return "bad-param";
    case BR_ERR_BAD_STATE:           return "bad-state";
    case BR_ERR_BAD_VERSION:         return "bad-version";
    case BR_ERR_BAD_LENGTH:          return "bad-length";
    case BR_ERR_BAD_HANDSHAKE:       return "bad-handshake";
    case BR_ERR_BAD_SIGNATURE:       return "bad-signature";
    case BR_ERR_UNEXPECTED:          return "unexpected";
    case BR_ERR_IO:                  return "io";
    case BR_ERR_RECV_FATAL_ALERT:    return "peer-alert";
    case BR_ERR_X509_OK:             return "x509-ok";
    case BR_ERR_X509_BAD_SERVER_NAME:return "x509-name-mismatch";
    case BR_ERR_X509_NOT_TRUSTED:    return "x509-not-trusted";
    case BR_ERR_X509_EXPIRED:        return "x509-expired";
    case BR_ERR_X509_BAD_TIME:       return "x509-bad-time";
    case BR_ERR_X509_BAD_SIGNATURE:  return "x509-bad-signature";
    case BR_ERR_X509_EMPTY_CHAIN:    return "x509-empty-chain";
    case BR_ERR_X509_NOT_CA:         return "x509-not-ca";
    default:                         return "other";
  }
}

static int dial(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons(HOST_PORT);
  inet_pton(AF_INET, HOST_ADDR, &sa.sin_addr);
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
  return fd;
}

/* Move bytes in both directions until the handshake either completes or the
 * engine gives up. Returns 1 when the engine is ready to carry application
 * data, 0 otherwise, and the engine's error name in *why. */
static int handshake(int fd, const char *sni, const char **why) {
  int nroots = load_roots();
  if (nroots <= 0) { *why = "no roots"; return 0; }

  /* A fresh connection gets a fresh context. br_ssl_client_reset() clears the
   * I/O buffers as part of its own restart, and on a context that has already
   * carried one handshake that left "ready, nothing to send" for the second one
   * to read as success - which is how the negative test passed when it should
   * have failed. */
  hs_in = hs_out = 0;
  memset(&cc, 0, sizeof cc);
  memset(&xc, 0, sizeof xc);

  br_ssl_client_init_full(&cc, &xc, ta, (size_t)nroots);
  br_ssl_engine_set_versions(&cc.eng, BR_TLS12, BR_TLS12);
  br_ssl_engine_set_buffer(&cc.eng, iobuf, sizeof iobuf, 0);

  /* The date, from the kernel's wall clock through the ordinary C library. A
   * certificate is only valid between two dates, so this is not decoration. */
  time_t now = time(NULL);
  br_x509_minimal_set_time(&xc, (uint32_t)((uint64_t)now / 86400) + EPOCH_YEAR0_DAYS,
                           (uint32_t)((uint64_t)now % 86400));

  memset(cn_el, 0, sizeof cn_el);
  memset(peer_cn, 0, sizeof peer_cn);
  static const unsigned char cn_oid[] = { 0x03, 0x55, 0x04, 0x03 };  /* 55 04 03 */
  cn_el[0].oid = cn_oid;
  cn_el[0].buf = peer_cn;
  cn_el[0].len = sizeof peer_cn;
  br_x509_minimal_set_name_elements(&xc, cn_el, 1);

  /* Entropy, from the kernel's getrandom through musl. BearSSL wants 32 bytes
   * once and stretches them itself. */
  unsigned char seed[32];
  if (getrandom(seed, sizeof seed, 0) != (long)sizeof seed) { *why = "no entropy"; return 0; }
  br_ssl_engine_inject_entropy(&cc.eng, seed, sizeof seed);

  /* The name goes in as the server name, so it is sent as SNI and checked
   * against the certificate. Passing NULL here would skip the check, which is
   * the mistake this whole exercise is meant to avoid. */
  br_ssl_client_reset(&cc, sni, 0);

  for (int guard = 0; guard < 400; guard++) {
    unsigned st = br_ssl_engine_current_state(&cc.eng);
    if (st & BR_SSL_CLOSED) { *why = err_name(br_ssl_engine_last_error(&cc.eng));
      
      return 0; }

    if (st & BR_SSL_SENDREC) {
      size_t len = 0;
      unsigned char *b = br_ssl_engine_sendrec_buf(&cc.eng, &len);
      long w = write(fd, b, len);
      if (w > 0) hs_out += w;
      if (w <= 0) { *why = "write"; return 0; }
      br_ssl_engine_sendrec_ack(&cc.eng, (size_t)w);
    } else if (st & BR_SSL_SENDAPP) {
      /* Before RECVREC, and that is the whole trick. Once the handshake is done
       * the engine reports SENDAPP and RECVREC together: it is ready to carry a
       * request and ready to read a record. Taking the RECVREC branch first makes
       * this function wait for a record that will never come, because the server
       * is waiting for the request - which is precisely how it failed, as a clean
       * "closed, no error" after a long silence. */
      return 1;
    } else if (st & BR_SSL_RECVREC) {
      size_t len = 0;
      unsigned char *b = br_ssl_engine_recvrec_buf(&cc.eng, &len);
      long r = read(fd, b, len);
      if (r > 0) hs_in += r;
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

/* Send a request over the established tunnel and collect the answer. */
static long exchange(int fd, const char *req, char *out, size_t outcap) {
  size_t rlen = strlen(req), done = 0;
  size_t total = 0;

  memcpy(out, req, rlen < outcap - 1 ? rlen : outcap - 1);

  /* Queue as much of the request as the output buffer takes. */
  size_t alen = 0;
  unsigned char *ab = br_ssl_engine_sendapp_buf(&cc.eng, &alen);
  if (!ab || alen < rlen) return -1;
  memcpy(ab, req, rlen);
  br_ssl_engine_sendapp_ack(&cc.eng, rlen);
  br_ssl_engine_flush(&cc.eng, 0);

  for (int guard = 0; guard < 4000 && total + 1 < outcap; guard++) {
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
      if (take > outcap - 1 - total) take = outcap - 1 - total;
      memcpy(out + total, b, take);
      total += take;
      br_ssl_engine_recvapp_ack(&cc.eng, len);
      if (strstr(out, "\r\n\r\n")) break;      /* headers are enough */
    } else break;
  }
  out[total] = 0;
  return (long)total;
}

int main(void) {
  int nroots = load_roots();
  check(nroots > 0, "pinned roots decode into trust anchors", nroots, 1);

  /* ---- the handshake ---- */
  int fd = dial();
  check(fd >= 3, "TCP connection to port 443", fd, 3);
  if (fd < 3) return 1;

  const char *why = "?";
  int ready = handshake(fd, HOST_NAME, &why);
  check(ready, "TLS handshake completes with the chain verified", ready, 1);
  if (ready) {
    printf("[tlsget] peer CN: %s\n", peer_cn[0] ? peer_cn : "(none)");
    check(peer_cn[0] != 0, "the leaf certificate names a peer", peer_cn[0] ? 1 : 0, 1);

    static char page[3072];
    long got = exchange(fd,
        "GET / HTTP/1.0\r\nHost: " HOST_NAME "\r\nConnection: close\r\n\r\n",
        page, sizeof page);
    check(got > 0 && strncmp(page, "HTTP/1.", 7) == 0,
          "an HTTP request crosses the tunnel", got, 1);
    if (got > 0) printf("[tlsget] peer said: %.40s\n", page);
  } else {
    printf("[tlsget] handshake failed: %s\n", why);
  }
  close(fd);

  /* ---- and the case that must fail ---- */
  fd = dial();
  if (fd >= 3) {
    why = "?";
    /* A name the certificate cannot cover, and the endpoint is chosen for it:
     * asked about a host that does not exist, it still returns the cloudflare-dns
     * certificate, so the chain is good and only the name is wrong. "wrong.example.com"
     * was the first attempt and it passed - correctly - because this endpoint
     * answers such names with an example.com certificate and *.example.com covers
     * it. The test was wrong then, not the client. */
    int bad = handshake(fd, "zzz.invalid", &why);
    check(!bad, "a certificate that does not cover the name is refused", bad, 0);
    printf("[tlsget] refusal was: %s\n", why);
    close(fd);
  }

  /* ---- the clock a certificate is judged by ---- */
  time_t now = time(NULL);
  struct tm *g = gmtime(&now);
  check(g && g->tm_year + 1900 >= 2024 && g->tm_year + 1900 <= 2099,
        "the wall clock is a real date, not 1970", g ? g->tm_year + 1900 : 0, 2026);
  if (g) printf("[tlsget] clock says %04d-%02d-%02d\n",
                g->tm_year + 1900, g->tm_mon + 1, g->tm_mday);

  printf("[tlsget] %d/%d", checks - fails, checks);
  if (fails) printf(" %d FAILED\n", fails); else printf(" OK\n");
  return fails ? 1 : 0;
}