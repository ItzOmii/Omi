/* See kernel/include/diskfs.h for the layout and what this is not. */

#include "../include/diskfs.h"
#include "../include/ata.h"
#include "../include/errno.h"

#define D_DIR_LBA   1
#define D_DIR_SECS  4
#define D_DATA_LBA  8
#define D_MAGIC     "OMIFS001"

typedef struct {
  char     magic[8];
  uint32_t version, nrec, next_lba, total_lba, sum;
  uint8_t  pad[512 - 8 - 20];
} __attribute__((packed)) dsuper_t;

typedef struct {
  char     name[32];
  uint32_t lba, len, secs, used;
  uint8_t  pad[16];
} __attribute__((packed)) drec_t;

static dsuper_t  sb;
static drec_t    dir[DISK_MAX_REC];
static uint8_t   sec[ATA_SECTOR];
static int       present;
static uint32_t  total;
static char      model[41];

static uint32_t sum_of(const dsuper_t *s) {
  uint32_t v = 0;
  for (int i = 0; i < 8; i++) v += (uint8_t)s->magic[i];
  return v + s->version + s->nrec + s->next_lba + s->total_lba;
}

static int str_eq(const char *a, const char *b) {
  int i = 0;
  for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
  return a[i] == b[i];
}

static void str_copy(char *dst, const char *src, int max) {
  int i = 0;
  for (; src[i] && i < max - 1; i++) dst[i] = src[i];
  dst[i] = 0;
}

static void zero(void *p, uint32_t n) {
  uint8_t *b = (uint8_t *)p;
  for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

static void copy(void *dst, const void *src, uint32_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static int load_dir(void) {
  for (int i = 0; i < D_DIR_SECS; i++)
    if (ata_read(D_DIR_LBA + i, (uint8_t *)dir + i * ATA_SECTOR) != 0) return -1;
  return 0;
}

static int store_dir(void) {
  for (int i = 0; i < D_DIR_SECS; i++)
    if (ata_write(D_DIR_LBA + i, (const uint8_t *)dir + i * ATA_SECTOR) != 0) return -1;
  return 0;
}

static int store_sb(void) {
  sb.sum = sum_of(&sb);
  return ata_write(0, &sb);
}

/* An empty disk, or one that does not carry our magic: write the layout. This
 * is the mkfs. It runs once per disk, on the first boot that finds it, because
 * a disk with no filesystem is a disk nobody can use yet. */
static int format(void) {
  zero(&sb, sizeof sb);
  copy(sb.magic, D_MAGIC, 8);
  sb.version = 1;
  sb.nrec = 0;
  sb.next_lba = D_DATA_LBA;
  sb.total_lba = total;
  zero(dir, sizeof dir);
  if (store_sb() != 0) return -1;
  if (store_dir() != 0) return -1;
  return 0;
}

int diskfs_present(void) { return present; }
uint32_t diskfs_sectors(void) { return total; }
const char *diskfs_model(void) { return model[0] ? model : "unknown"; }

int diskfs_init(void) {
  present = 0;
  if (ata_init() != 0) return -1;
  total = ata_sectors();
  str_copy(model, ata_model(), 41);

  if (ata_read(0, &sb) != 0) return -1;
  int ok = 1;
  for (int i = 0; i < 8; i++) if (sb.magic[i] != D_MAGIC[i]) ok = 0;
  if (ok && sb.sum != sum_of(&sb)) ok = 0;    /* a torn or foreign superblock */
  if (ok && (sb.next_lba < D_DATA_LBA || sb.next_lba > total)) ok = 0;
  if (!ok) {
    if (format() != 0) return -1;
  } else {
    if (load_dir() != 0) return -1;
    if (sb.nrec > DISK_MAX_REC) sb.nrec = DISK_MAX_REC;
  }
  present = 1;
  return 0;
}

int diskfs_count(void) { return present ? (int)sb.nrec : 0; }

int diskfs_entry(int i, const char **name, int *len) {
  if (!present || i < 0 || i >= (int)sb.nrec) return -1;
  if (!dir[i].used) return -1;
  dir[i].name[31] = 0;
  *name = dir[i].name;
  if (len) *len = (int)dir[i].len;
  return 0;
}

static drec_t *find(const char *name) {
  for (int i = 0; i < DISK_MAX_REC; i++)
    if (dir[i].used && str_eq(dir[i].name, name)) return &dir[i];
  return 0;
}

int diskfs_read(const char *name, uint8_t *buf, int max) {
  if (!present) return -ENODEV;
  drec_t *r = find(name);
  if (!r) return -ENOENT;
  int want = (int)r->len < max ? (int)r->len : max;
  int got = 0;
  for (uint32_t s = 0; s < r->secs && got < want; s++) {
    if (ata_read(r->lba + s, sec) != 0) return -EIO;
    int k = want - got < ATA_SECTOR ? want - got : ATA_SECTOR;
    copy(buf + got, sec, (uint32_t)k);
    got += k;
  }
  return got;
}

int diskfs_write(const char *name, const uint8_t *buf, int len) {
  if (!present) return -ENODEV;
  if (!name || !name[0] || len < 0 || len > (int)DISK_MAX_FILE) return -EINVAL;

  drec_t *r = find(name);
  if (!r) {
    for (int i = 0; i < DISK_MAX_REC; i++) if (!dir[i].used) { r = &dir[i]; break; }
    if (!r) return -ENOSPC;                    /* the directory is full, said out loud */
    zero(r, sizeof *r);
    str_copy(r->name, name, 32);
    r->used = 1;
    sb.nrec++;
  }

  uint32_t need = ((uint32_t)len + ATA_SECTOR - 1) / ATA_SECTOR;
  if (need > r->secs) {
    /* Growing: a new run of sectors from the bump pointer. The old run stays
     * allocated - there is no free list, and pretending otherwise is how a
     * filesystem corrupts itself. */
    if (sb.next_lba + need > total) return -ENOSPC;
    r->lba = sb.next_lba;
    r->secs = need;
    sb.next_lba += need;
  }

  for (uint32_t s = 0, off = 0; s < need; s++, off += ATA_SECTOR) {
    int k = len - (int)off < ATA_SECTOR ? len - (int)off : ATA_SECTOR;
    zero(sec, ATA_SECTOR);                     /* the tail of a short last sector */
    if (k > 0) copy(sec, buf + off, (uint32_t)k);
    if (ata_write(r->lba + s, sec) != 0) return -EIO;
  }
  r->len = (uint32_t)len;

  if (store_dir() != 0) return -EIO;
  if (store_sb() != 0) return -EIO;
  return 0;
}

int diskfs_remove(const char *name) {
  if (!present) return -ENODEV;
  drec_t *r = find(name);
  if (!r) return -ENOENT;
  r->used = 0;
  if (sb.nrec) sb.nrec--;
  if (store_dir() != 0) return -EIO;
  return store_sb();
}
