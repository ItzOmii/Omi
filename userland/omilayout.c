/*
 * Omi - HTML page rendering.
 *
 * Gumbo gives us the tree; this file measures it and emits a flat display list
 * (rectangles + text runs) that the compositor paints. Deliberately small: a
 * block/inline layout engine with the chunk of CSS that makes real pages
 * readable, no scripting, no images, no floats or flexbox.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include "omilayout.h"
#include "gumbo.h"

/* The kernel writes kmalloc/kfree; in a process these are libc's malloc/free.
 * Same names, so the engine did not need rewriting to move - which is the point
 * of moving it instead of reimplementing it. */
static void *kmalloc(size_t n) { return malloc(n); }
static void kfree(void *p) { free(p); }
static void *krealloc(void *p, size_t n) { return realloc(p, n); }

/* ------------------------------------------------------------------ state */

/* The display list grows on demand (see op_push), so this is only a ceiling on
 * what one page may turn into, not a cost paid up front. A reference page like
 * docs.python.org/3/library/stdtypes.html needs about 37k ops to the end; 9000
 * cut it off a quarter of the way down. At 64 bytes an op, a page that reaches
 * the ceiling costs 2.5 MB, against the 32 MB a process is tested to hold. */
#define MAX_OPS  40000
/* 2048 rules, not 96. A real theme's stylesheet is a few thousand rules, and 96
 * kept only the first few percent of one: the page looked unstyled because it
 * was, not because the engine could not apply what it had. Each rule is a pointer
 * pair and two lengths - 2048 of them is 48 KB. */
#define MAX_RULES 2048
#define MAX_SEGS  160
#define MAX_DEPTH 48
/* Form state: what a field is called and which form submits it, so a client can
 * build the query string without parsing the page a second time. */
#define MAX_FIELDS 32
#define MAX_FORMS   8
#define FIELD_NAME_MAX  48
#define FIELD_VALUE_MAX 48
#define FORM_ACTION_MAX 160

static web_op_t *ops;
static int nops, capops;
static int page_h;
static web_measure_fn meas;

typedef struct {
  const char *sel; int sellen;
  const char *decls; int decllen;
  int spec;        /* specificity: id*100 + (class|attr|pseudo-class)*10 + tag */
  int order;       /* source position, so equal specificity keeps sheet order */
  int min_px, max_px;   /* the @media window this rule lives in, -1 when none */
} rule_t;
static rule_t rules[MAX_RULES];
static int nrules;
static web_image_fn img_fn;
static web_css_fn   css_fn;

void web_image_provider(web_image_fn fn) { img_fn = fn; }
void web_css_provider(web_css_fn fn) { css_fn = fn; }
static char *rule_buf;
/* Every stylesheet text lives as long as the page does: a rule's selector points
 * into the buffer it came from, so freeing the previous buffer when the next
 * sheet arrives leaves every earlier rule reading freed memory. That is exactly
 * what killed the browser the first time it fetched a real site's CSS. */
#define CSS_STORE 8
static char *css_store[CSS_STORE];
static int   css_store_n;

static char *dup_n(const char *s, int n);   /* defined below; the store copies text */

static char *css_keep(const char *src, int len) {
  if (!src || len <= 0 || css_store_n >= CSS_STORE) return 0;
  char *d = dup_n(src, len);
  if (d) css_store[css_store_n++] = d;
  return d;
}

typedef struct {
  int display;                 /* 0 inline, 1 block, 2 none */
  int font;                    /* 0..4 */
  uint8_t bold, underline, italic, pre;
  uint32_t color;
  uint32_t bg;
  int has_bg;
  int mt, mb, ml, mr, pt, pb, pl, pr;
  int align;                   /* 0 left, 1 center, 2 right */
  int bw_t, bw_r, bw_b, bw_l;  /* border widths, css box model */
  uint32_t bcol;
  int radius;                  /* background corner radius */
  const char *href;            /* set while inside <a href>, does inherit */
  int form;                    /* form index, -1 outside a <form>; inherits */
  int grid;                    /* >0: lay children out in this many columns */
  int min_px;                  /* a heading's own size: CSS may not shrink it */
  int floated;                 /* float: left - a gallery cell, in practice */
  int inline_block;            /* display: inline-block */
  int max_w;                   /* max-width in px, 0 = none */
  int centered;                /* margin-left/right: auto */
  int list_none;               /* list-style: none - no bullet, no indent */
  int line_mul;                /* line-height as a percentage, 0 = the face's own */
  int flex_row;                /* display: flex over inline-only content */
} st_t;

#define OMI_CELL_LAYOUT 0   /* see the note at the cell placement, below */

/* ------------------------------------------------------------- form state */

typedef struct {
  char name[FIELD_NAME_MAX];
  char value[FIELD_VALUE_MAX];
  int  form;                   /* -1 when the field is outside any <form> */
  int  w, h;                   /* the box the layout gave it */
  int  hidden;                 /* submitted, never drawn */
} wfield_t;

typedef struct {
  char action[FORM_ACTION_MAX];
  char method[8];
} wform_t;

static wfield_t fields[MAX_FIELDS];
static int nfields;
static wform_t forms[MAX_FORMS];
static int nforms;

static void copy_trunc(char *dst, int cap, const char *src) {
  int i = 0;
  if (!dst || cap <= 0) return;
  if (src) for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
  dst[i] = 0;
}

static int form_add(const char *action, const char *method) {
  if (nforms >= MAX_FORMS) return -1;
  copy_trunc(forms[nforms].action, FORM_ACTION_MAX, action);
  copy_trunc(forms[nforms].method, 8, method && *method ? method : "get");
  return nforms++;
}

static int field_add(const char *name, const char *value, int form, int w, int h, int hidden) {
  if (nfields >= MAX_FIELDS) return -1;
  wfield_t *f = &fields[nfields];
  copy_trunc(f->name, FIELD_NAME_MAX, name);
  copy_trunc(f->value, FIELD_VALUE_MAX, value);
  f->form = form;
  f->w = w; f->h = h;
  f->hidden = hidden;
  return nfields++;
}

/* --------------------------------------------------------------- emission */

static void op_push(int kind, int x, int y, int w, int h, uint32_t col,
                    const char *text, int font, int bold, int underline) {
  if (nops >= MAX_OPS) return;
  if (nops >= capops) {
    int want = capops ? capops * 2 : 256;
    if (want > MAX_OPS) want = MAX_OPS;
    web_op_t *n = krealloc(ops, (size_t)want * sizeof(web_op_t));
    if (!n) return;
    ops = n;
    capops = want;
  }
  web_op_t *o = &ops[nops++];
  o->kind = (uint8_t)kind;
  o->font = (uint8_t)font;
  o->bold = (uint8_t)bold;
  o->underline = (uint8_t)underline;
  o->radius = 0;
  o->href = 0;
  o->x = x; o->y = y; o->w = w; o->h = h;
  o->color = col;
  o->text = text;
}

static char *dup_n(const char *s, int n) {
  if (n <= 0) return 0;
  char *d = kmalloc((size_t)n + 1);
  if (!d) return 0;
  for (int i = 0; i < n; i++) d[i] = s[i];
  d[n] = 0;
  return d;
}

/* ------------------------------------------------------------- text metrics */

/* Font ids and the sizes they really are *here*.
 *
 * The renderer in this process has one face: an 8x16 bitmap. A five-size face
 * table was carried over from the compositor's own type, where the sizes were
 * real, and every heading came out the same size as body text - the size field
 * was decoration. So a font id now means an integer scale of that bitmap, and the
 * metrics below are the bitmap's, not a wish: 1x is 8x16 in an 18px line, 2x is
 * 16x32 in a 34px line. The measure function the client supplies multiplies by the
 * same scale, which is what keeps wrapping honest. */
static int face_scale[5] = {1, 2, 1, 1, 2};   /* body, display(h1/h2), mono, alt, medium(h3/h4) */
static int face_asc[5]  = {12, 24, 12, 12, 24};
static int face_desc[5] = {4, 8, 4, 4, 8};

/* When the client has real outlines it also has real metrics, and the line boxes
 * below have to be its numbers rather than the bitmap's - otherwise a 26px heading
 * is laid out in an 18px line and the next block lands on its descenders. */
static int (*face_line_h_fn)(int font);
static int (*face_ascent_fn)(int font);

void web_font_metrics(int (*line_h)(int font), int (*ascent)(int font)) {
  face_line_h_fn = line_h;
  face_ascent_fn = ascent;
}

int web_font_ascent(int font) {
  if (font < 0) font = 0;
  if (font > 4) font = 4;
  if (face_ascent_fn) {
    int a = face_ascent_fn(font);
    if (a > 0) return a;
  }
  return face_asc[font];
}

int web_font_scale(int font) {
  if (font < 0) font = 0;
  if (font > 4) font = 4;
  return face_scale[font];
}

int web_line_height(int font) {
  if (font < 0) font = 0;
  if (font > 4) font = 4;
  if (face_line_h_fn) {
    int h = face_line_h_fn(font);
    if (h > 0) return h;
  }
  return face_asc[font] + face_desc[font] + 2;
}

static int tw(int font, const char *s) {
  if (!s || !*s) return 0;
  return meas ? meas(font, s) : 0;
}

/* ------------------------------------------------------------------- CSS */

static int ci_eq(const char *a, const char *b, int n) {
  for (int i = 0; i < n; i++) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb) return 0;
  }
  return 1;
}

static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }

/* The page's own paper and ink. A document that says nothing about colours is
 * read as black on white, which is what every browser does and what makes a page
 * legible; the window's dark palette belongs to the window, not to the text of
 * somebody else's document. Deriving the ink from the paper (rather than
 * assuming the paper) is what stops a dark-themed page rendering black on
 * black. */
static uint32_t doc_bg = 0x00FFFFFF;
static uint32_t doc_fg = 0x001A1B1D;
static uint32_t doc_link = 0x001F4E9C;
static int doc_lum;

static int color_lum(uint32_t c) {
  int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  return (r * 299 + g * 587 + b * 114) / 1000;
}

static uint32_t named_color(const char *v, int n) {
  struct { const char *n; uint32_t c; } tab[] = {
    {"black",0x1A1B1D}, {"white",0xFFFFFF}, {"silver",0xC0C0C0}, {"gray",0x808080},
    {"grey",0x808080}, {"red",0xB4231F}, {"maroon",0x7A1F1C}, {"orange",0xD9761F},
    {"yellow",0xE8C43A}, {"green",0x2E6B33}, {"olive",0x6B6B24}, {"teal",0x1F6B6B},
    {"blue",0x1F4E9C}, {"navy",0x16224A}, {"purple",0x6B2E6B}, {"fuchsia",0xB02BA0},
    {"transparent",0xFFFFFFFF},
  };
  for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++) {
    int k = 0;
    while (tab[i].n[k]) k++;
    if (k == n && ci_eq(v, tab[i].n, n)) return tab[i].c;
  }
  return 0xFFFFFFFF; /* sentinel: not a colour */
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static uint32_t parse_color(const char *v, int n) {
  while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == ';')) n--;
  if (n <= 0) return 0xFFFFFFFF;
  if (v[0] == '#') {
    int h[6], d = 0;
    for (int i = 1; i < n && d < 6; i++) { int x = hexv(v[i]); if (x < 0) return 0xFFFFFFFF; h[d++] = x; }
    if (d == 3) return (uint32_t)((h[0] * 17) << 16 | (h[1] * 17) << 8 | (h[2] * 17));
    if (d == 6) return (uint32_t)(h[0] << 20 | h[1] << 16 | h[2] << 12 | h[3] << 8 | h[4] << 4 | h[5]);
    return 0xFFFFFFFF;
  }
  if (n > 4 && ci_eq(v, "rgb(", 4)) {
    int px[3] = {0, 0, 0}, k = 0;
    for (int i = 4; i < n && k < 3; i++) {
      if (v[i] >= '0' && v[i] <= '9') px[k] = px[k] * 10 + (v[i] - '0');
      else if (v[i] == ',' || v[i] == ')' || v[i] == ' ') if (px[k] || v[i] == ',') k++;
    }
    for (int i = 0; i < 3; i++) if (px[i] > 255) px[i] = 255;
    return (uint32_t)(px[0] << 16 | px[1] << 8 | px[2]);
  }
  return named_color(v, n);
}

static int parse_px(const char *v, int n) {
  int neg = 0, val = 0;
  const char *p = v;
  while (n > 0 && (*p == ' ' || *p == ':')) { p++; n--; }
  if (n > 0 && (*p == '-' || *p == '+')) { neg = (*p == '-'); p++; n--; }
  while (n > 0 && *p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; n--; }
  if (n > 0 && *p == '.') { p++; n--; while (n > 0 && *p >= '0' && *p <= '9') { p++; n--; } }
  return neg ? -val : val;
}

/* Font sizes with their units honoured. parse_px() reads the digits and drops the
 * unit, which is right for a margin in px and wrong for every font-size on the
 * web: `font-size: 100%` parsed as 100px, so a page that styles its body with a
 * percentage rendered all of its text on the display face. Percentages resolve
 * against the 16px default, em/rem the same way (there is no cascade to inherit
 * from), and points convert at 4/3. */
static int parse_font_px(const char *v, int n) {
  int i = 0;
  while (i < n && (v[i] == ' ' || v[i] == ':')) i++;
  int neg = 0;
  if (i < n && (v[i] == '-' || v[i] == '+')) { neg = (v[i] == '-'); i++; }
  long ip = 0;
  while (i < n && v[i] >= '0' && v[i] <= '9') { ip = ip * 10 + (v[i] - '0'); i++; }
  long frac = 0; int fd = 0;
  if (i < n && v[i] == '.') {
    i++;
    while (i < n && v[i] >= '0' && v[i] <= '9' && fd < 2) { frac = frac * 10 + (v[i] - '0'); fd++; i++; }
  }
  /* the value in hundredths, so 1.5em does not become 1em */
  long h = ip * 100 + (fd == 0 ? 0 : fd == 1 ? frac * 10 : frac);
  const char *u = v + i;
  int un = n - i;
  long px;
  if (un >= 1 && u[0] == '%')      px = (h * 16 + 5000) / 10000;
  else if (un >= 2 && u[1] == 'm') px = (h * 16 + 50) / 100;     /* em, rem */
  else if (un >= 2 && u[1] == 't') px = (h * 4 + 50) / 300;      /* pt */
  else                             px = (h + 50) / 100;          /* px, unitless */
  if (px <= 0) return 0;
  return (int)(neg ? -px : px);
}

