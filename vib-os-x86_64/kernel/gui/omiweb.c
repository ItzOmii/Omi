/*
 * Omi - HTML page rendering.
 *
 * Gumbo gives us the tree; this file measures it and emits a flat display list
 * (rectangles + text runs) that the compositor paints. Deliberately small: a
 * block/inline layout engine with the chunk of CSS that makes real pages
 * readable, no scripting, no images, no floats or flexbox.
 */

#include "types.h"
#include "kmalloc.h"
#include "string.h"
#include "omiweb.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t v);
#include "gumbo.h"

/* provided by lib/gumbo/glue.c - the parser allocates from the kernel heap */
void *malloc(size_t n);
void free(void *p);

/* ------------------------------------------------------------------ state */

#define MAX_OPS   9000
#define MAX_RULES 96
#define MAX_SEGS  160
#define MAX_DEPTH 48

static web_op_t *ops;
static int nops, capops;
static int page_h;
static web_measure_fn meas;

typedef struct { const char *sel; int sellen; const char *decls; int decllen; } rule_t;
static rule_t rules[MAX_RULES];
static int nrules;
static web_image_fn img_fn;

void web_image_provider(web_image_fn fn) { img_fn = fn; }
static char *rule_buf;

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
} st_t;

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

/* font ids: 0 = body (13px), 1 = display (14px), 2 = mono small (11px),
 * 3 = alternate 11px, 4 = medium (12px) - see web_faces() in the compositor */
static int face_asc[5]  = {13, 14, 11, 11, 12};
static int face_desc[5] = {4, 4, 3, 4, 4};

