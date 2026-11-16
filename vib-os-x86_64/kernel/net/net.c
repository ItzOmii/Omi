/*
 * MeepOS - IPv4 configuration: DHCP client, ARP, ICMP echo
 *
 * All frames go out through the e1000 polling driver (no interrupts), so every
 * wait in here is a bounded spin. The point of the file is that nothing above
 * it has to assume the QEMU slirp defaults: we ask for a lease, and the
 * address, mask, router and DNS server all come back from the network.
 */

#include "net.h"
#include "omi_http.h"
#include "omi_net.h"
#include "e1000.h"
#include "types.h"

/* ---- port io for the serial log ------------------------------------- */
#define COM1 0x3F8
static inline void n_outb(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(p));
}
static inline uint8_t n_inb(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
  return v;
}
static void n_putc(char c) {
  while ((n_inb(COM1 + 5) & 0x20) == 0) {
  }
  n_outb(COM1, (uint8_t)c);
}
static void n_puts(const char *s) {
  while (*s) n_putc(*s++);
}
static void n_dec(uint32_t v) {
  char b[12];
  int i = 11;
  b[i--] = 0;
  if (!v) b[i--] = '0';
  while (v) {
    b[i--] = (char)('0' + (v % 10));
    v /= 10;
  }
  n_puts(&b[i + 1]);
}
static void n_ip(const uint8_t *ip);
/* the resolve path lives in tcp.c and logs through here */
void net_log_dns_via(const uint8_t *ip) {
  n_puts("[dns] via ");
  n_ip(ip);
  n_putc('\n');
}
static void n_ip(const uint8_t *ip) {
  for (int i = 0; i < 4; i++) {
    n_dec(ip[i]);
    if (i < 3) n_putc('.');
  }
}

/* ---- helpers --------------------------------------------------------- */
#define ETH_ARP 0x0806
#define ETH_IPV4 0x0800
#define ETH_LEN 14

static net_cfg_t g_cfg;
const net_cfg_t *net_get(void) { return &g_cfg; }

