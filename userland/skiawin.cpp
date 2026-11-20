/* skiawin - Skia drawing into a real Omi window.
 *
 * skiahello proved the raster pipeline runs. This is the other half: the surface
 * Skia draws into is not a private raster, it is the pixel buffer of a window the
 * compositor is going to blit. So the chain under test is
 *
 *   SkCanvas -> Skia's raster pipeline -> the window's shared pages
 *            -> SYS_SURF_PRESENT -> the compositor -> the framebuffer
 *
 * and it is checked at both ends. From the inside: read the window buffer back
 * through its own mapping, at coordinates Skia was told to paint, and compare the
 * colours. From the outside: a framebuffer dump has to contain the one colour on
 * screen that nothing but this program draws.
 *
 * That second check is why the magenta bar is there and why it is four pixels
 * wide - a dump that is off by a pixel, or scaled, keeps a four-pixel bar and
 * destroys a one-pixel line.
 */
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkGradient.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define SYS_WIN_OPEN     6
#define SYS_WIN_WAIT     12
#define SYS_WIN_RECT     13
#define SYS_WIN_GEOM     14
#define SYS_SURF_CREATE  35
#define SYS_SURF_PRESENT 36

#define W 400
#define H 260
#define TITLE_H 31     /* the window's outer height includes the title bar */

static int checks, fails;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) { fails++; printf("[skiawin] FAIL %s (got %ld want %ld)\n", what, got, want); }
  else printf("[skiawin] ok   %s\n", what);
}

/* int $0x80 carries four arguments in rdi, rsi, rdx, r8 - not r10. */
static long sys4(long n, long a, long b, long c, long d) {
  long r;
  register long r8 __asm__("r8") = d;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r8) : "memory");
  return r;
}

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

/* Wrap the window's pixels and draw. Skia's N32 on x86 is BGRA in memory, which
 * is the same 32-bit word Omi's surfaces carry, so no swizzle is needed - if that
 * were wrong the colours below would come back with red and blue swapped, and the
 * checks would say so. */
static int skia_paint(void *pixels) {
  SkImageInfo info = SkImageInfo::MakeN32Premul(W, H);
  sk_sp<SkSurface> surf = SkSurfaces::WrapPixels(info, pixels, (size_t)W * 4);
  if (!surf) return 1;
  SkCanvas *c = surf->getCanvas();

  const SkColor4f grad[2] = {{0.06f, 0.07f, 0.12f, 1.0f},
                             {0.16f, 0.22f, 0.40f, 1.0f}};
  SkGradient g(SkGradient::Colors(SkSpan<const SkColor4f>(grad, 2), SkTileMode::kClamp),
               SkGradient::Interpolation{});
  const SkPoint pts[2] = {SkPoint::Make(0, 0), SkPoint::Make(W, H)};
  SkPaint bg;
  bg.setShader(SkShaders::LinearGradient(pts, g));
  c->drawPaint(bg);

  /* A filled block: the inside check reads one pixel of this. */
  SkPaint block;
  block.setColor(0xFF8A2BE2);          /* blueviolet: nothing else on this screen */
  c->drawRect(SkRect::MakeXYWH(30, 20, 120, 60), block);

  SkPaint rrect;
  rrect.setAntiAlias(true);
  rrect.setColor(0xFFF03C3C);
  c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(180, 20, 120, 60), 14, 14), rrect);

  SkPathBuilder star;
  for (int i = 0; i < 10; i++) {
    float a = (float)i * 3.14159265f / 5.0f - 1.5707963f;
    float r = (i & 1) ? 26.0f : 62.0f;
    float x = 200.0f + r * cosf(a), y = 175.0f + r * sinf(a);
    if (i == 0) star.moveTo(x, y); else star.lineTo(x, y);
  }
  star.close();
  SkPaint edge;
  edge.setAntiAlias(true);
  edge.setStyle(SkPaint::kStroke_Style);
  edge.setStrokeWidth(5);
  edge.setColor(SK_ColorWHITE);
  c->drawPath(star.detach(), edge);

  /* The marker: a four-pixel neon-green border, a colour nothing else on this
   * screen uses, so a framebuffer dump can be asked for it by value. It is a
   * border rather than one bar because a window in a cascade is usually partly
   * covered, and four edges survive that where a single line does not. */
  SkPaint marker;
  marker.setColor(0xFF39FF14);
  c->drawRect(SkRect::MakeXYWH(0, 0, W, 4), marker);
  c->drawRect(SkRect::MakeXYWH(0, H - 4, W, 4), marker);
  c->drawRect(SkRect::MakeXYWH(0, 0, 4, H), marker);
  c->drawRect(SkRect::MakeXYWH(W - 4, 0, 4, H), marker);

  return 0;
}

