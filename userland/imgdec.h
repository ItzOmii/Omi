/*
 * imgdec - the image decoders Omi Net can afford.
 *
 * A browser that renders no images is a browser that makes every page look like a
 * document, so the first decoder is the one the smallest real pages use: GIF. It
 * is LZW over a palette, there is no compression layer to implement beyond the
 * code table, and the format is old enough that the awkward cases (a local colour
 * table, interlaced rows) are all documented in one page of the spec.
 *
 * Everything here writes 0x00RRGGBB words, which is what the compositor paints,
 * so no pixel format conversion happens anywhere else.
 */
#ifndef OMI_IMGDEC_H
#define OMI_IMGDEC_H

#include <stdint.h>

/* Decode the first frame of `data` as a GIF. On success the caller owns *px (a
 * kfree-able buffer of w*h words) and *w/*h describe it. Returns 0 on success,
 * -1 when the bytes are not a GIF this can read, -2 on allocation failure. */
int gif_decode(const uint8_t *data, int len, int *w, int *h, uint32_t **px);

/* True when the bytes start with a GIF signature. */
int gif_is(const uint8_t *data, int len);

/* Any image the browser can read: GIF through the decoder above, everything else
 * (JPEG, PNG, BMP) through stb_image, which the kernel already carries for the
 * media player. Pixels come back as 0x00RRGGBB. */
int img_decode(const uint8_t *data, int len, int *w, int *h, uint32_t **px);

/* Free pixels from img_decode. GIF buffers come from malloc, the rest from stb;
 * this is the one place that knows which, so nothing else has to. */
void img_free(uint32_t *px);

/* "gif", "jpg", "png", "bmp" or "unknown", for the log. */
const char *img_kind(const uint8_t *data, int len);

#endif