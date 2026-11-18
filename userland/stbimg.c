/*
 * img_decode: one entry point, two decoders.
 *
 * The kernel already carries stb_image (v2.32) for the media player and the
 * wallpaper loader, so the browser does not need a JPEG or PNG decoder written
 * for it: the same code, compiled into the process, handles everything that is
 * not a GIF. GIF stays on the decoder in imgdec.c because that one was written
 * here, is covered by a test with a generated fixture, and produces exactly the
 * pixel layout the compositor paints.
 *
 * stb hands back RGBA; everything in this browser is 0x00RRGGBB words, so the
 * conversion happens once, here.
 */

#define STB_IMAGE_IMPLEMENTATION
/* Everything stb_image has, except the floating-point HDR path: a page's images
 * are 8-bit, and HDR would drag libm into the process for nothing. That leaves
 * JPEG (baseline and progressive), PNG (every bit depth, interlaced or not),
 * BMP, TGA, PSD and PNM. GIF stays on the decoder in imgdec.c. */
#define STBI_NO_HDR
#include "stb_image.h"

#include <stdlib.h>
#include <stdint.h>
#include "imgdec.h"

int img_decode(const uint8_t *data, int len, int *w, int *h, uint32_t **px) {
  if (!data || len <= 8 || !w || !h || !px) return -1;
  *w = 0; *h = 0; *px = 0;

  if (gif_is(data, len)) return gif_decode(data, len, w, h, px);

  int comp = 0, dw = 0, dh = 0;
  unsigned char *rgba = stbi_load_from_memory(data, len, &dw, &dh, &comp, 4);
  if (!rgba) return -1;
  if (dw <= 0 || dh <= 0 || dw > 4096 || dh > 4096) { stbi_image_free(rgba); return -1; }

  /* Converted in place. RGBA and 0x00RRGGBB are both four bytes a pixel, and the
   * bytes are read before they are written one pixel at a time, so the decoded
   * buffer *is* the pixel buffer. On this process's memory that is the difference
   * between showing a 400x600 thumbnail and failing to allocate one: half the peak
   * for every image. The buffer is freed with img_free(), which knows whose
   * allocator made it. */
  size_t np = (size_t)dw * (size_t)dh;
  uint32_t *out = (uint32_t *)rgba;
  for (size_t i = 0; i < np; i++) {
    unsigned char r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
    out[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  }

  *w = dw;
  *h = dh;
  *px = out;
  return 0;
}

void img_free(uint32_t *px) {
  if (px) stbi_image_free((void *)px);
}

/* What the bytes are, for the log. The unsupported ones are named too: "webp" in
 * the log says the browser knew what it had and could not read it, which is a
 * different problem from bytes nobody recognised. */
const char *img_kind(const uint8_t *data, int len) {
  if (!data || len < 4) return "unknown";
  if (gif_is(data, len)) return "gif";
  if (data[0] == 0xFF && data[1] == 0xD8) return "jpg";
  if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') return "png";
  if (data[0] == 'B' && data[1] == 'M') return "bmp";
  if (data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
      len > 12 && data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P')
    return "webp";
  if (len > 12 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p')
    return "avif/heif";
  if (data[0] == 0 && data[1] == 0 && (data[2] == 1 || data[2] == 2)) return "ico";
  if (data[0] == '<') return "svg";
  return "unknown";
}