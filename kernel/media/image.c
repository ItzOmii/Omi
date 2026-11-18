/*
 * Omi - one image decoder for the whole system.
 *
 * JPEG goes through picojpeg (already used by the media player, integer only);
 * PNG and friends come from the vendored stb_image, which carries its own
 * inflate. Both land on the same pixel format so callers never care.
 */

#include "types.h"
#include "image.h"
#include "kmalloc.h"
#include "string.h"

/* stb_image expects libm; the kernel has none, and SSE is already on by the
 * time any of this runs (see cpu_enable_fpu_sse in limine_boot.c). */
static double im_fabs(double x) { return x < 0 ? -x : x; }
static double im_sqrt(double x) {
  if (x <= 0) return 0;
  double g = x > 1 ? x : 1;
  for (int i = 0; i < 40; i++) g = 0.5 * (g + x / g);
  return g;
}
static double im_pow(double b, double e) {
  int n = (int)e;
  if ((double)n == e) {
    double r = 1; int k = n < 0 ? -n : n;
    while (k--) r *= b;
    return n < 0 ? 1 / r : r;
  }
  return b;
}
static double im_ldexp(double x, int e) {
  while (e > 0) { x *= 2; e--; }
  while (e < 0) { x /= 2; e++; }
  return x;
}
static float im_sqrtf(float x) { return (float)im_sqrt((double)x); }
static float im_powf(float b, float e) { return (float)im_pow((double)b, (double)e); }
static float im_fabsf(float x) { return (float)im_fabs((double)x); }
static float im_ldexpf(float x, int e) { return (float)im_ldexp((double)x, e); }

/* freestanding: no <limits.h> */
#define SHRT_MAX 32767
#define SHRT_MIN (-32768)
#define USHRT_MAX 65535U
#define INT_MAX 2147483647
#define INT_MIN (-2147483647 - 1)
#define UINT_MAX 4294967295U
#define CHAR_BIT 8

/* the kernel has no thread-local storage set up: these are plain globals */
#define STBI_NO_THREAD_LOCALS

#define STBI_NO_STDIO
#define STBI_NO_GIF
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_MALLOC(sz)      kmalloc(sz)
#define STBI_REALLOC(p, sz)  krealloc(p, sz)
#define STBI_FREE(p)         kfree(p)
#define STBI_ASSERT(x)       ((void)0)
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

int image_sniff(const uint8_t *d, int len) {
  if (!d || len < 8) return 0;
  if (d[0] == 0xFF && d[1] == 0xD8) return 1;                       /* JPEG */
  if (d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return 2;
  return 0;
}

static uint32_t *rgba_to_rgb(const uint8_t *rgba, int w, int h) {
  uint32_t *out = (uint32_t *)kmalloc((size_t)w * (size_t)h * 4);
  if (!out) return 0;
  for (int i = 0; i < w * h; i++) {
    const uint8_t *p = rgba + (size_t)i * 4;
    uint32_t a = p[3], r = p[0], g = p[1], b = p[2];
    if (a != 255) {                    /* composite on the paper colour */
      r = (r * a + 252 * (255 - a)) / 255;
      g = (g * a + 250 * (255 - a)) / 255;
      b = (b * a + 244 * (255 - a)) / 255;
    }
    out[i] = (r << 16) | (g << 8) | b;
  }
  return out;
}

int image_decode(const uint8_t *data, int len, omi_image_t *out) {
  if (!out || !data || len < 8) return -1;
  out->w = out->h = 0; out->px = 0; out->format = 0;

  int fmt = image_sniff(data, len);
  if (!fmt) return -2;

  /* both formats go through stb_image: the picojpeg path in media.c is a
     media-player helper that has never been exercised on a real file, and it
     produced noise on the first test image it ever saw. */
  int w = 0, h = 0, comp = 0;
  uint8_t *rgba = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
  if (!rgba) return -4;
  out->px = rgba_to_rgb(rgba, w, h);
  stbi_image_free(rgba);
  if (!out->px) return -5;
  out->w = w; out->h = h; out->format = fmt;
  return 0;
}

void image_free(omi_image_t *img) {
  if (!img) return;
  if (img->px) { kfree(img->px); img->px = 0; }
  img->w = img->h = 0;
}
