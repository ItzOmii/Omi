/*
 * omiskia.cpp - the browser's text, drawn by Skia instead of by stb_truetype.
 *
 * omittf.h rasterises each glyph into an 8-bit mask with stb, caches it, and
 * blends it into the window buffer one pixel at a time. That works and it is
 * honest, but it is a second graphics stack in a program that already ships a
 * first one: Omi Net paints boxes, backgrounds and rules itself, and text went
 * through a different rasteriser than everything else on the same surface.
 *
 * This file replaces the text half with Skia's, so a glyph and a rounded
 * rectangle come out of the same rasteriser with the same antialiasing, and the
 * text gets FreeType's hinting and subpixel positioning instead of a mask
 * produced by a header-only library. The browser keeps its own layout, its own
 * display list, and its own scroll-by-memmove; only the drawing of a run of
 * characters changes hands.
 *
 * The contract is omittf.h's, exactly:
 *
 *   y is the baseline. Colours are 0x00RRGGBB. The caller's clip rectangle is
 *   honoured. measure() and draw() advance by the same numbers, because they are
 *   the same SkFont - a line cannot be wrapped for a width the painter then
 *   exceeds, which is the promise the old path made and had to work for.
 *
 * The window buffer is XRGB8888 in memory - ominet writes 0x00RRGGBB words and
 * ignores the top byte - which is Skia's BGRA_8888 with the alpha channel along
 * for the ride. The surface is declared kOpaque for that reason: the screen has
 * no alpha, so the top byte must not make a glyph translucent.
 */
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_empty.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "omiskia.h"

/* The five ids the layout engine emits, copied from omittf.h's omi_faces so the
 * two paths cannot disagree about which face is which size. */
static const struct { int face, px, bold; } kFaces[5] = {
    {0, 14, 0},   /* 0 body        14px */
    {0, 26, 1},   /* 1 display     26px, h1/h2 */
    {1, 13, 0},   /* 2 mono        13px, code and pre */
    {0, 12, 0},   /* 3 alt         12px, small print */
    {0, 17, 0},   /* 4 medium      17px, h3/h4 and lead-ins */
};

static sk_sp<SkTypeface> g_face[2];
static SkFont g_font[5];
static int g_ready, g_tried;

/* One wrapped surface per target buffer. ominet paints into two: the live window
 * surface and, when scrolling, a shadow it copies rows between. Wrapping is cheap
 * but not free, and paint() calls this once per text run. */
struct Target {
  unsigned *pix;
  int cw, ch;
  sk_sp<SkSurface> surf;
};
static Target g_targets[2];

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

/* SYS_FONT copies a boot module's bytes out of the kernel, the same call omittf.h
 * makes. No filesystem, no fontconfig, no directory scan. */
static sk_sp<SkData> load_face(int which) {
  long n = sys3(OMI_SYS_FONT, which, 0, 0);
  if (n <= 0 || n > (16 << 20)) return nullptr;
  void *buf = malloc((size_t)n);
  if (!buf) return nullptr;
  memset(buf, 0, (size_t)n);       /* touch it: the kernel cannot fault it in */
  if (sys3(OMI_SYS_FONT, which, (long)buf, n) != n) {
    free(buf);
    return nullptr;
  }
  sk_sp<SkData> d = SkData::MakeWithCopy(buf, (size_t)n);
  free(buf);
  return d;
}

extern "C" int omiskia_init(void) {
  if (g_tried) return g_ready;
  g_tried = 1;
  sk_sp<SkFontMgr> mgr = SkFontMgr_New_Custom_Empty();
  if (!mgr) return 0;
  for (int i = 0; i < 2; i++) {
    sk_sp<SkData> d = load_face(i);
    if (!d) continue;
    g_face[i] = mgr->makeFromData(d, 0);
  }
  if (!g_face[0]) return 0;          /* the display face is what body text uses */
  for (int i = 0; i < 5; i++) {
    g_font[i].setTypeface(g_face[kFaces[i].face]);
    g_font[i].setSize((SkScalar)kFaces[i].px);
    g_font[i].setEdging(SkFont::Edging::kAntiAlias);
    g_font[i].setSubpixel(false);
  }
  g_ready = 1;
  return 1;
}

extern "C" int omiskia_face_loaded(int which) {
  if (!omiskia_init() || which < 0 || which > 1) return 0;
  return g_face[which] ? 1 : 0;
}

static int clamp_font(int font) { return font < 0 ? 0 : font > 4 ? 4 : font; }

extern "C" int omiskia_line_h(int font) {
  if (!omiskia_init()) return 0;
  int id = clamp_font(font);
  SkFontMetrics m;
  g_font[id].getMetrics(&m);
  /* ascent - descent + two pixels of leading, the number omittf.h produced */
  return (int)(-m.fAscent + m.fDescent + 0.5f) + 2;
}

extern "C" int omiskia_ascent(int font) {
  if (!omiskia_init()) return 0;
  SkFontMetrics m;
  g_font[clamp_font(font)].getMetrics(&m);
  return (int)(-m.fAscent + 0.5f);
}

/* Truncating, not rounding, and for the same reason omittf.h floored its 26.6
 * sum: a measured width that is one pixel more than the ink a painter puts down
 * is harmless, and one that is a pixel less lets a glyph escape its box. */
extern "C" int omiskia_measure(int font, const char *s) {
  if (!omiskia_init() || !s) return 0;
  int id = clamp_font(font);
  SkScalar w = g_font[id].measureText(s, strlen(s), SkTextEncoding::kUTF8);
  return (int)w;
}

static Target *target_for(unsigned *pix, int cw, int ch) {
  for (int i = 0; i < 2; i++)
    if (g_targets[i].surf && g_targets[i].pix == pix && g_targets[i].cw == cw &&
        g_targets[i].ch == ch)
      return &g_targets[i];
  for (int i = 0; i < 2; i++) {
    if (!g_targets[i].surf || g_targets[i].pix != pix) {
      SkImageInfo info = SkImageInfo::Make(cw, ch, kBGRA_8888_SkColorType,
                                           kOpaque_SkAlphaType);
      sk_sp<SkSurface> s = SkSurfaces::WrapPixels(info, pix, (size_t)cw * 4);
      if (!s) return nullptr;
      g_targets[i].pix = pix;
      g_targets[i].cw = cw;
      g_targets[i].ch = ch;
      g_targets[i].surf = s;
      return &g_targets[i];
    }
  }
  return nullptr;
}

extern "C" void omiskia_draw(unsigned *pix, int cw, int ch, int font, int x, int y,
                             const char *s, unsigned c, int clip_on, int cx0,
                             int cy0, int cx1, int cy1) {
  if (!omiskia_init() || !pix || !s || !*s) return;
  Target *t = target_for(pix, cw, ch);
  if (!t) return;
  SkCanvas *canvas = t->surf->getCanvas();
  int id = clamp_font(font);

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(SkColorSetARGB(255, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));

  if (clip_on) {
    canvas->save();
    canvas->clipRect(SkRect::MakeLTRB((SkScalar)cx0, (SkScalar)cy0, (SkScalar)cx1,
                                      (SkScalar)cy1));
  }
  canvas->drawString(s, (SkScalar)x, (SkScalar)y, g_font[id], paint);
  /* Synthetic bold, exactly as the mask path did it: the same glyph again, one
   * pixel to the right. FreeType's embolden would change the advances and the
   * layout has already been measured. */
  if (kFaces[id].bold)
    canvas->drawString(s, (SkScalar)(x + 1), (SkScalar)y, g_font[id], paint);
  if (clip_on) canvas->restore();
}
