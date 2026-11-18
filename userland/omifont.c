/*
 * omifont - real type in the browser.
 *
 * The page has been drawn in one 8x16 bitmap, scaled 1x or 2x, since it was a test
 * harness. That is the largest visual difference from Firefox: not the CSS, but
 * that every glyph is a fixed-width cell with no antialiasing. The ISO already
 * ships two TrueType faces (Bricolage for text, JetBrains Mono for code) for the
 * shell, and stb_truetype is already in the tree, so proportional, antialiased
 * text costs a loader and a blitter rather than a new dependency.
 *
 * This file owns: reading the font file, sizing it, measuring a run, and blending
 * a run into a 0x00RRGGBB surface. It knows nothing about layout.
 */

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "omifont.h"

typedef struct {
  stbtt_fontinfo info;
  unsigned char *data;
  int            size;
  int            ready;
} face_t;

static face_t faces[FONT_FACES];

int font_load(int slot, const char *path) {
  if (slot < 0 || slot >= FONT_FACES) return -1;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (16 * 1024 * 1024)) {
    close(fd);
    return -1;
  }
  unsigned char *buf = (unsigned char *)malloc((size_t)st.st_size);
  if (!buf) { close(fd); return -1; }
  long got = 0;
  while (got < (long)st.st_size) {
    long n = read(fd, buf + got, (size_t)((long)st.st_size - got));
    if (n <= 0) break;
    got += n;
  }
  close(fd);
  if (got != (long)st.st_size) { free(buf); return -1; }

  int off = stbtt_GetFontOffsetForIndex(buf, 0);
  if (off < 0 || !stbtt_InitFont(&faces[slot].info, buf, off)) { free(buf); return -1; }
  faces[slot].data = buf;
  faces[slot].size = (int)st.st_size;
  faces[slot].ready = 1;
  return 0;
}

int font_ready(int slot) {
  return (slot >= 0 && slot < FONT_FACES && faces[slot].ready);
}

/* Vertical metrics at a pixel size: the line is ascent + descent + a little air. */
void font_vmetrics(int slot, int px, int *ascent, int *descent, int *line_h) {
  if (!font_ready(slot)) { if (ascent) *ascent = px; if (descent) *descent = px / 4; if (line_h) *line_h = px + px / 4 + 2; return; }
  float scale = stbtt_ScaleForPixelHeight(&faces[slot].info, (float)px);
  int a = 0, d = 0, g = 0;
  stbtt_GetFontVMetrics(&faces[slot].info, &a, &d, &g);
  if (ascent) *ascent = (int)(a * scale + 0.5f);
  if (descent) *descent = (int)(-d * scale + 0.5f);
  if (line_h) *line_h = (int)((a - d) * scale + 0.5f) + 2;
}

/* Width of a run, in pixels: advances plus kerning, rounded once at the end so a
 * measurement never drifts from the drawing by a pixel per character. */
int font_measure(int slot, int px, const char *s, int len) {
  if (!s || (len == 0 && !*s)) return 0;
  if (!font_ready(slot)) return (len > 0 ? len : (int)strlen(s)) * (px / 2);
  float scale = stbtt_ScaleForPixelHeight(&faces[slot].info, (float)px);
  float x = 0;
  int prev = 0;
  int n = len > 0 ? len : (int)strlen(s);
  for (int i = 0; i < n; i++) {
    int cp = (unsigned char)s[i];
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&faces[slot].info, cp, &adv, &lsb);
    if (prev) x += stbtt_GetCodepointKernAdvance(&faces[slot].info, prev, cp) * scale;
    x += adv * scale;
    prev = cp;
  }
  return (int)(x + 0.5f);
}

/* Blend one run into a 0x00RRGGBB surface. `color` is 0x00RRGGBB; the glyph's
 * coverage byte is the alpha, so edges come out smooth against whatever is
 * underneath instead of being carved from a bitmap. */
void font_draw(unsigned *pix, int stride, int clip_w, int clip_h,
               int x, int y, const char *s, int len, int px, unsigned color) {
  if (!font_ready(0) || !pix || !s) return;
  int slot = 0;
  float scale = stbtt_ScaleForPixelHeight(&faces[slot].info, (float)px);
  int asc = 0, desc = 0, gap = 0;
  stbtt_GetFontVMetrics(&faces[slot].info, &asc, &desc, &gap);
  int baseline = y + (int)(asc * scale + 0.5f);

  int n = len > 0 ? len : (int)strlen(s);
  int prev = 0;
  float fx = (float)x;
  unsigned cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;

  for (int i = 0; i < n; i++) {
    int cp = (unsigned char)s[i];
    if (prev) fx += stbtt_GetCodepointKernAdvance(&faces[slot].info, prev, cp) * scale;

    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&faces[slot].info, cp, &adv, &lsb);
    if (cp > 32) {
      int gx = 0, gy = 0, gw = 0, gh = 0;
      /* stb_truetype's argument order is (width, height, xoff, yoff); passing them
       * as (x, y, w, h) type-checks, compiles, and draws nothing at all. */
      unsigned char *bm = stbtt_GetCodepointBitmap(&faces[slot].info, scale, scale, cp,
                                                   &gw, &gh, &gx, &gy);
      if (bm) {
        int ox = (int)(fx + 0.5f) + gx;
        int oy = baseline + gy;
        for (int row = 0; row < gh; row++) {
          int py = oy + row;
          if (py < 0 || py >= clip_h) continue;
          unsigned *dst = pix + (size_t)py * (size_t)stride;
          const unsigned char *src = bm + (size_t)row * (size_t)gw;
          for (int col = 0; col < gw; col++) {
            int pxx = ox + col;
            if (pxx < 0 || pxx >= clip_w) continue;
            unsigned a = src[col];
            if (!a) continue;
            unsigned d = dst[pxx];
            unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            unsigned r = (dr * (255 - a) + cr * a) / 255;
            unsigned g = (dg * (255 - a) + cg * a) / 255;
            unsigned b = (db * (255 - a) + cb * a) / 255;
            dst[pxx] = (r << 16) | (g << 8) | b;
          }
        }
        stbtt_FreeBitmap(bm, NULL);
      }
    }
    fx += adv * scale;
    prev = cp;
  }
}