/* ominet - the browser's first ring-3 half: fetch, parse, lay out, paint.
 *
 * Omi Net renders inside the kernel today (kernel/gui/omiweb.c, ~1000 lines that
 * write into the framebuffer). That is a parser and a layout engine sharing an
 * address space with the scheduler and the heap, which is the arrangement a
 * browser cannot have: a malformed page becomes a kernel memory bug.
 *
 * This program takes the whole pipeline into ring 3. It dials TLS itself
 * (fetch.c), parses the HTML with Gumbo - the same parser the kernel uses, so
 * this is a move and not a rewrite - lays out block boxes, and paints the text
 * through the shared rasteriser (omitext.h). The kernel's part is a window and a
 * surface: it never learns what a character is, or what a tag is.
 *
 * Everything it claims is asserted in-process and then, for the pixels, checked
 * from outside at the coordinates it prints.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "gumbo.h"
#include "glyphs.h"
#include "fetch.h"

#define SYS_YIELD        3
#define SYS_WIN_OPEN     6
#define SYS_WIN_CLOSE    10
#define SYS_WIN_WAIT     12
#define SYS_WIN_GEOM     14
#define SYS_SURF_CREATE  35
#define SYS_SURF_PRESENT 36

#define WIN_W  440
#define WIN_H  360
#define CW     (WIN_W - 2)     /* client area inside the compositor's chrome */
#define CH     (WIN_H - 32)

#define C_BG     0x001E2024u
#define C_PANEL  0x0026292Fu
#define C_TEXT   0x00E6E6E6u
#define C_DIM    0x009AA0A8u
#define C_ACCENT 0x00C4622Du
#define C_MARK   0x0000FF00u

#define LH_HEAD  22
#define LH_BODY  18
#define MARGIN   8
#define MAX_PAGE 32768

/* A page with enough text to be worth scrolling. 10.9 KB of HTML, about 3200
 * characters of it text, so the document is roughly three viewports tall. */
#define HOST "www.iana.org"
#define PATH "/domains/reserved"

static unsigned *pix;
static GumboNode *doc_root;
static int checks, fails;

#include "omitext.h"

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) { fails++; printf("[net] FAIL %s (got %ld want %ld)\n", what, got, want); }
  else printf("[net] ok   %s\n", what);
}

/* ---- the DOM ---- */

static int count_elements(GumboNode *n) {
  if (n->type != GUMBO_NODE_ELEMENT) return 0;
  int c = 1;
  GumboVector *k = &n->v.element.children;
  for (unsigned i = 0; i < k->length; i++)
    c += count_elements((GumboNode *)k->data[i]);
  return c;
}

static int count_tag(GumboNode *n, GumboTag t) {
  if (n->type != GUMBO_NODE_ELEMENT) return 0;
  int c = (n->v.element.tag == t) ? 1 : 0;
  GumboVector *k = &n->v.element.children;
  for (unsigned i = 0; i < k->length; i++)
    c += count_tag((GumboNode *)k->data[i], t);
  return c;
}

/* Append text with every whitespace run collapsed to one space. HTML says a newline
 * in the source is a space on the screen, and a layout that draws the newline is a
 * layout that draws a box glyph the font does not have. */
static void norm_append(const char *s, size_t len, char *out, size_t cap, size_t *used) {
  for (size_t i = 0; i < len && *used + 2 < cap; i++) {
    unsigned char c = (unsigned char)s[i];
    /* The font is 8x16 ASCII. An entity like &ldquo; decodes to multi-byte UTF-8,
     * and drawing those bytes would index past the glyph table and paint noise. */
    if (c >= 0x80) continue;
    if (isspace(c)) {
      if (*used > 0 && out[*used - 1] != ' ') out[(*used)++] = ' ';
    } else {
      out[(*used)++] = (char)c;
    }
  }
  out[*used] = 0;
}