int main(void) {
  printf("[skiawin] start\n");

  long win = sys3(SYS_WIN_OPEN, (long)"skia", W, H + TITLE_H);
  check(win >= 0, "a window opens", win, 0);
  if (win < 0) { printf("[skiawin] %d/%d\n", checks - fails, checks); return 1; }

  long addr = sys3(SYS_SURF_CREATE, win, W, H);
  check(addr > 0, "its pixels are a user mapping", addr, 0);
  if (addr <= 0) { printf("[skiawin] %d/%d\n", checks - fails, checks); return 1; }

  int geom[4] = {0, 0, 0, 0};
  sys3(SYS_WIN_GEOM, win, (long)geom, 0);
  printf("[skiawin] window %ld at %d,%d %dx%d\n", win, geom[0], geom[1], geom[2], geom[3]);

  int rc = skia_paint((void *)addr);
  check(rc == 0, "Skia painted into the window", rc, 0);

  /* What Skia should have left behind, read back through the same mapping. The
   * point is that it is the window's memory, so a value here is a value the
   * compositor will read. */
  uint32_t *px = (uint32_t *)addr;
  uint32_t top   = px[1 * W + 200];          /* the top edge of the border */
  uint32_t left  = px[130 * W + 1];          /* the left edge */
  uint32_t block = px[40 * W + 60];          /* inside the blueviolet block */
  printf("[skiawin] border=%08x/%08x block=%08x\n", top, left, block);
  check(top == 0xFF39FF14u && left == 0xFF39FF14u,
        "the marker border is in the window buffer", top, 0xFF39FF14);
  check(block == 0xFF8A2BE2u, "a filled block is in the window buffer", block, 0xFF8A2BE2);

  /* Move out of the cascade the other windows are stacked in, so the framebuffer
   * dump has somewhere the compositor will actually blit this window to. */
  /* (id, x | y<<32, unused, (w<<16)|h) - the x and y travel in one argument and
   * the size in the fourth, which is not what the header used to say. */
  long mv = sys4(SYS_WIN_RECT, win, 560L | (30L << 32), 0,
                 ((long)W << 16) | (H + TITLE_H));
  check(mv == 0, "the window moves itself out of the cascade", mv, 0);
  sys3(SYS_WIN_GEOM, win, (long)geom, 0);
  printf("[skiawin] placed at %d,%d %dx%d - the marker border is at x=%d..%d, y=%d..%d\n",
         geom[0], geom[1], geom[2], geom[3],
         geom[0], geom[0] + geom[2] - 1, geom[1], geom[1] + geom[3] - 1);

  /* Full present, then hold the window: an outside dump can only see what is
   * still on screen, and a window that exits takes its pixels with it. */
  long p = sys3(SYS_SURF_PRESENT, win, 0, ((long)W << 16) | H);
  check(p == 0, "the surface presents", p, 0);

  printf("[skiawin] parked - the framebuffer should show a 4px neon-green border\n");
  int ev[4];
  while (sys3(SYS_WIN_WAIT, win, (long)ev, 0) >= 0)
    sys3(SYS_SURF_PRESENT, win, 0, ((long)W << 16) | H);
  return 0;
}