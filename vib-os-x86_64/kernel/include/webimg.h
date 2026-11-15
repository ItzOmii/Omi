/*
 * Omi - page images.
 *
 * Fetches and decodes the subresources a page asks for, and keeps a small
 * cache so a re-layout does not re-download. JPEG goes through the media
 * player's picojpeg wrapper; PNG through the vendored stb_image.
 */

#ifndef _OMI_WEBIMG_H
#define _OMI_WEBIMG_H

#include "types.h"

typedef struct {
  int w, h;
  const uint32_t *px;      /* 0x00RRGGBB, row-major */
} oweb_image_t;

/* Returns a decoded image for `src` (absolute http/https only), or NULL.
 * Cached: safe to call on every layout pass. */
const oweb_image_t *webimg_get(const char *src, int len);

void webimg_reset(void);   /* drop every cached image */

#endif
