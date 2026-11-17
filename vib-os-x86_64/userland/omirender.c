/* omirender - a text rasteriser that lives in ring 3 and paints a surface.
 *
 * The kernel draws Omi Net's text today: word wrap, glyphs, rules, all of it
 * inside compositor.c and omiweb.c, writing straight into the framebuffer. That
 * is the same coupling a single-user demo can afford and a browser cannot - a
 * layout bug in a parser becomes a memory bug in the kernel, and nothing can
 * scroll without the kernel noticing.
 *
 * This program is the other half of the split. It owns a surface, its own clip
 * rectangle, its own glyph blitter and its own word wrap; the kernel owns the
 * frames and the placement and never learns what a character is. The glyph
 * shapes come from the kernel's table, extracted by tools/gen_glyphs.py, so the
 * two cannot drift apart while both exist.
 *
 * What it can prove from the inside: that the mapping is writable, that the
 * clipper refuses to write outside the surface, that wrapping keeps every line
 * inside the measure, and that blending produces the arithmetic it claims.
 * What it cannot prove is that any of it reached the screen - that needs a
 * framebuffer dump from outside, at the coordinates printed below.
 */
#include <stdint.h>
#include <stdio.h>

#include "glyphs.h"

#define SYS_YIELD        3
#define SYS_WIN_OPEN     6
#define SYS_WIN_CLOSE    10
#define SYS_WIN_GEOM     14
#define SYS_SURF_CREATE  35
#define SYS_SURF_PRESENT 36

#define WIN_W  380
#define WIN_H  340
#define CW     (WIN_W - 2)     /* client area inside the compositor's chrome */
#define CH     (WIN_H - 32)

/* Same words the kernel theme uses, so the ring-3 window does not look foreign. */
#define C_BG     0x001E2024u
#define C_PANEL  0x0026292Fu
#define C_TEXT   0x00E6E6E6u
#define C_DIM    0x009AA0A8u
#define C_ACCENT 0x00C4622Du
#define C_GREEN  0x0000FF00u

#define LH 20                  /* line height, 16px glyphs plus 4 of leading */
#define MARGIN 8

static unsigned *pix;
static int checks, fails;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) { fails++; printf("[render] FAIL %s (got %ld want %ld)\n", what, got, want); }
  else printf("[render] ok   %s\n", what);
}

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

/* ---- the rasteriser: this is the part that used to be the kernel's ---- */

static void fill(int x, int y, int w, int h, unsigned c) {
  /* Clip to the surface. A rasteriser that trusts its callers is a rasteriser
   * that writes into whatever page follows the last row, so every write goes
   * through this. */
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > CW) w = CW - x;
  if (y + h > CH) h = CH - y;
  if (w <= 0 || h <= 0) return;
  for (int j = 0; j < h; j++) {
    unsigned *row = pix + (y + j) * CW + x;
    for (int i = 0; i < w; i++) row[i] = c;
  }
}

static unsigned blend_px(unsigned dst, unsigned src, int a) {
  /* a in 0..255. Integer, so the result is reproducible and checkable. */
  int r = (((dst >> 16) & 0xFF) * (255 - a) + ((src >> 16) & 0xFF) * a) / 255;
  int g = (((dst >>  8) & 0xFF) * (255 - a) + ((src >>  8) & 0xFF) * a) / 255;
  int b = (( dst        & 0xFF) * (255 - a) + ( src        & 0xFF) * a) / 255;
  return (unsigned)((r << 16) | (g << 8) | b);
}

static void blend(int x, int y, int w, int h, unsigned c, int a) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > CW) w = CW - x;
  if (y + h > CH) h = CH - y;
  if (w <= 0 || h <= 0) return;
  for (int j = 0; j < h; j++) {
    unsigned *row = pix + (y + j) * CW + x;
    for (int i = 0; i < w; i++) row[i] = blend_px(row[i], c, a);
  }
}

static int glyph_pixels;   /* counted, so the test can say what it actually drew */

static void glyph(int x, int y, char ch, unsigned c) {
  int idx = (unsigned char)ch - GLYPH_FIRST;
  if (idx < 0 || idx > GLYPH_LAST - GLYPH_FIRST) return;
  const unsigned char *rows = glyphs[idx];
  for (int r = 0; r < GLYPH_H; r++) {
    unsigned char bits = rows[r];
    if (!bits) continue;
    for (int col = 0; col < GLYPH_W; col++) {
      if (!(bits & (0x80 >> col))) continue;
      int px = x + col, py = y + r;
      if (px < 0 || py < 0 || px >= CW || py >= CH) continue;   /* clip */
      pix[py * CW + px] = c;
      glyph_pixels++;
    }
  }
}

static void text(int x, int y, const char *s, unsigned c) {
  for (; *s; s++, x += GLYPH_W) glyph(x, y, *s, c);
}

/* Greedy word wrap inside maxpx, drawing as it goes. Returns the line count and
 * leaves the widest line in *widest so the caller can assert on the wrap. */