/* The pixel size behind each face, so `em` can be resolved. */
static const int font_px_tab[5] = { 14, 26, 13, 12, 17 };
static int font_px_of(int font) { return (font >= 0 && font < 5) ? font_px_tab[font] : 16; }

/* A real CSS length. parse_px() drops the unit, which turns `padding: 0 1em` into
 * one pixel and `max-width: 60rem` into sixty - the reason a styled page rendered
 * as a cramped column with no container. em resolves against the element's own
 * font size, rem against the 16px root, % against the containing box, pt at 4/3. */
static int parse_len(const char *v, int n, int font_px, int box_px) {
  int i = 0;
  while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ':')) i++;
  if (i >= n) return 0;
  int neg = 0;
  if (v[i] == '-' || v[i] == '+') { neg = (v[i] == '-'); i++; }
  if (i < n && (v[i] < '0' || v[i] > '9') && v[i] != '.') {
    /* keywords that carry a length */
    if (n - i >= 4 && ci_eq(v + i, "auto", 4)) return 0;
    if (n - i >= 6 && ci_eq(v + i, "normal", 6)) return 0;
    return 0;
  }
  long ip = 0;
  while (i < n && v[i] >= '0' && v[i] <= '9') { ip = ip * 10 + (v[i] - '0'); i++; }
  int frac = 0, fdig = 0;
  if (i < n && v[i] == '.') { i++; while (i < n && v[i] >= '0' && v[i] <= '9') { frac = frac * 10 + (v[i] - '0'); fdig++; i++; } }
  long hundredths = ip * 100 + (fdig ? (frac * 100) / (fdig == 1 ? 10 : fdig == 2 ? 100 : 1000) : 0);
  long px;
  if (i < n && v[i] == '%')            px = box_px > 0 ? (hundredths * box_px) / 10000 : 0;
  else if (i + 2 < n && (v[i]|0x20) == 'r' && (v[i+1]|0x20) == 'e' && (v[i+2]|0x20) == 'm')
                                       px = (hundredths * 16) / 100;
  else if (i < n && (v[i]|0x20) == 'e' && i + 1 < n && (v[i+1]|0x20) == 'm')
                                       px = (hundredths * (font_px > 0 ? font_px : 16)) / 100;
  else if (i < n && (v[i]|0x20) == 'p' && i + 1 < n && (v[i+1]|0x20) == 't')
                                       px = (hundredths * 4) / 300;
  else if (i < n && ((v[i]|0x20) == 'e' || (v[i]|0x20) == 'c') && i + 1 < n && ((v[i+1]|0x20) == 'x' || (v[i+1]|0x20) == 'h'))
                                       px = (hundredths * (font_px > 0 ? font_px : 16)) / 200;
  else                                 px = (hundredths + 50) / 100;
  if (px > 4000) px = 4000;
  return (int)(neg ? -px : px);
}

/* css 1-4 value shorthand: all / v h / t h b / t r b l */
static int parse_box_lens(const char *v, int n, int *vals, int max, int font_px, int box_px) {
  int cnt = 0, i = 0;
  while (i < n && cnt < max) {
    while (i < n && (v[i] == ' ' || v[i] == '\t')) i++;
    if (i >= n) break;
    int s = i;
    while (i < n && v[i] != ' ' && v[i] != '\t') i++;
    vals[cnt++] = parse_len(v + s, i - s, font_px, box_px);
  }
  return cnt;
}

/* css 1-4 value shorthand: all / v h / t h b / t r b l */
static int parse_box_values(const char *v, int n, int *vals, int max) {
  int cnt = 0, i = 0;
  while (i < n && cnt < max) {
    while (i < n && (v[i] == ' ' || v[i] == '\t')) i++;
    if (i >= n) break;
    int s = i;
    while (i < n && v[i] != ' ' && v[i] != '\t') i++;
    vals[cnt++] = parse_px(v + s, i - s);
  }
  return cnt;
}

static int parse_border_width(const char *v, int n) {
  while (n > 0 && *v == ' ') { v++; n--; }
  if (n <= 0) return 0;
  if (n >= 4 && ci_eq(v, "none", 4)) return 0;
  if ((n >= 4 && ci_eq(v, "thin", 4))) return 1;
  if ((n >= 6 && ci_eq(v, "medium", 6))) return 2;
  int px = parse_px(v, n);
  if (px < 0) px = 0;
  if (px > 8) px = 8;
  return px;
}

/* `border: 1px solid #ddd` / `border-left: 3px solid red` / `border: none` */
static void apply_border(st_t *st, const char *v, int vn, int which) {
  int w = parse_border_width(v, vn);
  uint32_t col = 0xFFFFFFFF;
  for (int i = 0; i + 1 < vn; i++)
    if (v[i] == '#' || (v[i] != ' ' && v[i + 1] != ' ' && i == 0)) {
      uint32_t c2 = parse_color(v + i, vn - i);
      if (c2 != 0xFFFFFFFF) { col = c2; break; }
    }
  if (col != 0xFFFFFFFF) st->bcol = col;
  if (st->bcol == 0) st->bcol = 0xE3DED4;
  if (which == 0 || which == 1) st->bw_t = w;
  if (which == 0 || which == 2) st->bw_r = w;
  if (which == 0 || which == 3) st->bw_b = w;
  if (which == 0 || which == 4) st->bw_l = w;
}

/* A selector this engine understands well enough to trust a hide on. */
static int sel_is_simple(const char *s, int n) {
  if (!s || n <= 0) return 0;
  for (int i = 0; i < n; i++) {
    char c = s[i];
    if (c == ' ' || c == '>' || c == '+' || c == '~' || c == ',' || c == ':' ||
        c == '[' || c == '*' || c == '(') return 0;
  }
  return 1;
}
static int expand_vars(const char *v, int n, char *out, int cap);   /* below */
static int decl_ctx_simple;   /* set per rule, before its declarations run */
static int viewport_w = 800;
/* the width @media queries are answered against */

static void apply_decls(st_t *st, const char *d, int len) {
  int i = 0;
  while (i < len) {
    int s = i;
    while (i < len && d[i] != ';') i++;
    int e = i;
    if (i < len) i++;
    while (s < e && (d[s] == ' ' || d[s] == '\n' || d[s] == '\t')) s++;
    while (e > s && (d[e - 1] == ' ' || d[e - 1] == '\n' || d[e - 1] == '\t')) e--;
    if (s >= e) continue;
    int c = s;
    while (c < e && d[c] != ':') c++;
    if (c >= e) continue;
    const char *p = d + s;
    int pn = c - s;
    while (pn > 0 && p[pn - 1] == ' ') pn--;
    const char *v = d + c + 1;
    int vn = e - c - 1;
    while (vn > 0 && (*v == ' ')) { v++; vn--; }
    /* var(--x) is how every themed sheet writes its colours and sizes; expand
     * before parsing or the value is the literal text "var(--x)". */
    char vbuf[112];
    if (vn > 4) {
      for (int k = 0; k + 3 < vn; k++)
        if ((v[k]|0x20) == 'v' && (v[k+1]|0x20) == 'a' && (v[k+2]|0x20) == 'r' && v[k+3] == '(') {
          int got = expand_vars(v, vn, vbuf, (int)sizeof vbuf);
          if (got > 0) { v = vbuf; vn = got; }
          break;
        }
    }
    if (ci_eq(p, "display", pn)) {
      /* `display:none` is the one declaration whose false positive is fatal: a
       * complex selector that our matcher roughly matches hid the whole body and
       * the page came out empty (0 ops, 6 px tall). It is applied only for a
       * selector this engine can actually reason about - one simple tag, class or
       * id - and ignored otherwise. Not hiding something is a cosmetic miss;
       * hiding everything is a blank page. */
      if (vn == 4 && ci_eq(v, "none", 4)) { if (decl_ctx_simple) st->display = 2; }
      else if (vn == 5 && ci_eq(v, "block", 5)) st->display = 1;
      else if (vn == 6 && ci_eq(v, "inline", 6)) st->display = 0;
      /* A grid or a flex row is how every gallery, card list and menu bar on the
       * web is built. Without it each child is a block on its own line, which is
       * the difference between a wall of text and a page of thumbnails. */
      else if (vn == 4 && ci_eq(v, "grid", 4)) st->grid = 3;
      else if (vn == 11 && ci_eq(v, "inline-grid", 11)) st->grid = 3;
      else if (vn == 4 && ci_eq(v, "flex", 4)) {
        st->flex_row = 1;
      }
      else if (vn == 11 && ci_eq(v, "inline-flex", 11)) st->flex_row = 1;
      else if (vn == 12 && ci_eq(v, "inline-block", 12)) st->inline_block = 1;
    } else if (ci_eq(p, "line-height", pn)) {
      /* A unitless number is a multiple of the font size; a percentage or a
       * length is absolute. Tight leading is most of what makes unstyled text
       * look like a wall, so this one is worth honouring. */
      int i2 = 0;
      while (i2 < vn && (v[i2] == ' ' || v[i2] == '\t')) i2++;
      int is_num = 1;
      for (int k = i2; k < vn; k++) if (!((v[k] >= '0' && v[k] <= '9') || v[k] == '.')) is_num = 0;
          if (is_num && i2 < vn) {
        int hundredths = 0, dig = 0, dot = 0;
        for (int k = i2; k < vn; k++) {
          if (v[k] == '.') { dot = 1; continue; }
          hundredths = hundredths * 10 + (v[k] - '0');
          if (dot) dig++;
        }
        while (dig++ < 2) hundredths *= 10;
        if (hundredths >= 80 && hundredths <= 300) st->line_mul = hundredths;
      } else {
        int px2 = parse_len(v, vn, font_px_of(st->font), 800);
        int base = font_px_of(st->font);
        if (px2 > 0 && base > 0) {
          int pct = (px2 * 100) / base;
          if (pct >= 80 && pct <= 300) st->line_mul = pct;
        }
      }
    } else if (ci_eq(p, "list-style", pn) || (pn == 15 && ci_eq(p, "list-style-type", 15))) {
      if (vn == 4 && ci_eq(v, "none", 4)) st->list_none = 1;
    } else if (ci_eq(p, "max-width", pn)) {
      int w2 = parse_len(v, vn, font_px_of(st->font), 800);
          if (w2 > 40) st->max_w = w2;
    } else if (ci_eq(p, "color", pn)) {
      uint32_t c2 = parse_color(v, vn);
      if (c2 != 0xFFFFFFFF) st->color = c2;
    } else if (ci_eq(p, "background", pn) || (pn == 17 && ci_eq(p, "background-color", 17)) ||
               (pn == 11 && ci_eq(p, "background-color", 11))) {
      uint32_t c2 = parse_color(v, vn);
      if (c2 != 0xFFFFFFFF) { st->bg = c2; st->has_bg = 1; }
    } else if (ci_eq(p, "font-weight", pn)) {
      /* Numeric weights, and the keywords. The old test was `v[0] >= '6'`, which
       * is true of the letter 'n': `font-weight: normal` set bold on every
       * element that said it, so a page that styles its body "normal" rendered
       * entirely in fake bold. */
      if (ci_eq(v, "bold", 4) || ci_eq(v, "bolder", 6)) st->bold = 1;
      else if (ci_eq(v, "normal", 6) || ci_eq(v, "lighter", 7)) st->bold = 0;
      else if (vn > 0 && v[0] >= '0' && v[0] <= '9') st->bold = parse_px(v, vn) >= 600;
      /* A heading stays bold for the same reason it stays large: a sheet that
       * sets a weight on everything must not flatten the page's structure. */
      if (st->min_px) st->bold = 1;
    } else if (ci_eq(p, "font-size", pn)) {
      int px = parse_font_px(v, vn);
      /* CSS pixels to one of the five faces. The old rule was written for a
       * 1x/2x bitmap - anything 14px and up became the display face - so a page
       * asking for 16px body text got 26px and three words a line. Now the
       * sizes are a ladder: 12 / 13 / 14 / 17 / 26. */
      if (px > 0) {
        /* A heading keeps its own size. Sheets very often set body text with a
         * relative size (`font-size: 1em` on body), and a heading that inherits
         * that stops being a heading - the page renders as one flat column of
         * 17px text, which is exactly what this page did. */
        if (st->min_px && px < st->min_px) px = st->min_px;
        st->font = px <= 12 ? 3 : px <= 14 ? 0 : px <= 19 ? 4 : 1;
      }
    } else if (ci_eq(p, "text-align", pn)) {
      if (ci_eq(v, "center", 6)) st->align = 1;
      else if (ci_eq(v, "right", 5)) st->align = 2;
      else st->align = 0;
    } else if (ci_eq(p, "text-decoration", pn)) {
      if (vn >= 9 && ci_eq(v, "underline", 9)) st->underline = 1;
    } else if (ci_eq(p, "font-style", pn)) {
      st->italic = (vn == 6 && ci_eq(v, "italic", 6));
    } else if (ci_eq(p, "margin", pn) || ci_eq(p, "margin-top", pn) || ci_eq(p, "margin-bottom", pn) ||
               ci_eq(p, "margin-left", pn) || ci_eq(p, "margin-right", pn)) {
      int vals[4];
      int nv = parse_box_lens(v, vn, vals, 4, font_px_of(st->font), 800);
      for (int k = 0; k < nv; k++) {
        if (vals[k] > 60) vals[k] = 60;
        if (vals[k] < -20) vals[k] = 0;
      }
      /* `margin: 0 auto` is the standard way a page centres its content column */
      {
        int ai = 0;
        while (ai < vn && (v[ai] == ' ' || v[ai] == '\t')) ai++;
        if (vn - ai >= 4 && ci_eq(v + ai, "auto", 4)) { st->ml = st->mr = 0; st->centered = 1; }
        else if (nv == 2) {
          int hit_auto = 0;
          for (int k = 0; k < nv; k++) {
            int sp = 0;
            const char *q = v; int qn = vn;
            while (qn > 0 && sp < k) { while (qn > 0 && *q != ' ' && *q != '\t') { q++; qn--; } while (qn > 0 && (*q == ' ' || *q == '\t')) { q++; qn--; } sp++; }
            if (qn >= 4 && ci_eq(q, "auto", 4)) hit_auto = 1;
          }
          if (hit_auto) st->centered = 1;
        }
      }
      if (pn == 6) {
        if (nv == 1) { st->mt = st->mr = st->mb = st->ml = vals[0]; }
        else if (nv == 2) { st->mt = st->mb = vals[0]; st->mr = st->ml = vals[1]; }
        else if (nv >= 3) {
          st->mt = vals[0]; st->mr = st->ml = vals[1]; st->mb = vals[2];
          if (nv == 4) st->ml = vals[3];
        }
      }
      else if (ci_eq(p + 7, "top", 3)) st->mt = nv ? vals[0] : 0;
      else if (ci_eq(p + 7, "bottom", 6)) st->mb = nv ? vals[0] : 0;
      else if (ci_eq(p + 7, "left", 4)) st->ml = nv ? vals[0] : 0;
      else st->mr = nv ? vals[0] : 0;
    } else if (ci_eq(p, "padding", pn) || ci_eq(p, "padding-top", pn) || ci_eq(p, "padding-bottom", pn) ||
               ci_eq(p, "padding-left", pn) || ci_eq(p, "padding-right", pn)) {
      int vals[4];
      int nv = parse_box_lens(v, vn, vals, 4, font_px_of(st->font), 800);
      for (int k = 0; k < nv; k++) {
        if (vals[k] < 0) vals[k] = 0;
        if (vals[k] > 60) vals[k] = 60;
      }
      if (pn == 7) {
        if (nv == 1) { st->pt = st->pr = st->pb = st->pl = vals[0]; }
        else if (nv == 2) { st->pt = st->pb = vals[0]; st->pr = st->pl = vals[1]; }
        else if (nv >= 3) {
          st->pt = vals[0]; st->pr = st->pl = vals[1]; st->pb = vals[2];
          if (nv == 4) st->pl = vals[3];
        }
      }
      else if (ci_eq(p + 8, "top", 3)) st->pt = nv ? vals[0] : 0;
      else if (ci_eq(p + 8, "bottom", 6)) st->pb = nv ? vals[0] : 0;
      else if (ci_eq(p + 8, "left", 4)) st->pl = nv ? vals[0] : 0;
      else st->pr = nv ? vals[0] : 0;
    } else if (pn == 21 && ci_eq(p, "grid-template-columns", 21)) {
      /* repeat(3, 1fr) and "1fr 1fr 1fr" are the two shapes that appear; anything
       * else keeps the default. Reading the count is what makes a 4-up gallery
       * come out 4-up instead of 3-up. */
      int n = 0;
      const char *q = v;
      if (ci_eq(q, "repeat", 6)) {
        while (*q && *q != '(') q++;
        if (*q == '(') { q++; while (*q >= '0' && *q <= '9') { n = n * 10 + (*q - '0'); q++; } }
      } else {
        for (int i = 0; i < vn; i++) {
          if (v[i] == ' ') { while (i + 1 < vn && v[i + 1] == ' ') i++; n++; }
        }
        if (vn > 0) n++;
      }
      if (n >= 1 && n <= 8) st->grid = n;
    } else if (pn == 5 && ci_eq(p, "float", 5)) {
      if (vn == 4 && ci_eq(v, "left", 4)) st->floated = 1;
      else if (vn == 4 && ci_eq(v, "none", 4)) st->floated = 0;
    } else if (ci_eq(p, "border", pn)) {
      apply_border(st, v, vn, 0);
    } else if (pn > 7 && ci_eq(p, "border-", 7)) {
      const char *side = p + 7;
      int sn = pn - 7;
      if (ci_eq(side, "top", 3)) apply_border(st, v, vn, 1);
      else if (ci_eq(side, "right", 5)) apply_border(st, v, vn, 2);
      else if (ci_eq(side, "bottom", 6)) apply_border(st, v, vn, 3);
      else if (ci_eq(side, "left", 4)) apply_border(st, v, vn, 4);
      else if (ci_eq(side, "width", 5)) { int w2 = parse_border_width(v, vn);
        st->bw_t = st->bw_r = st->bw_b = st->bw_l = w2; }
      else if (ci_eq(side, "radius", 6)) st->radius = parse_px(v, vn) > 24 ? 24 : parse_px(v, vn);
      else if (ci_eq(side, "color", 5)) { uint32_t c2 = parse_color(v, vn); if (c2 != 0xFFFFFFFF) st->bcol = c2; }
    }
  }
}

