/*
 * Omi - page image cache.
 *
 * Fetching and caching only: decoding is the system-wide image service in
 * kernel/media/image.c, so a PNG that renders in a page also renders as a
 * wallpaper. Everything here runs on the layout thread, so a page with three
 * images stalls for three fetches - honest until there is a scheduler.
 */

#include "types.h"
#include "webimg.h"
#include "kmalloc.h"
#include "string.h"
#include "image.h"
#include "net.h"
#include "omi_http.h"

#define MAX_IMGS 6
#define IMG_MAXW 1024
#define IMG_MAXH 1024
#define IMG_BYTES (768 * 1024)

typedef struct {
  char src[160];
  int w, h;
  uint32_t *px;
  uint32_t stamp;
} wimg_t;

static wimg_t imgs[MAX_IMGS];
static int n_imgs;
static uint32_t stamp_now;
static oweb_image_t view;

void webimg_reset(void) {
  for (int i = 0; i < n_imgs; i++) {
    if (imgs[i].px) { kfree(imgs[i].px); imgs[i].px = 0; }
  }
  n_imgs = 0;
  stamp_now = 0;
}

static int same(const char *a, const char *b) {
  while (*a && *b) { if (*a != *b) return 0; a++; b++; }
  return *a == 0 && *b == 0;
}

extern const net_cfg_t *net_get(void);

const oweb_image_t *webimg_get(const char *src, int len) {
  if (!src || len < 8 || len >= 159) return 0;
  /* absolute http(s) only: a relative URL would need a base to resolve */
  if (!((len > 7 && memcmp(src, "http://", 7) == 0) ||
        (len > 8 && memcmp(src, "https://", 8) == 0)))
    return 0;

  char key[160];
  memcpy(key, src, (size_t)len);
  key[len] = 0;

  for (int i = 0; i < n_imgs; i++)
    if (imgs[i].px && same(imgs[i].src, key)) {
      imgs[i].stamp = ++stamp_now;
      view.w = imgs[i].w; view.h = imgs[i].h; view.px = imgs[i].px;
      return &view;
    }

  const net_cfg_t *cfg = net_get();
  if (!cfg) return 0;

  char *body = (char *)kmalloc(IMG_BYTES);
  if (!body) return 0;
  http_result_t res;
  int ok = http_get(cfg, key, body, IMG_BYTES - 1, &res);
  if (!ok || res.status != 200 || res.len < 64) { kfree(body); return 0; }

  omi_image_t im;
  int rc = image_decode((const uint8_t *)body, res.len, &im);
  kfree(body);
  if (rc != 0 || !im.px) return 0;
  if (im.w <= 0 || im.h <= 0 || im.w > IMG_MAXW || im.h > IMG_MAXH) {
    image_free(&im);
    return 0;
  }

  int slot;
  if (n_imgs < MAX_IMGS) slot = n_imgs++;
  else {
    slot = 0;
    for (int i = 1; i < n_imgs; i++) if (imgs[i].stamp < imgs[slot].stamp) slot = i;
    if (imgs[slot].px) kfree(imgs[slot].px);
  }

  int k = 0;
  while (key[k] && k < (int)sizeof imgs[slot].src - 1) { imgs[slot].src[k] = key[k]; k++; }
  imgs[slot].src[k] = 0;
  imgs[slot].w = im.w; imgs[slot].h = im.h; imgs[slot].px = im.px;
  imgs[slot].stamp = ++stamp_now;

  view.w = im.w; view.h = im.h; view.px = im.px;
  return &view;
}
