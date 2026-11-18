/* imgdec: a GIF is decoded to pixels, and the pixels are the ones that were put
 * in it. The fixture is an 8x8 image of four coloured quadrants, generated on the
 * host and embedded as bytes: if the LZW stream, the palette or the row order is
 * wrong, the quadrant check is what notices.
 */
#include <stdio.h>
#include <stdint.h>
#include "imgdec.h"

static int checks, fails;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) fails++;
  printf("[img] %s %s (got %ld want %ld)\n", ok ? "ok  " : "FAIL", what, got, want);
}

static const uint8_t fixture[] = {
  0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x08, 0x00, 0x08, 0x00, 0x81, 0x00,
  0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
  0xFF, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x08,
  0x22, 0x00, 0x01, 0x08, 0x04, 0x10, 0xA0, 0x60, 0x80, 0x81, 0x04, 0x0D,
  0x22, 0x34, 0x78, 0x70, 0x20, 0x43, 0x01, 0x10, 0x05, 0x0C, 0x98, 0x38,
  0x20, 0xA2, 0x44, 0x8A, 0x16, 0x29, 0x56, 0x8C, 0xA8, 0x31, 0x20, 0x00,
  0x3B
};
#define FIXTURE_LEN ((int)sizeof fixture)

int main(void) {
  int w = 0, h = 0;
  uint32_t *px = 0;

  check(gif_is(fixture, FIXTURE_LEN) == 1, "the fixture looks like a GIF", 1, 1);
  check(gif_is((const uint8_t *)"not a gif at all", 16) == 0, "and something else does not", 0, 0);

  int r = gif_decode(fixture, FIXTURE_LEN, &w, &h, &px);
  check(r == 0, "the fixture decodes", r, 0);
  check(w == 8 && h == 8, "at the size the header claims", w * 1000 + h, 8008);
  if (r != 0 || !px) { printf("[img] %d/%d\n", checks - fails, checks); return 1; }

  uint32_t tl = px[0] & 0xFFFFFF, tr = px[7] & 0xFFFFFF;
  uint32_t bl = px[7 * 8] & 0xFFFFFF, br = px[7 * 8 + 7] & 0xFFFFFF;
  check(tl == 0x000000, "the first quadrant is the first palette entry", tl, 0x000000);
  check(tr == 0xFF0000, "the second is red", tr, 0xFF0000);
  check(bl == 0x00FF00, "the third is green", bl, 0x00FF00);
  check(br == 0x0000FF, "the fourth is blue", br, 0x0000FF);

  /* Every pixel must be in its quadrant: a wrong code table smears instead. */
  int mixed = 0;
  static const uint32_t quad[2][2] = { { 0x000000, 0xFF0000 }, { 0x00FF00, 0x0000FF } };
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      if ((px[y * 8 + x] & 0xFFFFFF) != quad[(y < 4) ? 0 : 1][(x < 4) ? 0 : 1]) mixed++;
  check(mixed == 0, "every pixel is in the quadrant it belongs to", mixed, 0);

  check(gif_decode((const uint8_t *)"GIF87a", 6, &w, &h, &px) != 0,
        "a truncated GIF is refused, not decoded", 1, 1);

  printf("[img] %d/%d\n", checks - fails, checks);
  return fails ? 1 : 0;
}