/* selector match: tag, .class, #id, or comma-separated list of those */
static int is_ws(char ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; }

/* ---- CSS custom properties ------------------------------------------------
 * Every modern sheet defines its colours and sizes once on :root and refers to
 * them with var(--name). Without a table those declarations resolve to nothing,
 * so a page that looks deliberately themed renders as browser defaults. */
#define MAX_VARS 128
typedef struct { char name[48]; char val[72]; } cssvar_t;
static cssvar_t cssvars[MAX_VARS];
static int ncssvars;

static void cssvar_set(const char *name, int nlen, const char *val, int vlen) {
  if (nlen <= 0 || nlen >= (int)sizeof cssvars[0].name) return;
  if (vlen < 0) vlen = 0;
  if (vlen >= (int)sizeof cssvars[0].val) vlen = (int)sizeof cssvars[0].val - 1;
  for (int i = 0; i < ncssvars; i++)
    if (strlen(cssvars[i].name) == (size_t)nlen && !memcmp(cssvars[i].name, name, (size_t)nlen)) {
      memcpy(cssvars[i].val, val, (size_t)vlen); cssvars[i].val[vlen] = 0; return;
    }
  if (ncssvars >= MAX_VARS) return;
  memcpy(cssvars[ncssvars].name, name, (size_t)nlen); cssvars[ncssvars].name[nlen] = 0;
  memcpy(cssvars[ncssvars].val, val, (size_t)vlen);   cssvars[ncssvars].val[vlen] = 0;
  ncssvars++;
}

static const char *cssvar_get(const char *name, int n) {
  for (int i = 0; i < ncssvars; i++)
    if ((int)strlen(cssvars[i].name) == n && !memcmp(cssvars[i].name, name, (size_t)n)) return cssvars[i].val;
  return 0;
}

/* Expand var(--x) and var(--x, fallback) into `out`. One level of nesting is
 * enough for real sheets because a custom property almost never refers to
 * another one twice; the loop below handles the common chains anyway. */
static int expand_vars(const char *v, int n, char *out, int cap) {
  int o = 0, i = 0, changed = 0;
  while (i < n && o < cap - 1) {
    if (n - i >= 4 && (v[i] == 'v' || v[i] == 'V') && (v[i+1] == 'a' || v[i+1] == 'A') &&
        (v[i+2] == 'r' || v[i+2] == 'R') && v[i+3] == '(') {
      int j = i + 4;
      while (j < n && (v[j] == ' ' || v[j] == '\t')) j++;
      int ns = j;
      while (j < n && v[j] != ',' && v[j] != ')') j++;
      int ne = j;
      while (ne > ns && (v[ne-1] == ' ' || v[ne-1] == '\t')) ne--;
      /* the fallback, if the property is unknown */
      int fs = -1, fe = -1;
      if (j < n && v[j] == ',') {
        int k = j + 1;
        int depth = 0;
        while (k < n && (depth > 0 || v[k] != ')')) {
          if (v[k] == '(') depth++;
          else if (v[k] == ')') depth--;
          k++;
        }
        fs = j + 1; fe = k; j = k;
      }
      if (j < n && v[j] == ')') j++;
      const char *val = 0; int vlen = 0;
      if (ne > ns) {
        const char *hit = cssvar_get(v + ns, ne - ns);
        if (hit) { val = hit; vlen = (int)strlen(hit); }
      }
      if (!val && fs >= 0) {
        int a = fs, b = fe;
        while (a < b && (v[a] == ' ' || v[a] == '\t')) a++;
        while (b > a && (v[b-1] == ' ' || v[b-1] == '\t')) b--;
        val = v + a; vlen = b - a;
      }
      if (val) {
        for (int k = 0; k < vlen && o < cap - 1; k++) out[o++] = val[k];
        changed = 1;
      }
      i = j;
      continue;
    }
    out[o++] = v[i++];
  }
  out[o] = 0;
  return changed ? o : 0;
}

/* ---- selectors ------------------------------------------------------------
 * Right to left, the way a browser matches: the rightmost compound is the
 * element itself, and each compound to its left has to be satisfied by an
 * ancestor, either the nearest one ('>') or any (' '). The previous matcher
 * only knew single simple selectors, so `#sidebar ul li a` - the shape of every
 * navigation list on the web - matched nothing at all. */

/* the element's class list contains exactly this name */
static int class_has(const char *cls, const char *name, int n) {
  if (!cls) return 0;
  int i = 0;
  while (cls[i]) {
    while (cls[i] && is_ws(cls[i])) i++;
    int s = i;
    while (cls[i] && !is_ws(cls[i])) i++;
    if (i - s == n && !memcmp(cls + s, name, (size_t)n)) return 1;
  }
  return 0;
}

static GumboNode *node_parent(GumboNode *n) { return n ? n->parent : 0; }

static int node_index_in_parent(GumboNode *n) {
  GumboNode *p = node_parent(n);
  if (!p || p->type != GUMBO_NODE_ELEMENT) return -1;
  GumboVector *ch = &p->v.element.children;
  for (unsigned i = 0; i < ch->length; i++) if (ch->data[i] == n) return (int)i;
  return -1;
}

/* One compound: tag / .class / #id / [attr] / :pseudo, glued together. */
static int compound_matches(const char *sel, int s, int e, GumboNode *el) {
  if (!el || el->type != GUMBO_NODE_ELEMENT) return 0;
  GumboElement *ge = &el->v.element;
  const char *cls = 0, *id = 0;
  GumboAttribute *at;
  if ((at = gumbo_get_attribute(&ge->attributes, "class"))) cls = at->value;
  if ((at = gumbo_get_attribute(&ge->attributes, "id"))) id = at->value;

  int i = s;
  /* tag or '*' */
  int ts = i;
  while (i < e && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':') i++;
  if (i > ts) {
    if (!(i - ts == 1 && sel[ts] == '*')) {
      const char *tag = gumbo_normalized_tagname(ge->tag);
      if (!tag || (int)strlen(tag) != i - ts) return 0;
      for (int k = 0; k < i - ts; k++) if (tolower((unsigned char)tag[k]) != tolower((unsigned char)sel[ts + k])) return 0;
    }
  }
  while (i < e) {
    if (sel[i] == '.') {
      int ns = ++i;
      while (i < e && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':') i++;
      if (!class_has(cls, sel + ns, i - ns)) return 0;
    } else if (sel[i] == '#') {
      int ns = ++i;
      while (i < e && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':') i++;
      if (!id || (int)strlen(id) != i - ns || memcmp(id, sel + ns, (size_t)(i - ns))) return 0;
    } else if (sel[i] == '[') {
      /* attribute selectors: accept the ones that are a plain presence test or
       * an exact value, ignore the rest rather than failing the whole rule */
      int j = i + 1;
      while (j < e && sel[j] != ']') j++;
      int ns = i + 1, ne = j;
      int eq = -1;
      for (int k = ns; k < ne; k++) if (sel[k] == '=') { eq = k; break; }
      char name[48]; int nn = (eq < 0 ? ne : eq) - ns;
      if (nn > 0 && nn < (int)sizeof name) {
        memcpy(name, sel + ns, (size_t)nn); name[nn] = 0;
        GumboAttribute *a2 = gumbo_get_attribute(&ge->attributes, name);
        if (!a2) return 0;
        if (eq >= 0) {
          int vs = eq + 1, ve = ne;
          if (vs < ve && (sel[vs] == '"' || sel[vs] == '\'')) vs++;
          if (ve > vs && (sel[ve-1] == '"' || sel[ve-1] == '\'')) ve--;
          if ((int)strlen(a2->value) != ve - vs || memcmp(a2->value, sel + vs, (size_t)(ve - vs))) return 0;
        }
      }
      i = (j < e) ? j + 1 : e;
    } else if (sel[i] == ':') {
      int ps = ++i;
      if (i < e && sel[i] == ':') i++;     /* ::before */
      while (i < e && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':') i++;
      int pn = i - ps;
      /* pseudo-elements are not this element's box, and :hover has no pointer */
      if ((pn == 6 && !strncmp(sel + ps, "before", 6)) ||
          (pn == 5 && !strncmp(sel + ps, "after", 5)) ||
          (pn == 5 && !strncmp(sel + ps, "hover", 5)) ||
          (pn == 5 && !strncmp(sel + ps, "focus", 5)) ||
          (pn == 6 && !strncmp(sel + ps, "active", 6))) return 0;
      if (pn == 10 && !strncmp(sel + ps, "first-child", 11)) { /* fallthrough below */ }
      if (pn == 11 && !strncmp(sel + ps, "first-child", 11)) {
        if (node_index_in_parent(el) != 0) return 0;
      } else if (pn == 10 && !strncmp(sel + ps, "last-child", 10)) {
        GumboNode *p = node_parent(el);
        if (!p || p->type != GUMBO_NODE_ELEMENT) return 0;
        if ((int)p->v.element.children.length - 1 != node_index_in_parent(el)) return 0;
      } else if (pn == 4 && !strncmp(sel + ps, "root", 4)) {
        if (ge->tag != GUMBO_TAG_HTML) return 0;
      }
      /* everything else (:not, :nth-child, :link ...) is ignored, which is the
       * generous direction: better a rule that applies too widely than a page
       * that loses its layout. */
    } else {
      i++;
    }
  }
  return 1;
}

static int spec_of(const char *sel, int s, int e) {
  int a = 0, b = 0, c = 0, i = s;
  while (i < e) {
    if (sel[i] == '#') { a++; i++; while (i < e && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':') i++; }
    else if (sel[i] == '.' ) { b++; i++; while (i < e && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':') i++; }
    else if (sel[i] == '[') { b++; while (i < e && sel[i] != ']') i++; i++; }
    else if (sel[i] == ':') {
      i++; if (i < e && sel[i] == ':') i++;
      int ps = i;
      while (i < e && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':') i++;
      int pn = i - ps;
      if (pn == 6 && !strncmp(sel + ps, "before", 6)) c++;
      else if (pn == 5 && !strncmp(sel + ps, "after", 5)) c++;
      else b++;
    }
    else if (is_ws(sel[i]) || sel[i] == '>' || sel[i] == '+' || sel[i] == '~') i++;
    else { c++; while (i < e && !is_ws(sel[i]) && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':') i++; }
  }
  return a * 100 + b * 10 + c;
}

