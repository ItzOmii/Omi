/*
 * omittf.h - real outlines for the ring-3 rasteriser.
 *
 * The browser laid out and painted text with the kernel's 8x16 bitmap: one face,
 * one width, so every heading was the same size as body text and a paragraph
 * wrapped on a grid instead of on its own advances. This file gives the same
 * process real faces - the ones already on the ISO as boot modules, Bricolage for
 * text and JetBrains Mono for columns - and keeps the two promises the old path
 * made:
 *
 *   measure and paint agree. The wrapper asks omi_face_measure() how wide a run
 *   is and the painter advances by exactly the same number, kerning included.
 *   A line cannot be wrapped for a width the rasteriser then exceeds.
 *
 *   coverage, not a bitmask. A glyph arrives as an 8-bit alpha mask and is
 *   blended through omitext.h's blend_px, so text on a coloured card is text on
 *   the card's colour, and the mask is cached per (font, codepoint) rather than
 *   re-rasterised per paint.
 *
 * The faces come from SYS_FONT, which copies the boot module's bytes out of the
 * kernel. If that fails for either face, omi_face_init() returns 0 and the caller
 * keeps the bitmap path - a browser with no outlines should still be readable.
 *
 * Include after omitext.h: this depends on its blend_px and on CW/CH.
 */
#ifndef OMI_TTF_H
#define OMI_TTF_H

#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define OMI_SYS_FONT 59

/* The five ids the layout engine emits (omilayout.h). Face is which boot module,
 * px is the em size, bold is drawn twice a pixel apart - stb has no synthetic
 * weights, and a two-pass off-by-one is what the bitmap path did too. */
typedef struct { int face, px, bold; } omi_face_t;
static const omi_face_t omi_faces[5] = {
  {0, 14, 0},   /* 0 body        14px */
  {0, 26, 1},   /* 1 display     26px, h1/h2 */
  {1, 13, 0},   /* 2 mono        13px, code and pre */
  {0, 12, 0},   /* 3 alt         12px, small print */
  {0, 17, 0},   /* 4 medium      17px, h3/h4 and lead-ins */
};

static unsigned char *omi_font_bytes[2];
static stbtt_fontinfo omi_fi[2];
static int omi_have[2];
static int omi_init_done, omi_init_ok;

/* One cached glyph: the alpha mask stb produced, and where it sits relative to
 * the pen. `adv` is the advance in 26.6 fixed point so a page can add up
 * hundreds of them without drift. */
typedef struct {
  int used;
  int font, cp;
  unsigned char *bm;
  int w, h, xo, yo;
  int adv26;
} omi_glyph_t;

#define OMI_GMAX 4096
static omi_glyph_t omi_gcache[OMI_GMAX];
static int omi_gnext;

static long omi_sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

/* Ask the kernel how big the face is, then for the bytes. Returning 0 here is
 * normal on a kernel that predates SYS_FONT, so it is not an error. */
static int omi_face_load(int face) {
  long n = omi_sys3(OMI_SYS_FONT, face, 0, 0);
  if (n <= 0 || n > (16 << 20)) return 0;
  unsigned char *b = (unsigned char *)malloc((size_t)n);
  if (!b) return 0;
  /* Touch it before the kernel copies into it. malloc hands back address space,
   * not pages, and the kernel cannot demand-page on the client's behalf: it would
   * fault part-way through its own write and halt the process with a stack that
   * says only "syscall_dispatch". */
  memset(b, 0, (size_t)n);
  long got = omi_sys3(OMI_SYS_FONT, face, (long)b, n);
  if (got != n) { free(b); return 0; }
  int off = stbtt_GetFontOffsetForIndex(b, 0);
  if (off < 0 || !stbtt_InitFont(&omi_fi[face], b, off)) { free(b); return 0; }
  omi_font_bytes[face] = b;
  omi_have[face] = 1;
  return 1;
}

static int omi_face_init(void) {
  if (omi_init_done) return omi_init_ok;
  omi_init_done = 1;
  omi_face_load(0);
  omi_face_load(1);
  omi_init_ok = omi_have[0];        /* display face is what body text uses */
  return omi_init_ok;
}

static float omi_scale(int face, int px) {
  return stbtt_ScaleForPixelHeight(&omi_fi[face], (float)px);
}

int omi_face_line_h(int font) {
  if (!omi_face_init()) return 0;
  const omi_face_t *f = &omi_faces[font < 0 ? 0 : font > 4 ? 4 : font];
  if (!omi_have[f->face]) return 0;
  int a, d, g;
  stbtt_GetFontVMetrics(&omi_fi[f->face], &a, &d, &g);
  float sc = omi_scale(f->face, f->px);
  int h = (int)((float)(a - d) * sc + 0.5f) + 2;   /* two pixels of leading */
  return h;
}

int omi_face_ascent(int font) {
  if (!omi_face_init()) return 0;
  const omi_face_t *f = &omi_faces[font < 0 ? 0 : font > 4 ? 4 : font];
  if (!omi_have[f->face]) return 0;
  int a, d, g;
  stbtt_GetFontVMetrics(&omi_fi[f->face], &a, &d, &g);
  return (int)((float)a * omi_scale(f->face, f->px) + 0.5f);
}

/* Codepoints, not bytes: a page that says "Zurich" with a real umlaut should draw
 * one, and the fold-to-ASCII cleaner is being retired with this file. */