static void collect_text(GumboNode *n, char *out, size_t cap, size_t *used) {
  if (n->type == GUMBO_NODE_TEXT || n->type == GUMBO_NODE_WHITESPACE) {
    norm_append(n->v.text.text, strlen(n->v.text.text), out, cap, used);
    return;
  }
  if (n->type != GUMBO_NODE_ELEMENT) return;
  GumboTag t = n->v.element.tag;
  if (t == GUMBO_TAG_SCRIPT || t == GUMBO_TAG_STYLE || t == GUMBO_TAG_HEAD) return;
  /* A <br> is a container with no children, so without this the words on either
   * side of it fuse: IANA's nav came out as "DomainNames". A line break is a
   * space until there is inline layout that can honour the break itself. */
  if (t == GUMBO_TAG_BR) {
    if (*used > 0 && out[*used - 1] != ' ') out[(*used)++] = ' ';
    out[*used] = 0;
    return;
  }
  GumboVector *k = &n->v.element.children;
  for (unsigned i = 0; i < k->length; i++)
    collect_text((GumboNode *)k->data[i], out, cap, used);
}

enum { BLK_NONE = 0, BLK_HEAD = 1, BLK_BODY = 2 };

static int block_kind(GumboTag t) {
  switch (t) {
    case GUMBO_TAG_H1: case GUMBO_TAG_H2: case GUMBO_TAG_H3: return BLK_HEAD;
    case GUMBO_TAG_P:  case GUMBO_TAG_LI: case GUMBO_TAG_BLOCKQUOTE:
    case GUMBO_TAG_PRE: case GUMBO_TAG_TD: case GUMBO_TAG_DD: return BLK_BODY;
    default: return BLK_NONE;
  }
}

static void collect_text(GumboNode *n, char *out, size_t cap, size_t *used);

/* How many boxes the page should produce, by the same rule the layout applies:
 * a block-level tag, reached through non-block containers, with text inside it.
 * layout_node stops descending at a block (its subtree becomes one run of text),
 * and an empty block draws nothing at all - so a <p> inside an <li> is part of the
 * <li>'s box and a <td> holding only whitespace is no box. Counting any other way
 * makes the assertion disagree with the paint by however many nested or empty
 * blocks the page happens to have, and then the failure looks like a layout bug. */
static int count_boxes(GumboNode *n, int inside_block) {
  if (n->type != GUMBO_NODE_ELEMENT) return 0;
  GumboTag t = n->v.element.tag;
  if (t == GUMBO_TAG_SCRIPT || t == GUMBO_TAG_STYLE || t == GUMBO_TAG_HEAD ||
      t == GUMBO_TAG_TITLE || t == GUMBO_TAG_META || t == GUMBO_TAG_LINK) return 0;

  if (block_kind(t) != BLK_NONE) {
    if (inside_block) return 0;
    static char b[8192];
    size_t used = 0;
    b[0] = 0;
    collect_text(n, b, sizeof b, &used);
    return used ? 1 : 0;
  }

  int c = 0;
  GumboVector *k = &n->v.element.children;
  for (unsigned i = 0; i < k->length; i++)
    c += count_boxes((GumboNode *)k->data[i], inside_block);
  return c;
}

static const char *find_title(GumboNode *n, char *out, size_t cap) {
  if (n->type != GUMBO_NODE_ELEMENT) return NULL;
  if (n->v.element.tag == GUMBO_TAG_TITLE) {
    size_t used = 0;
    out[0] = 0;
    collect_text(n, out, cap, &used);
    return out;
  }
  GumboVector *k = &n->v.element.children;
  for (unsigned i = 0; i < k->length; i++) {
    const char *r = find_title((GumboNode *)k->data[i], out, cap);
    if (r) return r;
  }
  return NULL;
}

/* ---- layout ---- */

typedef struct {
  int y, blocks, lines, widest, glyphs;
} layout_t;

static void layout_node(GumboNode *n, layout_t *L, int measure, int draw);

/* One block: its text, wrapped, with the spacing the block kind asks for.
 * With draw == 0 it measures and places without touching a pixel, which is how
 * the fixture page is checked: same blocks, same wrap, no paint. */