/* Match a full complex selector against an element, walking up its ancestors. */
static int sel_matches(const char *sel, int len, GumboNode *el) {
  int s = 0;
  while (s < len && is_ws(sel[s])) s++;
  int e = len;
  while (e > s && is_ws(sel[e-1])) e--;
  if (s >= e) return 0;

  /* find the rightmost compound: scan back over the last combinator */
  int cs = s, ce = e, combs[32], ncomb = 0, comps[33][2], ncomp = 0;
  int i = s, last = s;
  while (i < e) {
    int depth = 0;
    while (i < e) {
      char ch = sel[i];
      if (ch == '[' || ch == '(') depth++;
      else if (ch == ']' || ch == ')') depth--;
      if (depth == 0 && (ch == '>' || ch == '+' || ch == '~')) break;
      if (depth == 0 && (ch == ',' )) break;
      i++;
    }
    if (i >= e) break;
    if (sel[i] == ',') { i++; last = i; continue; }
    /* a combinator: trim the compound that ended here */
    int a = last, b = i;
    while (a < b && is_ws(sel[a])) a++;
    while (b > a && is_ws(sel[b-1])) b--;
    if (b > a && ncomp < 32) { comps[ncomp][0] = a; comps[ncomp][1] = b; ncomp++; }
    if (ncomb < 31) combs[ncomb++] = (sel[i] == '+') ? ' ' : (sel[i] == '~') ? ' ' : sel[i];
    i++;
    while (i < e && is_ws(sel[i])) i++;
    last = i;
  }
  { int a = last, b = e;
    while (a < b && is_ws(sel[a])) a++;
    while (b > a && is_ws(sel[b-1])) b--;
    if (b > a && ncomp < 32) { comps[ncomp][0] = a; comps[ncomp][1] = b; ncomp++; } }
  if (!ncomp) return 0;

  /* rightmost first */
  GumboNode *cur = el;
  if (!compound_matches(sel, comps[ncomp-1][0], comps[ncomp-1][1], cur)) return 0;
  for (int k = ncomp - 2; k >= 0; k--) {
    char comb = combs[k];
    cur = node_parent(cur);
    if (comb == '>') {
      if (!compound_matches(sel, comps[k][0], comps[k][1], cur)) return 0;
      continue;
    }
    int found = 0;
    while (cur) {
      if (compound_matches(sel, comps[k][0], comps[k][1], cur)) { found = 1; break; }
      cur = node_parent(cur);
    }
    if (!found) return 0;
  }
  (void)cs; (void)ce;
  return 1;
}

/* ---- the sheet scanner ----------------------------------------------------
 * The old scanner looked for '{' and then the first '}', which is fine until a
 * stylesheet contains an @media block: the block's body holds whole rules, so
 * the first '}' ends a nested rule and every brace after that is off by one. On
 * iana.org - 83 @media blocks in one sheet - that lost all but a handful of
 * rules. This walks blocks by counting braces, recursing into conditional
 * groups, skipping comments and strings, and recording custom properties. */

static void css_scan_block(const char *css, int s, int e, int min_px, int max_px, int *order);

static void css_add_rule(const char *sel, int slen, const char *decls, int dlen,
                         int min_px, int max_px, int *order) {
  if (nrules >= MAX_RULES) return;
  int s = 0;
  while (s < slen && is_ws(sel[s])) s++;
  int e2 = slen;
  while (e2 > s && is_ws(sel[e2-1])) e2--;
  if (e2 <= s) return;
  rules[nrules].sel = sel + s;
  rules[nrules].sellen = e2 - s;
  rules[nrules].decls = decls;
  rules[nrules].decllen = dlen;
  rules[nrules].spec = spec_of(sel + s, 0, e2 - s);
  rules[nrules].order = (*order)++;
  rules[nrules].min_px = min_px;
  rules[nrules].max_px = max_px;
  nrules++;
}

/* `min-width: 600px` / `max-width: 800px`, in any order, alongside `screen` */
static void css_media_range(const char *q, int n, int *min_px, int *max_px) {
  for (int i = 0; i + 9 < n; i++) {
    int is_min = !strncmp(q + i, "min-width", 9);
    int is_max = !strncmp(q + i, "max-width", 9);
    if (!is_min && !is_max) continue;
    int j = i + 9;
    while (j < n && (q[j] == ' ' || q[j] == ':')) j++;
    int v = 0;
    while (j < n && q[j] >= '0' && q[j] <= '9') { v = v * 10 + (q[j] - '0'); j++; }
    if (j < n && (q[j] == 'e' || q[j] == 'r')) { /* em/rem */ v *= 16; }
    if (v > 0) { if (is_min) *min_px = v; else *max_px = v; }
  }
}

/* Skip a string, comment or url(...) so braces inside them never count. */
static int css_skip_atom(const char *css, int i, int e) {
  if (i >= e) return i;
  if (css[i] == '/' && i + 1 < e && css[i+1] == '*') {
    i += 2;
    while (i + 1 < e && !(css[i] == '*' && css[i+1] == '/')) i++;
    return (i + 1 < e) ? i + 2 : e;
  }
  if (css[i] == '"' || css[i] == '\'') {
    char q = css[i++];
    while (i < e && css[i] != q) { if (css[i] == '\\' && i + 1 < e) i++; i++; }
    return (i < e) ? i + 1 : e;
  }
  return -1;
}

static long css_steps;   /* a scan that loops must not take the browser with it */

static void css_scan_block(const char *css, int s, int e, int min_px, int max_px, int *order) {
  int i = s;
  while (i < e && nrules < MAX_RULES) {
    if (++css_steps > 4000000L) { printf("[css] step limit at %d of %d\n", i, e); return; }
    while (i < e && is_ws(css[i])) i++;
    if (i >= e) break;

    int skip = css_skip_atom(css, i, e);
    if (skip >= 0) { i = skip; continue; }

    if (css[i] == '@') {
      /* at-rule: name, prelude, then ';' or a block */
      int ns = ++i;
      while (i < e && (isalnum((unsigned char)css[i]) || css[i] == '-')) i++;
      int nlen = i - ns;
      int ps = i;
      int depth = 0;
      while (i < e) {
        int sk = css_skip_atom(css, i, e);
        if (sk >= 0) { i = sk; continue; }
        if (css[i] == '(') depth++;
        else if (css[i] == ')') depth--;
        else if (depth == 0 && (css[i] == ';' || css[i] == '{' || css[i] == '}')) break;
        i++;
      }
      if (i >= e) break;
      if (css[i] == ';') { i++; continue; }              /* @charset, @import */
      if (css[i] == '}') { i++; continue; }
      int bs = i + 1, bd = 1;
      i++;
      while (i < e && bd > 0) {
        int sk = css_skip_atom(css, i, e);
        if (sk >= 0) { i = sk; continue; }
        if (css[i] == '{') bd++;
        else if (css[i] == '}') bd--;
        i++;
      }
      int be = (bd == 0) ? i - 1 : e;

      /* apply the contents of conditional groups; skip everything else
       * (@font-face, @keyframes, @page) - those are not element styles */
      int is_media = (nlen == 5 && !strncmp(css + ns, "media", 5));
      int is_cond  = (nlen == 8 && !strncmp(css + ns, "supports", 8)) ||
                     (nlen == 5 && !strncmp(css + ns, "layer", 5));
      if (is_media || is_cond) {
        int mn = min_px, mx = max_px;
        if (is_media) {
          int print_only = 0;
          for (int k = ps; k + 5 <= bs; k++)
            if (!strncmp(css + k, "print", 5)) { print_only = 1; break; }
          if (print_only) continue;
          css_media_range(css + ps, bs - ps, &mn, &mx);
        }
        css_scan_block(css, bs, be, mn, mx, order);
      }
      continue;
    }

    /* a normal rule: selector up to '{' */
    int ss = i, depth2 = 0;
    while (i < e) {
      int sk = css_skip_atom(css, i, e);
      if (sk >= 0) { i = sk; continue; }
      if (css[i] == '(') depth2++;
      else if (css[i] == ')') depth2--;
      else if (depth2 == 0 && css[i] == '{') break;
      else if (depth2 == 0 && css[i] == '}') break;
      i++;
    }
    if (i >= e || css[i] != '{') { if (i < e) i++; continue; }
    int se = i;
    int bs = i + 1, bd = 1;
    i++;
    while (i < e && bd > 0) {
      int sk = css_skip_atom(css, i, e);
      if (sk >= 0) { i = sk; continue; }
      if (css[i] == '{') bd++;
      else if (css[i] == '}') bd--;
      i++;
    }
    int be = (bd == 0) ? i - 1 : e;
    if (be <= bs) continue;

    /* custom properties first: they are what the rest of the sheet refers to */
    {
      int j = bs;
      while (j < be) {
        while (j < be && is_ws(css[j])) j++;
        int ds = j;
        while (j < be && css[j] != ';') j++;
        int de = j;
        if (j < be) j++;
        while (ds < de && is_ws(css[ds])) ds++;
        if (de - ds > 3 && css[ds] == '-' && css[ds+1] == '-') {
          int k = ds;
          while (k < de && css[k] != ':') k++;
          if (k < de) {
            int ns2 = ds, ne2 = k;
            while (ne2 > ns2 && is_ws(css[ne2-1])) ne2--;
            int vs = k + 1, ve = de;
            while (vs < ve && is_ws(css[vs])) vs++;
            while (ve > vs && is_ws(css[ve-1])) ve--;
            cssvar_set(css + ns2, ne2 - ns2, css + vs, ve - vs);
          }
        }
      }
    }

    /* one rule per selector-list entry, so specificity is per-selector */
    int a = ss;
    while (a < se) {
      int b = a;
      int depth3 = 0;
      while (b < se) {
        int sk = css_skip_atom(css, b, se);
        if (sk >= 0) { b = sk; continue; }
        if (css[b] == '[' || css[b] == '(') depth3++;
        else if (css[b] == ']' || css[b] == ')') depth3--;
        else if (depth3 == 0 && css[b] == ',') break;
        b++;
      }
      css_add_rule(css + a, b - a, css + bs, be - bs, min_px, max_px, order);
      a = (b < se) ? b + 1 : se;
    }
  }
}

int web_rule_count(void) { return nrules; }
int web_var_count(void) { return ncssvars; }

static void css_scan(const char *css, int len) {
  static int scan_order;
  css_scan_block(css, 0, len, -1, -1, &scan_order);
}

/* --------------------------------------------------------------- defaults */

static void style_for(GumboTag tag, st_t *st) {
  st->font = 0;
  switch (tag) {
    case GUMBO_TAG_H1: st->font = 1; st->bold = 1; st->mt = 18; st->mb = 8; st->min_px = 26; break;
    case GUMBO_TAG_H2: st->font = 1; st->bold = 1; st->mt = 16; st->mb = 7; st->min_px = 26; break;
    case GUMBO_TAG_H3: st->font = 4; st->bold = 1; st->mt = 13; st->mb = 5; st->min_px = 17; break;
    case GUMBO_TAG_H4: case GUMBO_TAG_H5: case GUMBO_TAG_H6:
      st->font = 0; st->bold = 1; st->mt = 8; st->mb = 4; break;
    case GUMBO_TAG_P: st->mt = 0; st->mb = 12; break;
    case GUMBO_TAG_UL: case GUMBO_TAG_OL: st->mt = 0; st->mb = 12; st->pl = 22; break;
    case GUMBO_TAG_LI: st->mb = 4; break;
    case GUMBO_TAG_BLOCKQUOTE:
      st->ml = 16; st->pl = 12; st->mb = 12;
      st->bw_l = 3; st->bcol = 0xE3DED4;
      break;
    case GUMBO_TAG_HR: st->mt = 12; st->mb = 12; break;
    case GUMBO_TAG_PRE:
      st->pre = 1; st->font = 2; st->mb = 12;
      st->bg = 0xF1EDE4; st->has_bg = 1; st->radius = 6;
      st->pl = 10; st->pr = 10; st->pt = 8; st->pb = 8;
      break;
    case GUMBO_TAG_CODE: case GUMBO_TAG_KBD: case GUMBO_TAG_SAMP: st->font = 2; break;
    case GUMBO_TAG_TABLE: st->bw_t = st->bw_r = st->bw_b = st->bw_l = 1;
      st->bcol = 0xE3DED4; st->mb = 12; break;
    case GUMBO_TAG_CENTER: st->align = 1; break;
    case GUMBO_TAG_B: case GUMBO_TAG_STRONG: st->bold = 1; break;
    case GUMBO_TAG_I: case GUMBO_TAG_EM: st->italic = 1; break;
    case GUMBO_TAG_A: st->underline = 1; st->color = doc_lum < 128 ? 0x006FB4F0 : 0x001F4E9C; break;
    case GUMBO_TAG_SMALL: st->font = 2; break;
    case GUMBO_TAG_TD: case GUMBO_TAG_TH: st->pr = 8; st->pb = 4; break;
    case GUMBO_TAG_TITLE: case GUMBO_TAG_STYLE: case GUMBO_TAG_SCRIPT:
    case GUMBO_TAG_HEAD: case GUMBO_TAG_NOSCRIPT: case GUMBO_TAG_IFRAME:
      st->display = 2; break;
    default: break;
  }
}

