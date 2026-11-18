/*
 * Omi - image decoding, system wide.
 *
 * One decoder for the whole OS: the browser, the media player and wallpapers
 * all come through here, so a PNG that works in a page also works as a
 * wallpaper. Results are 0x00RRGGBB, row-major, alpha composited on paper.
 */

#ifndef _OMI_IMAGE_H
#define _OMI_IMAGE_H

#include "types.h"

typedef struct {
  int w, h;
  uint32_t *px;
  int format;               /* 1 = jpeg, 2 = png */
} omi_image_t;

/* Returns 0 on success, negative on failure. Caller frees with image_free(). */
int image_decode(const uint8_t *data, int len, omi_image_t *out);
void image_free(omi_image_t *img);

/* Cheap sniff so callers can reject a fetch before decoding. */
int image_sniff(const uint8_t *data, int len);

#endif
