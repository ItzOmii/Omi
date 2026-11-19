/*
 * MeepOS - Intel HDA (ICH6-class) audio driver
 *
 * Register offsets are the real ICH6 ones (NOT the order they appear in most
 * blog posts): CORB lives at 0x40, RIRB at 0x50, the int status at 0x24, and a
 * stream descriptor is 0x20 bytes with FORMAT at +0x12 and the BDL at +0x18.
 * Getting any of those wrong reads back plausible garbage and every verb times
 * out, so they are spelled out below rather than computed.
 */

#include "hda.h"
#include "pci.h"
#include "mmio.h"
#include "types.h"
#include "kmalloc.h"

/* ---- controller registers ------------------------------------------- */
#define REG_GCAP 0x00
#define REG_VMIN 0x02
#define REG_VMAJ 0x03
#define REG_GCTL 0x08
#define REG_STATESTS 0x0E
#define REG_GSTS 0x10
#define REG_INTCTL 0x20
#define REG_INTSTS 0x24
#define REG_CORBLBASE 0x40
#define REG_CORBUBASE 0x44
#define REG_CORBWP 0x48
#define REG_CORBRP 0x4A
#define REG_CORBCTL 0x4C
#define REG_CORBSIZE 0x4E
#define REG_RIRBLBASE 0x50
#define REG_RIRBUBASE 0x54
#define REG_RIRBWP 0x58
#define REG_RINTCNT 0x5A
#define REG_RIRBCTL 0x5C
#define REG_RIRBSIZE 0x5E

#define GCTL_RESET 0x01
#define CORBRP_RST 0x8000
#define CORBCTL_RUN 0x02
#define RIRBWP_RST 0x8000
#define RBCTL_DMA_EN 0x02

/* stream descriptor: SDI0..3 at 0x80, then SDO0..3 at 0x100 */
#define SD_BASE(i) (0x80 + 0x20 * (i))
#define SD_CTL 0x00
#define SD_STS 0x03
#define SD_LPIB 0x04
#define SD_CBL 0x08
#define SD_LVI 0x0C
#define SD_FORMAT 0x12
#define SD_BDLPL 0x18
#define SD_BDLPU 0x1C

/* stream control bits */
#define SD_CTL_SRST 0x00000001
#define SD_CTL_RUN 0x00000002
#define SD_CTL_IOCE 0x00000004
#define SD_CTL_DIR 0x00080000 /* 1 = capture */
#define SD_STS_BCIS 0x04

/* verb codes */
#define VERB_GET_PARAM 0xF00
#define VERB_SET_FORMAT 0x200
#define VERB_SET_CHAN_STREAMID 0x706
#define VERB_SET_PIN_CTL 0x707
#define VERB_SET_AMP 0x300
#define VERB_FUNC_GROUP_RESET 0x7FF

/* parameters */
#define PAR_NODE_COUNT 0x04
#define PAR_FUNC_TYPE 0x05
#define PAR_WIDGET_CAP 0x09
#define PAR_PIN_CAP 0x0C
#define PAR_CONN_LIST 0x0E

/* widget types */
#define WID_OUTPUT 0x0
#define WID_INPUT 0x1
#define WID_PIN 0x4
#define FUNC_AUDIO 0x01

/* amp payloads: output/input amp, both channels, gain 0x7f.
   QEMU (and the spec) start every amp at gain 0 = silence, so a stream that
   runs perfectly still produces zeros until this is written. */
#define AMP_OUT (0x8000 | 0x2000 | 0x1000 | 0x7F)
#define AMP_IN (0x4000 | 0x2000 | 0x1000 | 0x7F)
#define PIN_OUT_EN 0x40