static void style_resolve(GumboNode *el, st_t *out, const st_t *inherited) {
  GumboElement *e = &el->v.element;
  const char *tag = gumbo_normalized_tagname(e->tag);
  char tbuf[24];
  if (!tag || !*tag) {
    /* unknown tags only carry a name in the original text */
    int n = 0;
    if (e->original_tag.length > 2) {
      for (unsigned i = 1; i < e->original_tag.length && n < 20; i++) {
        char c = e->original_tag.data[i];
        if (c == '>' || c == ' ' || c == '/') break;
        tbuf[n++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
      }
    }
    tbuf[n] = 0;
    tag = tbuf;
  }

  *out = *inherited;
  /* text properties inherit, box properties do not */
  out->display = 0;                        /* is_block_tag() still decides */
  out->mt = out->mb = out->ml = out->mr = 0;
  out->pt = out->pb = out->pl = out->pr = 0;
  out->bw_t = out->bw_r = out->bw_b = out->bw_l = 0;
  out->radius = 0;
  /* Reset with the rest. Left set, a heading's size floor followed the heading
   * into every descendant and every sibling after it - the whole page came out
   * 26px and bold. */
  out->min_px = 0;
  out->has_bg = 0; out->bg = 0;
  out->bcol = 0xE3DED4;
  style_for(e->tag, out);
  /* inherited-only properties keep the parent's value */
  if (out->color == 0 && inherited->color) out->color = inherited->color;

  const char *cls = 0, *id = 0;
  GumboAttribute *a;
  if ((a = gumbo_get_attribute(&e->attributes, "class"))) cls = a->value;
  if ((a = gumbo_get_attribute(&e->attributes, "id"))) id = a->value;

  /* Cascade: every rule that matches, in increasing specificity, then source
   * order among equals. Applying in sheet order alone let a generic `a {}` run
   * after `#sidebar a {}` and win, which is not how CSS works. */
  int hit[160], nhit = 0;
  for (int i = 0; i < nrules && nhit < 160; i++) {
    if (rules[i].min_px > 0 && viewport_w < rules[i].min_px) continue;
    if (rules[i].max_px > 0 && viewport_w > rules[i].max_px) continue;
    if (sel_matches(rules[i].sel, rules[i].sellen, el)) hit[nhit++] = i;
  }
  for (int a = 1; a < nhit; a++) {          /* insertion sort: tiny, stable */
    int k = hit[a], b = a - 1;
    while (b >= 0 && (rules[hit[b]].spec > rules[k].spec ||
                     (rules[hit[b]].spec == rules[k].spec && rules[hit[b]].order > rules[k].order))) {
      hit[b + 1] = hit[b]; b--;
    }
    hit[b + 1] = k;
  }
  for (int a = 0; a < nhit; a++) {
    decl_ctx_simple = sel_is_simple(rules[hit[a]].sel, rules[hit[a]].sellen);
    apply_decls(out, rules[hit[a]].decls, rules[hit[a]].decllen);
  }
  decl_ctx_simple = 0;

  if ((a = gumbo_get_attribute(&e->attributes, "style")))
    apply_decls(out, a->value, (int)strlen(a->value));

  if (out->font < 0) out->font = 0;
  if (out->font > 4) out->font = 4;
}

/* ------------------------------------------------------------ line buffer */

typedef struct { char *t; int font; uint32_t color; uint8_t bold, underline; int w; const char *href; } seg_t;
static seg_t segs[MAX_SEGS];
static int line_mul_pct, line_mul_font;   /* the line-height in force, if any */
static int nsegs, seg_w, seg_maxh;
static int line_x, line_avail, line_align;
static int cur_y;

static void line_flush(void) {
  if (!nsegs) return;
  int x = line_x;
  if (line_align == 1) x += (line_avail - seg_w) / 2;
  else if (line_align == 2) x += line_avail - seg_w;
  if (x < line_x) x = line_x;
  int lh = seg_maxh > 0 ? seg_maxh : 15;
  if (line_mul_pct > 0) {
    int base = font_px_of(line_mul_font);
    int want = (base * line_mul_pct) / 100;
    if (want > lh) lh = want;
  }
  for (int i = 0; i < nsegs; i++) {
    seg_t *s = &segs[i];
    if (s->t && s->t[0]) {
      int opi = nops;
      op_push(1, x, cur_y, s->w, lh, s->color, s->t, s->font, s->bold, s->underline);
      if (s->href) ops[opi].href = dup_n(s->href, (int)strlen(s->href));
      if (s->underline) op_push(2, x, cur_y + lh - 3, s->w, 1, s->color, 0, s->font, 0, 0);
    }
    x += s->w;
  }
  cur_y += lh;
  nsegs = 0; seg_w = 0; seg_maxh = 0;
}

/* Put one segment on the line. `w` is its width, already measured: the caller
 * may have measured a string it still owns. */
static void seg_put(const char *t, int len, const st_t *st, int w) {
  if (nsegs >= MAX_SEGS) line_flush();
  char *copy = dup_n(t, len);
  if (!copy) return;
  segs[nsegs].t = copy;
  segs[nsegs].font = st->font;
  segs[nsegs].color = st->color ? st->color : doc_fg;
  segs[nsegs].bold = (uint8_t)st->bold;
  segs[nsegs].underline = (uint8_t)st->underline;
  segs[nsegs].w = w;
  segs[nsegs].href = st->href;
  nsegs++;
  seg_w += w;
  int lh = web_line_height(st->font);
  if (lh > seg_maxh) seg_maxh = lh;
}

static void line_add(const char *word, int len, const st_t *st, int space_before) {
  if (nsegs >= MAX_SEGS) line_flush();
  /* The separating space belongs in front of the word it precedes: carrying it
   * as extra width on the *previous* box drops the gap between the first and
   * second word of every run. */
  int pad = (space_before && nsegs) ? 1 : 0;
  char *copy = dup_n(word, len + pad);
  if (!copy) return;
  if (pad) { copy[0] = ' '; memcpy(copy + 1, word, (size_t)len); copy[len + 1] = 0; }
  int w = tw(st->font, copy);
  if (nsegs && seg_w + w > line_avail) {
    line_flush();
    /* a leading space on a fresh line would indent it */
    free(copy);
    pad = 0;
    copy = dup_n(word, len);
    if (!copy) return;
    w = tw(st->font, copy);
  }
  /* One word wider than the line - a URL, a long path, a hash - would be drawn
   * past the right edge and clipped by the window, losing its end. Break it at
   * the last character that fits and carry the rest to the next line. */
  if (line_avail > 0 && w > line_avail) {
    char *rest = copy;
    int rem = (int)strlen(copy);
    while (rem > 0) {
      int fit = rem;
      while (fit > 1) {
        char save = rest[fit];
        rest[fit] = 0;
        int ok = tw(st->font, rest) <= line_avail;
        rest[fit] = save;
        if (ok) break;
        fit--;
      }
      char save = rest[fit];
      rest[fit] = 0;
      int ww = tw(st->font, rest);
      seg_put(rest, fit, st, ww);
      rest[fit] = save;
      rest += fit;
      rem -= fit;
      if (rem > 0) line_flush();
    }
    free(copy);
    return;
  }
  seg_put(copy, (int)strlen(copy), st, w);
  free(copy);
}

/* ------------------------------------------------------------------ layout */

static void layout_node(GumboNode *node, st_t st, int x, int avail, int depth);

static int is_block_tag(GumboTag t) {
  switch (t) {
    case GUMBO_TAG_DIV: case GUMBO_TAG_P: case GUMBO_TAG_H1: case GUMBO_TAG_H2:
    case GUMBO_TAG_H3: case GUMBO_TAG_H4: case GUMBO_TAG_H5: case GUMBO_TAG_H6:
    case GUMBO_TAG_UL: case GUMBO_TAG_OL: case GUMBO_TAG_LI: case GUMBO_TAG_BLOCKQUOTE:
    case GUMBO_TAG_PRE: case GUMBO_TAG_HR: case GUMBO_TAG_TABLE: case GUMBO_TAG_TR:
    case GUMBO_TAG_TD: case GUMBO_TAG_TH: case GUMBO_TAG_TBODY: case GUMBO_TAG_THEAD:
    case GUMBO_TAG_FORM: case GUMBO_TAG_SECTION: case GUMBO_TAG_ARTICLE:
    case GUMBO_TAG_HEADER: case GUMBO_TAG_FOOTER: case GUMBO_TAG_NAV: case GUMBO_TAG_ASIDE:
    case GUMBO_TAG_MAIN: case GUMBO_TAG_FIGURE: case GUMBO_TAG_FIGCAPTION:
    case GUMBO_TAG_ADDRESS: case GUMBO_TAG_CENTER: case GUMBO_TAG_DL: case GUMBO_TAG_DT:
    case GUMBO_TAG_DD: case GUMBO_TAG_BODY: case GUMBO_TAG_HTML: case GUMBO_TAG_BR:
    case GUMBO_TAG_TITLE: case GUMBO_TAG_STYLE: case GUMBO_TAG_SCRIPT: case GUMBO_TAG_HEAD:
      return 1;
    default: return 0;
  }
}

/* collapse runs of whitespace, drop control bytes and anything non-ASCII */
/* --------------------------------------------------------- folding to ASCII
 * The font is 8x16 ASCII (glyphs.h, generated from kernel/gui/font.c), and the
 * old cleaner dropped every byte above 126. Real pages are full of exactly those
 * bytes: "IETF\xAE" lost its mark, "Z\xFCrich" lost the umlaut, and a page served
 * as Latin-1 lost whole words. Fold what the font cannot draw into the nearest
 * thing it can, where text becomes printable (clean_text), so the widths the
 * wrapper measures are the widths the rasteriser draws. Replace this with real
 * glyphs when the font grows. */
typedef struct { unsigned short cp; const char *asc; } fold_t;
static const fold_t fold_tbl[] = {
  {0x00A0, " "},
  {0x00A1, "!"},
  {0x00A2, "c"},
  {0x00A3, "GBP"},
  {0x00A4, "$"},
  {0x00A5, "JPY"},
  {0x00A6, "|"},
  {0x00A7, "S"},
  {0x00A8, "\""},
  {0x00A9, "(c)"},
  {0x00AA, "a"},
  {0x00AB, "\""},
  {0x00AC, "!"},
  {0x00AD, "-"},
  {0x00AE, "(r)"},
  {0x00AF, "-"},
  {0x00B0, "o"},
  {0x00B1, "+/-"},
  {0x00B2, "2"},
  {0x00B3, "3"},
  {0x00B4, "'"},
  {0x00B5, "u"},
  {0x00B6, "P"},
  {0x00B7, "*"},
  {0x00B8, ","},
  {0x00B9, "1"},
  {0x00BA, "o"},
  {0x00BB, "\""},
  {0x00BC, "1/4"},
  {0x00BD, "1/2"},
  {0x00BE, "3/4"},
  {0x00BF, "?"},
  {0x00C0, "A"},
  {0x00C1, "A"},
  {0x00C2, "A"},
  {0x00C3, "A"},
  {0x00C4, "A"},
  {0x00C5, "A"},
  {0x00C6, "AE"},
  {0x00C7, "C"},
  {0x00C8, "E"},
  {0x00C9, "E"},
  {0x00CA, "E"},
  {0x00CB, "E"},
  {0x00CC, "I"},
  {0x00CD, "I"},
  {0x00CE, "I"},
  {0x00CF, "I"},
  {0x00D0, "D"},
  {0x00D1, "N"},
  {0x00D2, "O"},
  {0x00D3, "O"},
  {0x00D4, "O"},
  {0x00D5, "O"},
  {0x00D6, "O"},
  {0x00D7, "x"},
  {0x00D8, "O"},
  {0x00D9, "U"},
  {0x00DA, "U"},
  {0x00DB, "U"},
  {0x00DC, "U"},
  {0x00DD, "Y"},
  {0x00DE, "TH"},
  {0x00DF, "ss"},
  {0x00E0, "a"},
  {0x00E1, "a"},
  {0x00E2, "a"},
  {0x00E3, "a"},
  {0x00E4, "a"},
  {0x00E5, "a"},
  {0x00E6, "ae"},
  {0x00E7, "c"},
  {0x00E8, "e"},
  {0x00E9, "e"},
  {0x00EA, "e"},
  {0x00EB, "e"},
  {0x00EC, "i"},
  {0x00ED, "i"},
  {0x00EE, "i"},
  {0x00EF, "i"},
  {0x00F0, "d"},
  {0x00F1, "n"},
  {0x00F2, "o"},
  {0x00F3, "o"},
  {0x00F4, "o"},
  {0x00F5, "o"},
  {0x00F6, "o"},
  {0x00F7, "/"},
  {0x00F8, "o"},
  {0x00F9, "u"},
  {0x00FA, "u"},
  {0x00FB, "u"},
  {0x00FC, "u"},
  {0x00FD, "y"},
  {0x00FE, "th"},
  {0x00FF, "y"},
  {0x0100, "A"},
  {0x0101, "a"},
  {0x0102, "A"},
  {0x0103, "a"},
  {0x0104, "A"},
  {0x0105, "a"},
  {0x0106, "C"},
  {0x0107, "c"},
  {0x0108, "C"},
  {0x0109, "c"},
  {0x010A, "C"},
  {0x010B, "c"},
  {0x010C, "C"},
  {0x010D, "c"},
  {0x010E, "D"},
  {0x010F, "d"},
  {0x0110, "D"},
  {0x0111, "d"},
  {0x0112, "E"},
  {0x0113, "e"},
  {0x0114, "E"},
  {0x0115, "e"},
  {0x0116, "E"},
  {0x0117, "e"},
  {0x0118, "E"},
  {0x0119, "e"},
  {0x011A, "E"},
  {0x011B, "e"},
  {0x011C, "G"},
  {0x011D, "g"},
  {0x011E, "G"},
  {0x011F, "g"},
  {0x0120, "G"},
  {0x0121, "g"},
  {0x0122, "G"},
  {0x0123, "g"},
  {0x0124, "H"},
  {0x0125, "h"},
  {0x0126, "H"},
  {0x0127, "h"},
  {0x0128, "I"},
  {0x0129, "i"},
  {0x012A, "I"},
  {0x012B, "i"},
  {0x012C, "I"},
  {0x012D, "i"},
  {0x012E, "I"},
  {0x012F, "i"},
  {0x0130, "I"},
  {0x0131, "i"},
  {0x0132, "I"},
  {0x0133, "J"},
  {0x0134, "J"},
  {0x0135, "j"},
  {0x0136, "J"},
  {0x0137, "j"},
  {0x0138, "k"},
  {0x0139, "L"},
  {0x013A, "l"},
  {0x013B, "L"},
  {0x013C, "l"},
  {0x013D, "L"},
  {0x013E, "l"},
  {0x013F, "L"},
  {0x0140, "l"},
  {0x0141, "L"},
  {0x0142, "l"},
  {0x0143, "N"},
  {0x0144, "n"},
  {0x0145, "N"},
  {0x0146, "n"},
  {0x0147, "N"},
  {0x0148, "n"},
  {0x0149, "n"},
  {0x014A, "N"},
  {0x014B, "n"},
  {0x014C, "N"},
  {0x014D, "n"},
  {0x014E, "N"},
  {0x014F, "n"},
  {0x0150, "N"},
  {0x0151, "n"},
  {0x0152, "O"},
  {0x0153, "E"},
  {0x0154, "R"},
  {0x0155, "r"},
  {0x0156, "R"},
  {0x0157, "r"},
  {0x0158, "R"},
  {0x0159, "r"},
  {0x015A, "S"},
  {0x015B, "s"},
  {0x015C, "S"},
  {0x015D, "s"},
  {0x015E, "S"},
  {0x015F, "s"},
  {0x0160, "S"},
  {0x0161, "s"},
  {0x0162, "T"},
  {0x0163, "t"},
  {0x0164, "T"},
  {0x0165, "t"},
  {0x0166, "T"},
  {0x0167, "t"},
  {0x0168, "U"},
  {0x0169, "u"},
  {0x016A, "U"},
  {0x016B, "u"},
  {0x016C, "U"},
  {0x016D, "u"},
  {0x016E, "U"},
  {0x016F, "u"},
  {0x0170, "U"},
  {0x0171, "u"},
  {0x0172, "U"},
  {0x0173, "u"},
  {0x0174, "W"},
  {0x0175, "w"},
  {0x0176, "W"},
  {0x0177, "w"},
  {0x0178, "W"},
  {0x0179, "w"},
  {0x017A, "W"},
  {0x017B, "w"},
  {0x017F, "s"},
  {0x017D, "Z"},
  {0x017E, "z"},
  {0x2018, "'"},
  {0x2019, "'"},
  {0x201A, "'"},
  {0x201B, "'"},
  {0x201C, "\""},
  {0x201D, "\""},
  {0x201E, "\""},
  {0x201F, "\""},
  {0x2010, "-"},
  {0x2011, "-"},
  {0x2012, "-"},
  {0x2013, "-"},
  {0x2014, "-"},
  {0x2015, "-"},
  {0x2022, "*"},
  {0x2026, "..."},
  {0x2032, "'"},
  {0x2033, "\""},
  {0x2039, "<"},
  {0x203A, ">"},
  {0x2044, "/"},
  {0x20AC, "EUR"},
  {0x2116, "No"},
  {0x2122, "(tm)"},
  {0x2190, "<"},
  {0x2192, ">"},
  {0x2191, "^"},
  {0x2193, "v"},
  {0x2212, "-"},
  {0x2215, "/"},
  {0x2260, "!="},
  {0x2264, "<="},
  {0x2265, ">="},
  {0x2009, " "},
  {0x200A, " "},
  {0x202F, " "},
  {0x2007, " "},
  {0x200B, ""},
  {0x200C, ""},
  {0x200D, ""},
  {0xFEFF, ""},
};
static const char *fold_cp(int cp) {
  for (unsigned i = 0; i < sizeof fold_tbl / sizeof fold_tbl[0]; i++)
    if (fold_tbl[i].cp == cp) return fold_tbl[i].asc;
  return "?";                      /* an unknown letter must not vanish */
}
/* One UTF-8 sequence, or a bare Latin-1 byte. Returns the bytes consumed. */
static int utf8_next(const char *s, int len, int *cp) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) { *cp = c; return 1; }
  int need = 0, v = 0;
  if ((c & 0xE0) == 0xC0) { need = 1; v = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { need = 2; v = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { need = 3; v = c & 0x07; }
  else { *cp = c; return 1; }                  /* stray Latin-1 high byte */
  if (need + 1 > len) { *cp = c; return 1; }
  for (int i = 1; i <= need; i++) {
    unsigned char cc = (unsigned char)s[i];
    if ((cc & 0xC0) != 0x80) { *cp = c; return 1; }
    v = (v << 6) | (cc & 0x3F);
  }
  *cp = v;
  return need + 1;
}

static int clean_text(const char *s, int len, char *out, int cap) {
  int o = 0, sp = 0;
  for (int i = 0; i < len; ) {
    int cp = 0;
    int used = utf8_next(s + i, len - i, &cp);
    i += used;
    if (cp == '\n' || cp == '\t' || cp == '\r' || cp == ' ') { sp = 1; continue; }
    if (cp < 32) continue;
    const char *rep = cp < 127 ? 0 : fold_cp(cp);
    if (cp < 127) {
      if (sp && o) out[o++] = ' ';
      sp = 0;
      out[o++] = (char)cp;
      continue;
    }
    if (!*rep) continue;                       /* zero-width: nothing to draw */
    if (rep[0] == ' ' && rep[1] == 0) { sp = 1; continue; }
    if (sp && o) out[o++] = ' ';
    sp = 0;
    for (const char *r = rep; *r && o < cap - 1; r++) out[o++] = *r;
  }
  out[o] = 0;
  return o;
}

static int text_lead_space(const char *s, int len) {
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') return 1;
    return 0;
  }
  return 0;
}

