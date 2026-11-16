/*
 * MeepOS - Intel 8254x (e1000) NIC driver, polling.
 *
 * Register map and descriptor layout follow the 82540EM datasheet; QEMU's
 * e1000 is a faithful 82540EM, and real 8254x cards behave the same way.
 */

#include "e1000.h"
#include "kmalloc.h"
#include "mmio.h"
#include "pci.h"
#include "types.h"

/* ---- port io for the serial log ------------------------------------- */
#define COM1 0x3F8
static inline void outb_p(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(p));
}
static inline uint8_t inb_p(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
  return v;
}
static void s_putc(char c) {
  while ((inb_p(COM1 + 5) & 0x20) == 0) {
  }
  outb_p(COM1, (uint8_t)c);
}
static void s_puts(const char *s) {
  while (*s) s_putc(*s++);
}
static void s_hex(uint64_t v) {
  char b[19];
  int i = 18;
  b[i--] = 0;
  if (!v) b[i--] = '0';
  while (v) {
    int d = (int)(v & 0xF);
    b[i--] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    v >>= 4;
  }
  s_puts(&b[i + 1]);
}
static void s_dec(uint32_t v) {
  char b[12];
  int i = 11;
  b[i--] = 0;
  if (!v) b[i--] = '0';
  while (v) {
    b[i--] = (char)('0' + (v % 10));
    v /= 10;
  }
  s_puts(&b[i + 1]);
}

/* ---- registers ------------------------------------------------------- */
#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_EECD 0x0010
#define REG_EERD 0x0014
#define REG_ICR 0x00C0
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_MTA 0x5200
#define REG_RAL 0x5400
#define REG_RAH 0x5404

#define CTRL_RST (1u << 26)
#define CTRL_SLU (1u << 6)
#define RCTL_EN (1u << 1)
#define RCTL_LPE (1u << 5)
#define RCTL_BAM (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN (1u << 1)
#define TCTL_PSP (1u << 3)
#define TX_CMD_EOP 0x01
#define TX_CMD_IFCS 0x02
#define TX_CMD_RS 0x08

#define NRX 32
#define NTX 8
#define RXBUF 2048
#define TXBUF 2048

typedef struct {
  uint64_t addr;
  uint16_t length;
  uint16_t csum;
  uint8_t status;
  uint8_t errors;
  uint16_t special;
} __attribute__((packed)) rx_desc_t;

/* legacy TX desc: length(16) cso(8) cmd(8) status(8) css(8) special(16).
   cso is ONE byte - widening it shifts cmd into the status byte and the card
   silently drops every frame. */
typedef struct {
  uint64_t addr;
  uint16_t length;
  uint8_t cso;
  uint8_t cmd;
  uint8_t status;
  uint8_t css;
  uint16_t special;
} __attribute__((packed)) tx_desc_t;

static volatile uint32_t *mmio;
static rx_desc_t *rx_ring;
static tx_desc_t *tx_ring;
static uint8_t *rx_bufs;
static uint8_t *tx_bufs;
static uint32_t rx_cur;
static uint32_t tx_cur;
static uint8_t mac_addr[E1000_MAC_LEN];
static int ready;

#define R(off) (mmio[(off) / 4])

static inline uint64_t v2p(const void *p) {
  uint64_t v = (uint64_t)(uintptr_t)p;
  return kernel_phys_base + (v - kernel_virt_base);
}

static void io_delay(void) {
  for (volatile int i = 0; i < 20000; i++) {
  }
}

static uint16_t eeprom_read(uint8_t word) {
  R(REG_EERD) = ((uint32_t)word << 8) | 0x1;
  for (int i = 0; i < 100000; i++) {
    uint32_t v = R(REG_EERD);
    if (v & 0x10) return (uint16_t)(v >> 16);
  }
  return 0xFFFF;
}