static void layout_block(GumboNode *n, layout_t *L, int measure, int draw) {
  static char buf[8192];
  size_t used = 0;
  buf[0] = 0;
  collect_text(n, buf, sizeof buf, &used);
  while (used > 0 && buf[used - 1] == ' ') buf[--used] = 0;
  if (!used) return;

  int head = block_kind(n->v.element.tag) == BLK_HEAD;
  int lh = head ? LH_HEAD : LH_BODY;
  unsigned col = head ? C_ACCENT : C_TEXT;

  L->y += head ? 6 : 4;                      /* the gap between blocks */
  int widest = 0;
  int lines = draw ? wrap_text(buf, MARGIN, L->y, measure, lh, col, &widest)
                   : measure_wrap(buf, measure, &widest);

  L->blocks++;
  L->lines += lines;
  if (widest > L->widest) L->widest = widest;
  L->y += lines * lh;

  if (head && draw) {
    /* A rule under a heading: the only exact-pixel anchor a page has that does
     * not depend on glyph shapes, so an outside check can be arithmetic. */
    fill(MARGIN, L->y - 3, 64, 3, C_ACCENT);
  }
}

static void layout_node(GumboNode *n, layout_t *L, int measure, int draw) {
  if (n->type == GUMBO_NODE_TEXT || n->type == GUMBO_NODE_WHITESPACE) return;
  if (n->type != GUMBO_NODE_ELEMENT) return;

  GumboTag t = n->v.element.tag;
  if (t == GUMBO_TAG_SCRIPT || t == GUMBO_TAG_STYLE || t == GUMBO_TAG_HEAD ||
      t == GUMBO_TAG_TITLE || t == GUMBO_TAG_META || t == GUMBO_TAG_LINK) return;

  if (block_kind(t) != BLK_NONE) { layout_block(n, L, measure, draw); return; }

  GumboVector *k = &n->v.element.children;
  for (unsigned i = 0; i < k->length; i++)
    layout_node((GumboNode *)k->data[i], L, measure, draw);
}

/* ---- painting the document at an offset ---- */

#define KEY_UP   0x100
#define KEY_DOWN 0x101
#define WIN_EV_KEY 4
#define WIN_EV_CLOSE 5
#define CONTENT_TOP 40

/* 16 bytes, matching win_event_t in kernel/include/guiipc.h: kind, x, y, code. */
typedef struct { int kind, x, y, code; } win_ev_user_t;

static int measure, content_h;

/* The whole page is laid out again for every offset, starting at a negative y.
 * Layout is cheap and stateless, and the alternative - caching placed boxes and
 * translating them - is a second layout engine that can disagree with the first. */
static void repaint(long win, int scroll) {
  fill(0, 0, CW, CH, C_BG);
  fill(0, 0, CW, 30, C_PANEL);
  text(MARGIN, 7, "Omi Net", C_TEXT);
  text(MARGIN + 9 * GLYPH_W, 7, "https://" HOST PATH, C_DIM);
  fill(0, 30, CW, 1, C_ACCENT);
  fill(0, 31, CW, 3, C_BG);

  layout_t L = { .y = CONTENT_TOP - scroll, .blocks = 0, .lines = 0,
                 .widest = 0, .glyphs = 0 };
  layout_node(doc_root, &L, measure, 1);

  /* A scrollbar, because a page that can move should say so. It is also the one
   * element whose position the outside check can predict arithmetically. */
  int vmax = content_h > CH ? content_h - CH : 0;
  if (vmax > 0) {
    int track_y = CONTENT_TOP, track_h = CH - CONTENT_TOP - 4;
    int bar_h = track_h * CH / content_h;
    if (bar_h < 12) bar_h = 12;
    int bar_y = track_y + (track_h - bar_h) * scroll / vmax;
    fill(CW - 5, track_y, 5, track_h, C_PANEL);
    fill(CW - 5, bar_y, 5, bar_h, C_DIM);
  }

  /* the exact-pixel marker: chrome, so it does not move with the page */
  fill(MARGIN, CH - 6, 24, 4, C_MARK);

  sys3(SYS_SURF_PRESENT, win, 0, ((long)CW << 16) | CH);
}