static int text_trail_space(const char *s, int len) {
  for (int i = len - 1; i >= 0; i--) {
    unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') return 1;
    return 0;
  }
  return 0;
}

/* Did the whitespace between the last text and the one about to be drawn really
 * exist in the source? Runs are positioned by x, so a space is only ever wanted
 * where the page had one: "<b>mathematics</b>, a" must not become "mathematics ,".
 * clean_text eats the whitespace, so it is remembered here instead. */
static int pending_space;

static void layout_text(GumboText *t, st_t *st) {
  /* The line height in force while this run's lines are flushed. Text nodes are
   * leaves, so a save/restore here cannot leak into a sibling block. */
  int save_mul = line_mul_pct, save_font = line_mul_font;
  line_mul_pct = st->line_mul;
  line_mul_font = st->font;
  static char buf[4096];
  int raw = (int)t->original_text.length;
  int n = clean_text(t->text, raw, buf, (int)sizeof buf);
  int lead = text_lead_space(t->text, raw);
  int trail = text_trail_space(t->text, raw);
  if (!n) { pending_space = pending_space || lead; line_mul_pct = save_mul; line_mul_font = save_font; return; }
  int first = 1;
  int i = 0;
  while (i < n) {
    while (i < n && buf[i] == ' ') i++;
    if (i >= n) break;
    int s = i;
    while (i < n && buf[i] != ' ') i++;
    int sep = first ? (pending_space || lead) : 1;
    line_add(buf + s, i - s, st, sep);
    if (sep) pending_space = 0;
    first = 0;
  }
  pending_space = trail;
  line_mul_pct = save_mul;
  line_mul_font = save_font;
}


/* ------------------------------------------------------------------ tables
 * The walker is one vertical pass, so a table gets a pass of its own: collect
 * the grid, size the columns from the text, then paint row by row. Cell
 * content is flattened to text; boxes nested inside a cell come later. */
#define TBL_ROWS  48
#define TBL_COLS  10
#define TBL_CELLW 144
#define TBL_PAD   6

typedef struct { char txt[TBL_CELLW]; uint8_t head; } tcell_t;
static tcell_t tbl[TBL_ROWS][TBL_COLS];

/* A forced line break inside a cell's flattened text. Cells are one string, and
 * the things that filled them were block elements: a footer cell holding a <ul>
 * of links used to come out as "Registry.INT Registry.ARPA Registry" because the
 * block boundaries - the only thing separating them - were dropped. */
#define CELL_BREAK '\x01'

static void tbl_sep(char *out, int cap, int *pn) {
  if (*pn > 0 && *pn < cap - 1 && out[*pn - 1] != CELL_BREAK) out[(*pn)++] = CELL_BREAK;
}

static void tbl_text(GumboNode *n, char *out, int cap, int *pn, int depth) {
  if (!n || depth > 12) return;
  if (n->type == GUMBO_NODE_TEXT) {
    const char *s = n->v.text.text;
    int len = (int)strlen(s);
    for (int i = 0; i < len; ) {
      int cp = 0;
      i += utf8_next(s + i, len - i, &cp);
      if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
        if (*pn > 0 && out[*pn - 1] != ' ' && *pn < cap - 1) out[(*pn)++] = ' ';
        continue;
      }
      /* cells fold the same way body text does, or a table keeps the mojibake
       * the paragraph beside it lost */
      const char *rep = cp < 127 && cp >= 32 ? 0 : fold_cp(cp);
      if (!rep) {
        if (*pn < cap - 1) out[(*pn)++] = (char)cp;
      } else {
        for (const char *r = rep; *r && *pn < cap - 1; r++)
          out[(*pn)++] = *r == ' ' ? ' ' : *r;
      }
    }
    return;
  }
  if (n->type != GUMBO_NODE_ELEMENT) return;
  int block = is_block_tag(n->v.element.tag);
  if (block) tbl_sep(out, cap, pn);
  GumboVector *k = &n->v.element.children;
  for (unsigned i = 0; i < k->length; i++) tbl_text(k->data[i], out, cap, pn, depth + 1);
  if (block) tbl_sep(out, cap, pn);
}

static void tbl_rows(GumboNode *n, GumboNode **rows, int *nr, int depth) {
  if (!n || depth > 3 || *nr >= TBL_ROWS) return;
  GumboVector *k = &n->v.element.children;
  for (unsigned i = 0; i < k->length && *nr < TBL_ROWS; i++) {
    GumboNode *c = k->data[i];
    if (c->type != GUMBO_NODE_ELEMENT) continue;
    GumboTag t = c->v.element.tag;
    if (t == GUMBO_TAG_TR) rows[(*nr)++] = c;
    else if (t == GUMBO_TAG_THEAD || t == GUMBO_TAG_TBODY || t == GUMBO_TAG_TFOOT)
      tbl_rows(c, rows, nr, depth + 1);
  }
}

/* how many lines this text needs in `width` */
/* How many lines a single word needs when it is wider than the column. A cell
 * holds text, not words: "documentation" in a 66px column has to break, or it is
 * drawn past the table's edge and clipped by the window. */
static int word_chunks(int font, const char *word, int width) {
  int len = (int)strlen(word), n = 1, i = 0;
  while (i < len) {
    char tmp[64];
    int fit = 0;
    while (i + fit < len) {
      int take = fit + 1;
      memcpy(tmp, word + i, (size_t)take);
      tmp[take] = 0;
      if (tw(font, tmp) > width) break;
      fit = take;
    }
    if (!fit) fit = 1;                     /* narrower than one glyph: take it */
    i += fit;
    if (i < len) n++;
  }
  return n;
}

static int wrap_count(const char *s, int font, int width) {
  if (width < 8) width = 8;
  int lines = 1, w = 0, sp = tw(font, " ");
  while (*s) {
    while (*s == ' ') s++;
    while (*s == CELL_BREAK) { s++; if (w || lines) { lines++; w = 0; } }
    if (!*s) break;
    char word[64]; int len = 0;
    while (*s && *s != ' ' && *s != CELL_BREAK && len < 63) word[len++] = *s++;
    word[len] = 0;
    int ww = tw(font, word);
    if (w && w + sp + ww > width) { lines++; w = ww; }
    else w += (w ? sp : 0) + ww;
    if (ww > width) {
      /* it breaks over several lines by itself, and the next word starts fresh */
      lines += word_chunks(font, word, width) - 1;
      w = width;
    }
  }
  return lines;
}

static void draw_wrapped(const char *s, int font, uint32_t color, int x, int y,
                         int width, int lh, int bold) {
  if (width < 8) width = 8;
  int cx = x, cy = y, sp = tw(font, " ");
  while (*s) {
    while (*s == ' ') s++;
    while (*s == CELL_BREAK) { s++; if (cx != x) { cx = x; cy += lh; } }
    if (!*s) break;
    char word[64]; int len = 0;
    while (*s && *s != ' ' && *s != CELL_BREAK && len < 63) word[len++] = *s++;
    word[len] = 0;
    int ww = tw(font, word);
    if (cx > x && cx + ww > x + width) { cx = x; cy += lh; }
    if (ww > width && cx == x) {
      /* the word is wider than the column on its own: break it, same as
       * word_chunks counts it */
      int i = 0;
      while (i < len) {
        char tmp[64];
        int fit = 0;
        while (i + fit < len) {
          int take = fit + 1;
          memcpy(tmp, word + i, (size_t)take);
          tmp[take] = 0;
          if (tw(font, tmp) > width) break;
          fit = take;
        }
        if (!fit) fit = 1;
        memcpy(tmp, word + i, (size_t)fit);
        tmp[fit] = 0;
        char *part = dup_n(tmp, fit);
        if (part) op_push(1, cx, cy, tw(font, tmp), lh, color, part, font, bold, 0);
        i += fit;
        if (i < len) cy += lh;
      }
      cx = x;
      cy += lh;                          /* the next word starts a fresh line */
      continue;
    }
    /* op_push keeps the pointer, so every op needs its own copy */
    char *keep = dup_n(word, len);
    if (keep) op_push(1, cx, cy, ww, lh, color, keep, font, bold, 0);
    cx += ww + sp;
  }
}

static void layout_table(GumboNode *table, st_t *st, int x, int avail, int depth) {
  (void)depth;
  GumboNode *rows[TBL_ROWS];
  int nr = 0;
  tbl_rows(table, rows, &nr, 0);
  if (!nr) return;

  int cols = 0;
  for (int r = 0; r < nr; r++) {
    GumboVector *k = &rows[r]->v.element.children;
    int c = 0;
    for (unsigned i = 0; i < k->length; i++) {
      GumboNode *e = k->data[i];
      if (e->type != GUMBO_NODE_ELEMENT) continue;
      GumboTag t = e->v.element.tag;
      if (t != GUMBO_TAG_TD && t != GUMBO_TAG_TH) continue;
      if (c >= TBL_COLS) break;
      int pn = 0;
      tbl[r][c].txt[0] = 0;
      tbl_text(e, tbl[r][c].txt, TBL_CELLW, &pn, 0);
      while (pn > 0 && tbl[r][c].txt[pn - 1] == ' ') pn--;
      tbl[r][c].txt[pn] = 0;
      tbl[r][c].head = (t == GUMBO_TAG_TH) ? 1 : 0;
      c++;
    }
    if (c > cols) cols = c;
  }
  if (!cols) return;

  int font = st->font;
  int lh = web_line_height(font);
  int natw[TBL_COLS], wcol[TBL_COLS];
  int total = 0;
  for (int c = 0; c < cols; c++) {
    int m = 40;
    for (int r = 0; r < nr; r++) {
      int cw = tw(font, tbl[r][c].txt) + 2 * TBL_PAD;
      if (cw > m) m = cw;
    }
    natw[c] = m;
    total += m;
  }
  int availw = avail > 8 ? avail - 2 : avail;
  if (total > availw) {
    int acc = 0;
    for (int c = 0; c < cols; c++) {
      wcol[c] = natw[c] * availw / total;
      if (wcol[c] < 44) wcol[c] = 44;
      acc += wcol[c];
    }
    /* Take the deficit from the widest column, a step at a time. The 44px floor
     * keeps a column readable, but when several columns are floored it puts the
     * table back over the page - and a table that runs off the edge loses the
     * end of its right column, which is worse than a narrow column. So the floor
     * gives way: 44 at first, then anything down to 8. */
    while (acc > availw) {
      int big = 0;
      for (int c = 1; c < cols; c++) if (wcol[c] > wcol[big]) big = c;
      if (wcol[big] <= 8) break;
      int d = acc - availw; if (d > 8) d = 8;
      int take = wcol[big] - 8; if (take > d) take = d;
      if (take < 1) break;
      wcol[big] -= take; acc -= take;
    }
  } else {
    for (int c = 0; c < cols; c++) wcol[c] = natw[c];
  }

  if (getenv("OMI_TBL_DEBUG"))
    for (int c = 0; c < cols; c++) fprintf(stderr, "  col%d nat=%d w=%d\n", c, natw[c], wcol[c]);
  /* Rules and the header band are the width of the table, not of the page: a
   * narrow table used to get a band running out past its last column. */
  int tblw = 0;
  for (int c = 0; c < cols; c++) tblw += wcol[c];
  line_flush();
  cur_y += st->mt;
  op_push(0, x, cur_y, tblw, 1, 0xD8D3C9, 0, font, 0, 0);
  cur_y += 1;

  for (int r = 0; r < nr; r++) {
    int lines = 1;
    for (int c = 0; c < cols; c++) {
      int l = wrap_count(tbl[r][c].txt, font, wcol[c] - 2 * TBL_PAD);
      if (l > lines) lines = l;
    }
    int rowh = lines * lh + 2 * TBL_PAD;
    if (tbl[r][0].head) op_push(0, x, cur_y, tblw, rowh, 0xF4F0E7, 0, font, 0, 0);
    int cx = x;
    for (int c = 0; c < cols; c++) {
      draw_wrapped(tbl[r][c].txt, font, st->color ? st->color : doc_fg,
                   cx + TBL_PAD, cur_y + TBL_PAD, wcol[c] - 2 * TBL_PAD, lh,
                   tbl[r][c].head);
      cx += wcol[c];
    }
    cur_y += rowh;
    op_push(0, x, cur_y, tblw, 1, 0xE3DED4, 0, font, 0, 0);
    cur_y += 1;
  }
  cur_y += st->mb > 0 ? st->mb : 10;
}

