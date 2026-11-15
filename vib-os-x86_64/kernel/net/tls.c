/*
 * Omi - TLS 1.2 client over the kernel's own TCP
 *
 * The crypto and the protocol live in BearSSL (kernel/lib/bearssl). What is
 * here is the plumbing a library cannot do for us:
 *
 *   - trust anchors, decoded once from the DER blobs in roots.c
 *   - a reference date from the CMOS clock, so certificate validity means
 *     something
 *   - entropy: RDRAND when the CPU has it, otherwise TSC jitter
 *   - the blocking pump that moves engine records over tcp_send/tcp_recv
 *
 * TLS 1.2 only. BearSSL has no TLS 1.3, and every host Omi can reach still
 * negotiates 1.2 with ECDHE + AES-GCM or ChaCha20-Poly1305.
 */

#include "types.h"
#include "string.h"
#include "omi_net.h"
#include "omi_tls.h"
#include "omi_roots.h"
#include "bearssl.h"

#define TLS_MAX_ROOTS   32
/* Days from 0000-01-01 to 1970-01-01 in the proleptic Gregorian calendar.
 * br_x509_minimal_set_time() counts from year 0, not the Unix epoch. */
#define BR_EPOCH_SHIFT  719528u
#define TLS_DN_MAX      600
#define TLS_IOBUF_SIZE  16384

static br_ssl_client_context  s_cc;
static br_x509_minimal_context s_xc;
static unsigned char s_iobuf[TLS_IOBUF_SIZE];

static br_x509_decoder_context s_dec[TLS_MAX_ROOTS];
static br_x509_trust_anchor   s_ta[TLS_MAX_ROOTS];
static unsigned char          s_dn[TLS_MAX_ROOTS][TLS_DN_MAX];
static int s_ta_count = -1;          /* -1 = not loaded yet */

static int  s_last_err = 0;
static char s_peer_cn[96];


/* ------------------------------------------------------------------ */
/* port I/O: CMOS clock                                                */
/* ------------------------------------------------------------------ */

static uint8_t t_inb(uint16_t port) {
  uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static void t_outb(uint16_t port, uint8_t v) {
  __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static uint8_t cmos(uint8_t reg) { t_outb(0x70, reg); return t_inb(0x71); }
static int bcd(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }

/* Days since 1970-01-01 (Howard Hinnant's civil-days algorithm).
 * BearSSL wants days since 0 AD, which is this plus BR_EPOCH_SHIFT. */
static uint32_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint32_t)(era * 146097 + (int)doe - 719468);
}

/* Fills days + seconds-since-midnight from the RTC. Returns 0 if the clock
 * looks unset (bad BCD, year outside 2020..2099), so the caller can fall
 * back instead of failing every certificate as "not yet valid". */
static int rtc_now(uint32_t *days, uint32_t *secs) {
  for (int i = 0; i < 100000; i++)
    if (!(cmos(0x0A) & 0x80)) break;              /* update-in-progress */

  uint8_t sec = cmos(0x00), min = cmos(0x02), hr = cmos(0x04);
  uint8_t day = cmos(0x07), mon = cmos(0x08), yr = cmos(0x09);
  uint8_t regb = cmos(0x0B);

  int y, mo, d, h, mi, s;
  if (regb & 0x04) {                              /* binary mode */
    s = sec; mi = min; h = hr & 0x7F; d = day; mo = mon; y = yr;
  } else {                                        /* BCD */
    s = bcd(sec); mi = bcd(min); h = bcd(hr & 0x7F);
    d = bcd(day); mo = bcd(mon); y = bcd(yr);
  }
  if (!(regb & 0x02)) {                           /* 12h clock */
    int pm = (hr & 0x80) != 0;
    if (h == 12) h = 0;
    if (pm) h += 12;
  }

  y += 2000;                                       /* century: assume 20xx */
  if (y < 2020 || y > 2099) return 0;
  if (mo < 1 || mo > 12) return 0;
  if (d < 1 || d > 31) return 0;
  if (h > 23 || mi > 59 || s > 59) return 0;

  *days = days_from_civil(y, mo, d);
  *secs = (uint32_t)(h * 3600 + mi * 60 + s);
  return 1;
}