/* ---- serial log ------------------------------------------------------ */
#define COM1 0x3F8
static inline void h_outb(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(p));
}
static inline uint8_t h_inb(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
  return v;
}
static void h_putc(char c) {
  while ((h_inb(COM1 + 5) & 0x20) == 0) {
  }
  h_outb(COM1, (uint8_t)c);
}
static void h_puts(const char *s) {
  while (*s) h_putc(*s++);
}
static void h_dec(uint32_t v) {
  char b[12];
  int i = 11;
  b[i--] = 0;
  if (!v) b[i--] = '0';
  while (v) {
    b[i--] = (char)('0' + (v % 10));
    v /= 10;
  }
  h_puts(&b[i + 1]);
}
static void h_hex(uint32_t v) {
  h_puts("0x");
  for (int s = 28; s >= 0; s -= 4) h_putc("0123456789abcdef"[(v >> s) & 0xF]);
}

/* ---- state ----------------------------------------------------------- */
static volatile uint8_t *mmio;
static uint32_t *corb;
static uint64_t *rirb;
static int ready;
static uint8_t dac_nid, adc_nid, out_pin, in_pin;
static int have_dac, have_adc;
static int corb_wp;      /* local copy of our write pointer */
static uint32_t rirb_seen;

#define NBUF 4
#define BUFSZ 8192
#define RING (NBUF * BUFSZ)

/* DMA memory used by both streams; static so it is identity-mapped and
   trivially converted with v2p(). */
static uint8_t out_ring[RING] __attribute__((aligned(4096)));
static uint8_t in_ring[RING] __attribute__((aligned(4096)));

typedef struct {
  uint64_t addr;
  uint32_t len;
  uint32_t flags;
} __attribute__((packed)) bdl_entry_t;

static bdl_entry_t out_bdl[NBUF] __attribute__((aligned(128)));
static bdl_entry_t in_bdl[NBUF] __attribute__((aligned(128)));

static inline uint64_t v2p(const void *p) {
  uint64_t v = (uint64_t)p;
  return kernel_phys_base + (v - kernel_virt_base);
}

/* ---- mmio ------------------------------------------------------------ */
static uint32_t rd32(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }
static void wr32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio + off) = v; }
static uint16_t rd16(uint32_t off) { return *(volatile uint16_t *)(mmio + off); }
static void wr16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(mmio + off) = v; }
static uint8_t rd8(uint32_t off) { return *(volatile uint8_t *)(mmio + off); }
static void wr8(uint32_t off, uint8_t v) { *(volatile uint8_t *)(mmio + off) = v; }

int hda_ready(void) { return ready && have_dac && have_adc; }



static int wait_bits(uint32_t off, uint32_t mask, uint32_t want, int tries) {
  for (int i = 0; i < tries; i++) {
    if ((rd32(off) & mask) == want) return 1;
    for (volatile int d = 0; d < 1000; d++) {
    }
  }
  return 0;
}

/* ---- CORB/RIRB verb channel ------------------------------------------ */
static int send_verb(uint8_t cad, uint8_t nid, uint16_t verb, uint16_t payload,
                     uint32_t *resp) {
  uint32_t cmd = ((uint32_t)cad << 28) | ((uint32_t)nid << 20) |
                 ((uint32_t)verb << 8) | payload;
  corb_wp = (corb_wp + 1) & 0xFF;
  corb[corb_wp] = cmd;
  __sync_synchronize();
  wr16(REG_CORBWP, (uint16_t)corb_wp);

  for (int tries = 0; tries < 200000; tries++) {
    uint16_t wp = rd16(REG_RIRBWP);
    if ((wp & 0xFF) != rirb_seen) {
      rirb_seen = wp & 0xFF;
      if (resp) *resp = (uint32_t)rirb[rirb_seen];
      return 1;
    }
    for (volatile int d = 0; d < 200; d++) {
    }
  }
  return 0;
}

static int param(uint8_t nid, uint16_t par, uint32_t *out) {
  return send_verb(0, nid, VERB_GET_PARAM, par, out);
}