/* An <input> is a box, and the box is the whole feature on the engine side: the
 * client draws the value, owns the caret and does the submitting, because those
 * are things that change without the page changing. What the engine knows is the
 * field's name, the form it belongs to, and how wide it should be.
 *
 * It is placed on a line of its own. A real inline box would sit in the text run
 * beside its label; that needs the line buffer to carry non-text items, and a
 * form that reads well is worth more than one that shares a line. */
static void layout_input(GumboNode *node, st_t *st, int x, int avail) {
  GumboAttribute *at = gumbo_get_attribute(&node->v.element.attributes, "type");
  GumboAttribute *an = gumbo_get_attribute(&node->v.element.attributes, "name");
  GumboAttribute *av = gumbo_get_attribute(&node->v.element.attributes, "value");
  GumboAttribute *as = gumbo_get_attribute(&node->v.element.attributes, "size");
  const char *type = (at && at->value && *at->value) ? at->value : "text";
  const char *name = an ? an->value : 0;
  const char *value = av ? av->value : 0;

  if (ci_eq(type, "hidden", 6)) {          /* submitted, never drawn */
    field_add(name, value, st->form, 0, 0, 1);
    return;
  }

  if (ci_eq(type, "submit", 6) || ci_eq(type, "button", 6) ||
      ci_eq(type, "reset", 5) || ci_eq(type, "image", 5)) {
    const char *label = (value && *value) ? value : "Submit";
    int w = (int)strlen(label) * 8 + 18;
    if (w > avail - st->pl) w = avail - st->pl;
    if (w < 24) w = 24;
    line_flush();
    int bx = x + st->pl;
    if (st->align == 1) bx = x + (avail - w) / 2;
    else if (st->align == 2) bx = x + avail - w;
    if (bx < x) bx = x;
    int op = nops;
    op_push(5, bx, cur_y, w, 22, 0, dup_n(label, (int)strlen(label)), 0, 0, 0);
    if (op < nops) ops[op].iw = st->form;   /* which form this button sends */
    cur_y += 26;
    return;
  }

  int cols = 24;
  if (as && as->value) {
    cols = 0;
    for (const char *q = as->value; *q >= '0' && *q <= '9'; q++) cols = cols * 10 + (*q - '0');
  }
  if (cols < 6) cols = 6;
  if (cols > 48) cols = 48;
  int w = cols * 8 + 8;
  if (w > avail - st->pl) w = avail - st->pl;
  if (w < 40) w = 40;

  line_flush();
  /* Inside <center> (or text-align: center) the box goes where the text around it
   * goes; a field pinned to the left under a centred label reads as a bug. */
  int fx = x + st->pl;
  if (st->align == 1) fx = x + (avail - w) / 2;
  else if (st->align == 2) fx = x + avail - w;
  if (fx < x) fx = x;
  int fi = field_add(name, value, st->form, w, 20, 0);
  int op = nops;
  op_push(4, fx, cur_y, w, 20, 0, 0, 0, 0, 0);
  if (op < nops) ops[op].iw = fi;
  cur_y += 24;
}

/* ------------------------------------------------------------------- flex
 * `display: flex` over inline-only content - the nav bar, a breadcrumb trail, a
 * row of footer links - is a single line of inline boxes. Laid out as blocks the
 * menu stacks, which is the most visible way a page stops looking like itself.
 *
 * This is not a flexbox: no growth, no shrinking, no alignment. It only takes the
 * case where every item is text (however deeply wrapped in <a>/<span>) and puts
 * the items on one line, wrapped at the container's edge. Anything containing a
 * block falls back to the normal path rather than being faked. */

static int text_has_content(const char *t) {
  if (!t) return 0;
  for (; *t; t++) if (*t != ' ' && *t != '\t' && *t != '\n' && *t != '\r') return 1;
  return 0;
}

/* A flex container often holds a single block wrapper (nav > ul > li). Descend
 * through such a wrapper so the row is built from the items, not the list. */
static GumboNode *flex_unwrap(GumboNode *n, st_t st, st_t *out) {
  for (int g = 0; g < 2 && n && n->type == GUMBO_NODE_ELEMENT; g++) {
    GumboVector *kids = &n->v.element.children;
    GumboNode *only = 0;
    int nel = 0, ntext = 0;
    for (unsigned i = 0; i < kids->length; i++) {
      GumboNode *k = kids->data[i];
      if (k->type == GUMBO_NODE_ELEMENT) { nel++; only = k; }
      else if (k->type == GUMBO_NODE_TEXT && text_has_content(k->v.text.text)) ntext++;
    }
    if (nel != 1 || ntext || !only) break;
    if (!is_block_tag(only->v.element.tag)) break;
    st_t cs;
    style_resolve(only, &cs, &st);
    if (cs.display == 2) break;
    st = cs; n = only;
    continue;
  }
  *out = st;
  return n;
}

/* Pure check: can this subtree be one line? Laid out afterwards, so a failure
 * half-way through cannot leave half a row on the page. */
static int has_block_descendant(GumboNode *n, int depth) {
  if (!n || n->type != GUMBO_NODE_ELEMENT || depth > 4) return 0;
  GumboVector *kids = &n->v.element.children;
  for (unsigned i = 0; i < kids->length; i++) {
    GumboNode *k = kids->data[i];
    if (k->type != GUMBO_NODE_ELEMENT) continue;
    if (is_block_tag(k->v.element.tag)) return 1;
    if (has_block_descendant(k, depth + 1)) return 1;
  }
  return 0;
}

static int flex_ok(GumboNode *n, st_t st, int depth, int *any) {
  if (!n || n->type != GUMBO_NODE_ELEMENT || depth > 5) return 0;
  GumboVector *kids = &n->v.element.children;
  for (unsigned i = 0; i < kids->length; i++) {
    GumboNode *k = kids->data[i];
    if (k->type == GUMBO_NODE_TEXT) { if (text_has_content(k->v.text.text)) *any = 1; continue; }
    if (k->type != GUMBO_NODE_ELEMENT) continue;
    st_t cs;
    style_resolve(k, &cs, &st);
    if (cs.display == 2) continue;
    if ((is_block_tag(k->v.element.tag) || cs.display == 1) && has_block_descendant(k, 0))
      return 0;                       /* a real block inside the row: not a row */
    if (!flex_ok(k, cs, depth + 1, any)) return 0;
  }
  return 1;
}

static void flex_lay(GumboNode *n, st_t st, int depth) {
  if (!n || n->type != GUMBO_NODE_ELEMENT || depth > 5) return;
  GumboVector *kids = &n->v.element.children;
  for (unsigned i = 0; i < kids->length; i++) {
    GumboNode *k = kids->data[i];
    if (k->type == GUMBO_NODE_TEXT) { layout_text(&k->v.text, &st); continue; }
    if (k->type != GUMBO_NODE_ELEMENT) continue;
    st_t cs;
    style_resolve(k, &cs, &st);
    if (cs.display == 2) continue;
    if (nsegs > 0 || seg_w > 0) pending_space = 1;   /* a space between items */
    flex_lay(k, cs, depth + 1);
  }
}