int e1000_init(void) {
  if (ready) return 0;

  pci_device_info_t devs[8];
  int n = pci_scan_class(0x02, 0x00, 0xFF, devs, 8);
  pci_device_info_t *nic = 0;
  for (int i = 0; i < n; i++) {
    if (devs[i].vendor_id == 0x8086) {
      uint16_t d = devs[i].device_id;
      if (d == 0x100E || d == 0x100F || d == 0x1004 || d == 0x10D3 ||
          d == 0x153A || d == 0x15B8) {
        nic = &devs[i];
        break;
      }
    }
  }
  if (!nic) {
    s_puts("[nic] no e1000 on the PCI bus\n");
    return -1;
  }

  s_puts("[nic] e1000 found ");
  s_hex(nic->vendor_id);
  s_puts(":");
  s_hex(nic->device_id);
  s_puts(" at ");
  s_dec(nic->bus);
  s_puts(":");
  s_dec(nic->device);
  s_puts(".");
  s_dec(nic->function);
  s_puts("\n");

  /* memory space + bus master */
  uint32_t cmd = pci_read32(nic->bus, nic->device, nic->function, 0x04);
  pci_write32(nic->bus, nic->device, nic->function, 0x04,
              (cmd & 0xFFFF) | 0x6);

  uint64_t bar0 = nic->bar[0] & ~0xFULL;
  if (bar0 == 0 || (nic->bar[0] & 0x1)) {
    s_puts("[nic] BAR0 is not a memory BAR, giving up\n");
    return -1;
  }
  mmio = (volatile uint32_t *)(uintptr_t)mmio_map_range(bar0, 0x20000);
  if (!mmio) {
    s_puts("[nic] mmio map failed\n");
    return -1;
  }

  /* reset */
  R(REG_CTRL) = CTRL_RST;
  io_delay();
  for (int i = 0; i < 1000; i++) {
    if (!(R(REG_CTRL) & CTRL_RST)) break;
    io_delay();
  }
  R(REG_ICR) = 0xFFFFFFFFu;

  /* MAC from EEPROM, fall back to whatever the card latched */
  uint16_t w0 = eeprom_read(0), w1 = eeprom_read(1), w2 = eeprom_read(2);
  if (w0 != 0xFFFF && w0 != 0x0000) {
    mac_addr[0] = (uint8_t)(w0 & 0xFF);
    mac_addr[1] = (uint8_t)(w0 >> 8);
    mac_addr[2] = (uint8_t)(w1 & 0xFF);
    mac_addr[3] = (uint8_t)(w1 >> 8);
    mac_addr[4] = (uint8_t)(w2 & 0xFF);
    mac_addr[5] = (uint8_t)(w2 >> 8);
  } else {
    uint32_t ral = R(REG_RAL), rah = R(REG_RAH);
    mac_addr[0] = (uint8_t)(ral & 0xFF);
    mac_addr[1] = (uint8_t)((ral >> 8) & 0xFF);
    mac_addr[2] = (uint8_t)((ral >> 16) & 0xFF);
    mac_addr[3] = (uint8_t)((ral >> 24) & 0xFF);
    mac_addr[4] = (uint8_t)(rah & 0xFF);
    mac_addr[5] = (uint8_t)((rah >> 8) & 0xFF);
  }
  R(REG_RAL) = (uint32_t)mac_addr[0] | ((uint32_t)mac_addr[1] << 8) |
               ((uint32_t)mac_addr[2] << 16) | ((uint32_t)mac_addr[3] << 24);
  R(REG_RAH) = (uint32_t)mac_addr[4] | ((uint32_t)mac_addr[5] << 8) |
               (1u << 31);
  for (int i = 0; i < 128; i++) R(REG_MTA + i * 4) = 0;

  /* rings */
  void *rrd = kmalloc(sizeof(rx_desc_t) * NRX + 16);
  void *trd = kmalloc(sizeof(tx_desc_t) * NTX + 16);
  rx_bufs = (uint8_t *)kmalloc(RXBUF * NRX + 16);
  tx_bufs = (uint8_t *)kmalloc(TXBUF * NTX + 16);
  if (!rrd || !trd || !rx_bufs || !tx_bufs) {
    s_puts("[nic] out of memory for rings\n");
    return -1;
  }
  rx_ring = (rx_desc_t *)(((uintptr_t)rrd + 15) & ~(uintptr_t)15);
  tx_ring = (tx_desc_t *)(((uintptr_t)trd + 15) & ~(uintptr_t)15);
  rx_bufs = (uint8_t *)(((uintptr_t)rx_bufs + 15) & ~(uintptr_t)15);
  tx_bufs = (uint8_t *)(((uintptr_t)tx_bufs + 15) & ~(uintptr_t)15);
  for (int i = 0; i < NRX * RXBUF; i++) rx_bufs[i] = 0;
  for (int i = 0; i < NTX * TXBUF; i++) tx_bufs[i] = 0;

  R(REG_RCTL) = 0;
  R(REG_TCTL) = 0;

  for (int i = 0; i < NRX; i++) {
    rx_ring[i].addr = v2p(rx_bufs + (uint64_t)i * RXBUF);
    rx_ring[i].status = 0;
    rx_ring[i].length = 0;
  }
  for (int i = 0; i < NTX; i++) {
    tx_ring[i].addr = v2p(tx_bufs + (uint64_t)i * TXBUF);
    tx_ring[i].status = 0x1; /* free */
    tx_ring[i].cmd = 0;
  }

  R(REG_RDBAH) = 0;
  R(REG_RDBAL) = (uint32_t)v2p(rx_ring);
  R(REG_RDLEN) = NRX * sizeof(rx_desc_t);
  R(REG_RDH) = 0;
  R(REG_RDT) = NRX - 1;

  R(REG_TDBAH) = 0;
  R(REG_TDBAL) = (uint32_t)v2p(tx_ring);
  R(REG_TDLEN) = NTX * sizeof(tx_desc_t);
  R(REG_TDH) = 0;
  R(REG_TDT) = 0;

  R(REG_TIPG) = 0x0060200A;
  R(REG_TCTL) = TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12);
  R(REG_RCTL) = RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_LPE | (1u << 3) | (1u << 4);

  /* link up (SLU) so a cable-less QEMU still reports a carrier */
  R(REG_CTRL) = R(REG_CTRL) | CTRL_SLU;
  io_delay();

  rx_cur = 0;
  tx_cur = 0;
  ready = 1;

  s_puts("[nic] mac ");
  for (int i = 0; i < 6; i++) {
    if (i) s_putc(':');
    if (mac_addr[i] < 16) s_putc('0');
    s_hex(mac_addr[i]);
  }
  s_puts(" link=");
  s_puts((R(REG_STATUS) & 2) ? "up" : "down");
  s_puts("\n");
  return 0;
}

