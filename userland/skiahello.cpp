// Skia on Omi: rasterise something, then prove the pixels are the ones we asked
// for. No fonts (no font manager in this build), so shapes and a gradient only -
// what matters is that the CPU backend runs, blends, antialiases and encodes.
//
// Written against the 2026 Skia API: raster surfaces come from SkSurfaces,
// paths are built with SkPathBuilder, and shaders from SkShaders take an
// SkGradient rather than the old (colors, positions, count, tilemode) list.
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkGradient.h"
#include "include/encode/SkPngEncoder.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t crc32_of(const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= b[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

int main(void) {
  const int W = 240, H = 160;
  SkImageInfo info = SkImageInfo::MakeN32Premul(W, H);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  if (!surface) {
    printf("[skia] FAIL Raster surface\n");
    return 1;
  }
  SkCanvas *canvas = surface->getCanvas();
  canvas->clear(SK_ColorBLACK);

  const SkColor4f grad[2] = {{0.08f, 0.12f, 0.35f, 1.0f},
                             {0.35f, 0.63f, 0.86f, 1.0f}};
  SkGradient g(SkGradient::Colors(SkSpan<const SkColor4f>(grad, 2),
                                  SkTileMode::kClamp),
               SkGradient::Interpolation{});
  const SkPoint pts[2] = {SkPoint::Make(0, 0), SkPoint::Make(W, H)};
  SkPaint bg;
  bg.setShader(SkShaders::LinearGradient(pts, g));
  canvas->drawPaint(bg);

  SkPaint box;
  box.setAntiAlias(true);
  box.setColor(SkColorSetARGB(255, 240, 60, 60));
  canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(20, 20, 100, 60), 12, 12),
                    box);

  SkPathBuilder star;
  for (int i = 0; i < 10; i++) {
    float a = (float)i * 3.14159265f / 5.0f - 1.5707963f;
    float r = (i & 1) ? 22.0f : 52.0f;
    float x = 175.0f + r * cosf(a), y = 90.0f + r * sinf(a);
    if (i == 0) star.moveTo(x, y); else star.lineTo(x, y);
  }
  star.close();

  SkPaint edge;
  edge.setAntiAlias(true);
  edge.setStyle(SkPaint::kStroke_Style);
  edge.setStrokeWidth(6);
  edge.setColor(SK_ColorWHITE);
  canvas->drawPath(star.detach(), edge);

  // Raster surfaces draw immediately; no flush needed (and SkCanvas no
  // longer has one in this version).

  // The proof: read the pixels back and check the colours we drew landed.
  SkPixmap pm;
  if (!surface->peekPixels(&pm)) {
    printf("[skia] FAIL peekPixels\n");
    return 1;
  }
  SkColor inBox = pm.getColor(70, 50);
  SkColor corner = pm.getColor(2, 2);
  printf("[skia] box=%08x corner=%08x\n", inBox, corner);
  if (SkColorGetR(inBox) < 200 || SkColorGetG(inBox) > 120) {
    printf("[skia] FAIL rect colour\n");
    return 1;
  }
  if (SkColorGetR(corner) == 0 && SkColorGetG(corner) == 0) {
    printf("[skia] FAIL gradient\n");
    return 1;
  }

  // Encode too, so the codec path is exercised, and hash the PNG so the same
  // binary can be compared byte for byte against another machine.
  SkDynamicMemoryWStream out;
  if (!SkPngEncoder::Encode(&out, pm, SkPngEncoder::Options{})) {
    printf("[skia] FAIL png encode\n");
    return 1;
  }
  sk_sp<SkData> png = out.detachAsData();
  printf("[skia] png=%zu bytes crc=%08x\n", png->size(),
         crc32_of(png->data(), png->size()));
  printf("[skia] %dx%d ok\n", W, H);
  return 0;
}