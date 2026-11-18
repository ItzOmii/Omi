/* The shared file: pages, owned by the object, handed to mappings rather than
 * copied into them. See shfile.h for why that is the whole design. */

#include "../include/shfile.h"
#include "../include/kmalloc.h"
#include "../include/pmm.h"
#include "../include/types.h"
#include "../include/errno.h"

static int live_objects;
static int live_pages;

static void *frame_new(void) {
  uint64_t phys = pmm_alloc_page();
  if (!phys) return 0;
  return (void *)(uintptr_t)(hhdm_offset + phys);
}

shfile_t *shfile_new(void) {
  shfile_t *f = kzalloc(sizeof *f);
  if (!f) return 0;
  f->refs = 1;
  f->pages = 0;
  f->cap = 0;
  f->page = 0;
  live_objects++;
  return f;
}

void shfile_ref(shfile_t *f) {
  if (f) f->refs++;
}

void shfile_unref(shfile_t *f) {
  if (!f) return;
  if (--f->refs > 0) return;
  for (int i = 0; i < f->cap; i++) {
    if (!f->page[i]) continue;
    /* The frames were never mapped into a user space by this call, so they are
     * ours to free: a mapping that is still alive is holding a reference, and
     * this branch only runs when it is not. */
    pmm_free_page((uint64_t)(uintptr_t)f->page[i] - hhdm_offset);
    f->page[i] = 0;
    live_pages--;
  }
  live_objects--;
  kfree(f->page);
  kfree(f);
}

int shfile_truncate(shfile_t *f, uint64_t size) {
  if (!f) return -EINVAL;
  uint64_t pg = (size + 0xFFF) / 0x1000;
  if (pg > SHFILE_MAX_PAGES) return -EINVAL;
  /* Growing does not allocate frames here: a page appears when it is first
   * touched, through shfile_page(create=1) on the fault path or through a write.
   * The index of frames is a different matter - it has to exist to be indexed,
   * so it grows in steps and is freed with the object. Two levels were avoided
   * on purpose: at 8 bytes a page a 256 MB region needs a 512 KB index, which is
   * one kmalloc, and a second level would only pay off past 16 GB. */
  if ((int)pg > f->cap) {
    int cap = f->cap ? f->cap : 16;
    while (cap < (int)pg) cap *= 2;
    void **n = (void **)kzalloc((size_t)cap * sizeof(void *));
    if (!n) return -ENOMEM;
    for (int i = 0; i < f->cap; i++) n[i] = f->page[i];
    kfree(f->page);
    f->page = n;
    f->cap = cap;
  }
  f->pages = (int)pg;
  return 0;
}

void *shfile_page(shfile_t *f, int idx, int create) {
  if (!f || idx < 0 || idx >= f->cap) return 0;
  if (f->page[idx]) return f->page[idx];
  if (idx >= f->pages) return 0;          /* past EOF: not a page to hand out */
  if (!create) return 0;
  void *p = frame_new();
  if (!p) return 0;
  live_pages++;
  f->page[idx] = p;
  return p;
}

int64_t shfile_read(shfile_t *f, uint64_t off, void *dst, uint64_t n) {
  if (!f) return -EINVAL;
  uint64_t size = shfile_size(f);
  if (off >= size) return 0;              /* EOF, not an error */
  if (off + n > size) n = size - off;
  uint8_t *d = dst;
  uint64_t done = 0;
  while (done < n) {
    uint64_t at = off + done;
    int idx = (int)(at >> 12);
    uint64_t skip = at & 0xFFF;
    uint64_t take = 4096 - skip;
    if (take > n - done) take = n - done;
    const uint8_t *src = idx < f->cap ? f->page[idx] : 0;
    /* A page inside the file that was never written is zeroes, and reading it
     * must not allocate one: the file's size is a claim about bytes, not about
     * frames. */
    if (!src) {
      for (uint64_t i = 0; i < take; i++) d[done + i] = 0;
    } else {
      const uint8_t *s = src + skip;
      for (uint64_t i = 0; i < take; i++) d[done + i] = s[i];
    }
    done += take;
  }
  return (int64_t)done;
}

int64_t shfile_write(shfile_t *f, uint64_t off, const void *src, uint64_t n) {
  if (!f) return -EINVAL;
  if (off + n > (uint64_t)SHFILE_MAX_PAGES * 4096) return -ENOSPC;   /* the claim ceiling */
  /* A write past the end grows the file, as it does on Linux: the descriptor is
   * a stream, and refusing would make a program that writes-then-maps fail for a
   * reason that has nothing to do with sharing. */
  if (off + n > shfile_size(f)) {
    int rc = shfile_truncate(f, off + n);
    if (rc != 0) return rc;
  }
  const uint8_t *s = src;
  uint64_t done = 0;
  while (done < n) {
    uint64_t at = off + done;
    int idx = (int)(at >> 12);
    uint64_t skip = at & 0xFFF;
    uint64_t take = 4096 - skip;
    if (take > n - done) take = n - done;
    uint8_t *d = shfile_page(f, idx, 1);
    if (!d) return -ENOMEM;
    d += skip;
    for (uint64_t i = 0; i < take; i++) d[i] = s[done + i];
    done += take;
  }
  return (int64_t)done;
}

uint64_t shfile_size(shfile_t *f) {
  return f ? (uint64_t)f->pages * 4096 : 0;
}

int shfile_live_objects(void) { return live_objects; }
int shfile_live_pages(void)   { return live_pages; }
