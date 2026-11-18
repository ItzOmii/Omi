/*
 * GIF, first frame only.
 *
 * The decoder is a straight LZW implementation of the GIF variant: a colour-index
 * stream, a code table that starts at the root size plus two reserved codes
 * (clear and end), and a code width that grows as the table fills. Getting the
 * width changes right is most of the work; everything else is reading the header
 * and the colour table.
 */

#include <stdlib.h>
#include <string.h>
#include "imgdec.h"

#define GIF_MAX_DIM 1024          /* a frame bigger than this is refused, not clipped */
#define LZW_MAX 4096              /* the code table is 12 bits wide, by the spec */

typedef struct {
  const uint8_t *p;
  int n, i;
} rd_t;

static int rd_u8(rd_t *r) { return r->i < r->n ? r->p[r->i++] : -1; }

static int rd_u16(rd_t *r) {
  int lo = rd_u8(r), hi = rd_u8(r);
  if (lo < 0 || hi < 0) return -1;
  return lo | (hi << 8);
}

int gif_is(const uint8_t *data, int len) {
  if (!data || len < 6) return 0;
  if (memcmp(data, "GIF87a", 6) && memcmp(data, "GIF89a", 6)) return 0;
  return 1;
}

/* Skip a chain of data sub-blocks; returns 0 on success. */
static int skip_subblocks(rd_t *r) {
  for (;;) {
    int n = rd_u8(r);
    if (n < 0) return -1;
    if (n == 0) return 0;
    r->i += n;
    if (r->i > r->n) return -1;
  }
}

/* Decompress the index stream into `out`, which holds at most w*h bytes.
 * Returns the number of pixels written, or -1 on malformed input. */
static int lzw_decode(const uint8_t *src, int srclen, int min_code, uint8_t *out, int outcap) {
  if (min_code < 2 || min_code > 8) return -1;

  /* The code table: prefix and suffix per entry, which is enough to walk an entry
   * backwards into the output without storing every string. */
  static uint16_t prefix[LZW_MAX];
  static uint8_t suffix[LZW_MAX];
  static uint8_t stack[LZW_MAX + 1];

  int clear = 1 << min_code;
  int end = clear + 1;
  int avail = clear + 2;
  int width = min_code + 1;
  int prev = -1;

  for (int i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; }

  int bitpos = 0, outn = 0;
  for (;;) {
    if ((bitpos + width + 7) / 8 > srclen) break;
    int code = 0;
    for (int b = 0; b < width; b++) {
      int pos = bitpos + b;
      int byte = src[pos >> 3];
      int bit = (byte >> (pos & 7)) & 1;
      code |= bit << b;
    }
    bitpos += width;

    if (code == clear) {
      avail = clear + 2;
      width = min_code + 1;
      prev = -1;
      continue;
    }
    if (code == end) break;

    int sp = 0;
    int c = code;
    if (code >= avail) {
      /* The one special case: a code that has not been defined yet is the
       * previous string plus its own first byte. */
      if (prev < 0) return -1;
      stack[sp++] = suffix[prev];
      c = prev;
    }
    while (c >= 0 && c < LZW_MAX) {
      if (sp >= LZW_MAX) return -1;
      stack[sp++] = suffix[c];
      if (prefix[c] == 0xFFFF) break;
      c = prefix[c];
    }
    /* The stack holds the string backwards, so its top is the first character -
     * which is the byte the next table entry needs. Take it before the walk
     * empties the stack. */
    int first = stack[sp - 1];
    while (sp > 0) {
      if (outn >= outcap) return outn;
      out[outn++] = stack[--sp];
    }

    if (prev >= 0 && avail < LZW_MAX) {
      prefix[avail] = (uint16_t)prev;
      suffix[avail] = (uint8_t)first;
      avail++;
      if (avail == (1 << width) && width < 12) width++;
    }
    prev = code;
  }
  return outn;
}