/* ---- init ------------------------------------------------------------ */
int hda_init(void) {
  pci_device_info_t devs[4];
  int n = pci_scan_class(0x04, 0x03, 0xFF, devs, 4);
  if (n <= 0) n = pci_scan_class(0x04, 0x03, 0x00, devs, 4);
  if (n <= 0) {
    h_puts("[hda] no audio controller\n");
    return -1;
  }
  uint64_t bar = devs[0].bar[0] & 0xFFFFFFF0ull;
  h_puts("[hda] controller ");
  h_hex(devs[0].vendor_id);
  h_puts(":");
  h_hex(devs[0].device_id);
  h_puts(" bar0=");
  h_hex((uint32_t)bar);
  h_puts("\n");

  /* bus master + memory space so CORB/RIRB DMA works */
  uint32_t cmd = pci_read32(devs[0].bus, devs[0].device, devs[0].function, 0x04);
  cmd |= 0x6;
  pci_write32(devs[0].bus, devs[0].device, devs[0].function, 0x04, cmd);

  mmio = (volatile uint8_t *)mmio_map_range(bar, 0x4000);
  if (!mmio) {
    h_puts("[hda] mmio map failed\n");
    return -1;
  }
  h_puts("[hda] gcap=");
  h_hex(rd16(REG_GCAP));
  h_puts(" vmin=");
  h_dec(rd8(REG_VMIN));
  h_puts(" v=");
  h_dec(rd8(REG_VMAJ));
  h_puts("\n");

  /* controller reset: CRST 1 then 0, wait for GSTS to settle */
  wr32(REG_GCTL, rd32(REG_GCTL) & ~1u);
  for (volatile int d = 0; d < 100000; d++) {
  }
  wr32(REG_GCTL, rd32(REG_GCTL) | GCTL_RESET);
  for (volatile int d = 0; d < 100000; d++) {
  }
  wr32(REG_GCTL, rd32(REG_GCTL) & ~GCTL_RESET);
  if (!wait_bits(REG_GCTL, GCTL_RESET, 0, 20000)) {
    h_puts("[hda] reset stuck\n");
    return -1;
  }

  /* codec presence */
  uint16_t statests = rd16(REG_STATESTS);
  h_puts("[hda] statests=");
  h_hex(statests);
  h_puts("\n");
  wr16(REG_STATESTS, statests);

  /* CORB: 256 dwords */
  static uint32_t corb_mem[256] __attribute__((aligned(256)));
  static uint64_t rirb_mem[256] __attribute__((aligned(256)));
  corb = corb_mem;
  rirb = rirb_mem;
  corb_wp = 0;

  wr8(REG_CORBSIZE, 0x02); /* 256 entries */
  wr8(REG_RIRBSIZE, 0x02);
  wr32(REG_CORBLBASE, (uint32_t)v2p(corb));
  wr32(REG_CORBUBASE, (uint32_t)(v2p(corb) >> 32));
  wr32(REG_RIRBLBASE, (uint32_t)v2p(rirb));
  wr32(REG_RIRBUBASE, (uint32_t)(v2p(rirb) >> 32));

  /* reset both pointers (write 1 to the top bit, then clear it) */
  wr16(REG_CORBRP, CORBRP_RST);
  if (!wait_bits(REG_CORBRP, CORBRP_RST, CORBRP_RST, 1000))
    h_puts("[hda] corbrp reset did not latch\n");
  wr16(REG_CORBRP, 0);
  wr16(REG_RIRBWP, RIRBWP_RST);
  wr16(REG_RINTCNT, 0xFF);
  wr8(REG_CORBCTL, CORBCTL_RUN);
  wr8(REG_RIRBCTL, RBCTL_DMA_EN);

  /* find the audio function group: root's subnodes, first FUNC_AUDIO */
  uint32_t v = 0;
  if (!param(0, PAR_NODE_COUNT, &v)) {
    h_puts("[hda] root verb timeout\n");
    return -1;
  }
  int first_node = (v >> 16) & 0xFF, node_count = v & 0xFF;
  h_puts("[hda] root nodes start=");
  h_dec((unsigned)first_node);
  h_puts(" count=");
  h_dec((unsigned)node_count);
  h_puts("\n");

  uint8_t afg = 0;
  for (int i = 0; i < node_count; i++) {
    uint32_t ft = 0;
    if (!param((uint8_t)(first_node + i), PAR_FUNC_TYPE, &ft)) continue;
    if ((ft & 0xFF) == FUNC_AUDIO) {
      afg = (uint8_t)(first_node + i);
      break;
    }
  }
  if (!afg) {
    h_puts("[hda] no audio function group\n");
    return -1;
  }
  h_puts("[hda] afg nid=");
  h_dec(afg);
  h_puts("\n");

  send_verb(0, afg, VERB_FUNC_GROUP_RESET, 0, 0);

  if (!param(afg, PAR_NODE_COUNT, &v)) {
    h_puts("[hda] afg verb timeout\n");
    return -1;
  }
  int wfirst = (v >> 16) & 0xFF, wcount = v & 0xFF;
  h_puts("[hda] widgets start=");
  h_dec((unsigned)wfirst);
  h_puts(" count=");
  h_dec((unsigned)wcount);
  h_puts("\n");

  for (int i = 0; i < wcount; i++) {
    uint8_t nid = (uint8_t)(wfirst + i);
    uint32_t cap = 0, pc = 0;
    if (!param(nid, PAR_WIDGET_CAP, &cap)) continue;
    int type = (int)((cap >> 20) & 0xF);
    if (type == WID_OUTPUT && !have_dac) {
      dac_nid = nid;
      have_dac = 1;
    } else if (type == WID_INPUT && !have_adc) {
      adc_nid = nid;
      have_adc = 1;
    } else if (type == WID_PIN) {
      param(nid, PAR_PIN_CAP, &pc);
      /* PIN_CAP bit4 = output capable, bit5 = input capable.
         (bit0 is impedance sense - reading that as "output" finds no pin.) */
      if ((pc & 0x10) && !out_pin) out_pin = nid;
      if ((pc & 0x20) && !in_pin) in_pin = nid;
    }
  }
  h_puts("[hda] dac=");
  have_dac ? h_dec(dac_nid) : h_puts("-");
  h_puts(" adc=");
  have_adc ? h_dec(adc_nid) : h_puts("-");
  h_puts(" out_pin=");
  out_pin ? h_dec(out_pin) : h_puts("-");
  h_puts(" in_pin=");
  in_pin ? h_dec(in_pin) : h_puts("-");
  h_puts("\n");

  if (!have_dac && !have_adc) return -1;

  /* 48 kHz, 16 bit, 2 channels = 0x0011 (rate 0 = 48k, size 1 = 16 bit,
     channels-1 = 1) */
  uint16_t fmt = 0x0011;
  if (have_dac) {
    send_verb(0, dac_nid, VERB_SET_FORMAT, fmt, 0);
    send_verb(0, dac_nid, VERB_SET_CHAN_STREAMID, 0x10, 0); /* tag 1, ch 0 */
    send_verb(0, dac_nid, VERB_SET_AMP, AMP_OUT, 0);
    if (out_pin) send_verb(0, out_pin, VERB_SET_PIN_CTL, PIN_OUT_EN, 0);
  }
  if (have_adc) {
    send_verb(0, adc_nid, VERB_SET_FORMAT, fmt, 0);
    send_verb(0, adc_nid, VERB_SET_CHAN_STREAMID, 0x20, 0); /* tag 2, ch 0 */
    send_verb(0, adc_nid, VERB_SET_AMP, AMP_IN, 0);
    if (in_pin) send_verb(0, in_pin, VERB_SET_PIN_CTL, 0x00, 0);
  }

  ready = 1;
  h_puts("[hda] codec configured 48k/16/2\n");
  return 0;
}

