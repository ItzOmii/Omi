/*
 * Omi - TCP client + DNS resolver (see include/omi_net.h)
 */

#include "types.h"
#include "string.h"
#include "net.h"
#include "omi_net.h"
#include "e1000.h"

#define ETH_IPV4 0x0800
#define ETH_LEN 14
#define IP_PROTO_TCP 6

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

static net_cfg_t s_cfg;
static uint8_t s_dip[4];
static uint8_t s_dmac[6];
static uint16_t s_sport, s_dport;
static uint32_t s_seq, s_ack;
static int s_open;
static uint32_t s_port_seed = 0x5A17;

/* ---- pacer ------------------------------------------------------------
   The stack has no timer subsystem, so timeouts are measured straight off
   the PIT's channel-0 counter (1.193182 MHz, ~0.838 us per tick). Without
   this, "spin N times" is a handful of milliseconds and every request
   gives up before the reply can land. */
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
static uint16_t s_pit_last;
static uint32_t s_ticks;
static void pace_init(void) {
  s_pit_last = pit_read();
  s_ticks = 0;
}
/* Accumulate raw PIT ticks and divide the total. Dividing each delta loses
   1 ms to truncation on every call, and this is polled thousands of times a
   second, so a "2 second" timeout never expired and connects hung forever. */
