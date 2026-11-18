#ifndef OMI_SHFILE_H
#define OMI_SHFILE_H

/* A file that lives in pages the kernel can map, which is what makes
 * MAP_SHARED honest: there is no write-back path to write, because the pages a
 * mapping sees *are* the file's storage. Reading through the descriptor reads
 * the bytes a program just wrote through its mapping, and the other way round,
 * without either side copying anything.
 *
 * Chrome's shared-memory regions are this pair - memfd_create for the object,
 * then mmap MAP_SHARED over it - so the point of the thing is the sharing, not
 * the file. Linux has a page cache per file and a mapping is a window onto it;
 * here the file is the window's backing and there is only one copy of the bytes.
 *
 * Frames are owned by the object and handed out, never copied, which is why a
 * mapping sets PTE_SHARED: an unmap must not free a frame the file still owns,
 * and a fork must carry it across rather than copy it. */

#include <stdint.h>

/* How much a region may claim. This is a ceiling on the claim, not on memory:
 * pages are allocated when they are touched, so a 256 MB request costs nothing
 * until a program uses it. The old ceiling was 64 pages - 256 KiB - which is
 * smaller than one 1080p frame, so the first thing a browser would have done
 * with this file (hand a renderer a frame buffer) could never have worked. */
#define SHFILE_MAX_PAGES 65536   /* 256 MB per region */

typedef struct shfile {
  int    refs;                   /* one per owner: the descriptor, each mapping */
  int    pages;                  /* pages the file currently has */
  int    cap;                    /* slots in `page`, grown to fit `pages` */
  void **page;                   /* kernel address of each frame, or NULL */
} shfile_t;

shfile_t *shfile_new(void);
void      shfile_ref(shfile_t *f);
void      shfile_unref(shfile_t *f);

/* Grow or shrink to `size`, rounded up to a page. Shrinking frees whole pages and
 * leaves the count shorter; growing allocates zeroed frames on demand, and a page
 * the mapping touches before this call exists only after it.
 * Returns 0, -EINVAL for a size past the ceiling. */
int  shfile_truncate(shfile_t *f, uint64_t size);

/* The frame for page `idx`, allocating it if it is inside the file's size.
 * NULL when beyond EOF - the caller turns that into a real fault rather than
 * inventing a page, because a mapping past the end of its file is a bug in the
 * program and Linux reports it as one. */
void *shfile_page(shfile_t *f, int idx, int create);

int64_t shfile_read(shfile_t *f, uint64_t off, void *dst, uint64_t n);
int64_t shfile_write(shfile_t *f, uint64_t off, const void *src, uint64_t n);
uint64_t shfile_size(shfile_t *f);

/* Live objects and the pages they hold, for the boot audit: a program that
 * created a memfd, mapped it and exited must leave both at zero, and a leak of a
 * reference shows up here as an object that outlived its program. */
int shfile_live_objects(void);
int shfile_live_pages(void);

#endif