/* ---- stream helpers -------------------------------------------------- */
static void sd_stop(int sd) {
  uint32_t base = SD_BASE(sd);
  wr32(base + SD_CTL, rd32(base + SD_CTL) & ~SD_CTL_RUN);
  for (int i = 0; i < 10000; i++) {
    if (!(rd32(base + SD_CTL) & SD_CTL_RUN)) break;
  }
  wr8(base + SD_STS, 0xFF);
}

/* Program a stream descriptor for a ring of NBUF buffers. */
static int sd_start(int sd, bdl_entry_t *bdl, uint8_t *ring, uint32_t tag,
                    int capture) {
  uint32_t base = SD_BASE(sd);
  for (int i = 0; i < NBUF; i++) {
    bdl[i].addr = v2p(ring + (uint64_t)i * BUFSZ);
    bdl[i].len = BUFSZ;
    bdl[i].flags = 0x01; /* IOC */
  }
  __sync_synchronize();

  sd_stop(sd);
  wr32(base + SD_CTL, SD_CTL_SRST);
  for (int i = 0; i < 100000; i++) {
    if (rd32(base + SD_CTL) & SD_CTL_SRST) break;
  }
  wr32(base + SD_CTL, 0);
  for (int i = 0; i < 100000; i++) {
    if (!(rd32(base + SD_CTL) & SD_CTL_SRST)) break;
  }

  wr32(base + SD_BDLPL, (uint32_t)v2p(bdl));
  wr32(base + SD_BDLPU, (uint32_t)(v2p(bdl) >> 32));
  wr32(base + SD_CBL, RING);
  wr32(base + SD_LVI, NBUF - 1);
  wr16(base + SD_FORMAT, 0x0011);
  wr32(base + SD_LPIB, 0);
  wr8(base + SD_STS, 0xFF);

  uint32_t ctl = (tag << 20) | SD_CTL_RUN | SD_CTL_IOCE;
  if (capture) ctl |= SD_CTL_DIR;
  wr32(base + SD_CTL, ctl);
  __sync_synchronize();

  for (int i = 0; i < 100000; i++) {
    if (rd32(base + SD_CTL) & SD_CTL_RUN) return 0;
  }
  return -1;
}