int e1000_link(void) { return ready && (R(REG_STATUS) & 2) ? 1 : 0; }

void e1000_mac(uint8_t out[E1000_MAC_LEN]) {
  for (int i = 0; i < E1000_MAC_LEN; i++) out[i] = mac_addr[i];
}

int e1000_send(const void *frame, uint16_t len) {
  if (!ready || len > TXBUF) return -1;
  tx_desc_t *d = &tx_ring[tx_cur];
  /* the status byte lives in DMA memory: read it volatile or the compiler
     hoists the load out of the poll loop and every send looks stuck */
  volatile uint8_t *dstat = &d->status;
  for (int i = 0; i < 100000; i++) {
    if (*dstat & 0x1) break;
  }
  if (!(*dstat & 0x1)) return -1; /* ring stuck: nothing was posted */

  const uint8_t *src = (const uint8_t *)frame;
  uint8_t *dst = tx_bufs + (uint64_t)tx_cur * TXBUF;
  for (int i = 0; i < len; i++) dst[i] = src[i];

  d->length = (uint16_t)len;
  d->cso = 0;
  d->css = 0;
  d->special = 0;
  d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
  d->status = 0;
  __sync_synchronize(); /* mfence: DMA order, not just compiler order */
  R(REG_TDT) = (tx_cur + 1) % NTX;

  for (int i = 0; i < 2000000 && !(*dstat & 0x1); i++) __sync_synchronize();

  /* Three outcomes, because the caller cannot retry what it cannot identify.
   *  0  the descriptor came back: it is on the wire.
   * -1  the ring was stuck before anything was posted: definitely not sent, and
   *     retrying is safe.
   * -2  posted, but the completion bit never appeared. The frame may well have
   *     gone out, so the caller must count it as sent: a retry here duplicates
   *     bytes on the far side, which is worse than a hole it will never know
   *     about. Collapsing -2 into -1 is how a "clean" retry corrupts a stream. */
  int ok = (*dstat & 0x1) ? 0 : -2;
  tx_cur = (tx_cur + 1) % NTX;
  return ok;
}

int e1000_poll_recv(void *buf, int max) {
  if (!ready) return -1;
  rx_desc_t *d = &rx_ring[rx_cur];
  volatile uint8_t *dstat = &d->status;
  if (!(*dstat & 0x1)) return 0;
  __asm__ volatile("" ::: "memory");

  int len = d->length;
  if (len > max) len = max;
  if (!(d->errors) && len > 0) {
    uint8_t *src = rx_bufs + (uint64_t)rx_cur * RXBUF;
    uint8_t *dst = (uint8_t *)buf;
    for (int i = 0; i < len; i++) dst[i] = src[i];
  } else {
    len = 0;
  }
  d->status = 0;
  d->length = 0;
  R(REG_RDT) = rx_cur;
  rx_cur = (rx_cur + 1) % NRX;
  return len;
}

/* ---- ARP self test --------------------------------------------------- */
static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

