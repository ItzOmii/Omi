/*
 * Omi - TrueType text via stb_truetype.
 *
 * The kernel is freestanding: no libm, no malloc headers, no SSE (the build is
 * soft-float on purpose). stb_truetype lets every one of those be replaced, so
 * the maths below are plain C and the allocator is kmalloc.
 */

#include "types.h"
#include "ttf.h"
#include "gui.h"
#include "kmalloc.h"
#include "limine_modules.h"
#include "string.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t v);

static double tt_fabs(double x) { return x < 0 ? -x : x; }

static double tt_sqrt(double x) {
  if (x <= 0) return 0;
  double g = x > 1 ? x : 1;
  for (int i = 0; i < 48; i++) {
    double n = 0.5 * (g + x / g);
    if (tt_fabs(n - g) < 1e-10) return n;
    g = n;
  }
  return g;
}

static double tt_cbrt(double x) {
  if (x <= 0) return -tt_cbrt(-x);
  double g = x > 1 ? x : 1;
  for (int i = 0; i < 48; i++) g = (2 * g + x / (g * g)) / 3;
  return g;
}

static double tt_fmod(double x, double y) {
  if (y == 0) return 0;
  long long q = (long long)(x / y);
  return x - (double)q * y;
}

static double tt_pow(double b, double e) {
  int n = (int)e;
  if ((double)n == e) {
    double r = 1;
    int k = n < 0 ? -n : n;
    while (k--) r *= b;
    return n < 0 ? 1 / r : r;
  }
  if (e > 0.333 && e < 0.334) return tt_cbrt(b);
  return b;                      /* never reached by the rasteriser */
}

static double tt_cos(double x) {  /* cosine only feeds glyph transforms */
  double x2 = x * x, term = 1, sum = 1;
  for (int i = 1; i < 12; i++) {
    term *= -x2 / (double)((2 * i - 1) * (2 * i));
    sum += term;
  }
  return sum;
}

static double tt_acos(double x) { /* likewise: no rotation paths in use */
  if (x >= 1) return 0;
  if (x <= -1) return 3.14159265358979;
  double s = tt_sqrt(1 - x * x);
  double a = 0, x2 = (x - 1) / (x + 1), t = x2;
  for (int i = 1; i < 40; i += 2) { a += t / (double)i; t *= x2 * x2; }
  return 3.14159265358979 - 2 * a - s * 0;
}

#define STBTT_ifloor(x)   ((int)(x) - ((x) < 0 && (double)(int)(x) != (x) ? 1 : 0))
#define STBTT_iceil(x)    ((int)(x) + ((x) > 0 && (double)(int)(x) != (x) ? 1 : 0))
#define STBTT_sqrt(x)     tt_sqrt(x)
#define STBTT_pow(x, y)   tt_pow(x, y)
#define STBTT_fmod(x, y)  tt_fmod(x, y)
#define STBTT_cos(x)      tt_cos(x)
#define STBTT_acos(x)     tt_acos(x)
#define STBTT_fabs(x)     tt_fabs(x)
#define STBTT_malloc(s, u) ((void)(u), kmalloc(s))
#define STBTT_free(p, u)   ((void)(u), kfree(p))
#define STBTT_assert(x)    ((void)0)
#define STBTT_strlen(x)    strlen(x)
#define STBTT_memcpy       memcpy
#define STBTT_memset       memset

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

/* ------------------------------------------------------------------------- */

#define GCACHE 384              /* glyph slots; a page of text reuses them hard */
#define MAX_GLYPH_PX 96

typedef struct {
  uint32_t key;                 /* 0 = empty */
  int w, h, x0, y0, adv;
  uint8_t *bm;
} gcache_t;

static gcache_t gcache[GCACHE];
static stbtt_fontinfo fi[2];
static int have[2];
static int ready;
static int inited;
static float ttf_idf(float x) { return x; }

static unsigned char *face_bytes(int face) {
  void *p = 0;
  uint64_t n = 0;
  const char *want = face == TTF_MONO ? "JBMono.ttf" : "Bricolage.ttf";
  if (!meep_module_find(want, &p, &n) || !p || n < 1024) return 0;
  return (unsigned char *)p;
}

int ttf_init(void) {
  if (inited) return ready;
  inited = 1;
  for (int f = 0; f < 2; f++) {
    unsigned char *b = face_bytes(f);
    if (!b) continue;
    int off = stbtt_GetFontOffsetForIndex(b, 0);
    if (off < 0) continue;
    if (!stbtt_InitFont(&fi[f], b, off)) continue;
    have[f] = 1;

  }
  ready = have[TTF_DISPLAY] || have[TTF_MONO];

  return ready;
}

int ttf_ready(void) { return ttf_init(); }

int ttf_blob(int face, unsigned char **p) {
  if (face != TTF_DISPLAY && face != TTF_MONO) return 0;
  void *b = 0;
  uint64_t n = 0;
  const char *want = face == TTF_MONO ? "JBMono.ttf" : "Bricolage.ttf";
  if (!meep_module_find(want, &b, &n) || !b || n < 1024) return 0;
  if (p) *p = (unsigned char *)b;
  return (int)n;
}

