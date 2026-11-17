/* omitext.h - the rasteriser both ring-3 programs use.
 *
 * The first version of this lived inside omirender.c. A browser needs the same
 * fill, blend, glyph blit and wrap, and two copies of a rasteriser is how a
 * clipping bug survives in one of them. So the code moved here, unchanged, and
 * the including file supplies the surface and its dimensions:
 *
 *   static unsigned *pix;    the mapped surface
 *   #define CW, CH          its size, in pixels
 *   text_glyph_pixels     the blitter's own count, reset per paint pass
 *
 * Everything here clips. A rasteriser that trusts its callers writes into
 * whatever page follows the last row.
 */
#ifndef OMI_TEXT_H
#define OMI_TEXT_H

static unsigned blend_px(unsigned dst, unsigned src, int a) {
  /* a in 0..255. Integer, so the result is reproducible and checkable. */
  int r = (((dst >> 16) & 0xFF) * (255 - a) + ((src >> 16) & 0xFF) * a) / 255;
  int g = (((dst >>  8) & 0xFF) * (255 - a) + ((src >>  8) & 0xFF) * a) / 255;
  int b = (( dst        & 0xFF) * (255 - a) + ( src        & 0xFF) * a) / 255;
  return (unsigned)((r << 16) | (g << 8) | b);
}

/* An optional clip rectangle, honoured by every primitive below. A page is drawn
 * from a display list whose rectangles are the full content width, so without
 * this a page's own background paints over the title bar and the scrollbar. Off
 * until the includer turns it on: omirender draws its whole surface. */
static int clip_x0, clip_y0, clip_x1, clip_y1, clip_on;

static void clip_rect(int x0, int y0, int x1, int y1) {
  clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1; clip_on = 1;
}
static void clip_none(void) { clip_on = 0; }

/* Narrow a rectangle to the surface and then to the clip box. */
static void clip_bounds(int *x, int *y, int *w, int *h) {
  if (*x < 0) { *w += *x; *x = 0; }
  if (*y < 0) { *h += *y; *y = 0; }
  if (*x + *w > CW) *w = CW - *x;
  if (*y + *h > CH) *h = CH - *y;
  if (clip_on) {
    if (*x < clip_x0) { *w -= clip_x0 - *x; *x = clip_x0; }
    if (*y < clip_y0) { *h -= clip_y0 - *y; *y = clip_y0; }
    if (*x + *w > clip_x1) *w = clip_x1 - *x;
    if (*y + *h > clip_y1) *h = clip_y1 - *y;
  }
}

static void fill(int x, int y, int w, int h, unsigned c) {
  clip_bounds(&x, &y, &w, &h);
  if (w <= 0 || h <= 0) return;
  for (int j = 0; j < h; j++) {
    unsigned *row = pix + (y + j) * CW + x;
    for (int i = 0; i < w; i++) row[i] = c;
  }
}

static void blend(int x, int y, int w, int h, unsigned c, int a) {
  clip_bounds(&x, &y, &w, &h);
  if (w <= 0 || h <= 0) return;
  for (int j = 0; j < h; j++) {
    unsigned *row = pix + (y + j) * CW + x;
    for (int i = 0; i < w; i++) row[i] = blend_px(row[i], c, a);
  }
}

/* Counted by the blitter itself, so a caller can say how much it actually drew
 * rather than how much it asked to draw. Reset it before a paint pass. */
static int text_glyph_pixels;

/* `code`, not `ch`: a client defines CH as its live height (#define CH ch in
 * ominet.c), so a parameter called ch silently rewrote the bottom bound into
 * "py >= <character code>" and sheared every glyph below its ASCII value. */
static void glyph(int x, int y, char code, unsigned c) {
  int idx = (unsigned char)code - GLYPH_FIRST;
  if (idx < 0 || idx > GLYPH_LAST - GLYPH_FIRST) return;
  const unsigned char *rows = glyphs[idx];
  for (int r = 0; r < GLYPH_H; r++) {
    unsigned char bits = rows[r];
    if (!bits) continue;
    for (int col = 0; col < GLYPH_W; col++) {
      if (!(bits & (0x80 >> col))) continue;
      int px = x + col, py = y + r;
      if (px < 0 || py < 0 || px >= CW || py >= CH) continue;
      if (clip_on && (px < clip_x0 || py < clip_y0 || px >= clip_x1 || py >= clip_y1)) continue;
      pix[py * CW + px] = c;
      text_glyph_pixels++;
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

/* The same wrap, measured instead of drawn: a layout pass has to know where the
 * next block starts before it paints this one. Identical rules to wrap_text, on
 * purpose - two different measurements of the same string is how a line ends up
 * drawn 8px wider than the box it was measured for. */
static int measure_wrap(const char *s, int maxpx, int *widest) {
  int lines = 1, cw = 0, wmax = 0;
  int chars = maxpx / GLYPH_W;
  if (widest) *widest = 0;
  while (*s) {
    int wl = 0;
    while (s[wl] && s[wl] != ' ') wl++;
    if (cw > 0 && cw + 1 + wl > chars) {
      if (cw > wmax) wmax = cw;
      lines++; cw = 0;
    }
    if (cw > 0) cw += 1;
    cw += wl;
    s += wl;
    while (*s == ' ') s++;
  }
  if (cw > wmax) wmax = cw;
  if (widest) *widest = wmax * GLYPH_W;
  return lines;
}

#endif