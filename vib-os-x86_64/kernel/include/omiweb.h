/*
 * Omi - HTML rendering
 *
 * Gumbo parses the document; this file turns the tree into a flat display list
 * of rectangles and text runs that the compositor paints inside the browser
 * window. There is no scripting, no images and no CSS beyond a small subset -
 * the goal is that a page of real HTML (headings, paragraphs, lists, links,
 * basic colours and boxes) comes out readable instead of as one run-on line.
 */

#ifndef _OMIWEB_H
#define _OMIWEB_H

#include "types.h"

/* one drawing instruction, in page coordinates (origin = top left of content) */
typedef struct {
  uint8_t kind;      /* 0 = filled rect, 1 = text run, 2 = underline */
  uint8_t font;      /* 0..4, picked by the compositor's face table */
  uint8_t bold;      /* draw twice, 1px apart */
  uint8_t underline; /* text: draw a rule beneath the run */
  uint8_t radius;    /* rects: corner radius in px, 0 = square */
  uint8_t pad[2];
  int32_t x, y, w, h;
  uint32_t color;
  const char *text;  /* NULL for rects */
  const char *href;    /* kind 1: link target, when the run is inside <a> */
  const uint32_t *px;  /* kind 3: source pixels, 0x00RRGGBB */
  int32_t iw, ih;      /* kind 3: source dimensions */
} web_op_t;

/* Images: the renderer asks for a decoded bitmap by URL. Returning NULL makes
 * the renderer fall back to alt text, so a failed fetch never leaves a hole. */
typedef struct { int w, h; const uint32_t *px; } web_image_t;
typedef const web_image_t *(*web_image_fn)(const char *src, int len);
void web_image_provider(web_image_fn fn);

/* measure a string in font `font`, supplied by the compositor so the font
 * bitmaps are only ever compiled into one translation unit */
typedef int (*web_measure_fn)(int font, const char *s);

/* Parse and lay out `html`. Returns 0 on success, -1 on parse failure.
 * Any previous page is discarded. */
int web_build(const char *html, int len, int width, web_measure_fn measure);

/* Link hit test in page coordinates; NULL when the point is not over a link. */
const char *web_hit_link(int x, int y);

void web_free(void);
int web_height(void);
int web_count(void);
const web_op_t *web_ops(void);
int web_line_height(int font);

#endif