int gif_decode(const uint8_t *data, int len, int *w, int *h, uint32_t **px) {
  if (!gif_is(data, len)) return -1;
  rd_t r = { data, len, 6 };

  int sw = rd_u16(&r), sh = rd_u16(&r);
  if (sw <= 0 || sh <= 0 || sw > GIF_MAX_DIM || sh > GIF_MAX_DIM) return -1;
  int packed = rd_u8(&r);
  if (packed < 0) return -1;
  if (rd_u8(&r) < 0) return -1;      /* background index */
  if (rd_u8(&r) < 0) return -1;      /* aspect ratio */

  uint8_t gct[256 * 3];
  int gct_n = 0;
  if (packed & 0x80) {
    gct_n = 2 << (packed & 7);
    if (r.i + gct_n * 3 > r.n) return -1;
    memcpy(gct, r.p + r.i, (size_t)gct_n * 3);
    r.i += gct_n * 3;
  }

  /* Walk the blocks until the first image descriptor. */
  for (;;) {
    int b = rd_u8(&r);
    if (b < 0) return -1;
    if (b == 0x3B) return -1;                     /* trailer before any image */
    if (b == 0x21) {                              /* extension */
      if (rd_u8(&r) < 0) return -1;
      if (skip_subblocks(&r) != 0) return -1;
      continue;
    }
    if (b != 0x2C) return -1;                     /* unknown block */

    int ix = rd_u16(&r), iy = rd_u16(&r);
    int iw = rd_u16(&r), ih = rd_u16(&r);
    int ip = rd_u8(&r);
    if (ix < 0 || iy < 0 || iw <= 0 || ih <= 0 || ip < 0) return -1;
    if (iw > GIF_MAX_DIM || ih > GIF_MAX_DIM) return -1;

    const uint8_t *ct = gct;
    int ct_n = gct_n;
    uint8_t lct[256 * 3];
    if (ip & 0x80) {
      ct_n = 2 << (ip & 7);
      if (r.i + ct_n * 3 > r.n) return -1;
      memcpy(lct, r.p + r.i, (size_t)ct_n * 3);
      r.i += ct_n * 3;
      ct = lct;
    }

    int min_code = rd_u8(&r);
    if (min_code < 0) return -1;

    /* Gather the sub-blocks into one contiguous run: LZW reads a bit stream and
     * does not care where the boundaries were. */
    static uint8_t comp[64 * 1024];
    int clen = 0;
    for (;;) {
      int n = rd_u8(&r);
      if (n < 0) return -1;
      if (n == 0) break;
      if (r.i + n > r.n) return -1;
      if (clen + n > (int)sizeof comp) return -1;
      memcpy(comp + clen, r.p + r.i, (size_t)n);
      clen += n;
      r.i += n;
    }

    size_t npix = (size_t)iw * (size_t)ih;
    uint8_t *idx = (uint8_t *)malloc(npix);
    if (!idx) return -2;
    int got = lzw_decode(comp, clen, min_code, idx, (int)npix);
    if (got < 0) { free(idx); return -1; }

    uint32_t *out = (uint32_t *)malloc(npix * sizeof(uint32_t));
    if (!out) { free(idx); return -2; }

    /* Rows go either straight down or in the four interlace passes. */
    int rowmap[GIF_MAX_DIM];
    if (ip & 0x40) {
      int k = 0;
      int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
      for (int pass = 0; pass < 4; pass++)
        for (int y = start[pass]; y < ih; y += step[pass]) rowmap[k++] = y;
    } else {
      for (int y = 0; y < ih; y++) rowmap[y] = y;
    }

    int src_i = 0;
    for (int ry = 0; ry < ih; ry++) {
      int y = rowmap[ry];
      uint32_t *row = out + (size_t)y * (size_t)iw;
      for (int x = 0; x < iw; x++) {
        int ci = (src_i < got) ? idx[src_i++] : 0;
        if (ci >= ct_n) ci = 0;
        const uint8_t *c = ct + ci * 3;
        row[x] = ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
      }
    }
    free(idx);

    *w = iw;
    *h = ih;
    *px = out;
    return 0;
  }
}