uint16_t net_csum(const void *data, int len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t sum = 0;
  for (int i = 0; i + 1 < len; i += 2) sum += ((uint32_t)p[i] << 8) | p[i + 1];
  if (len & 1) sum += (uint32_t)p[len - 1] << 8;
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* Build the ethernet + IPv4 + UDP headers of a frame. Returns the offset of
   the UDP payload; *total is the frame length (headers + payload). */
static int frame_udp(uint8_t *f, const uint8_t *dst_mac, const uint8_t *src_mac,
                     const uint8_t *src_ip, const uint8_t *dst_ip, uint16_t sport,
                     uint16_t dport, int payload) {
  for (int i = 0; i < 6; i++) f[i] = dst_mac[i];
  for (int i = 0; i < 6; i++) f[6 + i] = src_mac[i];
  put16(f + 12, ETH_IPV4);

  uint8_t *ip = f + ETH_LEN;
  int iplen = 20 + 8 + payload;
  ip[0] = 0x45;
  ip[1] = 0;
  put16(ip + 2, (uint16_t)iplen);
  put16(ip + 4, 0);
  put16(ip + 6, 0x4000); /* don't fragment */
  ip[8] = 64;
  ip[9] = 17; /* udp */
  put16(ip + 10, 0);
  for (int i = 0; i < 4; i++) ip[12 + i] = src_ip[i];
  for (int i = 0; i < 4; i++) ip[16 + i] = dst_ip[i];
  put16(ip + 10, net_csum(ip, 20));

  uint8_t *udp = ip + 20;
  put16(udp, sport);
  put16(udp + 2, dport);
  put16(udp + 4, (uint16_t)(8 + payload));
  put16(udp + 6, 0); /* checksum optional in IPv4; the peer accepts 0 */
  return ETH_LEN + 28;
}


/* One UDP datagram. Used by DNS (see net/tcp.c). Returns 1 on success. */
int net_udp_send(const net_cfg_t *cfg, const uint8_t dst_ip[4], uint16_t sport,
                 uint16_t dport, const void *payload, int len) {
  if (!cfg || !cfg->valid || len <= 0 || len > 1200) return 0;
  uint8_t mac[6];
  uint8_t me[6];
  e1000_mac(me);
  int on_link = 1;
  for (int i = 0; i < 4; i++)
    if ((cfg->ip[i] & cfg->mask[i]) != (dst_ip[i] & cfg->mask[i])) on_link = 0;
  if (on_link) {
    if (!net_arp_resolve(cfg, dst_ip, mac)) return 0;
  } else {
    for (int i = 0; i < 6; i++) mac[i] = cfg->gw_mac[i];
  }
  uint8_t f[1400];
  int hdr = frame_udp(f, mac, me, cfg->ip, dst_ip, sport, dport, len);
  for (int i = 0; i < len; i++) f[hdr + i] = ((const uint8_t *)payload)[i];
  return e1000_send(f, (uint16_t)(hdr + len)) == 0;
}

/* ---- ARP ------------------------------------------------------------- */
static int arp_send(uint8_t *f, const uint8_t *dst_mac, const uint8_t *src_mac,
                    const uint8_t *sha_ip, const uint8_t *tha_mac,
                    const uint8_t *tha_ip, uint16_t op) {
  for (int i = 0; i < 6; i++) f[i] = dst_mac[i];
  for (int i = 0; i < 6; i++) f[6 + i] = src_mac[i];
  put16(f + 12, ETH_ARP);
  put16(f + 14, 1);  /* ethernet */
  put16(f + 16, ETH_IPV4);
  f[18] = 6;
  f[19] = 4;
  put16(f + 20, op);
  for (int i = 0; i < 6; i++) f[22 + i] = src_mac[i];
  for (int i = 0; i < 4; i++) f[28 + i] = sha_ip[i];
  for (int i = 0; i < 6; i++) f[32 + i] = tha_mac[i];
  for (int i = 0; i < 4; i++) f[38 + i] = tha_ip[i];
  for (int i = 42; i < 60; i++) f[i] = 0;
  return e1000_send(f, 60);
}

int net_arp_resolve(const net_cfg_t *cfg, const uint8_t ip[4], uint8_t mac_out[6]) {
  uint8_t f[64];
  uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
  uint8_t src[4] = {0, 0, 0, 0};
  uint8_t me[6];
  for (int i = 0; i < 4; i++) src[i] = cfg->ip[i];
  e1000_mac(me);
  if (arp_send(f, bcast, me, src, zero, ip, 1) != 0) return 0;

  uint8_t r[2048];
  for (int tries = 0; tries < 4000; tries++) {
    int n = e1000_poll_recv(r, sizeof r);
    if (n >= 42 && r[12] == 0x08 && r[13] == 0x06 && r[20] == 0x00 &&
        r[21] == 0x02) {
      int match = 1;
      for (int i = 0; i < 4; i++)
        if (r[28 + i] != ip[i]) match = 0;
      if (match) {
        for (int i = 0; i < 6; i++) mac_out[i] = r[22 + i];
        return 1;
      }
    }
    if (n > 0) tcp_input(r, n);   /* somebody else's frame, not ours to drop */
    for (volatile int i = 0; i < 20000; i++) {
    }
  }
  return 0;
}

/* ---- ICMP ------------------------------------------------------------ */
/* Echo request straight into an ethernet frame. Returns the frame length. */
static int icmp_send(uint8_t *f, const uint8_t *dst_mac, const uint8_t *src_mac,
                     const uint8_t *src_ip, const uint8_t *dst_ip, uint16_t seq) {
  for (int i = 0; i < 6; i++) f[i] = dst_mac[i];
  for (int i = 0; i < 6; i++) f[6 + i] = src_mac[i];
  put16(f + 12, ETH_IPV4);

  uint8_t *ip = f + ETH_LEN;
  int total = 20 + 8 + 32;
  ip[0] = 0x45;
  ip[1] = 0;
  put16(ip + 2, (uint16_t)total);
  put16(ip + 4, 0);
  put16(ip + 6, 0x4000);
  ip[8] = 64;
  ip[9] = 1; /* icmp */
  put16(ip + 10, 0);
  for (int i = 0; i < 4; i++) ip[12 + i] = src_ip[i];
  for (int i = 0; i < 4; i++) ip[16 + i] = dst_ip[i];
  put16(ip + 10, net_csum(ip, 20));

  uint8_t *ic = ip + 20;
  ic[0] = 8;
  ic[1] = 0;
  put16(ic + 2, 0);
  put16(ic + 4, 0x4D45);
  put16(ic + 6, seq);
  for (int i = 0; i < 32; i++) ic[8 + i] = (uint8_t)('a' + (i % 26));
  put16(ic + 2, net_csum(ic, 8 + 32));

  int len = ETH_LEN + total;
  if (len < 60) {
    for (int i = len; i < 60; i++) f[i] = 0;
    len = 60;
  }
  return len;
}

int net_ping(const net_cfg_t *cfg, const uint8_t dst_ip[4], uint16_t seq) {
  uint8_t mac[6];
  uint8_t me[6];
  e1000_mac(me);
  int on_link = 1;
  for (int i = 0; i < 4; i++)
    if ((cfg->ip[i] & cfg->mask[i]) != (dst_ip[i] & cfg->mask[i])) on_link = 0;
  if (on_link) {
    if (!net_arp_resolve(cfg, dst_ip, mac)) return 0;
  } else {
    for (int i = 0; i < 6; i++) mac[i] = cfg->gw_mac[i];
  }

  uint8_t f[128];
  int len = icmp_send(f, mac, me, cfg->ip, dst_ip, seq);
  if (len <= 0 || e1000_send(f, (uint16_t)len) != 0) return 0;

  uint8_t r[2048];
  for (int tries = 0; tries < 20000; tries++) {
    int n = e1000_poll_recv(r, sizeof r);
    if (n >= 42 && r[12] == 0x08 && r[13] == 0x00 && r[23] == 1 &&
        r[ETH_LEN + 20] == 0 && get16(r + ETH_LEN + 26) == seq) {
      return 1;
    }
    if (n > 0) tcp_input(r, n);   /* a ping must not swallow a download */
    for (volatile int i = 0; i < 50000; i++) {
    }
  }
  return 0;
}

/* ---- DHCP ------------------------------------------------------------ */
#define DHCP_MAGIC_OFF 236
#define DHCP_OPT_OFF 240
#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER 2
#define DHCP_MSG_REQUEST 3
#define DHCP_MSG_ACK 5

static uint32_t g_xid = 0x4D454550; /* "MEEP" */

/* Returns the bootp payload length to send for the given message type. */
static int dhcp_build(uint8_t *payload, uint8_t msg, const uint8_t *mac,
                      const uint8_t *requested_ip) {
  for (int i = 0; i < 300; i++) payload[i] = 0;
  payload[0] = 1;    /* request */
  payload[1] = 1;    /* ethernet */
  payload[2] = 6;    /* hw addr len */
  payload[3] = 0;
  payload[4] = (uint8_t)(g_xid >> 24);
  payload[5] = (uint8_t)(g_xid >> 16);
  payload[6] = (uint8_t)(g_xid >> 8);
  payload[7] = (uint8_t)g_xid;
  put16(payload + 10, 0x8000); /* broadcast reply, we have no address yet */
  for (int i = 0; i < 6; i++) payload[28 + i] = mac[i];
  payload[DHCP_MAGIC_OFF + 0] = 99;
  payload[DHCP_MAGIC_OFF + 1] = 130;
  payload[DHCP_MAGIC_OFF + 2] = 83;
  payload[DHCP_MAGIC_OFF + 3] = 99;

  int o = DHCP_OPT_OFF;
  payload[o++] = 53;
  payload[o++] = 1;
  payload[o++] = msg;
  if (requested_ip) {
    payload[o++] = 50;
    payload[o++] = 4;
    for (int i = 0; i < 4; i++) payload[o++] = requested_ip[i];
  }
  payload[o++] = 55; /* parameter request list */
  payload[o++] = 4;
  payload[o++] = 1;  /* subnet mask */
  payload[o++] = 3;  /* router */
  payload[o++] = 6;  /* dns */
  payload[o++] = 15; /* domain name */
  payload[o++] = 255;
  return o;
}

/* Parse a BOOTP reply. Returns the DHCP message type, 0 when malformed. */
static int dhcp_parse(const uint8_t *payload, int len, uint8_t *yiaddr,
                      net_cfg_t *cfg) {
  if (len < DHCP_OPT_OFF) return 0;
  if (payload[0] != 2) return 0;
  uint32_t xid = ((uint32_t)payload[4] << 24) | ((uint32_t)payload[5] << 16) |
                 ((uint32_t)payload[6] << 8) | payload[7];
  if (xid != g_xid) return 0;
  for (int i = 0; i < 4; i++) yiaddr[i] = payload[16 + i];

  int msg = 0;
  int o = DHCP_OPT_OFF;
  while (o < len) {
    uint8_t code = payload[o++];
    if (code == 0) continue;
    if (code == 255) break;
    if (o >= len) break;
    uint8_t olen = payload[o++];
    if (o + olen > len) break;
    const uint8_t *v = payload + o;
    if (code == 53 && olen >= 1) msg = v[0];
    else if (code == 1 && olen == 4) { for (int i = 0; i < 4; i++) cfg->mask[i] = v[i]; }
    else if (code == 3 && olen >= 4) { for (int i = 0; i < 4; i++) cfg->gw[i] = v[i]; }
    else if (code == 6 && olen >= 4) { for (int i = 0; i < 4; i++) cfg->dns[i] = v[i]; }
    o += olen;
  }
  return msg;
}

/* Send a DHCP message and wait for the matching reply type. */
static int dhcp_exchange(uint8_t msg_out, const uint8_t *requested_ip,
                         uint8_t msg_in, uint8_t *yiaddr, net_cfg_t *cfg) {
  uint8_t me[6];
  e1000_mac(me);
  uint8_t f[600];
  uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t any[4] = {0, 0, 0, 0};
  uint8_t bcast_ip[4] = {255, 255, 255, 255};

  uint8_t *payload = f + ETH_LEN + 28;
  int plen = dhcp_build(payload, msg_out, me, requested_ip);
  frame_udp(f, bcast, me, any, bcast_ip, 68, 67, plen);
  int total = ETH_LEN + 28 + plen;
  for (int i = total; i < 60; i++) f[i] = 0;
  if (total < 60) total = 60;
  if (e1000_send(f, (uint16_t)total) != 0) return 0;

  uint8_t r[2048];
  for (int tries = 0; tries < 20000; tries++) {
    int n = e1000_poll_recv(r, sizeof r);
    if (n >= ETH_LEN + 28 + DHCP_OPT_OFF && r[12] == 0x08 && r[13] == 0x00 &&
        r[23] == 17 && get16(r + ETH_LEN + 22) == 68) {
      int got = dhcp_parse(r + ETH_LEN + 28, n - ETH_LEN - 28, yiaddr, cfg);
      if (got == msg_in) return 1;
    }
    if (n > 0) tcp_input(r, n);
    for (volatile int i = 0; i < 50000; i++) {
    }
  }
  return 0;
}

int dhcp_configure(net_cfg_t *cfg) {
  for (int i = 0; i < (int)sizeof *cfg; i++) ((uint8_t *)cfg)[i] = 0;
  uint8_t yiaddr[4] = {0, 0, 0, 0};

  n_puts("[net] dhcp discover\n");
  if (!dhcp_exchange(DHCP_MSG_DISCOVER, 0, DHCP_MSG_OFFER, yiaddr, cfg)) {
    n_puts("[net] no DHCP offer\n");
    return -1;
  }
  n_puts("[net] offer ");
  n_ip(yiaddr);
  n_puts(" request\n");

  if (!dhcp_exchange(DHCP_MSG_REQUEST, yiaddr, DHCP_MSG_ACK, yiaddr, cfg)) {
    n_puts("[net] no DHCP ack\n");
    return -1;
  }
  for (int i = 0; i < 4; i++) cfg->ip[i] = yiaddr[i];
  cfg->valid = 1;
  *(&g_cfg) = *cfg;

  n_puts("[net] dhcp lease ip=");
  n_ip(cfg->ip);
  n_puts(" mask=");
  n_ip(cfg->mask);
  n_puts(" gw=");
  n_ip(cfg->gw);
  n_puts(" dns=");
  n_ip(cfg->dns);
  n_puts("\n");
  return 0;
}

/* ---- boot self test -------------------------------------------------- */
void net_selftest(void) {
  if (e1000_init() != 0) {
    n_puts("[net] no NIC, skipping\n");
    return;
  }
  net_cfg_t cfg;
  if (dhcp_configure(&cfg) != 0) return;

  uint8_t gw_mac[6];
  if (!net_arp_resolve(&cfg, cfg.gw, gw_mac)) {
    n_puts("[net] arp for gateway failed\n");
    return;
  }
  for (int i = 0; i < 6; i++) cfg.gw_mac[i] = gw_mac[i];
  *(&g_cfg) = cfg;
  n_puts("[net] gateway mac resolved\n");

  /* TCP / DNS / HTTP proof: fetch a real page over the wire. Serial only -
     the GUI comes later, but this is the layer everything else stands on. */
  {
    static char page[4096];
    static http_result_t hr;
    n_puts("[net] https get example.com ...\n");
    int got = http_get(&cfg, "https://example.com/", page, sizeof page, &hr);
    n_puts("[net] http ");
    if (!got) {
      n_puts("FAILED ");
      n_puts(hr.err[0] ? hr.err : "(dns or tcp)");
      n_puts("\n");
    } else {
      n_dec((uint32_t)hr.status);
      n_puts(" len=");
      n_dec((uint32_t)hr.len);
      n_puts(hr.secure ? " tls=yes" : " tls=no");
      n_puts(" title=\"");
      n_puts(hr.title);
      n_puts("\"\n");
    }
  }

  uint8_t router[4];
  for (int i = 0; i < 4; i++) router[i] = cfg.gw[i];
  int ok = net_ping(&cfg, router, 1);
  n_puts("[net] ping ");
  n_ip(router);
  n_puts(ok ? " REPLY - configured and reachable\n" : " no reply\n");
}