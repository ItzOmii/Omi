/* Copy-on-write bookkeeping for forked address spaces.
 *
 * A forked child shares its parent's frames read-only; the first store on either
 * side gets a private copy. That needs one fact the page tables cannot answer on
 * their own: *how many address spaces map this frame*. This table holds it, and
 * only for frames that are actually shared - a frame with no entry has exactly
 * one mapping and needs no protection.
 *
 * The interface is deliberately three calls and no policy: share (one more
 * mapping appeared), amount (is it shared right now), release (one mapping went
 * away - the caller frees the frame if this was the last). Everything about
 * *when* to copy lives in the mapping code, so there is one place to read.
 *
 * The table is fixed-size and open-addressed with tombstones. When it is full,
 * cow_share says so and the caller copies eagerly instead: a slower fork is a
 * much better failure than a wrong one. */

#ifndef COW_H
#define COW_H

#include <stdint.h>

/* One more address space now maps this frame. Returns 0 when the table is full
 * and the caller must copy instead, 1 when the frame is now shared. */
int cow_share(uint64_t phys);

/* How many mappings the frame has, 0 when it is unshared (no entry). */
int cow_amount(uint64_t phys);

/* Counts, so a summary line can state what the mechanism did rather than what it
 * was supposed to do. Saturations are counted, not silently absorbed: a frame
 * shared by more spaces than the counter holds is a fact worth seeing. */
typedef struct {
  uint64_t shares;        /* mappings created on an existing frame */
  uint64_t releases;      /* mappings dropped */
  uint64_t saturations;   /* shares that hit the counter ceiling */
  uint64_t live_frames;   /* frames with an entry right now */
} cow_stats_t;

void cow_get_stats(cow_stats_t *out);

/* Visit every frame that still has an entry. The callback returns non-zero to
 * stop. Whether a frame *should* still be mapped is a question only page tables
 * can answer, so that check stays with the caller and this stays a walk. */
int cow_walk(int (*fn)(uint64_t phys, int refs, void *ctx), void *ctx);

/* One mapping went away. Returns 1 when that was the last one, in which case the
 * caller owns the frame and must free it; 0 when someone else still maps it. */
int cow_release(uint64_t phys);

#endif