/* ---- playback -------------------------------------------------------- */
/* sd: descriptor index. SDO0 == 4 (0..3 are capture on ICH6). */
#define SD_OUT 4
#define SD_IN 0

int hda_play(const int16_t *pcm, int bytes) {
  if (!ready || !have_dac || bytes <= 0) return -1;
  const uint8_t *src = (const uint8_t *)pcm;
  int produced = 0;

  /* prime the ring: real audio first, silence if we have less than a ring */
  for (int b = 0; b < NBUF; b++) {
    uint8_t *dst = out_ring + (uint64_t)b * BUFSZ;
    for (int i = 0; i < BUFSZ; i++) dst[i] = 0;
    int n = bytes - produced;
    if (n > BUFSZ) n = BUFSZ;
    if (n > 0) {
      for (int i = 0; i < n; i++) dst[i] = src[produced + i];
      produced += n;
    }
  }

  if (sd_start(SD_OUT, out_bdl, out_ring, 1, 0) != 0) {
    h_puts("[hda] output stream did not start\n");
    return -1;
  }

  uint32_t base = SD_BASE(SD_OUT);
  uint32_t last = 0;
  uint64_t played = 0;
  int stalled = 0;

  /* The ring plays sequentially, so buffer b may be refilled once the play
     pointer has passed its end, i.e. once played + RING >= produced + BUFSZ.
     One buffer of headroom keeps the codec from ever seeing a hole. */
  while (played < (uint64_t)bytes) {
    uint32_t lpib = rd32(base + SD_LPIB) % RING;
    uint32_t delta = (lpib + RING - last) % RING;
    last = lpib;
    if (!delta) {
      if (++stalled > 200000) break;
      for (volatile int d = 0; d < 500; d++) {
      }
      continue;
    }
    stalled = 0;
    played += delta;

    while (produced < bytes &&
           (uint64_t)played + RING >= (uint64_t)produced + BUFSZ) {
      int b = (produced / BUFSZ) % NBUF;
      uint8_t *dst = out_ring + (uint64_t)b * BUFSZ;
      int n = bytes - produced;
      if (n > BUFSZ) n = BUFSZ;
      for (int i = 0; i < BUFSZ; i++) dst[i] = 0;
      for (int i = 0; i < n; i++) dst[i] = src[produced + i];
      produced += BUFSZ;
      __sync_synchronize();
    }
  }

  sd_stop(SD_OUT);
  h_puts("[hda] played ");
  h_dec((uint32_t)played);
  h_puts(" bytes\n");
  return (int)played;
}

