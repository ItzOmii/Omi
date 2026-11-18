/* Copy-on-write frame counts. See include/cow.h for what the three calls mean. */
#include "cow.h"

#define COW_SLOTS 4096          /* power of two; the mask below depends on it */

typedef struct {
  uint64_t phys;                /* frame address, 0 = never used, 1 = tombstone */
  uint32_t refs;                /* how many address spaces map it */
} cow_ent_t;

static cow_ent_t tab[COW_SLOTS];

static cow_stats_t stats;

/* Frames arrive page-aligned and clustered, so the low bits are worthless; the
 * multiply spreads the rest. Truncated to the table width. */
static uint64_t cow_hash(uint64_t phys) {
  uint64_t k = (phys >> 12) * 0x9E3779B97F4A7C15ULL;
  return (k >> 32) & (COW_SLOTS - 1);
}

/* The entry for a frame, or 0. With create set, an empty or tombstoned slot is
 * claimed and starts at two references: the mapping that already exists, plus
 * the one being added. */
static cow_ent_t *cow_slot(uint64_t phys, int create) {
  uint64_t h = cow_hash(phys);
  cow_ent_t *free_slot = 0;
  for (uint64_t i = 0; i < COW_SLOTS; i++) {
    cow_ent_t *e = &tab[(h + i) & (COW_SLOTS - 1)];
    if (e->phys == phys) return e;
    if (e->phys == 1) {                 /* tombstone: keep probing, but reuse it */
      if (!free_slot) free_slot = e;
      continue;
    }
    if (!e->phys) {
      if (!create) return 0;
      cow_ent_t *slot = free_slot ? free_slot : e;
      slot->phys = phys;
      slot->refs = 2;
      return slot;
    }
  }
  if (create && free_slot) {            /* the probe ran out with only tombstones */
    free_slot->phys = phys;
    free_slot->refs = 2;
    return free_slot;
  }
  return 0;
}

int cow_share(uint64_t phys) {
  if (!phys) return 0;
  cow_ent_t *e = cow_slot(phys, 0);
  if (e) {                              /* already shared: one more mapping */
    if (e->refs == 0xFFFFFFFFu) {       /* counted, not ignored */
      stats.saturations++;
      return 1;                         /* the ceiling is still "shared" */
    }
    e->refs++;
    stats.shares++;
    return 1;
  }
  if (!cow_slot(phys, 1)) return 0;
  stats.shares++;
  stats.live_frames++;
  return 1;
}

int cow_amount(uint64_t phys) {
  cow_ent_t *e = cow_slot(phys, 0);
  return e ? (int)e->refs : 0;
}

void cow_get_stats(cow_stats_t *out) { if (out) *out = stats; }

int cow_walk(int (*fn)(uint64_t phys, int refs, void *ctx), void *ctx) {
  int seen = 0;
  for (int i = 0; i < COW_SLOTS; i++) {
    cow_ent_t *e = &tab[i];
    if (e->phys < 2) continue;           /* empty, or a tombstone */
    seen++;
    if (fn(e->phys, (int)e->refs, ctx)) break;
  }
  return seen;
}

int cow_release(uint64_t phys) {
  cow_ent_t *e = cow_slot(phys, 0);
  if (!e) return 1;                     /* never shared: the caller is the owner */
  stats.releases++;
  if (e->refs > 1) {
    e->refs--;
    return 0;                           /* someone else still maps it */
  }
  e->phys = 1;                          /* tombstone, not empty: the probe must
                                         * continue past it */
  e->refs = 0;
  if (stats.live_frames) stats.live_frames--;
  return 1;
}