/* ------------------------------------------------------------------ */
/* entropy                                                             */
/* ------------------------------------------------------------------ */

static uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

/* RDRAND is not on every x86-64 CPU (qemu64 lacks it, and executing it
 * there is an illegal instruction — which is a triple fault, not an error we
 * can catch). Probe CPUID leaf 1, ECX bit 30, once. */
static int cpu_has_rdrand(void) {
  static int cached = -1;
  if (cached >= 0) return cached;
  uint32_t a = 1, b = 0, c = 0, d = 0;
  __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
  cached = (int)((c >> 30) & 1u);
  return cached;
}

static int rdrand64(uint64_t *out) {
  if (!cpu_has_rdrand()) return 0;
  uint64_t v = 0; unsigned char ok = 0;
  __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
  *out = v;
  return ok ? 1 : 0;
}

static void seed_bytes(unsigned char *out, int len) {
  uint64_t mix = rdtsc() ^ 0x9E3779B97F4A7C15ULL;

  /* With no RDRAND the only entropy in a kernel this small is timing jitter:
   * TSC deltas over a loop that does varying work. It is not ideal, and it is
   * why br_ssl_engine_inject_entropy is called with 32 bytes rather than 16. */
  for (int i = 0; i < 64; i++) {
    volatile int spin = (int)(rdtsc() & 0x3F);
    for (int k = 0; k < spin; k++) __asm__ volatile("pause");
    mix ^= rdtsc();
    mix *= 0x2545F4914F6CDD1DULL;
    mix ^= mix >> 29;
  }

  for (int i = 0; i < len; i += 8) {
    uint64_t r;
    if (!rdrand64(&r)) {
      for (int k = 0; k < 8; k++) { mix ^= rdtsc(); mix *= 0xBF58476D1CE4E5B9ULL; }
      r = mix ^ (rdtsc() << 17);
    }
    mix ^= r;
    mix *= 0xBF58476D1CE4E5B9ULL;
    r ^= mix;
    for (int k = 0; k < 8 && i + k < len; k++) out[i + k] = (unsigned char)(r >> (8 * k));
  }
}

/* ------------------------------------------------------------------ */
/* trust anchors                                                       */
/* ------------------------------------------------------------------ */

typedef struct { unsigned char *p; int used; int max; } dn_box_t;

static void dn_sink(void *ctx, const void *buf, size_t len) {
  dn_box_t *b = (dn_box_t *)ctx;
  const unsigned char *s = (const unsigned char *)buf;
  for (size_t i = 0; i < len; i++) {
    if (b->used < b->max) b->p[b->used++] = s[i];
  }
}

static int load_roots(void) {
  if (s_ta_count >= 0) return s_ta_count;

  int n = 0;
  for (int i = 0; i < omi_root_count && i < TLS_MAX_ROOTS; i++) {
    br_x509_decoder_context *d = &s_dec[i];
    dn_box_t box = { s_dn[i], 0, TLS_DN_MAX };

    br_x509_decoder_init(d, dn_sink, &box);
    br_x509_decoder_push(d, omi_roots[i].der, omi_roots[i].len);

    br_x509_pkey *pk = br_x509_decoder_get_pkey(d);
    if (!pk || br_x509_decoder_last_error(d) != 0) continue;   /* skip, not fatal */

    s_ta[n].dn.data = box.p;
    s_ta[n].dn.len = (size_t)box.used;
    s_ta[n].flags = BR_X509_TA_CA;
    s_ta[n].pkey = *pk;      /* points into s_dec[i].pkey_data, which we never reuse */
    n++;
  }
  s_ta_count = n;
  return n;
}

int tls_root_count(void) { return load_roots(); }

/* ------------------------------------------------------------------ */
/* engine pump                                                         */
/* ------------------------------------------------------------------ */