static void layout_node(GumboNode *node, st_t st, int x, int avail, int depth) {
  if (depth > MAX_DEPTH || !node) return;
  if (st.display == 2) return;
  if (avail < 24) avail = 24;

  /* max-width, and `margin: 0 auto` centered: the content column every site
   * wraps its text in. Without it a page stretched to the window width and read
   * like a terminal dump. */
  if (st.max_w > 0 && st.max_w < avail) {
    int w = st.max_w;
    if (st.centered) x += (avail - w) / 2;
    avail = w;
    st.max_w = 0;
  }

  if (node->type == GUMBO_NODE_TEXT) { layout_text(&node->v.text, &st); return; }
  if (node->type != GUMBO_NODE_ELEMENT) return;

  GumboTag tag = node->v.element.tag;

  if (tag == GUMBO_TAG_A) {
    GumboAttribute *h = gumbo_get_attribute(&node->v.element.attributes, "href");
    if (h && h->value && *h->value) st.href = h->value;   /* lives in the gumbo doc */
  }

  if (tag == GUMBO_TAG_SCRIPT || tag == GUMBO_TAG_STYLE || tag == GUMBO_TAG_HEAD ||
      tag == GUMBO_TAG_TITLE || tag == GUMBO_TAG_NOSCRIPT)
    return;

  /* A form is a container, but it is also the thing that says where its fields
   * are posted - so it is remembered before the children are walked. */
  if (tag == GUMBO_TAG_FORM) {
    GumboAttribute *act = gumbo_get_attribute(&node->v.element.attributes, "action");
    GumboAttribute *mth = gumbo_get_attribute(&node->v.element.attributes, "method");
    st.form = form_add(act ? act->value : 0, mth ? mth->value : 0);
  }

  /* The marker is drawn by the block walker, which is the only place that can
   * see whether the list is ordered. Drawing one here as well put a square and a
   * dash on every item. */

  if (tag == GUMBO_TAG_INPUT) { layout_input(node, &st, x, avail); return; }

  if (tag == GUMBO_TAG_IMG) {
    GumboAttribute *a = gumbo_get_attribute(&node->v.element.attributes, "src");
    GumboAttribute *aw = gumbo_get_attribute(&node->v.element.attributes, "width");
    GumboAttribute *ah = gumbo_get_attribute(&node->v.element.attributes, "height");
    int w = 0, h2 = 0;
    if (aw) { for (const char *q = aw->value; *q >= '0' && *q <= '9'; q++) w = w * 10 + (*q - '0'); }
    if (ah) { for (const char *q = ah->value; *q >= '0' && *q <= '9'; q++) h2 = h2 * 10 + (*q - '0'); }

    const web_image_t *im = (a && img_fn) ? img_fn(a->value, (int)strlen(a->value)) : 0;
    if (im && im->px && im->w > 0 && im->h > 0) {
      if (!w && !h2) { w = im->w; h2 = im->h; }
      else if (w && !h2) h2 = im->h * w / im->w;
      else if (h2 && !w) w = im->w * h2 / im->h;
      if (w > line_avail) { h2 = h2 * line_avail / w; w = line_avail; }
      if (w > 0 && h2 > 0) {
        line_flush();
        int op = nops;
        op_push(3, x, cur_y, w, h2, 0, 0, 0, 0, 0);
        /* A thumbnail is nearly always wrapped in <a href>: the picture is the
         * link. Without this the biggest click target on the page did nothing. */
        if (op < nops && st.href) ops[op].href = dup_n(st.href, (int)strlen(st.href));
        ops[op].px = im->px;
        ops[op].iw = im->w;
        ops[op].ih = im->h;
        cur_y += h2 + 8;
        return;
      }
    }
    /* no image: show the alt text if the page gave us one */
    GumboAttribute *alt = gumbo_get_attribute(&node->v.element.attributes, "alt");
    if (alt && alt->value && *alt->value) {
      line_add(alt->value, (int)strlen(alt->value), &st, nsegs > 0);
      line_flush();
    }
    return;
  }

  /* <hr> is a rule, not a container */
  if (tag == GUMBO_TAG_HR) {
    line_flush();
    cur_y += st.mt;
    op_push(0, x, cur_y, avail, 1, 0xC9C4BA, 0, 0, 0, 0);
    cur_y += 1 + st.mb;
    return;
  }

  if (tag == GUMBO_TAG_TABLE) { layout_table(node, &st, x, avail, depth); return; }

  /* A grid container lays its element children out in cells, row by row. Each
   * child is laid out on its own with the cell width, and the row advances by the
   * tallest cell - which is as much of `display: grid` as a page needs to stop
   * being a column. */
  /* A gallery in the wild is a run of floated or inline-block children with
   * percentage widths - that is how this page is built, and its CSS has no grid at
   * all. Counting those children is enough to lay them out as cells instead of as
   * one column, which is the difference between a wall of text and a wall of
   * pictures. */
  int auto_cells = 0;
  if (st.grid == 0 && node->type == GUMBO_NODE_ELEMENT &&
      (is_block_tag(tag) || st.display == 1)) {
    GumboVector *kk = &node->v.element.children;
    int nf = 0;
    for (unsigned i = 0; i < kk->length; i++) {
      GumboNode *k = kk->data[i];
      if (k->type != GUMBO_NODE_ELEMENT) continue;
      st_t cs;
      style_resolve(k, &cs, &st);
      if (cs.display == 2) continue;
      if (cs.floated || cs.inline_block) nf++;
    }
    if (nf >= 3) auto_cells = nf >= 8 ? 4 : (nf >= 4 ? 3 : 2);
  }

  /* OFF, deliberately: this lays floated children out as cells, and it produced
   * rows - but it also broke three of the browser's own layout invariants (a text
   * run escaping the column, link boxes no longer hit-testing to their own URLs, a
   * padded background losing its height), because a cell is not an inline box and
   * the cursor engine has no line box to put one in. Doing this properly means an
   * inline formatting context, not a second placement pass. Kept behind a switch
   * so the work is not lost, and off so the suite stays honest. */
  if (OMI_CELL_LAYOUT && (st.grid > 0 || auto_cells > 0) && node->type == GUMBO_NODE_ELEMENT && !is_block_tag(tag) == 0) {
    GumboVector *kids = &node->v.element.children;
    int cols = st.grid > 0 ? st.grid : auto_cells;
    if (cols < 1) cols = 1;
    int gap = 6;
    int cell = (avail - gap * (cols - 1)) / cols;
    if (cell < 24) { cols = 1; cell = avail; }
    line_flush();
    cur_y += st.mt;
    int col = 0, y0 = cur_y, row_h = 0;
    for (unsigned i = 0; i < kids->length; i++) {
      GumboNode *k = kids->data[i];
      if (k->type == GUMBO_NODE_TEXT) {
        /* stray whitespace between cells is not a cell */
        if (!k->v.text.text || !k->v.text.text[0]) continue;
        continue;
      }
      if (k->type != GUMBO_NODE_ELEMENT) continue;
      st_t cs;
      style_resolve(k, &cs, &st);
      if (cs.display == 2) continue;
      cs.grid = 0;                       /* a cell is laid out as a block */
      cs.display = 1;
      int cx = x + st.pl + col * (cell + gap);
      int y_save = cur_y;
      cur_y = y0;
      layout_node(k, cs, cx, cell, depth + 1);
      int h = cur_y - y0;
      if (h > row_h) row_h = h;
      cur_y = y_save;
      if (++col >= cols) {
        col = 0;
        cur_y = y0 + row_h + gap;
        y0 = cur_y;
        row_h = 0;
      }
    }
    if (col > 0) cur_y = y0 + row_h + gap;
    cur_y += st.mb;
    return;
  }

  int block = is_block_tag(tag) || st.display == 1;
  if (!block) {
    GumboVector *kids = &node->v.element.children;
    for (unsigned i = 0; i < kids->length; i++) {
      GumboNode *k = kids->data[i];
      if (k->type == GUMBO_NODE_ELEMENT) {
        st_t cs;
        style_resolve(k, &cs, &st);
        if (cs.display == 2) continue;
        int cblock = is_block_tag(k->v.element.tag) || cs.display == 1;
        if (cblock) { line_flush(); layout_node(k, cs, x, avail, depth + 1); }
        else layout_node(k, cs, x, avail, depth + 1);
      } else if (k->type == GUMBO_NODE_TEXT) {
        layout_text(&k->v.text, &st);
      }
    }
    return;
  }

  /* ---- block box: margin -> border -> padding -> content ---- */
  line_flush();
  cur_y += st.mt;
  int bt = st.bw_t, bb = st.bw_b, bl = st.bw_l, brw = st.bw_r;
  int bx = x + st.ml;                       /* border box */
  int bww = avail - st.ml - st.mr;
  if (bww < 24) bww = 24;
  int cw = bww - bl - brw - st.pl - st.pr;
  if (cw < 24) cw = 24;
  int cx = bx + bl + st.pl;
  int top = cur_y;

  int bg_op = -1;
  if (st.has_bg) {
    bg_op = nops;
    op_push(0, bx, top, bww, 0, st.bg, 0, 0, 0, 0);
    ops[bg_op].radius = (uint8_t)(st.radius < 0 ? 0 : st.radius);
  }

  int saved_x = line_x, saved_avail = line_avail, saved_align = line_align;
  line_x = cx;
  line_avail = cw;
  line_align = st.align;
  cur_y = top + bt + st.pt;

  /* list markers */
  int ordered = 0, lnum = 0;
  if (tag == GUMBO_TAG_OL) ordered = 1;
  if (tag == GUMBO_TAG_LI) {
    /* the parent's list type decides the marker */
    GumboNode *par = node->parent;
    int num = 1;
    if (par && par->type == GUMBO_NODE_ELEMENT && par->v.element.tag == GUMBO_TAG_OL) {
      num = 1;
      for (unsigned i = 0; i < par->v.element.children.length; i++) {
        GumboNode *sib = par->v.element.children.data[i];
        if (sib == node) break;
        if (sib->type == GUMBO_NODE_ELEMENT && sib->v.element.tag == GUMBO_TAG_LI) num++;
      }
      ordered = 1;
    } else ordered = 0;
    /* An item at the very left edge has no room for a marker in front of it, and
     * drawing one at a negative x puts it off the window. Move the item in by
     * what the marker needs instead - the text keeps its own left edge, it just
     * starts a little further right. */
    if (cx < 14) {
      int push = 14 - cx;
      cx += push;
      cw -= push;
      line_x = cx;
      line_avail = cw;
    }
    st_t ms = st;
    ms.font = st.font;
    ms.color = st.color ? st.color : doc_fg;
    /* The marker sits in the list's padding. An ordered item needs its number as
     * text; an unordered one is a small filled square, because the face has no
     * bullet glyph and a square is what the rasteriser draws exactly. */
    if (ordered) {
      char mk[12];
      snprintf(mk, sizeof mk, "%d.", num);
      int mw = tw(ms.font, mk);
      op_push(1, cx - 14, cur_y, mw, web_line_height(ms.font), ms.color,
              dup_n(mk, (int)strlen(mk)), ms.font, 0, 0);
    } else if (!ms.list_none) {
      op_push(0, cx - 12, cur_y + 6, 4, 4, ms.color, 0, 0, 0, 0);
    }
  }

  int did_flex = 0;
  if (st.flex_row) {
    st_t rs;
    GumboNode *row = flex_unwrap(node, st, &rs);
    int any = 0;
    if (row && flex_ok(row, rs, 0, &any) && any) {
      pending_space = 0;
      flex_lay(row, rs, 0);
      line_flush();
      did_flex = 1;
    }
  }

  GumboVector *kids = &node->v.element.children;
  for (unsigned i = 0; i < kids->length && !did_flex; i++) {
    GumboNode *k = kids->data[i];
    if (k->type == GUMBO_NODE_TEXT) { layout_text(&k->v.text, &st); continue; }
    if (k->type != GUMBO_NODE_ELEMENT) continue;
    st_t cs;
    style_resolve(k, &cs, &st);
    if (cs.display == 2) continue;
    /* inline children inside a block stay on the line; blocks break it */
    int cblock = is_block_tag(k->v.element.tag) || cs.display == 1;
    if (cblock) {
      line_flush();
      layout_node(k, cs, cx, cw, depth + 1);
    } else {
      layout_node(k, cs, cx, cw, depth + 1);
    }
  }
  line_flush();
  cur_y += st.pb;

  line_x = saved_x; line_avail = saved_avail; line_align = saved_align;

  int box_h = cur_y - top + bb;
  if (bg_op >= 0) ops[bg_op].h = box_h;
  if (bt) op_push(0, bx, top, bww, bt, st.bcol, 0, 0, 0, 0);
  if (bb) op_push(0, bx, top + box_h - bb, bww, bb, st.bcol, 0, 0, 0, 0);
  if (bl) op_push(0, bx, top, bl, box_h, st.bcol, 0, 0, 0, 0);
  if (brw) op_push(0, bx + bww - brw, top, brw, box_h, st.bcol, 0, 0, 0, 0);
  cur_y += st.mb;
  (void)ordered; (void)lnum;

}

/* -------------------------------------------------------------------- API */

static void rules_free(void) {
  nrules = 0;
  for (int i = 0; i < css_store_n; i++) { kfree(css_store[i]); css_store[i] = 0; }
  css_store_n = 0;
  rule_buf = 0;
}

const char *web_hit_link(int x, int y) {
  for (int i = 0; i < nops; i++) {
    const web_op_t *o = &ops[i];
    /* Text runs and pictures: an <a> around an <img> makes the picture the link,
     * and on a gallery page that is most of what there is to click. */
    if ((o->kind != 1 && o->kind != 3) || !o->href) continue;
    if (x < o->x || x >= o->x + o->w) continue;
    /* The box, not the box plus slack: text boxes on consecutive lines touch,
     * and two pixels of downward grace made a click on a wrapped link's second
     * line land on the first, opening the wrong page. An empty box keeps a
     * little height so a bare anchor is still reachable. */
    if (y < o->y || y > o->y + (o->h > 0 ? o->h - 1 : 4)) continue;
    return o->href;
  }
  return 0;
}

void web_free(void) {
  if (ops) {
    for (int i = 0; i < nops; i++) {
      if (ops[i].text) kfree((void *)ops[i].text);
      ops[i].text = 0;
      if (ops[i].href) kfree((void *)ops[i].href);
      ops[i].href = 0;
    }
    kfree(ops);
    ops = 0;
  }
  nops = capops = 0;
  page_h = 0;
  for (int i = 0; i < nsegs; i++) if (segs[i].t) { kfree(segs[i].t); segs[i].t = 0; }
  nsegs = 0;
  rules_free();
}

/* <style> bodies and <title> text live in the tree; scan them before layout */
static void collect_css(GumboNode *n, int depth) {
  if (!n || depth > 8) return;
  if (n->type == GUMBO_NODE_ELEMENT) {
    if (n->v.element.tag == GUMBO_TAG_LINK && css_fn) {
      GumboAttribute *rel = gumbo_get_attribute(&n->v.element.attributes, "rel");
      GumboAttribute *href = gumbo_get_attribute(&n->v.element.attributes, "href");
      int is_sheet = 0;
      if (rel && rel->value) {
        for (const char *q = rel->value; *q; q++)
          if ((q[0] == 's' || q[0] == 'S') && ci_eq(q, "stylesheet", 10)) { is_sheet = 1; break; }
      }
      if (is_sheet && href && href->value && *href->value) {
        static char sheet[512 * 1024];   /* a theme's whole stylesheet, not a slice */
        int got = css_fn(href->value, (int)strlen(href->value), sheet, (int)sizeof sheet - 1);
        if (got > 0) {
          sheet[got < (int)sizeof sheet ? got : (int)sizeof sheet - 1] = 0;
          rule_buf = css_keep(sheet, got);
          if (rule_buf) css_scan(rule_buf, (int)strlen(rule_buf));
        }
      }
    }
    if (n->v.element.tag == GUMBO_TAG_STYLE) {
      GumboVector *kids = &n->v.element.children;
      for (unsigned i = 0; i < kids->length; i++) {
        GumboNode *k = kids->data[i];
        if (k->type == GUMBO_NODE_TEXT) {
          rule_buf = css_keep(k->v.text.text, (int)k->v.text.original_text.length);
          if (rule_buf) css_scan(rule_buf, (int)strlen(rule_buf));
          break;
        }
      }
    }
    GumboVector *kids = &n->v.element.children;
    for (unsigned i = 0; i < kids->length; i++) collect_css(kids->data[i], depth + 1);
  }
}

int web_build(const char *html, int len, int width, web_measure_fn measure) {
  web_free();
  meas = measure;
  if (!html || len <= 0 || width < 32) return -1;
  viewport_w = width;

  GumboOutput *out = gumbo_parse_with_options(&kGumboDefaultOptions, html, (size_t)len);
  if (!out) return -1;

  collect_css(out->root, 0);

  st_t base;
  memset(&base, 0, sizeof base);   /* every field, including ones added later */
  base.display = 1; base.font = 0; base.bold = 0; base.underline = 0; base.italic = 0;
  base.pre = 0; base.color = 0x1A1B1D; base.bg = 0; base.has_bg = 0;
  base.mt = base.mb = base.ml = base.mr = 0;
  base.pt = base.pb = base.pl = base.pr = 0;
  base.align = 0; base.bcol = 0xE3DED4;
  base.bw_t = base.bw_r = base.bw_b = base.bw_l = 0;
  base.href = 0;
  base.radius = 0;
  base.form = -1;

  nfields = 0;
  nforms = 0;
  line_x = 0;
  line_avail = width;
  line_align = 0;
  cur_y = 0;
  nsegs = 0; seg_w = 0; seg_maxh = 0;

  GumboNode *body = out->root;
  if (body->type == GUMBO_NODE_ELEMENT) {
    GumboVector *kids = &body->v.element.children;
    for (unsigned i = 0; i < kids->length; i++) {
      GumboNode *k = kids->data[i];
      if (k->type == GUMBO_NODE_ELEMENT && k->v.element.tag == GUMBO_TAG_HTML) { body = k; break; }
    }
    if (body == out->root) {
      for (unsigned i = 0; i < kids->length; i++) {
        GumboNode *k = kids->data[i];
        if (k->type == GUMBO_NODE_ELEMENT && k->v.element.tag == GUMBO_TAG_BODY) { body = k; break; }
      }
    }
  }

  /* the body (or html, if that is where the colour was put) decides the paper */
  {
    st_t bs;
  memset(&bs, 0, sizeof bs);   /* every field, including ones added later */
    style_resolve(body, &bs, &base);
    if (bs.has_bg) doc_bg = bs.bg & 0x00FFFFFF;
    doc_lum = color_lum(doc_bg);
    doc_fg = doc_lum < 128 ? 0x00E8EAEE : 0x001A1B1D;
    if (bs.color) doc_fg = bs.color;
  }

  GumboVector *bk = &body->v.element.children;
  for (unsigned i = 0; i < bk->length; i++) {
    GumboNode *k = bk->data[i];
    if (k->type == GUMBO_NODE_TEXT) { layout_text(&k->v.text, &base); continue; }
    if (k->type != GUMBO_NODE_ELEMENT) continue;
    st_t cs;
    style_resolve(k, &cs, &base);
    if (cs.display == 2) continue;
    layout_node(k, cs, 0, width, 1);
  }
  line_flush();

  page_h = cur_y + 6;
  gumbo_destroy_output(&kGumboDefaultOptions, out);

  return 0;
}

uint32_t web_bg(void) { return doc_bg; }
uint32_t web_fg(void) { return doc_fg; }
int web_height(void) { return page_h; }
int web_count(void) { return nops; }

/* A page whose display list hit the ceiling is *incomplete*: everything past the
 * last op was parsed and laid out and then dropped. Reporting that is the
 * difference between a page that is short and a browser that looks broken. */
int web_ops_full(void) { return nops >= MAX_OPS; }

/* ---- fields and forms, for a client that lets the reader type ---- */

int web_field_count(void) { return nfields; }
const char *web_field_name(int i) { return (i >= 0 && i < nfields) ? fields[i].name : 0; }
const char *web_field_value(int i) { return (i >= 0 && i < nfields) ? fields[i].value : 0; }
int web_field_form(int i) { return (i >= 0 && i < nfields) ? fields[i].form : -1; }
int web_field_hidden(int i) { return (i >= 0 && i < nfields) ? fields[i].hidden : 0; }
int web_form_count(void) { return nforms; }
const char *web_form_action(int f) { return (f >= 0 && f < nforms) ? forms[f].action : 0; }
const char *web_form_method(int f) { return (f >= 0 && f < nforms) ? forms[f].method : 0; }
const web_op_t *web_ops(void) { return ops; }