/* ---- capture --------------------------------------------------------- */
/* ---- streaming playback ------------------------------------------------
   hda_play() restarts the stream for every call, which is right for a
   one-shot burst and wrong for a media player (it clicks and stalls). These
   keep one stream running and let the caller top up the ring as space
   frees up. Sizes are bytes of interleaved s16le at 48 kHz stereo. */
static uint64_t st_written;    /* bytes handed to the ring since start */
static uint64_t st_consumed;   /* bytes the codec has played since start */
static uint32_t st_last_lpib;
static int      st_running;

int hda_stream_start(void) {
  if (!ready || !have_dac) return -1;
  for (int i = 0; i < RING; i++) out_ring[i] = 0;
  st_written = 0;
  st_consumed = 0;
  st_last_lpib = 0;
  if (sd_start(SD_OUT, out_bdl, out_ring, 1, 0) != 0) {
    st_running = 0;
    return -1;
  }
  st_running = 1;
  return 0;
}

/* Bytes that can be written without overrunning the play pointer.
   LPIB is a position inside the ring, so a wrapped ring can sit at an
   address below an earlier one -- compare deltas, never absolute values,
   or the ring looks permanently full after the first lap. */
int hda_stream_free(void) {
  if (!st_running) return 0;
  uint32_t lpib = rd32(SD_BASE(SD_OUT) + SD_LPIB) % RING;
  uint32_t delta = (lpib + RING - st_last_lpib) % RING;
  st_last_lpib = lpib;
  st_consumed += delta;
  if (st_consumed > st_written) st_consumed = st_written;
  uint64_t in_ring = st_written - st_consumed;
  if (in_ring >= (uint64_t)RING) return 0;
  uint64_t free_bytes = (uint64_t)RING - in_ring;
  /* one buffer of headroom so the codec never reads a hole */
  if (free_bytes > BUFSZ) free_bytes -= BUFSZ;
  else free_bytes = 0;
  return (int)free_bytes;
}

int hda_stream_write(const int16_t *pcm, int bytes) {
  if (!st_running || bytes <= 0) return -1;
  int free_bytes = hda_stream_free();
  if (bytes > free_bytes) bytes = free_bytes;
  if (bytes <= 0) return 0;
  const uint8_t *src = (const uint8_t *)pcm;
  int written = 0;
  while (written < bytes) {
    uint64_t at = (st_written + (uint64_t)written) % RING;
    int chunk = (int)(RING - at);
    int left = bytes - written;
    if (chunk > left) chunk = left;
    for (int i = 0; i < chunk; i++) out_ring[at + i] = src[written + i];
    written += chunk;
  }
  st_written += (uint64_t)written;
  return written;
}

void hda_stream_stop(void) {
  if (!st_running) return;
  /* let what is queued drain, then stop the engine */
  for (int spins = 0; spins < 400000; spins++) {
    uint32_t lpib = rd32(SD_BASE(SD_OUT) + SD_LPIB) % RING;
    if ((uint64_t)lpib == (st_written % RING)) break;
    for (volatile int d = 0; d < 200; d++) {
    }
  }
  sd_stop(SD_OUT);
  st_running = 0;
}

int hda_stream_active(void) { return st_running; }