static int flush_sendrec(void) {
  size_t len = 0;
  unsigned char *buf = br_ssl_engine_sendrec_buf(&s_cc.eng, &len);
  if (buf == 0 || len == 0) return 0;

  size_t sent = 0;
  while (sent < len) {
    int n = tcp_send(buf + sent, (int)(len - sent));
    if (n <= 0) return -1;
    sent += (size_t)n;
  }
  br_ssl_engine_sendrec_ack(&s_cc.eng, sent);
  return 0;
}

/* One step of "make progress", so both the handshake and the data path can
 * share it. Returns 1 when there is app data ready to read, 0 when more work
 * is needed, or a negative TLS_E_* code. */
static int pump_once(int timeout_ms) {
  unsigned st = br_ssl_engine_current_state(&s_cc.eng);

  if (st & BR_SSL_CLOSED) {
    s_last_err = br_ssl_engine_last_error(&s_cc.eng);
    return TLS_E_HANDSHAKE;
  }
  if (st & BR_SSL_RECVAPP) return 1;               /* decrypted bytes waiting */

  /* Order matters. After the handshake the engine reports SENDAPP *and*
   * RECVREC at once (ready to accept data, ready to read a record). Returning
   * early on SENDAPP meant we never read the response at all. */
  if (st & BR_SSL_SENDREC) {
    if (flush_sendrec() < 0) return TLS_E_IO;
    return 0;
  }
  if (st & BR_SSL_RECVREC) {
    size_t len = 0;
    unsigned char *buf = br_ssl_engine_recvrec_buf(&s_cc.eng, &len);
    if (buf == 0 || len == 0) return TLS_E_STUCK;

    int n = tcp_recv(buf, (int)len, timeout_ms);
    if (n > 0) {
      br_ssl_engine_recvrec_ack(&s_cc.eng, (size_t)n);
      return 0;
    }
    return TLS_E_IO;                                /* idle timeout or close */
  }

  if (st & BR_SSL_SENDAPP) return 0;               /* nothing to do either way */
  return TLS_E_STUCK;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int tls_last_error(void) { return s_last_err; }
const char *tls_peer_name(void) { return s_peer_cn; }

const char *tls_error_text(int code) {
  switch (code) {
    case TLS_OK:            return "ok";
    case TLS_E_SETUP:       return "could not start the TLS engine";
    case TLS_E_IO:          return "the secure connection stalled or closed";
    case TLS_E_HANDSHAKE:   return "the server's handshake did not complete";
    case TLS_E_CERT:        return "the certificate did not check out";
    case TLS_E_STUCK:       return "the TLS engine stopped making progress";
    default:                return "TLS failed";
  }
}

int tls_start(const char *host) {
  s_last_err = 0;
  s_peer_cn[0] = 0;

  int nroots = load_roots();
  if (nroots <= 0) return TLS_E_SETUP;

  br_ssl_client_init_full(&s_cc, &s_xc, s_ta, (size_t)nroots);
  br_ssl_engine_set_versions(&s_cc.eng, BR_TLS12, BR_TLS12);
  br_ssl_engine_set_buffer(&s_cc.eng, s_iobuf, sizeof s_iobuf, 0);

  /* Reference date for certificate validity. A machine with a dead RTC would
   * otherwise reject every chain as not-yet-valid, so fall back to a date we
   * know is inside the current roots' windows. */
  uint32_t days = 0, secs = 0;
  if (!rtc_now(&days, &secs)) {
    days = days_from_civil(2026, 1, 1);
    secs = 0;
  }
  br_x509_minimal_set_time(&s_xc, days + BR_EPOCH_SHIFT, secs);

  /* Ask the X.509 engine to hand us the leaf CN for the status strip. */
  static br_name_element cn_el[1];
  /* DER OID for id-at-commonName, without the tag: length byte then 55 04 03. */
  static const unsigned char cn_oid[] = { 0x03, 0x55, 0x04, 0x03 };
  cn_el[0].oid = cn_oid;
  cn_el[0].buf = s_peer_cn;
  cn_el[0].len = sizeof s_peer_cn;
  cn_el[0].status = 0;
  br_x509_minimal_set_name_elements(&s_xc, cn_el, 1);

  unsigned char seed[32];
  seed_bytes(seed, sizeof seed);
  br_ssl_engine_inject_entropy(&s_cc.eng, seed, sizeof seed);

  if (!br_ssl_client_reset(&s_cc, host, 0)) {
    s_last_err = br_ssl_engine_last_error(&s_cc.eng);
    return TLS_E_SETUP;
  }

  /* Drive the handshake to completion. */
  for (int guard = 0; guard < 100000; guard++) {
    unsigned st = br_ssl_engine_current_state(&s_cc.eng);

    if (st & BR_SSL_CLOSED) {
      s_last_err = br_ssl_engine_last_error(&s_cc.eng);
      /* The certificate engine's codes are exactly 32..63. Anything else is a
       * protocol or transport failure and must not be blamed on the cert. */
      if (s_last_err >= BR_ERR_X509_OK && s_last_err <= 63) return TLS_E_CERT;
      return TLS_E_HANDSHAKE;
    }
    if (st & BR_SSL_SENDAPP) return TLS_OK;         /* handshake done */

    int r = pump_once(15000);
    if (r < 0) return r;
  }
  return TLS_E_STUCK;
}

int tls_send_all(const void *buf, int len) {
  const unsigned char *p = (const unsigned char *)buf;
  int off = 0;

  while (off < len) {
    size_t chunk = 0;
    unsigned char *dst = br_ssl_engine_sendapp_buf(&s_cc.eng, &chunk);
    if (dst == 0 || chunk == 0) {
      int r = pump_once(15000);
      if (r < 0) return r;
      continue;
    }
    size_t take = (size_t)(len - off);
    if (take > chunk) take = chunk;
    memcpy(dst, p + off, take);
    br_ssl_engine_sendapp_ack(&s_cc.eng, take);
    off += (int)take;

    /* force=1: emit the record now. With force=0 the engine waits for a full
     * buffer, and a request that fits in one record would never leave. */
    br_ssl_engine_flush(&s_cc.eng, 1);
    for (int spin = 0; spin < 8; spin++) {
      if (!(br_ssl_engine_current_state(&s_cc.eng) & BR_SSL_SENDREC)) break;
      if (flush_sendrec() < 0) return TLS_E_IO;
    }
  }
  return TLS_OK;
}

int tls_recv(void *buf, int max, int timeout_ms) {
  if (max <= 0) return 0;

  size_t len = 0;
  unsigned char *src = br_ssl_engine_recvapp_buf(&s_cc.eng, &len);
  if (src && len > 0) {
    size_t take = len;
    if (take > (size_t)max) take = (size_t)max;
    memcpy(buf, src, take);
    br_ssl_engine_recvapp_ack(&s_cc.eng, take);
    return (int)take;
  }

  for (int guard = 0; guard < 64; guard++) {
    int r = pump_once(timeout_ms);
    if (r < 0) return r;

    src = br_ssl_engine_recvapp_buf(&s_cc.eng, &len);
    if (src && len > 0) {
      size_t take = len;
      if (take > (size_t)max) take = (size_t)max;
      memcpy(buf, src, take);
      br_ssl_engine_recvapp_ack(&s_cc.eng, take);
      return (int)take;
    }

    unsigned st = br_ssl_engine_current_state(&s_cc.eng);
    if (st & BR_SSL_CLOSED) return 0;              /* clean end of stream */
  }
  return 0;
}

void tls_shutdown(void) {
  br_ssl_engine_close(&s_cc.eng);
  /* Push the close_notify out if the socket is still there; failures are not
   * interesting because we are tearing down anyway. */
  if (br_ssl_engine_current_state(&s_cc.eng) & BR_SSL_SENDREC) flush_sendrec();
  s_peer_cn[0] = 0;
}
