/*
 * Omi - TCP client + DNS resolver (see include/omi_net.h)
 *
 * One connection in globals was the limit: a second socket could only exist by
 * tearing down the first. State now lives in tcp_conn_t, one per connection,
 * and every inbound segment is demultiplexed by 4-tuple into the connection
 * that owns it. A socket that is not currently polling still collects its
 * bytes, because whoever holds the NIC hands each frame to the right owner.
 *
 * What is still global: the interface config and the single e1000 receive
 * queue. That is honest - one NIC, one poller at a time (rx_busy).
 */

#include "types.h"
#include "string.h"
#include "net.h"
#include "omi_net.h"
#include "net_backend.h"
#include "e1000.h"
#include "task.h"

extern void serial_puts(const char *s);

#define ETH_IPV4 0x0800
#define ETH_LEN 14
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* Eight concurrent connections is enough to prove the design and to run a
 * couple of programs at once; the receive buffer is what costs memory. */
#define TCP_MAX_CONN 8
#define TCP_RX_MAX   4096

typedef struct tcp_conn {
  net_cfg_t cfg;
  uint8_t  dip[4];
  uint8_t  dmac[6];
  uint16_t sport, dport;      /* our port, theirs */
  uint32_t seq, ack;
  int      open;              /* handshake completed */
  int      peer_closed;       /* FIN or RST seen */
  int      used;
  uint8_t  rx[TCP_RX_MAX];
  int      rx_len;
  /* Readiness callback. The backend reports state changes to whoever owns the
   * endpoint without knowing what a socket is; NULL until somebody binds. */
  void   (*cb)(void *, unsigned);
  void    *ctx;
} tcp_conn_t;

static void conn_signal(tcp_conn_t *c);

static tcp_conn_t conns[TCP_MAX_CONN];
static uint32_t port_seed = 0x5A17;

/* ---- pacer ------------------------------------------------------------
   Timeouts are measured off the PIT's channel-0 counter. The accumulator used
   to be a pair of globals, which two tasks waiting at once would reset under
   each other - so it lives on the waiter's stack now. */
typedef struct pacer { uint16_t last; uint32_t ticks; } pacer_t;

static inline void io_w(uint16_t port, uint8_t v) {
  __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port));
}
static inline uint8_t io_r(uint16_t port) {
  uint8_t v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}
static uint16_t pit_read(void) {
  io_w(0x43, 0x00);
  uint8_t lo = io_r(0x40);
  uint8_t hi = io_r(0x40);
  return (uint16_t)(((uint16_t)hi << 8) | lo);
}
static void pace_begin(pacer_t *p) {
  p->last = pit_read();
  p->ticks = 0;
}
/* Accumulate raw PIT ticks and divide the total. Dividing each delta loses
   1 ms to truncation on every call, and this is polled thousands of times a
   second, so a "2 second" timeout never expired and connects hung forever. */
