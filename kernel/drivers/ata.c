/* ATA PIO: IDENTIFY, then 28-bit LBA reads and writes, one sector at a time.
 * See kernel/include/ata.h for why PIO and why every wait is bounded. */

#include "ata.h"

extern void serial_puts(const char *s);

#define ATA_DATA    0x1F0
#define ATA_ERR     0x1F1
#define ATA_SECCNT  0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_STATUS  0x1F7
#define ATA_CMD     0x1F7
#define ATA_CTRL    0x3F6   /* device control: bit 1 set = no interrupts */

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_DRDY 0x40
#define ST_BSY  0x80

#define CMD_READ   0x20
#define CMD_WRITE  0x30
#define CMD_IDENT  0xEC
#define CMD_FLUSH  0xE7

/* Generous and finite. A poll loop over a port register on real hardware that
 * runs this long means the device is gone, not slow. */
#define POLL_MAX 2000000

static inline void outb(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
  return v;
}
/* Word-at-a-time, explicitly. The obvious `rep insw` with a "+D"(port) operand
 * assembles to `rep insw (%dx),%es:(%rdi)` with the port in %di - the compiler
 * picks the register the mnemonic does not use - and the first boot after that
 * faulted on a memory write to address 0x1F0. Two instructions per word is
 * slower and cannot be wrong. */
static inline uint16_t inw(uint16_t p) {
  uint16_t v;
  __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p));
  return v;
}
static inline void outw(uint16_t p, uint16_t v) {
  __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p));
}
static void insw(uint16_t p, void *buf, int words) {
  uint16_t *w = (uint16_t *)buf;
  for (int i = 0; i < words; i++) w[i] = inw(p);
}
static void outsw(uint16_t p, const void *buf, int words) {
  const uint16_t *w = (const uint16_t *)buf;
  for (int i = 0; i < words; i++) outw(p, w[i]);
}

static int      present;
static uint32_t total_sectors;
static char     model[41];
static uint32_t timeouts;

/* The 400 ns the spec asks for after a drive select is four reads of the status
 * register; on a machine that actually takes 400 ns to answer, they are what
 * gives it the time. */
static void delay400(void) {
  for (int i = 0; i < 4; i++) (void)inb(ATA_STATUS);
}

static int wait_ready(void) {
  for (int i = 0; i < POLL_MAX; i++) {
    uint8_t st = inb(ATA_STATUS);
    if (st == 0xFF) return -1;              /* floating bus: nothing on the cable */
    if (!(st & ST_BSY)) return (st & (ST_ERR | ST_DF)) ? -1 : 0;
  }
  timeouts++;
  return -1;
}

static int wait_drq(void) {
  for (int i = 0; i < POLL_MAX; i++) {
    uint8_t st = inb(ATA_STATUS);
    if (st & (ST_ERR | ST_DF)) return -1;
    if (!(st & ST_BSY) && (st & ST_DRQ)) return 0;
  }
  timeouts++;
  return -1;
}

int ata_present(void) { return present; }
uint32_t ata_sectors(void) { return total_sectors; }
const char *ata_model(void) { return model[0] ? model : "unknown"; }
uint32_t ata_timeouts(void) { return timeouts; }

int ata_init(void) {
  present = 0;
  model[0] = 0;
  total_sectors = 0;

  outb(ATA_CTRL, 0x02);                     /* nIEN: this driver polls only */
  outb(ATA_DRIVE, 0xA0);                    /* master, CHS, 28-bit later */
  delay400();

  if (inb(ATA_STATUS) == 0xFF) return -1;   /* no controller, or nothing on it */

  outb(ATA_SECCNT, 0);
  outb(ATA_LBA0, 0);
  outb(ATA_LBA1, 0);
  outb(ATA_LBA2, 0);
  outb(ATA_CMD, CMD_IDENT);
  if (inb(ATA_STATUS) == 0) return -1;      /* the command was not taken */

  if (wait_ready() != 0) return -1;
  /* A non-zero count here is how a packet device answers IDENTIFY; it is not a
   * disk and this driver cannot use it. */
  if (inb(ATA_LBA1) || inb(ATA_LBA2)) return -1;

  if (wait_drq() != 0) return -1;

  uint16_t id[256];
  insw(ATA_DATA, id, 256);

  total_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
  if (!total_sectors) return -1;

  /* Words 27..46 are the model in big-endian pairs, space padded. */
  int n = 0;
  for (int w = 27; w <= 46 && n < 40; w++) {
    model[n++] = (char)(id[w] >> 8);
    model[n++] = (char)(id[w] & 0xFF);
  }
  while (n > 0 && (model[n - 1] == ' ' || model[n - 1] == 0)) model[--n] = 0;
  model[40] = 0;

  present = 1;
  return 0;
}

/* Split the two operations at the command byte and share the setup: they differ
 * only in the command and in which direction 256 words travel. */
static int rw(uint32_t lba, void *buf, int write) {
  if (!present) return -1;
  if (lba >= total_sectors) return -1;
  if (wait_ready() != 0) return -1;

  outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
  delay400();
  outb(ATA_SECCNT, 1);
  outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
  outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
  outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
  outb(ATA_CMD, write ? CMD_WRITE : CMD_READ);
  delay400();

  if (wait_drq() != 0) return -1;
  if (write) {
    outsw(ATA_DATA, buf, 256);
    /* Without the flush the drive may still be holding the sector when the next
     * one is written, and a reboot in between loses it. QEMU does not need it;
     * a real disk does, and this is the layer where that matters. */
    outb(ATA_CMD, CMD_FLUSH);
    if (wait_ready() != 0) return -1;
  } else {
    insw(ATA_DATA, buf, 256);
  }
  return 0;
}

int ata_read(uint32_t lba, void *buf)  { return rw(lba, buf, 0); }
int ata_write(uint32_t lba, const void *buf) { return rw(lba, (void *)buf, 1); }