int ttf_ascent(int face, int px) {
  if (!ttf_init() || !have[face]) return px;
  int a, d, g;
  stbtt_GetFontVMetrics(&fi[face], &a, &d, &g);
  return (int)(a * stbtt_ScaleForPixelHeight(&fi[face], (float)px) + 0.5f);
}

int ttf_line_h(int face, int px) {
  if (!ttf_init() || !have[face]) return px + 4;
  int a, d, g;
  stbtt_GetFontVMetrics(&fi[face], &a, &d, &g);
  float sc = stbtt_ScaleForPixelHeight(&fi[face], (float)px);
  return (int)((float)(a - d + g) * sc + 0.5f);
}

/* glyph slot: direct-mapped, evicting whatever sat there before */
static gcache_t *glyph(int face, int px, unsigned cp) {
  uint32_t key = ((uint32_t)cp * 2654435761u) ^ ((uint32_t)px << 7) ^ (uint32_t)face;
  if (!key) key = 1;
  gcache_t *e = &gcache[key % GCACHE];
  if (e->key == key) return e;

  if (e->bm) { kfree(e->bm); e->bm = 0; }
  e->key = 0;

  float sc = stbtt_ScaleForPixelHeight(&fi[face], (float)px);
  int adv = 0, lsb = 0;
  stbtt_GetCodepointHMetrics(&fi[face], (int)cp, &adv, &lsb);

  int x0, y0, x1, y1;
  stbtt_GetCodepointBitmapBox(&fi[face], (int)cp, sc, sc, &x0, &y0, &x1, &y1);
  if (0) {
    serial_puts("[ttf] glyph adv="); serial_puthex((uint64_t)(uint32_t)adv);
    serial_puts(" box="); serial_puthex((uint64_t)(uint32_t)((x1 - x0) & 0xFFFF));
    serial_puts("x"); serial_puthex((uint64_t)(uint32_t)((y1 - y0) & 0xFFFF));
    serial_puts("\n");
  }
  int w = x1 - x0, h = y1 - y0;
  if (w > 0 && h > 0 && w <= MAX_GLYPH_PX && h <= MAX_GLYPH_PX) {
    uint8_t *bm = (uint8_t *)kmalloc((size_t)(w * h));
    if (bm) {
      stbtt_MakeCodepointBitmap(&fi[face], bm, w, h, w, sc, sc, (int)cp);
      e->bm = bm;
      e->w = w; e->h = h; e->x0 = x0; e->y0 = y0;
    } else {
      e->w = e->h = 0;
    }
  } else {
    e->w = e->h = 0;
  }
  e->adv = (int)(adv * sc + 0.5f);
  e->key = key;
  return e;
}

int ttf_width(int face, int px, const char *s) {
  if (!ttf_init() || !have[face] || !s) return 0;
  float sc = stbtt_ScaleForPixelHeight(&fi[face], (float)px);
  int x = 0;
  unsigned prev = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    unsigned cp = *p;
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&fi[face], (int)cp, &adv, &lsb);
    x += (int)(adv * sc + 0.5f);
    if (prev) x += (int)(stbtt_GetCodepointKernAdvance(&fi[face], (int)prev, (int)cp) * sc + 0.5f);
    prev = cp;
  }
  return x;
}

void ttf_draw(int face, int px, int x, int baseline, const char *s, color_t color) {
  if (!ttf_init() || !have[face] || !s || px <= 0 || px > MAX_GLYPH_PX) return;
  if (!backbuffer) return;

  float sc = stbtt_ScaleForPixelHeight(&fi[face], (float)px);
  int pen = x;
  unsigned prev = 0;
  int W = (int)screen_width, H = (int)screen_height;
  uint32_t cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;

  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    unsigned cp = *p;
    if (cp == '\n') continue;                /* callers place their own lines */
    if (prev) pen += (int)(stbtt_GetCodepointKernAdvance(&fi[face], (int)prev, (int)cp) * sc + 0.5f);

    gcache_t *g = glyph(face, px, cp);
    if (g->bm) {
      int gx = pen + g->x0, gy = baseline + g->y0;
      for (int yy = 0; yy < g->h; yy++) {
        int dy = gy + yy;
        if (dy < 0 || dy >= H) continue;
        uint32_t *row = backbuffer + (size_t)dy * screen_width;
        const uint8_t *srow = g->bm + (size_t)yy * g->w;
        for (int xx = 0; xx < g->w; xx++) {
          int dx = gx + xx;
          if (dx < 0 || dx >= W) continue;
          unsigned a = srow[xx];
          if (!a) continue;
          uint32_t d = row[dx];
          unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
          unsigned nr = (cr * a + dr * (255 - a)) / 255;
          unsigned ng = (cg * a + dg * (255 - a)) / 255;
          unsigned nb = (cb * a + db * (255 - a)) / 255;
          row[dx] = (nr << 16) | (ng << 8) | nb;
        }
      }
    }
    pen += g->adv;
    prev = cp;
  }
}