/* ---- IPv4/ICMP echo ------------------------------------------------- */
static uint16_t ip_csum(const void *data, int len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t sum = 0;
  for (int i = 0; i + 1 < len; i += 2) sum += ((uint32_t)p[i] << 8) | p[i + 1];
  if (len & 1) sum += (uint32_t)p[len - 1] << 8;
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

/* Echo one ICMP request. Returns 1 when the reply came back. */
int e1000_ping(const uint8_t dst_mac[6], const uint8_t src_ip[4],
               const uint8_t dst_ip[4], uint16_t seq) {
  if (!ready) return 0;
  uint8_t f[98];
  for (int i = 0; i < (int)sizeof f; i++) f[i] = 0;
  for (int i = 0; i < 6; i++) f[i] = dst_mac[i];
  for (int i = 0; i < 6; i++) f[6 + i] = mac_addr[i];
  f[12] = 0x08;
  f[13] = 0x00;
  /* IPv4 */
  f[14] = 0x45;
  f[16] = 0;
  f[17] = (uint8_t)(20 + 8 + 32);
  f[18] = 0x4d;
  f[19] = 0x45;
  f[22] = 64;
  f[23] = 1; /* icmp */
  for (int i = 0; i < 4; i++) f[26 + i] = src_ip[i];
  for (int i = 0; i < 4; i++) f[30 + i] = dst_ip[i];
  uint16_t ipc = ip_csum(f + 14, 20);
  f[24] = (uint8_t)(ipc >> 8);
  f[25] = (uint8_t)ipc;
  /* ICMP echo request */
  f[34] = 8;
  f[38] = 0x4d;
  f[39] = 0x45;
  f[40] = (uint8_t)(seq >> 8);
  f[41] = (uint8_t)seq;
  for (int i = 0; i < 32; i++) f[42 + i] = (uint8_t)('a' + (i % 26));
  uint16_t ic = ip_csum(f + 34, 40);
  f[36] = (uint8_t)(ic >> 8);
  f[37] = (uint8_t)ic;

  if (e1000_send(f, sizeof f) != 0) return 0;

  uint8_t r[2048];
  for (int tries = 0; tries < 20000; tries++) {
    int n = e1000_poll_recv(r, sizeof r);
    if (n >= 42 && r[12] == 0x08 && r[13] == 0x00 && r[23] == 1 &&
        r[34] == 0 && r[40] == (uint8_t)(seq >> 8) && r[41] == (uint8_t)seq) {
      return 1;
    }
    for (volatile int i = 0; i < 50000; i++) {
    }
  }
  return 0;
}

void e1000_selftest(void) {
  if (!ready) {
    s_puts("[nic] selftest skipped, no nic\n");
    return;
  }
  uint8_t frame[64];
  for (int i = 0; i < 64; i++) frame[i] = 0;
  for (int i = 0; i < 6; i++) frame[i] = 0xFF; /* broadcast */
  for (int i = 0; i < 6; i++) frame[6 + i] = mac_addr[i];
  put16(frame + 12, 0x0806);
  put16(frame + 14, 0x0001); /* ethernet */
  put16(frame + 16, 0x0800); /* ipv4 */
  frame[18] = 6;
  frame[19] = 4;
  put16(frame + 20, 0x0001); /* request */
  for (int i = 0; i < 6; i++) frame[22 + i] = mac_addr[i];
  frame[28] = 10;
  frame[29] = 0;
  frame[30] = 2;
  frame[31] = 15; /* our slirp address */
  /* target hw left zero */
  frame[38] = 10;
  frame[39] = 0;
  frame[40] = 2;
  frame[41] = 2; /* gateway */

  int rc = -1;
  for (int attempt = 0; attempt < 6; attempt++) {
    rc = e1000_send(frame, 60);
    if (rc == 0) break;
  }
  s_puts("[nic] arp request -> 10.0.2.2 tx=");
  s_puts(rc == 0 ? "ok" : "fail");
  s_puts("\n");
  if (rc != 0) return;

  uint8_t rxbuf[2048];
  for (int tries = 0; tries < 20000; tries++) {
    int n = e1000_poll_recv(rxbuf, sizeof rxbuf);
    if (n >= 42 && rxbuf[12] == 0x08 && rxbuf[13] == 0x06 &&
        rxbuf[20] == 0x00 && rxbuf[21] == 0x02) {
      s_puts("[nic] arp REPLY from ");
      s_dec(rxbuf[28]);
      s_putc('.');
      s_dec(rxbuf[29]);
      s_putc('.');
      s_dec(rxbuf[30]);
      s_putc('.');
      s_dec(rxbuf[31]);
      s_puts(" hw ");
      for (int i = 0; i < 6; i++) {
        if (i) s_putc(':');
        if (rxbuf[22 + i] < 16) s_putc('0');
        s_hex(rxbuf[22 + i]);
      }
      s_puts("  RX/TX verified\n");
      uint8_t gw[6];
      for (int i = 0; i < 6; i++) gw[i] = rxbuf[22 + i];
      uint8_t me[4] = {10, 0, 2, 15}, them[4] = {10, 0, 2, 2};
      int ok = e1000_ping(gw, me, them, 1);
      s_puts("[nic] icmp echo 10.0.2.2 seq=1 ");
      s_puts(ok ? "REPLY - ip stack path verified\n" : "no reply\n");
      return;
    }
    for (volatile int i = 0; i < 50000; i++) {
    }
  }
  s_puts("[nic] arp timed out, no reply\n");
}