int web_line_height(int font) {
  if (font < 0) font = 0;
  if (font > 4) font = 4;
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
    if (ci_eq(p, "display", pn)) {
      if (vn == 4 && ci_eq(v, "none", 4)) st->display = 2;
      else if (vn == 5 && ci_eq(v, "block", 5)) st->display = 1;
      else if (vn == 6 && ci_eq(v, "inline", 6)) st->display = 0;
    } else if (ci_eq(p, "color", pn)) {
      uint32_t c2 = parse_color(v, vn);
      if (c2 != 0xFFFFFFFF) st->color = c2;
    } else if (ci_eq(p, "background", pn) || (pn == 17 && ci_eq(p, "background-color", 17)) ||
               (pn == 11 && ci_eq(p, "background-color", 11))) {
      uint32_t c2 = parse_color(v, vn);
      if (c2 != 0xFFFFFFFF) { st->bg = c2; st->has_bg = 1; }
    } else if (ci_eq(p, "font-weight", pn)) {
      st->bold = (ci_eq(v, "bold", 4) || (vn >= 3 && v[0] >= '6')) ? 1 : 0;
    } else if (ci_eq(p, "font-size", pn)) {
      int px = parse_px(v, vn);
      if (px > 0) st->font = px <= 11 ? 2 : px == 12 ? 4 : px <= 13 ? 0 : 1;
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
      int nv = parse_box_values(v, vn, vals, 4);
      for (int k = 0; k < nv; k++) {
        if (vals[k] > 60) vals[k] = 60;
        if (vals[k] < -20) vals[k] = 0;
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
      int nv = parse_box_values(v, vn, vals, 4);
      for (int k = 0; k < nv; k++) {
        if (vals[k] < 0) vals[k] = 0;
        if (vals[k] > 40) vals[k] = 40;
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

static int sel_matches(const char *sel, int len, const char *tag, const char *cls, const char *id) {
  int i = 0;
  while (i < len) {
    int s = i;
    while (i < len && sel[i] != ',') i++;
    int e = i;
    if (i < len) i++;
    while (s < e && is_ws(sel[s])) s++;
    while (e > s && is_ws(sel[e - 1])) e--;
    if (s >= e) continue;
    if (sel[s] == '*') return 1;
    if (sel[s] == '.' && cls) {
      int n = e - s - 1, k = 0;
      while (k < n && cls[k] && cls[k] == sel[s + 1 + k]) k++;
      if (k == n && (cls[k] == 0 || cls[k] == ' ')) return 1;
      continue;
    }
    if (sel[s] == '#' && id) {
      int n = e - s - 1, k = 0;
      while (k < n && id[k] && id[k] == sel[s + 1 + k]) k++;
      if (k == n && id[k] == 0) return 1;
      continue;
    }
    if (tag) {
      int n = e - s, k = 0;
      while (k < n && tag[k] && ci_eq(&tag[k], &sel[s + k], 1)) k++;
      if (k == n && tag[k] == 0) return 1;
    }
  }
  return 0;
}

static void css_scan(const char *css, int len) {
  int i = 0;
  while (i < len && nrules < MAX_RULES) {
    int s = i;
    while (i < len && css[i] != '{') i++;
    if (i >= len) break;
    int se = i;
    i++;
    int ds = i;
    while (i < len && css[i] != '}') i++;
    int de = i;
    if (i < len) i++;
    while (ds < de && (css[ds] == ' ' || css[ds] == '\n' || css[ds] == '\t')) ds++;
    while (de > ds && (css[de - 1] == ' ' || css[de - 1] == '\n' || css[de - 1] == '\t')) de--;
    /* drop at-rules and pseudo-classes we do not implement */
    int at = 0;
    while (s < se && (css[s] == ' ' || css[s] == '\n' || css[s] == '\r' || css[s] == '\t')) s++;
    int se2 = se;
    while (se2 > s && (css[se2 - 1] == ' ' || css[se2 - 1] == '\n' || css[se2 - 1] == '\r' || css[se2 - 1] == '\t')) se2--;
    if (s < se && css[s] == '@') at = 1;
    for (int k = s; k < se; k++) if (css[k] == ':') { at = 1; break; }
    if (at || se <= s || de <= ds) continue;
    rules[nrules].sel = css + s;
    rules[nrules].sellen = se2 - s;
    rules[nrules].decls = css + ds;
    rules[nrules].decllen = de - ds;
    nrules++;
  }
}

/* --------------------------------------------------------------- defaults */

static void style_for(GumboTag tag, st_t *st) {
  st->font = 0;
  switch (tag) {
    case GUMBO_TAG_H1: st->font = 1; st->bold = 1; st->mt = 18; st->mb = 8; break;
    case GUMBO_TAG_H2: st->font = 1; st->bold = 1; st->mt = 16; st->mb = 7; break;
    case GUMBO_TAG_H3: st->font = 4; st->bold = 1; st->mt = 13; st->mb = 5; break;
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
    case GUMBO_TAG_A: st->underline = 1; st->color = 0x1F4E9C; break;
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
  out->has_bg = 0; out->bg = 0;
  out->bcol = 0xE3DED4;
  style_for(e->tag, out);
  /* inherited-only properties keep the parent's value */
  if (out->color == 0 && inherited->color) out->color = inherited->color;

  const char *cls = 0, *id = 0;
  GumboAttribute *a;
  if ((a = gumbo_get_attribute(&e->attributes, "class"))) cls = a->value;
  if ((a = gumbo_get_attribute(&e->attributes, "id"))) id = a->value;

  for (int i = 0; i < nrules; i++)
    if (sel_matches(rules[i].sel, rules[i].sellen, tag, cls, id))
      apply_decls(out, rules[i].decls, rules[i].decllen);

  if ((a = gumbo_get_attribute(&e->attributes, "style")))
    apply_decls(out, a->value, (int)strlen(a->value));

  if (out->font < 0) out->font = 0;
  if (out->font > 4) out->font = 4;
}

/* ------------------------------------------------------------ line buffer */

typedef struct { char *t; int font; uint32_t color; uint8_t bold, underline; int w; const char *href; } seg_t;
static seg_t segs[MAX_SEGS];
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
  segs[nsegs].t = copy;
  segs[nsegs].font = st->font;
  segs[nsegs].color = st->color ? st->color : 0x1A1B1D;
  segs[nsegs].bold = (uint8_t)st->bold;
  segs[nsegs].underline = (uint8_t)st->underline;
  segs[nsegs].w = w;
  segs[nsegs].href = st->href;
  nsegs++;
  seg_w += w;
  int lh = web_line_height(st->font);
  if (lh > seg_maxh) seg_maxh = lh;
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
static int clean_text(const char *s, int len, char *out, int cap) {
  int o = 0, sp = 0;
  for (int i = 0; i < len && o < cap - 1; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\n' || c == '\t' || c == '\r' || c == ' ') { sp = 1; continue; }
    if (c < 32 || c >= 127) continue;
    if (sp && o) out[o++] = ' ';
    sp = 0;
    out[o++] = (char)c;
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

static void layout_text(GumboText *t, st_t *st) {
  static char buf[4096];
  int raw = (int)t->original_text.length;
  int n = clean_text(t->text, raw, buf, (int)sizeof buf);
  if (!n) return;
  /* whitespace at the start of this node separates it from whatever inline
   * content came before it, even though clean_text eats it */
  int lead = text_lead_space(t->text, raw);
  int first = 1;
  int i = 0;
  while (i < n) {
    while (i < n && buf[i] == ' ') i++;
    if (i >= n) break;
    int s = i;
    while (i < n && buf[i] != ' ') i++;
    line_add(buf + s, i - s, st, first ? (nsegs > 0 || lead) : 1);
    first = 0;
  }
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

static void tbl_text(GumboNode *n, char *out, int cap, int *pn, int depth) {
  if (!n || depth > 12) return;
  if (n->type == GUMBO_NODE_TEXT) {
    for (const char *s = n->v.text.text; *s; s++) {
      if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        if (*pn > 0 && out[*pn - 1] != ' ' && *pn < cap - 1) out[(*pn)++] = ' ';
      } else if (*pn < cap - 1) out[(*pn)++] = *s;
    }
    return;
  }
  if (n->type != GUMBO_NODE_ELEMENT) return;
  if (n->v.element.tag == GUMBO_TAG_BR || n->v.element.tag == GUMBO_TAG_P) {
    if (*pn > 0 && out[*pn - 1] != ' ' && *pn < cap - 1) out[(*pn)++] = ' ';
  }
  GumboVector *k = &n->v.element.children;
  for (unsigned i = 0; i < k->length; i++) tbl_text(k->data[i], out, cap, pn, depth + 1);
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
static int wrap_count(const char *s, int font, int width) {
  if (width < 8) width = 8;
  int lines = 1, w = 0, sp = tw(font, " ");
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    char word[64]; int len = 0;
    while (*s && *s != ' ' && len < 63) word[len++] = *s++;
    word[len] = 0;
    int ww = tw(font, word);
    if (w && w + sp + ww > width) { lines++; w = ww; }
    else w += (w ? sp : 0) + ww;
  }
  return lines;
}

static void draw_wrapped(const char *s, int font, uint32_t color, int x, int y,
                         int width, int lh, int bold) {
  if (width < 8) width = 8;
  int cx = x, cy = y, sp = tw(font, " ");
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    char word[64]; int len = 0;
    while (*s && *s != ' ' && len < 63) word[len++] = *s++;
    word[len] = 0;
    int ww = tw(font, word);
    if (cx > x && cx + ww > x + width) { cx = x; cy += lh; }
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
    while (acc > availw) {
      int big = 0;
      for (int c = 1; c < cols; c++) if (wcol[c] > wcol[big]) big = c;
      int d = acc - availw; if (d > 8) d = 8;
      if (wcol[big] - d < 44) break;
      wcol[big] -= d; acc -= d;
    }
  } else {
    for (int c = 0; c < cols; c++) wcol[c] = natw[c];
  }

  line_flush();
  cur_y += st->mt;
  op_push(0, x, cur_y, avail, 1, 0xD8D3C9, 0, font, 0, 0);
  cur_y += 1;

  for (int r = 0; r < nr; r++) {
    int lines = 1;
    for (int c = 0; c < cols; c++) {
      int l = wrap_count(tbl[r][c].txt, font, wcol[c] - 2 * TBL_PAD);
      if (l > lines) lines = l;
    }
    int rowh = lines * lh + 2 * TBL_PAD;
    if (tbl[r][0].head) op_push(0, x, cur_y, avail, rowh, 0xF4F0E7, 0, font, 0, 0);
    int cx = x;
    for (int c = 0; c < cols; c++) {
      draw_wrapped(tbl[r][c].txt, font, st->color ? st->color : 0x1A1B1D,
                   cx + TBL_PAD, cur_y + TBL_PAD, wcol[c] - 2 * TBL_PAD, lh,
                   tbl[r][c].head);
      cx += wcol[c];
    }
    cur_y += rowh;
    op_push(0, x, cur_y, avail, 1, 0xE3DED4, 0, font, 0, 0);
    cur_y += 1;
  }
  cur_y += st->mb > 0 ? st->mb : 10;
}

static void layout_node(GumboNode *node, st_t st, int x, int avail, int depth) {
  if (depth > MAX_DEPTH || !node) return;
  if (st.display == 2) return;
  if (avail < 24) avail = 24;

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
    char mk[12];
    if (ordered) { snprintf(mk, sizeof mk, "%d.", num); }
    else { mk[0] = '-'; mk[1] = 0; }
    st_t ms = st;
    ms.font = st.font;
    ms.color = st.color ? st.color : 0x1A1B1D;
    /* marker sits in the list's padding, one text row above the item's first line */
    int mw = tw(ms.font, mk);
    op_push(1, cx - 14 + (ordered ? 0 : 4), cur_y, mw, web_line_height(ms.font), ms.color, dup_n(mk, (int)strlen(mk)), ms.font, 0, 0);
    cw -= 0;
  }

  GumboVector *kids = &node->v.element.children;
  for (unsigned i = 0; i < kids->length; i++) {
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
  if (rule_buf) { kfree(rule_buf); rule_buf = 0; }
}

const char *web_hit_link(int x, int y) {
  for (int i = 0; i < nops; i++) {
    const web_op_t *o = &ops[i];
    if (o->kind != 1 || !o->href) continue;
    if (x < o->x || x >= o->x + o->w) continue;
    if (y < o->y || y > o->y + (o->h > 0 ? o->h : 4) + 2) continue;
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
    if (n->v.element.tag == GUMBO_TAG_STYLE) {
      GumboVector *kids = &n->v.element.children;
      for (unsigned i = 0; i < kids->length; i++) {
        GumboNode *k = kids->data[i];
        if (k->type == GUMBO_NODE_TEXT) {
          rule_buf = dup_n(k->v.text.text, (int)k->v.text.original_text.length);
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

  GumboOutput *out = gumbo_parse_with_options(&kGumboDefaultOptions, html, (size_t)len);
  if (!out) return -1;

  collect_css(out->root, 0);

  st_t base;
  base.display = 1; base.font = 0; base.bold = 0; base.underline = 0; base.italic = 0;
  base.pre = 0; base.color = 0x1A1B1D; base.bg = 0; base.has_bg = 0;
  base.mt = base.mb = base.ml = base.mr = 0;
  base.pt = base.pb = base.pl = base.pr = 0;
  base.align = 0; base.bcol = 0xE3DED4;
  base.bw_t = base.bw_r = base.bw_b = base.bw_l = 0;
  base.href = 0;
  base.radius = 0;

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

int web_height(void) { return page_h; }
int web_count(void) { return nops; }
const web_op_t *web_ops(void) { return ops; }