static uint32_t pace_ms(void) {
  uint16_t n = pit_read();
  s_ticks += (uint16_t)(s_pit_last - n);
  s_pit_last = n;
  return s_ticks / 1193u;
}
static void pace_yield(void) {
  for (volatile int i = 0; i < 50; i++) {
  }
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

static uint16_t tcp_csum(const uint8_t *seg, int len) {
  uint32_t sum = 0;
  for (int i = 0; i < 4; i += 2) sum += get16(s_cfg.ip + i);
  for (int i = 0; i < 4; i += 2) sum += get16(s_dip + i);
  sum += IP_PROTO_TCP;
  sum += (uint32_t)len;
  int i = 0;
  for (; i + 1 < len; i += 2) sum += get16(seg + i);
  if (i < len) sum += ((uint32_t)seg[i] << 8);
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

static int tcp_seg(uint8_t flags, const uint8_t *data, int dlen) {
  uint8_t f[1600];
  uint8_t me[6];
  if (dlen > 1200) return -1;
  e1000_mac(me);
  for (int i = 0; i < 6; i++) f[i] = s_dmac[i];
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
  memcpy(ip + 12, s_cfg.ip, 4);
  memcpy(ip + 16, s_dip, 4);
  put16(ip + 10, net_csum(ip, 20));

  uint8_t *t = ip + 20;
  put16(t, s_sport);
  put16(t + 2, s_dport);
  put32(t + 4, s_seq);
  put32(t + 8, s_ack);
  t[12] = 0x50;
  t[13] = flags;
  put16(t + 14, 4096);
  put16(t + 16, 0);
  put16(t + 18, 0);
  if (dlen && data) memcpy(t + 20, data, (size_t)dlen);
  put16(t + 16, tcp_csum(t, 20 + dlen));
  return e1000_send(f, (uint16_t)(ETH_LEN + iplen));
}

/* Returns the offset of the TCP header for an inbound segment we care about,
   or -1 when the frame is not ours. */
static int tcp_match(const uint8_t *f, int n) {
  if (n < ETH_LEN + 40) return -1;
  if (get16(f + 12) != ETH_IPV4) return -1;
  const uint8_t *ip = f + ETH_LEN;
  if ((ip[0] >> 4) != 4) return -1;
  if (ip[9] != IP_PROTO_TCP) return -1;
  int ihl = (ip[0] & 0x0F) * 4;
  const uint8_t *t = ip + ihl;
  if (n < ETH_LEN + ihl + 20) return -1;
  if (get16(t + 2) != s_sport) return -1;
  if (get16(t) != s_dport) return -1;
  return ETH_LEN + ihl;
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

int tcp_connect(const net_cfg_t *cfg, const uint8_t ip[4], uint16_t port) {
  if (!cfg || !cfg->valid) return 0;
  s_cfg = *cfg;
  memcpy(s_dip, ip, 4);
  s_dport = port;
  s_port_seed = s_port_seed * 1103515245u + 12345u;
  s_sport = (uint16_t)(49152u + ((s_port_seed >> 16) % 14000u));
  pick_route(cfg, ip, s_dmac);

  s_seq = s_port_seed ^ 0x9E3779B9u;
  s_ack = 0;
  s_open = 0;

  for (int attempt = 0; attempt < 3 && !s_open; attempt++) {
    if (tcp_seg(TCP_SYN, 0, 0) != 0) return 0;
    uint8_t r[1600];
    /* First try short, then give slow hosts real time (neverssl waits seconds). */
    const uint32_t budget = attempt == 0 ? 1500u : (attempt == 1 ? 3500u : 5000u);
    if (attempt) {
      /* A fresh 5-tuple per retry: a retried SYN on the same port is often
         dropped by the middlebox that ate the first one, and some hosts
         (neverssl among them) take seconds to answer at all. */
      s_port_seed = s_port_seed * 1103515245u + 12345u;
      s_sport = (uint16_t)(49152u + ((s_port_seed >> 16) % 14000u));
      s_seq = s_port_seed ^ 0x9E3779B9u;
      s_ack = 0;
    }
    pace_init();
    while (pace_ms() < budget) {
      int n = e1000_poll_recv(r, sizeof r);
      if (n > 0) {
        int to = tcp_match(r, n);
        if (to > 0) {
          const uint8_t *t = r + to;
          uint8_t flags = t[13];
          if (flags & TCP_RST) return 0;
          if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            s_ack = get32(t + 4) + 1;
            s_seq += 1;                 /* our SYN consumed one sequence number */
            tcp_seg(TCP_ACK, 0, 0);
            s_open = 1;
            return 1;
          }
        }
      }
      pace_yield();
    }
  }
  return 0;
}

int tcp_send(const void *buf, int len) {
  if (!s_open) return -1;
  const uint8_t *p = (const uint8_t *)buf;
  int sent = 0;
  while (sent < len) {
    int chunk = len - sent;
    if (chunk > 1000) chunk = 1000;
    if (tcp_seg((uint8_t)(TCP_ACK | TCP_PSH), p + sent, chunk) != 0) return sent;
    s_seq += (uint32_t)chunk;
    sent += chunk;
  }
  return sent;
}

int tcp_recv(void *buf, int max, int timeout_ms) {
  if (!s_open) return -1;
  uint8_t r[1600];
  uint8_t *out = (uint8_t *)buf;
  if (max <= 0) return 0;
  pace_init();
  while (pace_ms() < (uint32_t)timeout_ms) {
    int n = e1000_poll_recv(r, sizeof r);
    if (n <= 0) {
      pace_yield();
      continue;
    }
    int to = tcp_match(r, n);
    if (to <= 0) continue;
    const uint8_t *t = r + to;
    int ihl = (t[12] >> 4) * 4;
    int iplen = get16(r + ETH_LEN + 2);
    int payload = iplen - (to - ETH_LEN) - ihl;
    uint8_t flags = t[13];
    uint32_t seq = get32(t + 4);

    if (payload > 0) {
      int take = payload;
      if (take > max) take = max;
      if (seq == s_ack) {
        memcpy(out, t + ihl, (size_t)take);
        s_ack += (uint32_t)payload;
        tcp_seg(TCP_ACK, 0, 0);
        if (flags & TCP_FIN) { s_ack += 1; tcp_seg(TCP_ACK, 0, 0); s_open = 0; }
        return take;
      }
      /* duplicate or out of order: ACK what we have and move on */
      tcp_seg(TCP_ACK, 0, 0);
      continue;
    }
    if (flags & TCP_FIN) {
      s_ack = seq + 1;
      tcp_seg(TCP_ACK, 0, 0);
      s_open = 0;
      return -1;
    }
    if (flags & TCP_RST) {
      s_open = 0;
      return -1;
    }
  }
  return 0;
}

void tcp_close(void) {
  if (s_open) {
    s_seq += 1;
    tcp_seg((uint8_t)(TCP_FIN | TCP_ACK), 0, 0);
  }
  s_open = 0;
}

int tcp_is_open(void) { return s_open; }

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

int net_dns_resolve(const net_cfg_t *cfg, const char *host, uint8_t ip_out[4]) {
  uint8_t q[300];
  uint8_t r[1600];
  static uint16_t dns_id = 0x3301;
  if (!cfg || !cfg->valid) return 0;
  uint8_t server[4];
  int have_dns = 0;
  for (int i = 0; i < 4; i++) if (cfg->dns[i]) have_dns = 1;
  if (have_dns) memcpy(server, cfg->dns, 4);
  else { server[0] = 1; server[1] = 1; server[2] = 1; server[3] = 1; }

  for (int attempt = 0; attempt < 3; attempt++) {
    uint16_t id = ++dns_id;
    int qlen = dns_build(q, id, host);
    if (qlen <= 0) return 0;
    if (!net_udp_send(cfg, server, 5353, 53, q, qlen)) return 0;

    pace_init();
    while (pace_ms() < 2500) {
      int n = e1000_poll_recv(r, sizeof r);
      if (n > ETH_LEN + 28) {
        const uint8_t *ip = r + ETH_LEN;
        int ihl = (ip[0] & 0x0F) * 4;
        if (get16(r + 12) == ETH_IPV4 && ip[9] == 17) {
          const uint8_t *udp = ip + ihl;
          if (get16(udp + 2) == 5353 && get16(udp) == 53) {
            int plen = get16(udp + 4) - 8;
            if (plen > 0 && dns_parse(udp + 8, plen, id, ip_out)) return 1;
          }
        }
      }
      for (volatile int i = 0; i < 200; i++) {
      }
    }
  }
  return 0;
}