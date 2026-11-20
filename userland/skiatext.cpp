// Text, on Omi, through Skia's own font stack: FreeType for the rasterising,
// Skia's text blob for the layout, and a real TrueType face read from the ISO.
//
// The point of this program is the same as skiahello's: a number that can be
// compared against the same binary on Linux. FreeType is deterministic, Skia's
// raster pipeline is deterministic, so the ink and the encoded PNG should hash
// the same on both machines. If they do not, something in the port is guessing.
//
// The face is loaded from a path, not discovered: Omi has no fontconfig and no
// directory enumeration in ring 3, so the program opens the file it was told
// about. On Omi that is a boot module; on Linux it is the same file in the tree.
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/encode/SkPngEncoder.h"
#include "include/ports/SkFontMgr_empty.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t crc32_of(const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= b[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

static int fails;

static void check(int ok, const char *what, long got, long want) {
  if (ok) {
    printf("[skiatext] ok   %s\n", what);
  } else {
    printf("[skiatext] FAIL %s (got %ld, wanted %ld)\n", what, got, want);
    fails++;
  }
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/fonts/Bricolage.ttf";
  const char *mono_path = argc > 2 ? argv[2] : "/fonts/JBMono.ttf";

  // An empty manager plus explicit data: no fontconfig, no directory scan, just
  // the bytes we hand it. This is the whole font stack Omi needs for now.
  sk_sp<SkFontMgr> mgr = SkFontMgr_New_Custom_Empty();
  check(mgr != nullptr, "a font manager exists", 0, 1);
  if (!mgr) return 1;

  sk_sp<SkData> data = SkData::MakeFromFileName(path);
  long dsize = data ? (long)data->size() : 0;
  printf("[skiatext] face %s = %ld bytes\n", path, dsize);
  check(dsize > 1024, "the face file is readable", dsize, 1024);
  if (!data || dsize <= 1024) return 1;

  sk_sp<SkTypeface> face = mgr->makeFromData(data, 0);
  check(face != nullptr, "FreeType parses it into a typeface", 0, 1);
  if (!face) return 1;

  SkString family;
  face->getFamilyName(&family);
  printf("[skiatext] family \"%s\" glyphs=%d upem=%d\n", family.c_str(),
         face->countGlyphs(), face->getUnitsPerEm());
  check(face->countGlyphs() > 100, "the face has glyphs", face->countGlyphs(), 100);

  const int W = 360, H = 150;
  SkImageInfo info = SkImageInfo::MakeN32Premul(W, H);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  if (!surface) {
    printf("[skiatext] FAIL Raster surface\n");
    return 1;
  }
  SkCanvas *canvas = surface->getCanvas();
  canvas->clear(SkColorSetARGB(255, 18, 20, 28));

  SkFont big(face, 40);
  big.setEdging(SkFont::Edging::kAntiAlias);
  big.setSubpixel(false);

  SkPaint text_ink;
  text_ink.setColor(SkColorSetARGB(255, 240, 244, 250));
  canvas->drawString("Skia on Omi", 16, 62, big, text_ink);

  // Measure the same run: the advance is FreeType's, and it is a number worth
  // printing because a wrong one means the metrics are being guessed.
  SkRect bounds;
  float advance = big.measureText("Skia on Omi", 11, SkTextEncoding::kUTF8, &bounds);
  printf("[skiatext] advance=%.2f bounds=%.2fx%.2f\n", advance, bounds.width(),
         bounds.height());
  check(advance > 150 && advance < 260, "the advance is a 40px run of 11 glyphs",
        (long)advance, 200);

  // A second, smaller line from the mono face, so two faces are exercised.
  sk_sp<SkData> mdata = SkData::MakeFromFileName(mono_path);
  sk_sp<SkTypeface> mono = mdata ? mgr->makeFromData(mdata, 0) : nullptr;
  if (mono) {
    SkFont small(mono, 17);
    small.setEdging(SkFont::Edging::kAntiAlias);
    SkPaint dim;
    dim.setColor(SkColorSetARGB(255, 120, 200, 170));
    canvas->drawString("freetype, raster, present", 18, 100, small, dim);
    printf("[skiatext] mono face \"%s\"\n", "loaded");
  } else {
    printf("[skiatext] mono face missing (%s)\n", mono_path);
  }

  // Ink: count pixels that are not the background, and hash them. Both numbers
  // travel between machines better than a picture does.
  SkPixmap pm;
  if (!surface->peekPixels(&pm)) {
    printf("[skiatext] FAIL peekPixels\n");
    return 1;
  }
  long lit = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      SkColor c = pm.getColor(x, y);
      if (SkColorGetR(c) > 40 || SkColorGetG(c) > 40 || SkColorGetB(c) > 40) lit++;
    }
  }
  printf("[skiatext] ink=%ld of %d px\n", lit, W * H);
  check(lit > 1500 && lit < 12000, "there is text on the canvas, not a smear", lit, 4000);
  printf("[skiatext] surface crc=%08x\n",
         crc32_of(pm.addr(), (size_t)pm.rowBytes() * H));

  SkDynamicMemoryWStream out;
  if (!SkPngEncoder::Encode(&out, pm, SkPngEncoder::Options{})) {
    printf("[skiatext] FAIL png encode\n");
    return 1;
  }
  sk_sp<SkData> png = out.detachAsData();
  printf("[skiatext] png=%zu bytes crc=%08x\n", png->size(),
         crc32_of(png->data(), png->size()));
  printf("[skiatext] %s\n", fails ? "FAILED" : "ok");
  return fails ? 1 : 0;
}