static uint32_t pace_ms(pacer_t *p) {
  uint16_t n = pit_read();
  p->ticks += (uint16_t)(p->last - n);
  p->last = n;
  return p->ticks / 1193u;
}
static void pace_yield(void) {
  task_yield();          /* let the other programs make progress too */
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t tcp_csum(const tcp_conn_t *c, const uint8_t *seg, int len) {
  uint32_t sum = 0;
  for (int i = 0; i < 4; i += 2) sum += get16(c->cfg.ip + i);
  for (int i = 0; i < 4; i += 2) sum += get16(c->dip + i);
  sum += IP_PROTO_TCP;
  sum += (uint32_t)len;
  int i = 0;
  for (; i + 1 < len; i += 2) sum += get16(seg + i);
  if (i < len) sum += ((uint32_t)seg[i] << 8);
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

static void pick_route(const net_cfg_t *cfg, const uint8_t ip[4], uint8_t mac[6]) {
  int on_link = 1;
  for (int i = 0; i < 4; i++)
    if ((cfg->ip[i] & cfg->mask[i]) != (ip[i] & cfg->mask[i])) on_link = 0;
  if (on_link) {
    if (net_arp_resolve(cfg, ip, mac)) return;
  }
  memcpy(mac, cfg->gw_mac, 6);
}

/* One segment on one connection. Nothing here reads another connection's
 * state, which is the whole point of the refactor. */
static int conn_seg(tcp_conn_t *c, uint8_t flags, const uint8_t *data, int dlen) {
  uint8_t f[1600];
  uint8_t me[6];
  if (dlen > 1200) return -1;
  e1000_mac(me);
  for (int i = 0; i < 6; i++) f[i] = c->dmac[i];
  for (int i = 0; i < 6; i++) f[6 + i] = me[i];
  put16(f + 12, ETH_IPV4);

  uint8_t *ip = f + ETH_LEN;
  int iplen = 40 + dlen;
  ip[0] = 0x45; ip[1] = 0;
  put16(ip + 2, (uint16_t)iplen);
  put16(ip + 4, 0);
  put16(ip + 6, 0x4000);
  ip[8] = 64; ip[9] = IP_PROTO_TCP;
  put16(ip + 10, 0);
  memcpy(ip + 12, c->cfg.ip, 4);
  memcpy(ip + 16, c->dip, 4);
  put16(ip + 10, net_csum(ip, 20));

  uint8_t *t = ip + 20;
  put16(t, c->sport);
  put16(t + 2, c->dport);
  put32(t + 4, c->seq);
  put32(t + 8, c->ack);
  t[12] = 0x50;
  t[13] = flags;
  put16(t + 14, (uint16_t)TCP_RX_MAX);
  put16(t + 16, 0);
  put16(t + 18, 0);
  if (dlen && data) memcpy(t + 20, data, (size_t)dlen);
  put16(t + 16, tcp_csum(c, t, 20 + dlen));
  int rc = e1000_send(f, (uint16_t)(ETH_LEN + iplen));
  if (rc != 0) {
    char m[40]; int o = 0;
    const char *pre = "[tcp] seg tx failed ";
    while (*pre) m[o++] = *pre++;
    m[o++] = (char)('0' + (int)(c - conns));
    m[o++] = '\n'; m[o] = 0;
    serial_puts(m);
  }
  return rc;
}

static tcp_conn_t *find_conn(uint16_t our_port, uint16_t their_port, const uint8_t *their_ip) {
  for (int i = 0; i < TCP_MAX_CONN; i++) {
    tcp_conn_t *c = &conns[i];
    if (!c->used || c->sport != our_port || c->dport != their_port) continue;
    if (memcmp(c->dip, their_ip, 4) == 0) return c;
  }
  return 0;
}

/* Every inbound TCP frame goes through here, whoever polled for it. Handing it
 * to the owning connection instead of dropping it is what makes two clients at
 * once possible. */
static void tcp_dispatch(const uint8_t *f, int n) {
  if (n < ETH_LEN + 40) return;
  if (get16(f + 12) != ETH_IPV4) return;
  const uint8_t *ip = f + ETH_LEN;
  if ((ip[0] >> 4) != 4 || ip[9] != IP_PROTO_TCP) return;
  int ihl = (ip[0] & 0x0F) * 4;
  const uint8_t *t = ip + ihl;
  if (n < ETH_LEN + ihl + 20) return;
  int payload = get16(ip + 2) - ihl - ((t[12] >> 4) * 4);
  if (payload < 0) return;

  tcp_conn_t *c = find_conn(get16(t + 2), get16(t), ip + 12);
  if (!c) return;

  uint8_t flags = t[13];
  uint32_t seq = get32(t + 4);

  if (flags & TCP_RST) { c->open = 0; c->peer_closed = 1; conn_signal(c); return; }

  if ((flags & TCP_SYN) && (flags & TCP_ACK) && !c->open) {
    c->ack = seq + 1;
    c->seq += 1;                 /* our SYN consumed one sequence number */
    c->open = 1;
    conn_seg(c, TCP_ACK, 0, 0);
    conn_signal(c);              /* POLLOUT appears: a send can progress now */
    return;
  }

  if (payload > 0) {
    if (seq != c->ack) { conn_seg(c, TCP_ACK, 0, 0); return; }   /* duplicate or out of order */
    int room = TCP_RX_MAX - c->rx_len;
    if (payload > room) { conn_seg(c, TCP_ACK, 0, 0); return; }  /* no room: do not ACK, peer retries */
    memcpy(c->rx + c->rx_len, t + ((t[12] >> 4) * 4), (size_t)payload);
    c->rx_len += payload;
    c->ack += (uint32_t)payload;
    conn_seg(c, TCP_ACK, 0, 0);
    conn_signal(c);              /* data is readable: wake whoever waits */
  }

  if (flags & TCP_FIN) {
    c->ack = seq + (uint32_t)(payload > 0 ? payload : 0) + 1;
    conn_seg(c, TCP_ACK, 0, 0);
    c->peer_closed = 1;
    conn_signal(c);              /* EOF is readable, and it never un-readies */
  }
}

static void conn_log(const char *what, tcp_conn_t *c) {
  char msg[56];
  int o = 0;
  const char *pre = "[tcp] conn ";
  while (*pre) msg[o++] = *pre++;
  while (*what) msg[o++] = *what++;
  msg[o++] = ' ';
  msg[o++] = (char)('0' + (int)(c - conns));
  const char *mid = " port ";
  while (*mid) msg[o++] = *mid++;
  char d[8]; int k = 0; uint16_t v = c->sport;
  if (!v) d[k++] = '0';
  while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
  while (k) msg[o++] = d[--k];
  /* destination too: "who connected where" is the first question asked of a
     log line like this, and the answer is never derivable afterwards. */
  msg[o++] = ' '; msg[o++] = '-'; msg[o++] = '>'; msg[o++] = ' ';
  for (int a = 0; a < 4; a++) {
    char db[4]; int dk = 0; uint8_t b = c->dip[a];
    if (!b) db[dk++] = '0';
    while (b) { db[dk++] = (char)('0' + b % 10); b /= 10; }
    while (dk) msg[o++] = db[--dk];
    msg[o++] = a == 3 ? ':' : '.';
  }
  k = 0; v = c->dport;
  if (!v) d[k++] = '0';
  while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
  while (k) msg[o++] = d[--k];
  msg[o++] = '\n'; msg[o] = 0;
  serial_puts(msg);
}

void tcp_input(const uint8_t *f, int n) { tcp_dispatch(f, n); }



static volatile int rx_busy;

/* One frame, then get out. If another task is already inside the NIC we do
 * nothing: its pump dispatches our frames for us. */
static void tcp_pump(void) {
  if (rx_busy) return;
  rx_busy = 1;
  uint8_t r[1600];
  int n = e1000_poll_recv(r, sizeof r);
  if (n > 0 && n < (int)sizeof r) tcp_dispatch(r, n);
  rx_busy = 0;
}

/* The pump is a service, not a favour: it is one frame at a time, callable from
 * anywhere, and safe to call while a connection call is also inside the NIC. */
void tcp_service(void) { tcp_pump(); }

/* ---- the connection API ------------------------------------------------ */

tcp_conn_t *tcp_conn_open(const net_cfg_t *cfg, const uint8_t ip[4], uint16_t port) {
  if (!cfg || !cfg->valid) return 0;
  tcp_conn_t *c = 0;
  for (int i = 0; i < TCP_MAX_CONN; i++) if (!conns[i].used) { c = &conns[i]; break; }
  if (!c) return 0;

  memset(c, 0, sizeof *c);
  c->used = 1;
  c->cfg = *cfg;
  memcpy(c->dip, ip, 4);
  c->dport = port;

  for (int attempt = 0; attempt < 3 && !c->open; attempt++) {
    /* A fresh 5-tuple per retry: a retried SYN on the same port is often
       dropped by the middlebox that ate the first one, and some hosts
       (neverssl among them) take seconds to answer at all. */
    port_seed = port_seed * 1103515245u + 12345u;
    c->sport = (uint16_t)(49152u + ((port_seed >> 16) % 14000u));
    pick_route(cfg, ip, c->dmac);
    c->seq = port_seed ^ 0x9E3779B9u;
    c->ack = 0;

    if (conn_seg(c, TCP_SYN, 0, 0) != 0) break;

    pacer_t p;
    const uint32_t budget = attempt == 0 ? 1500u : (attempt == 1 ? 3500u : 5000u);
    pace_begin(&p);
    while (pace_ms(&p) < budget && !c->open) {
      tcp_pump();
      if (!c->open) pace_yield();
    }
  }

  if (!c->open) { c->used = 0; return 0; }

  /* One line per connection, annotated with the slot: with several in flight
   * the log has to say which connection is which, or a mixed-up stream is
   * unfindable. Slot numbers also make "how many at once" checkable. */
  conn_log("up", c);
  return c;
}

int tcp_conn_send(tcp_conn_t *c, const void *buf, int len) {
  if (!c || !c->open) return -1;
  const uint8_t *p = (const uint8_t *)buf;
  int sent = 0;
  while (sent < len) {
    int chunk = len - sent;
    if (chunk > 1000) chunk = 1000;
    if (conn_seg(c, (uint8_t)(TCP_ACK | TCP_PSH), p + sent, chunk) != 0) {
      char m[44]; int o = 0;
      const char *pre = "[tcp] short write, got ";
      while (*pre) m[o++] = *pre++;
      char d[8]; int k = 0; int v = sent;
      if (!v) d[k++] = '0';
      while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
      while (k) m[o++] = d[--k];
      m[o++] = '\n'; m[o] = 0;
      serial_puts(m);
      return sent;
    }
    c->seq += (uint32_t)chunk;
    sent += chunk;
  }
  return sent;
}

static int rx_take(tcp_conn_t *c, void *buf, int max) {
  int n = c->rx_len < max ? c->rx_len : max;
  if (n > 0) memcpy(buf, c->rx, (size_t)n);
  if (n < c->rx_len) memmove(c->rx, c->rx + n, (size_t)(c->rx_len - n));
  c->rx_len -= n;
  return n;
}

int tcp_conn_recv(tcp_conn_t *c, void *buf, int max, int timeout_ms) {
  if (!c || !c->used) return -1;
  if (max <= 0) return 0;
  if (c->rx_len > 0) return rx_take(c, buf, max);
  if (c->peer_closed) return -1;

  pacer_t p;
  pace_begin(&p);
  while (pace_ms(&p) < (uint32_t)timeout_ms) {
    tcp_pump();
    if (c->rx_len > 0) return rx_take(c, buf, max);
    if (c->peer_closed || !c->open) return -1;
    pace_yield();
  }
  return 0;
}

void tcp_conn_close(tcp_conn_t *c) {
  /* A second close on the same connection would mean the reference count and
   * the connection lifetime have come apart: say so instead of returning
   * quietly, because silence here looks like "cleanup happened once". */
  if (!c) return;
  if (!c->used) { conn_log("DOUBLE-CLOSE", c); return; }
  if (c->open) {
    c->seq += 1;
    conn_seg(c, (uint8_t)(TCP_FIN | TCP_ACK), 0, 0);
  }
  conn_log("down", c);
  c->used = 0;
  c->rx_len = 0;
  c->cb = 0;                    /* the endpoint is gone: no more news */
  c->ctx = 0;
}

int tcp_conn_used(int n) {
  int k = 0;
  for (int i = 0; i < TCP_MAX_CONN && i < n; i++) if (conns[i].used) k++;
  return k;
}

/* ---- the single-connection API the in-kernel browser still uses ---------
   http.c and tls.c were written against one global connection. Keeping the
   old names on top of one dedicated tcp_conn_t means the browser keeps working
   unchanged while sockets get real per-connection state. */

static tcp_conn_t *legacy;

int tcp_connect(const net_cfg_t *cfg, const uint8_t ip[4], uint16_t port) {
  if (legacy) { tcp_conn_close(legacy); legacy = 0; }
  legacy = tcp_conn_open(cfg, ip, port);
  return legacy ? 1 : 0;
}

int tcp_send(const void *buf, int len) { return legacy ? tcp_conn_send(legacy, buf, len) : -1; }

int tcp_recv(void *buf, int max, int timeout_ms) {
  return legacy ? tcp_conn_recv(legacy, buf, max, timeout_ms) : -1;
}

void tcp_close(void) {
  if (legacy) { tcp_conn_close(legacy); legacy = 0; }
}

int tcp_is_open(void) { return legacy && legacy->open; }

int tcp_conn_count(void) { return tcp_conn_used(TCP_MAX_CONN); }

/* ---- DNS ------------------------------------------------------------- */

static int dns_build(uint8_t *p, uint16_t id, const char *host) {
  put16(p, id);
  put16(p + 2, 0x0100);        /* recursion desired */
  put16(p + 4, 1);             /* one question */
  put16(p + 6, 0); put16(p + 8, 0); put16(p + 10, 0);
  int o = 12;
  const char *s = host;
  while (*s) {
    int len = 0;
    while (s[len] && s[len] != '.' && len < 63) len++;
    if (len == 0) return 0;
    p[o++] = (uint8_t)len;
    for (int i = 0; i < len; i++) p[o++] = (uint8_t)s[i];
    s += len;
    if (*s == '.') s++;
  }
  p[o++] = 0;
  put16(p + o, 1); o += 2;     /* A */
  put16(p + o, 1); o += 2;     /* IN */
  return o;
}

/* Walks the answer section, skipping the question and any CNAMEs, and returns
   the first A record. */
static int dns_parse(const uint8_t *p, int len, uint16_t id, uint8_t out[4]) {
  if (len < 12) return 0;
  if (get16(p) != id) return 0;
  if (!(p[2] & 0x80)) return 0;                 /* not a response */
  int ancount = get16(p + 6);
  int qd = get16(p + 4);
  int o = 12;
  for (int q = 0; q < qd; q++) {
    while (o < len && p[o] != 0) {
      if ((p[o] & 0xC0) == 0xC0) { o += 2; goto q_done; }
      o += p[o] + 1;
    }
    o++;
  q_done:
    o += 4;
  }
  for (int a = 0; a < ancount && o + 10 <= len; a++) {
    if ((p[o] & 0xC0) == 0xC0) o += 2;
    else { while (o < len && p[o] != 0) o += p[o] + 1; o++; }
    if (o + 10 > len) return 0;
    int type = get16(p + o);
    int rdlen = get16(p + o + 8);
    o += 10;
    if (type == 1 && rdlen == 4 && o + 4 <= len) {
      for (int i = 0; i < 4; i++) out[i] = p[o + i];
      return 1;
    }
    o += rdlen;
  }
  return 0;
}

static int dns_resolve_once(const net_cfg_t *cfg, const char *host, uint8_t ip_out[4]) {
  uint8_t q[300];
  uint8_t r[1600];
  static uint16_t dns_id = 0x3301;
  if (!cfg || !cfg->valid) return 0;
  /* Resolvers to try, in order: Google first, then whatever DHCP handed us,
   * then Google's secondary. A router stub resolver that is slow or broken
   * should not take the whole browser down with it. */
  static const uint8_t google[2][4] = {{8, 8, 8, 8}, {8, 8, 4, 4}};
  uint8_t servers[3][4];
  int n_servers = 0;
  int have_dns = 0;
  for (int i = 0; i < 4; i++) if (cfg->dns[i]) have_dns = 1;

  memcpy(servers[n_servers], google[0], 4);
  n_servers++;
  if (have_dns && memcmp(cfg->dns, google[0], 4) != 0) {
    memcpy(servers[n_servers], cfg->dns, 4);
    n_servers++;
  }
  if (memcmp(servers[n_servers - 1], google[1], 4) != 0) {
    memcpy(servers[n_servers], google[1], 4);
    n_servers++;
  }

  for (int attempt = 0; attempt < n_servers; attempt++) {
    const uint8_t *server = servers[attempt];
    uint16_t id = ++dns_id;
    int qlen = dns_build(q, id, host);
    if (qlen <= 0) return 0;
    if (!net_udp_send(cfg, server, 5353, 53, q, qlen)) return 0;

    pacer_t p;
    pace_begin(&p);
    while (pace_ms(&p) < 2500) {
      if (rx_busy) { pace_yield(); continue; }
      rx_busy = 1;
      int n = e1000_poll_recv(r, sizeof r);
      rx_busy = 0;
      if (n > ETH_LEN + 28) {
        const uint8_t *ip = r + ETH_LEN;
        int ihl = (ip[0] & 0x0F) * 4;
        if (get16(r + 12) == ETH_IPV4 && ip[9] == IP_PROTO_UDP) {
          const uint8_t *udp = ip + ihl;
          if (get16(udp + 2) == 5353 && get16(udp) == 53) {
            int plen = get16(udp + 4) - 8;
            if (plen > 0 && dns_parse(udp + 8, plen, id, ip_out)) {
              extern void net_log_dns_via(const uint8_t *ip);
              net_log_dns_via(server);
              return 1;
            }
          }
        } else {
          /* A resolution must not swallow another connection's segments. */
          tcp_dispatch(r, n);
        }
      }
      pace_yield();
    }
  }
  return 0;
}

/* Every query leaves from the same source port, so two resolutions in flight
 * would read and discard each other's replies: each would see the other's ID,
 * reject it, and both would sit out their timeout. Resolving one at a time is
 * the honest fix while there is a single UDP path - and it is invisible except
 * as a few milliseconds of queueing. Chromium-style parallel lookups need
 * per-query ports and a real UDP receive queue first. */
static volatile int dns_busy;

int net_dns_resolve(const net_cfg_t *cfg, const char *host, uint8_t ip_out[4]) {
  pacer_t spin;
  pace_begin(&spin);
  while (dns_busy && pace_ms(&spin) < 20000u) pace_yield();
  if (dns_busy) return 0;                     /* waited 20 s: say no rather than corrupt */
  dns_busy = 1;
  int ok = dns_resolve_once(cfg, host, ip_out);
  dns_busy = 0;
  return ok;
}

/* ---- the backend boundary ---------------------------------------------
   fd.c calls these through socket_backend(), never by name. That is the whole
   point: a userspace network service answering the same five calls over IPC
   would be a different table, not a different VFS. */

/* Readiness, computed from endpoint state. Never blocks and never polls the NIC:
   bytes only reach rx[] through the existing receive path, so this answers from
   what has already arrived. */
unsigned tcp_conn_poll(struct tcp_conn *c) {
  if (!c || !c->used) return OMI_POLLERR | OMI_POLLHUP;
  unsigned r = 0;
  if (c->rx_len > 0)      r |= OMI_POLLIN;      /* data waiting */
  if (c->peer_closed)     r |= OMI_POLLIN | OMI_POLLHUP;  /* EOF is readable */
  if (c->open)            r |= OMI_POLLOUT;
  if (!c->open && !c->peer_closed) r |= OMI_POLLERR;
  return r;
}

/* The one place the backend reaches upward. It reports a mask, not a socket. */
static void conn_signal(tcp_conn_t *c) {
  if (c && c->used && c->cb) c->cb(c->ctx, tcp_conn_poll(c));
}

/* Thin adapters: the ops table speaks void*, this file speaks tcp_conn_t, and
 * the cast happens in exactly these lines rather than at every call site. */
static void op_bind(void *h, void (*cb)(void *, unsigned), void *ctx) {
  tcp_conn_t *c = h;
  if (!c) return;
  c->cb = cb;
  c->ctx = ctx;
  conn_signal(c);       /* state that is already ready must not wait for news */
}

static void *op_open(const net_cfg_t *cfg, const uint8_t ip[4], uint16_t port) {
  return tcp_conn_open(cfg, ip, port);
}
static int  op_send(void *h, const void *buf, int len) {
  return tcp_conn_send((struct tcp_conn *)h, buf, len);
}
static int  op_recv(void *h, void *buf, int max, int timeout_ms) {
  return tcp_conn_recv((struct tcp_conn *)h, buf, max, timeout_ms);
}
static void op_close(void *h) { tcp_conn_close((struct tcp_conn *)h); }
static unsigned op_poll(void *h) { return tcp_conn_poll((struct tcp_conn *)h); }

static const struct socket_ops tcp_kernel_ops = {
  .name  = "kernel-tcp",
  .open  = op_open,
  .send  = op_send,
  .recv  = op_recv,
  .close = op_close,
  .poll  = op_poll,
  .bind  = op_bind,
};

/* Static, not installed by an init call: a caller must never be able to observe
   "no backend" because it ran one line early. */
static const struct socket_ops *backend = &tcp_kernel_ops;

const struct socket_ops *socket_backend(void) { return backend; }

void socket_backend_select(const struct socket_ops *ops) {
  if (ops) backend = ops;
}