int main(void) {
  long win = sys3(SYS_WIN_OPEN, (long)"Omi Net", WIN_W, WIN_H);
  check(win > 0, "the window opens", win, 1);

  int32_t g[4] = {0, 0, 0, 0};
  if (sys3(SYS_WIN_GEOM, win, (long)g, 0) == 0)
    printf("[net] client at %d,%d size %dx%d\n",
           g[0] + 1, g[1] + 31, g[2] - 2, g[3] - 32);

  long s = sys3(SYS_SURF_CREATE, win, CW, CH);
  check(s > 0, "the surface maps into the process", s, 1);
  if (s <= 0) { printf("[net] 0/%d\n", checks); return 1; }
  pix = (unsigned *)s;

  /* ---- fetch ---- */
  static char page[MAX_PAGE];
  const char *why = "not tried";
  long blen = 0, status;
  char ip[16];

  status = https_get(HOST, 443, PATH, page, sizeof page, &why, &blen, ip);
  printf("[net] %s is %s; status=%ld body=%ld %s\n",
         HOST, ip[0] ? ip : "?", status, blen, why);
  check(ip[0] && strchr(ip, '.'), "the kernel resolved the name", (long)strlen(ip), 7);
  check(status == 200, "the page answers 200 over TLS", status, 200);
  check(blen > 200, "the body arrived, not just the headers", blen, 200);

  /* ---- parse ---- */
  GumboOutput *out = gumbo_parse(page);
  check(out != NULL, "gumbo parsed the page", out != NULL, 1);
  if (!out) { printf("[net] %d/%d\n", checks - fails, checks); return 1; }

  /* Counted, not assumed. The first version of this asserted a heading and a
   * link because that is what example.com used to serve; the page is now one
   * paragraph and a script, so those checks failed on a parser that was right.
   * Whatever the page is, structure has to hold: a tree with elements, a title
   * the parser found, and at least one text block to lay out. */
  int elems  = count_elements(out->root);
  int blocks = count_boxes(out->root, 0);
  char tbuf[256];
  const char *title = find_title(out->root, tbuf, sizeof tbuf);
  printf("[net] dom %d elements, %d boxes, title \"%s\"\n",
         elems, blocks, title ? title : "?");
  check(elems >= 8, "the DOM is a tree, not a stray text node", elems, 8);
  check(blocks >= 1, "the page has a text block to lay out", blocks, 1);
  check(title && strlen(title) >= 3,
        "the parser found a title", title ? (long)strlen(title) : 0, 3);

  /* ---- measure, then paint ---- */
  /* One measuring pass first: the height decides whether the page scrolls, and a
   * repaint needs to know where the bottom is before it draws anything. */
  measure = CW - 2 * MARGIN;
  layout_t L = { .y = CONTENT_TOP, .blocks = 0, .lines = 0, .widest = 0, .glyphs = 0 };
  layout_node(out->root, &L, measure, 0);
  content_h = L.y;
  doc_root = out->root;
  repaint(win, 0);
  L.glyphs = text_glyph_pixels;

  printf("[net] layout %d blocks, %d lines, widest %d px, %d glyph pixels, bottom %d\n",
         L.blocks, L.lines, L.widest, L.glyphs, L.y);
  /* The layout pass and the tree walk must agree on how many boxes there are:
   * a check that hard-codes "at least two" only describes the page I happened to
   * fetch today. */
  check(L.blocks == blocks, "every counted block became a box", L.blocks, blocks);
  check(L.lines >= 4, "the text wrapped onto several lines", L.lines, 4);
  check(L.widest <= measure, "no line escapes the measure", L.widest, measure);
  check(L.glyphs > 800, "the glyph blitter drew the page", L.glyphs, 800);
  check(content_h > CH - CONTENT_TOP,
        "the page is taller than the viewport, so it scrolls", content_h, CH);

  /* ---- the fixture page ----
   * A page fetched from the internet can be any shape tomorrow, so it cannot be
   * the thing that proves multi-block layout. This one is compiled in: a heading,
   * two paragraphs and a list, laid out through the same code with drawing off.
   * The live page proves the pipeline end to end; this proves the boxes. */
  static const char *const fixture =
    "<!doctype html><html><head><title>fixture</title></head><body>"
    "<h1>Layout Fixture</h1>"
    "<p>Alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi "
    "omicron pi rho sigma tau upsilon phi chi psi omega.</p>"
    "<p>Short second paragraph.</p>"
    "<ul><li>One item.</li><li>Two item.</li></ul>"
    "<p>Joined<br>by a break.</p>"
    "</body></html>";
  GumboOutput *fx = gumbo_parse(fixture);
  layout_t F = { .y = 40, .blocks = 0, .lines = 0, .widest = 0, .glyphs = 0 };
  layout_node(fx->root, &F, measure, 0);
  printf("[net] fixture %d blocks, %d lines, widest %d px, bottom %d\n",
         F.blocks, F.lines, F.widest, F.y);
  /* A <br> has no children, so text on either side of it fuses unless the
   * collector turns it into a space. Checked on known input, because on a live
   * page a missing space is invisible until someone reads the screen. */
  {
    GumboNode *p = 0;
    GumboVector *b = &fx->root->v.element.children;
    for (unsigned i = 0; i < b->length; i++) {
      GumboNode *h = (GumboNode *)b->data[i];
      if (h->type == GUMBO_NODE_ELEMENT && h->v.element.tag == GUMBO_TAG_BODY) {
        GumboVector *k = &h->v.element.children;
        for (unsigned j = 0; j < k->length; j++) {
          GumboNode *c = (GumboNode *)k->data[j];
          if (c->type == GUMBO_NODE_ELEMENT && c->v.element.tag == GUMBO_TAG_P) {
            GumboVector *kc = &c->v.element.children;
            for (unsigned m = 0; m < kc->length; m++) {
              GumboNode *g = (GumboNode *)kc->data[m];
              if (g->type == GUMBO_NODE_ELEMENT && g->v.element.tag == GUMBO_TAG_BR) p = c;
            }
          }
        }
      }
    }
    static char tb[256];
    size_t u = 0; tb[0] = 0;
    int found = 0;
    check(p != 0, "the fixture has a paragraph with a break in it", p ? 1 : 0, 1);
    if (p) {
      collect_text(p, tb, sizeof tb, &u);
      found = strstr(tb, "Joined by") != 0 || strstr(tb, "Joined  by") != 0;
    }
    printf("[net] br case: \"%s\"\n", tb);
    check(found, "a <br> leaves a space between the words", found, 1);
  }

  check(F.blocks == 6, "the fixture lays out six boxes", F.blocks, 6);
  check(F.lines >= 5, "the fixture's long paragraph wraps", F.lines, 5);
  check(F.widest <= measure, "the fixture respects the measure", F.widest, measure);
  check(F.y <= CH, "the fixture fits", F.y, CH);
  gumbo_destroy_output(&kGumboDefaultOptions, fx);

  /* the exact-pixel marker: below everything, known coordinate */
  int my = CH - 6;
  fill(MARGIN, my, 24, 4, C_MARK);
  printf("[net] marker at %d,%d 24x4\n", MARGIN, my);
  check(pix[my * CW + MARGIN] == C_MARK, "the marker is in the surface",
        pix[my * CW + MARGIN], (long)C_MARK);

  long r = sys3(SYS_SURF_PRESENT, win, 0, ((long)CW << 16) | CH);
  check(r == 0, "presenting the page succeeds", r, 0);

  printf("[net] %d/%d\n", checks - fails, checks);
  if (fails) { gumbo_destroy_output(&kGumboDefaultOptions, out); return 1; }

  /* ---- scrolling ----
   * Arrow keys move the page by one line. Each move is printed, so the serial log
   * shows the input arriving rather than the program claiming it would. */
  int vmax = content_h > CH ? content_h - CH : 0;
  int scroll = 0;
  printf("[net] scroll 0/%d (content %d, viewport %d)\n", vmax, content_h, CH);
  for (;;) {
    win_ev_user_t ev;
    long r = sys3(SYS_WIN_WAIT, win, (long)&ev, 0);
    if (r < 0) break;
    if (ev.kind == WIN_EV_CLOSE) break;
    if (ev.kind == WIN_EV_KEY) {
      int before = scroll;
      if (ev.code == KEY_DOWN) scroll += LH_BODY;
      else if (ev.code == KEY_UP) scroll -= LH_BODY;
      if (scroll < 0) scroll = 0;
      if (scroll > vmax) scroll = vmax;
      if (scroll != before) {
        printf("[net] scroll %d/%d\n", scroll, vmax);
        repaint(win, scroll);
      }
    }
  }
  gumbo_destroy_output(&kGumboDefaultOptions, out);
  return 0;
}