static int omi_utf8(const char **sp) {
  const unsigned char *s = (const unsigned char *)*sp;
  int cp;
  if (s[0] < 0x80) { cp = s[0]; *sp += 1; }
  else if ((s[0] & 0xE0) == 0xC0) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); *sp += 2; }
  else if ((s[0] & 0xF0) == 0xE0) { cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); *sp += 3; }
  else if ((s[0] & 0xF8) == 0xF0) { cp = 0x1F600; *sp += 4; }   /* out of scope: draw a box */
  else { cp = '?'; *sp += 1; }
  return cp;
}

/* Find or rasterise one glyph. Cached by (font, codepoint): a page re-uses about
 * seventy distinct glyphs, and rasterising them once is the difference between a
 * scroll that keeps up and one that does not. */
static omi_glyph_t *omi_glyph(int font, int cp) {
  for (int i = 0; i < omi_gnext; i++)
    if (omi_gcache[i].used && omi_gcache[i].font == font && omi_gcache[i].cp == cp)
      return &omi_gcache[i];

  const omi_face_t *f = &omi_faces[font < 0 ? 0 : font > 4 ? 4 : font];
  if (!omi_have[f->face]) return 0;

  omi_glyph_t *g;
  if (omi_gnext < OMI_GMAX) g = &omi_gcache[omi_gnext++];
  else { g = &omi_gcache[cp % OMI_GMAX]; free(g->bm); }   /* full: recycle */
  memset(g, 0, sizeof *g);
  g->used = 1; g->font = font; g->cp = cp;

  float sc = omi_scale(f->face, f->px);
  int adv, lsb;
  stbtt_GetCodepointHMetrics(&omi_fi[f->face], cp, &adv, &lsb);
  g->adv26 = (int)((float)adv * sc * 64.0f + 0.5f);
  g->bm = stbtt_GetCodepointBitmap(&omi_fi[f->face], 0, sc, cp, &g->w, &g->h, &g->xo, &g->yo);
  return g;
}

static int omi_face_kern(int font, int a, int b);   /* 26.6, defined below */

/* Whitespace: kerning and advances still apply, there is just no mask. */
int omi_face_measure(int font, const char *s) {
  if (!omi_face_init()) return 0;
  if (font < 0) font = 0; if (font > 4) font = 4;
  const omi_face_t *f = &omi_faces[font];
  if (!omi_have[f->face]) return 0;

  long total26 = 0;
  int prev = -1;
  const char *p = s;
  while (*p) {
    int cp = omi_utf8(&p);
    omi_glyph_t *g = omi_glyph(font, cp);
    if (!g) break;
    total26 += g->adv26;
    if (prev >= 0) total26 += omi_face_kern(font, prev, cp);
    prev = cp;
  }
  /* Floor, not round: the painter advances in 26.6 and converts each pen
   * position the same way, so the width the wrapper measured is exactly the
   * width the painter occupies. Rounding here and flooring there left a long
   * line 8 px wider than its box - enough for the engine's own check to catch
   * it, and enough to push a glyph past a background's edge. */
  return (int)(total26 >> 6);
}

static int omi_face_kern(int font, int a, int b) {
  if (!omi_face_init()) return 0;
  const omi_face_t *f = &omi_faces[font < 0 ? 0 : font > 4 ? 4 : font];
  if (!omi_have[f->face]) return 0;
  float sc = omi_scale(f->face, f->px);
  return (int)((float)stbtt_GetCodepointKernAdvance(&omi_fi[f->face], a, b) * sc * 64.0f);
}

/* Blend one glyph's coverage at (px,py). Clipped by omitext.h's box, because a
 * page's text is drawn before the title bar goes over it. */
static void omi_blit(unsigned char *bm, int w, int h, int px, int py, unsigned c) {
  for (int r = 0; r < h; r++) {
    int y = py + r;
    if (y < 0 || y >= CH) continue;
    if (clip_on && (y < clip_y0 || y >= clip_y1)) continue;
    const unsigned char *row = bm + (size_t)r * w;
    for (int i = 0; i < w; i++) {
      int a = row[i];
      if (!a) continue;
      int x = px + i;
      if (x < 0 || x >= CW) continue;
      if (clip_on && (x < clip_x0 || x >= clip_x1)) continue;
      unsigned *d = &pix[(size_t)y * CW + x];
      *d = blend_px(*d, c, a);
      text_glyph_pixels++;
    }
  }
}

/* x is the pen start; y is the baseline. The layout engine asks for the ascent so
 * it can turn a line box into a baseline, and this uses the same number. */
void omi_face_draw(int font, int x, int y, const char *s, unsigned c) {
  if (!omi_face_init()) return;
  if (font < 0) font = 0; if (font > 4) font = 4;
  const omi_face_t *f = &omi_faces[font];
  if (!omi_have[f->face]) return;

  long pen26 = (long)x << 6;     /* the same fixed point omi_face_measure adds in */
  int prev = -1;
  const char *p = s;
  while (*p) {
    int cp = omi_utf8(&p);
    omi_glyph_t *g = omi_glyph(font, cp);
    if (!g) break;
    if (prev >= 0) pen26 += omi_face_kern(font, prev, cp);
    int pen = (int)(pen26 >> 6);
    if (g->bm && g->w > 0 && g->h > 0) {
      omi_blit(g->bm, g->w, g->h, pen + g->xo, y + g->yo, c);
      if (f->bold) omi_blit(g->bm, g->w, g->h, pen + g->xo + 1, y + g->yo, c);
    }
    pen26 += g->adv26;
    prev = cp;
  }
}

/* The widest line, for a caller that has to centre something. */
int omi_face_widest(int font, const char *s) { return omi_face_measure(font, s); }

#endif