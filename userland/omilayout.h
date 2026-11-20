/*
 * Omi - HTML rendering, in the process that owns the window
 *
 * Gumbo parses the document; this file turns the tree into a flat display list
 * of rectangles and text runs that the compositor paints inside the browser
 * window. There is no scripting, no images and no CSS beyond a small subset -
 * the goal is that a page of real HTML (headings, paragraphs, lists, links,
 * basic colours and boxes) comes out readable instead of as one run-on line.
 */

#ifndef _OMILAYOUT_H
#define _OMILAYOUT_H

#include <stdint.h>
#include <stddef.h>

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

/* External stylesheets. A page's CSS is nearly always a separate file, and until
 * it is fetched the engine sees an unstyled document - which is why real pages
 * came out as a wall of text with no grid and no widths. The provider gets an
 * href and fills `out`; returning <= 0 means "no stylesheet", which is not an
 * error worth stopping for. */
typedef int (*web_css_fn)(const char *href, int len, char *out, int cap);
void web_css_provider(web_css_fn fn);

/* measure a string in font `font`, supplied by the compositor so the font
 * bitmaps are only ever compiled into one translation unit */
typedef int (*web_measure_fn)(int font, const char *s);

/* Parse and lay out `html`. Returns 0 on success, -1 on parse failure.
 * Any previous page is discarded. */
int web_build(const char *html, int len, int width, web_measure_fn measure);

/* Link hit test in page coordinates; NULL when the point is not over a link. */
const char *web_hit_link(int x, int y);

/* the page's own colours: paper to fill behind the content, ink for text that
 * does not name a colour itself */
uint32_t web_bg(void);
uint32_t web_fg(void);
void web_free(void);
int web_height(void);
int web_count(void);
/* Non-zero when the display list filled up: the page is shown only up to here. */
int web_ops_full(void);
const web_op_t *web_ops(void);
int web_line_height(int font);
/* Supply real metrics once the client has real faces: line height and ascent per
 * font id. Both are optional - leaving them off keeps the bitmap's numbers. */
void web_font_metrics(int (*line_h)(int font), int (*ascent)(int font));
/* How much of a stylesheet survived the scanner: rules, and custom properties. */
int web_rule_count(void);
int web_var_count(void);
int web_font_ascent(int font);
/* How many times the 8x16 bitmap a font id is drawn at: 1 or 2. The measure
 * function must multiply by this too, or the layout wraps at the wrong width. */
int web_font_scale(int font);

/* ---- forms -------------------------------------------------------------
 * Ops of kind 4 are text fields (iw = field index) and kind 5 are submit
 * buttons (iw = form index, text = the label). The client draws their contents,
 * owns what has been typed, and does the submitting. */
int web_field_count(void);
const char *web_field_name(int i);
const char *web_field_value(int i);   /* the value="" the page started with */
int web_field_form(int i);            /* -1 when the field is outside a form */
int web_field_hidden(int i);
int web_form_count(void);
const char *web_form_action(int f);   /* may be "" (post to this URL) */
const char *web_form_method(int f);   /* "get" or "post" */

#endif