static int wrap_text(const char *s, int x, int y, int maxpx, int lh,
                     unsigned c, int *widest) {
  int lines = 0, cx = x, cw = 0, wmax = 0;
  int chars = maxpx / GLYPH_W;
  if (widest) *widest = 0;
  while (*s) {
    /* measure the next word */
    int wl = 0;
    while (s[wl] && s[wl] != ' ') wl++;
    if (cw > 0 && cw + 1 + wl > chars) {      /* the word would not fit */
      if (cw > wmax) wmax = cw;
      lines++; y += lh; cx = x; cw = 0;
    }
    if (cw > 0) { cx += GLYPH_W; cw += 1; }   /* the space */
    for (int i = 0; i < wl; i++) {
      glyph(cx, y, s[i], c);
      cx += GLYPH_W;
    }
    cw += wl;
    s += wl;
    while (*s == ' ') s++;
  }
  if (cw > wmax) wmax = cw;
  lines++;
  if (widest) *widest = wmax * GLYPH_W;
  return lines;
}

static unsigned at(int x, int y) { return pix[y * CW + x]; }

/* ---- the page ---- */

static const char *body =
  "The kernel draws this text today. Word wrap, glyph shapes, rules and all of "
  "it live inside compositor dot c and omiweb dot c, writing straight into the "
  "framebuffer. That coupling is a layout bug away from being a memory bug, and "
  "nothing scrolls without the kernel noticing. Here the process owns the "
  "pixels, its own clip, its own glyphs - and the kernel never learns what a "
  "character is.";

int main(void) {
  long win = sys3(SYS_WIN_OPEN, (long)"render", WIN_W, WIN_H);
  check(win > 0, "the window opens", win, 1);

  int32_t g[4] = {0, 0, 0, 0};      /* the kernel writes {x, y, w, h} here */
  if (sys3(SYS_WIN_GEOM, win, (long)g, 0) == 0)
    printf("[render] client at %d,%d size %dx%d\n",
           g[0] + 1, g[1] + 31, g[2] - 2, g[3] - 32);

  long s = sys3(SYS_SURF_CREATE, win, CW, CH);
  check(s > 0, "the surface maps into the process", s, 1);
  if (s <= 0) { printf("[render] 0/%d\n", checks); return 1; }
  pix = (unsigned *)s;

  /* writable at the far end, not just at the start: a surface that runs out of
   * pages reads as a short mapping and only the last row notices */
  pix[(CH - 1) * CW + (CW - 1)] = 0x00AABBCCu;
  check(at(CW - 1, CH - 1) == 0x00AABBCCu, "the last pixel of the surface is writable",
        (long)at(CW - 1, CH - 1), 0x00AABBCC);

  fill(0, 0, CW, CH, C_BG);
  fill(0, 0, CW, 26, C_PANEL);
  text(MARGIN, 5, "Omi Net", C_TEXT);
  text(CW - MARGIN - 9 * GLYPH_W, 5, "ring 3", C_DIM);
  fill(0, 26, CW, 1, C_ACCENT);

  glyph_pixels = 0;
  int widest = 0;
  int lines = wrap_text(body, MARGIN, 40, CW - 2 * MARGIN, LH, C_TEXT, &widest);
  printf("[render] %d lines, widest %d px, %d glyph pixels\n", lines, widest, glyph_pixels);
  int content_bottom = 40 + lines * LH;
  check(content_bottom <= CH, "the text fits the surface the window gave it",
        content_bottom, CH);
  check(lines >= 3, "the text wraps onto several lines", lines, 3);
  check(widest <= CW - 2 * MARGIN, "no line escapes the measure", widest, CW - 2 * MARGIN);
  check(glyph_pixels > 400, "the glyph blitter drew something", glyph_pixels, 400);

  /* a 50% wash of white over the page colour: the arithmetic is asserted, then
   * the screen is checked at the same coordinate from outside */
  int blend_y = content_bottom + 8;
  fill(MARGIN, blend_y, 140, 16, C_BG);
  blend(MARGIN, blend_y, 140, 16, 0x00FFFFFFu, 128);
  unsigned got = at(MARGIN + 4, blend_y + 4);
  printf("[render] blend at %d,%d -> %u,%u,%u\n", MARGIN + 4, blend_y + 4,
         (got >> 16) & 0xFF, (got >> 8) & 0xFF, got & 0xFF);
  check(got == 0x008E8F91u, "half white over 1E2024 is 8E8F91", (long)got, 0x008E8F91);

  /* the clipper: a rectangle that runs off the bottom-right corner */
  fill(CW - 8, CH - 8, 40, 40, C_GREEN);
  check(at(CW - 1, CH - 1) == C_GREEN, "the clipped rectangle still paints what fits",
        (long)at(CW - 1, CH - 1), (long)C_GREEN);
  check(at(CW - 9, CH - 8) != C_GREEN, "and nothing left of it",
        (long)at(CW - 9, CH - 8), 0);

  long r = sys3(SYS_SURF_PRESENT, win, 0, ((long)CW << 16) | CH);
  check(r == 0, "presenting the whole surface succeeds", r, 0);

  /* the damage path: repaint the header band only, and say so */
  fill(0, 0, 90, 26, C_ACCENT);
  text(MARGIN, 5, "Omi", C_TEXT);
  long d = sys3(SYS_SURF_PRESENT, win, 0, ((long)90 << 16) | 26);
  check(d == 0, "presenting one damaged rectangle succeeds", d, 0);

  for (int i = 0; i < 600; i++) sys3(SYS_YIELD, 0, 0, 0);
  printf("[render] %d/%d\n", checks - fails, checks);
  return fails ? 1 : 0;
}