int hda_record(int16_t *buf, int bytes) {
  if (!ready || !have_adc || bytes <= 0) return -1;
  for (int i = 0; i < RING; i++) in_ring[i] = 0;
  if (sd_start(SD_IN, in_bdl, in_ring, 2, 1) != 0) {
    h_puts("[hda] input stream did not start\n");
    return -1;
  }

  uint32_t base = SD_BASE(SD_IN);
  uint32_t last = 0;
  uint64_t consumed = 0;
  uint8_t *dst = (uint8_t *)buf;
  int written = 0;
  int stalled = 0;

  while (written < bytes) {
    uint32_t lpib = rd32(base + SD_LPIB) % RING;
    uint32_t delta = (lpib + RING - last) % RING;
    last = lpib;
    if (!delta) {
      if (++stalled > 200000) break;
      for (volatile int d = 0; d < 500; d++) {
      }
      continue;
    }
    stalled = 0;
    consumed += delta;

    /* every whole buffer the hardware has finished writing is ours to copy */
    while (written + BUFSZ <= (int)consumed && written < bytes) {
      int b = (written / BUFSZ) % NBUF;
      uint8_t *srcb = in_ring + (uint64_t)b * BUFSZ;
      int n = bytes - written;
      if (n > BUFSZ) n = BUFSZ;
      for (int i = 0; i < n; i++) dst[written + i] = srcb[i];
      written += n;
    }
  }
  sd_stop(SD_IN);
  return written;
}

/* ---- self test ------------------------------------------------------- */
/* A 440 Hz tone at 48 kHz, 0.25 s, so playback noise is recognisable and the
   WAV capture has something with an obvious spectrum. */
static int16_t tone[24000];

void hda_selftest(void) {
  if (hda_init() != 0) {
    h_puts("[hda] init failed\n");
    return;
  }
  /* ~30 ms of 440 Hz so boot stays quick; the point is a nonzero, correct
     amplitude signal reaching the codec, not a concert. */
  int n = 24000 / 8; /* samples, stereo -> 3000 frames = 62 ms */
  for (int i = 0; i < n; i++) {
    int ph = (i / 2) % 109; /* 48k/440 ~= 109 samples per cycle */
    int16_t v = (int16_t)((ph < 54 ? 1 : -1) * 9000);
    tone[i] = v;
  }
  if (have_dac) {
    int played = hda_play(tone, n * 2);
    h_puts("[hda] tx=");
    h_dec((uint32_t)played);
    h_puts("\n");
  } else {
    h_puts("[hda] no DAC, playback skipped\n");
  }

  if (!have_adc) {
    h_puts("[hda] no ADC, capture skipped\n");
    return;
  }
  static int16_t cap[24000]; /* 250 ms stereo at 48 kHz */
  int got = hda_record(cap, sizeof cap);
  if (got > 0) {
    uint64_t sumsq = 0;
    int peak = 0, nz = 0;
    int samples = got / 2;
    for (int i = 0; i < samples; i++) {
      int v = cap[i];
      int a = v < 0 ? -v : v;
      if (a > peak) peak = a;
      if (v) nz++;
      sumsq += (uint64_t)((int64_t)a * a);
    }
    h_puts("[hda] rx=");
    h_dec((uint32_t)got);
    h_puts(" peak=");
    h_dec((uint32_t)peak);
    h_puts(" nonzero=");
    h_dec((uint32_t)nz);
    h_puts(" rms=");
    h_dec(samples ? (uint32_t)(sumsq / (uint64_t)samples) : 0);
    h_puts("\n");
  } else {
    h_puts("[hda] capture failed\n");
  }
}

/* ---- volume ----------------------------------------------------------
 *
 * The DAC's output amp is the only gain this driver owns: gain 0 of 0x7f is
 * silence, so the tray slider is a write to that amp and nothing else. QEMU's
 * mixer and a real codec both honour it, which is why muting is a gain of 0
 * rather than a flag anyone has to check while mixing.
 */
static int vol_pct = 70;
static int vol_muted = 0;

static void apply_volume(void) {
  if (!have_dac) return;
  int gain = vol_muted ? 0 : (vol_pct * 0x7F) / 100;
  send_verb(0, dac_nid, VERB_SET_AMP,
            (uint16_t)(0x8000 | 0x2000 | 0x1000 | (gain & 0x7F)), 0);
}

int audio_volume(void) { return vol_pct; }
int audio_muted(void) { return vol_muted; }

void audio_volume_set(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  vol_pct = pct;
  if (pct > 0) vol_muted = 0;      /* moving the slider is how you unmute */
  apply_volume();
}

void audio_mute(int on) {
  vol_muted = on ? 1 : 0;
  apply_volume();
}
