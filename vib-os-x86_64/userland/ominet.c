/* ominet - the browser, in a process.
 *
 * Fetch over TLS (fetch.c), parse with Gumbo, lay out with the engine that used
 * to run inside the kernel (omilayout.c, moved here rather than rewritten), and
 * paint the display list it returns through the shared rasteriser (omitext.h).
 *
 * The kernel's part is a window and a surface: it never learns what a character
 * or a tag is. That is the arrangement a browser needs - a malformed page is a
 * process's memory bug, not the kernel's.
 *
 * Everything it claims is asserted in-process, and the pixels are checked from
 * outside at the coordinates it prints.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "gumbo.h"
#include "glyphs.h"
#include "fetch.h"
#include "omilayout.h"

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

#define LH_BODY  18
#define MARGIN   8
#define CONTENT_TOP 40
#define MAX_PAGE 32768

/* A page with enough text to be worth scrolling: ~11 KB of HTML, about three
 * viewports tall, with links to follow. */
#define HOST0 "www.iana.org"
#define PATH0 "/domains/reserved"

static unsigned *pix;
static int checks, fails;

static char cur_host[128] = HOST0;
static char cur_path[256] = PATH0;
static char cur_url[192] = "https://" HOST0 PATH0;
static char page[MAX_PAGE];

static long win_id;

/* The address bar: '/' opens it, Enter loads, Esc drops it. */
static int editing, edit_len;
static char err_msg[96];      /* the last failure, shown in the chrome */
/* What the browser is doing right now: resolving, connecting, reading. A load
 * takes seconds on a good day, and the window used to show nothing at all until
 * it finished - a black rectangle that is indistinguishable from a dead
 * process. This is drawn in the address strip while there is no page yet. */
static char status_msg[64];
/* Defined with the painter, called from the loader above it. */
static void show_status(const char *s);
static char edit[192];
static int measure_w, content_h, scroll;
/* While a load is in flight the engine still holds whatever was built last -
 * the fixture at startup - and painting it would show a page that is not the one
 * being fetched. The status frames suppress the display list entirely. */
static int show_ops = 1;

/* After the surface and its size: the rasteriser works on the includer's pix,
 * CW and CH, which is what makes it shareable by two clients. */
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

static const char *find_title(GumboNode *n, char *out, size_t cap) {
  if (n->type != GUMBO_NODE_ELEMENT) return NULL;
  if (n->v.element.tag == GUMBO_TAG_TITLE) {
    size_t used = 0;
    out[0] = 0;
    GumboVector *k = &n->v.element.children;
    for (unsigned i = 0; i < k->length && used + 1 < cap; i++) {
      GumboNode *c = (GumboNode *)k->data[i];
      if (c->type != GUMBO_NODE_TEXT && c->type != GUMBO_NODE_WHITESPACE) continue;
      for (const char *t = c->v.text.text; *t && used + 1 < cap; t++)
        out[used++] = (*t == '\n' || *t == '\r' || *t == '\t') ? ' ' : *t;
    }
    out[used] = 0;
    return out;
  }
  GumboVector *k = &n->v.element.children;
  for (unsigned i = 0; i < k->length; i++) {
    const char *r = find_title((GumboNode *)k->data[i], out, cap);
    if (r) return r;
  }
  return NULL;
}




#define WIN_EV_DOWN 2
#define WIN_EV_KEY  4
#define WIN_EV_CLOSE 5
#define KEY_UP   0x100
#define KEY_DOWN 0x101
#define KEY_ESC  0x107

/* 16 bytes, matching win_event_t in kernel/include/guiipc.h: kind, x, y, code. */
typedef struct { int kind, x, y, code; } win_ev_user_t;

/* The engine measures through this. A process has one face - 8x16 - so every
 * font index the engine asks about is answered with the same width; the engine
 * keeps its own metrics for placement. */
static long last_status, last_blen;
static char last_ip[16];

static int user_measure(int font, const char *s) {
  (void)font;
  return (int)strlen(s) * GLYPH_W;
}

/* Fetch the current URL and lay it out. Returns the status, and leaves
 * content_h at 0 when there is nothing to paint. */
const char *https_get_location(void);   /* fetch.c: the Location header, if any */

static long load_page(void) {
  const char *why = "not tried";
  long blen = 0;
  char ip[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  /* A dropped SYN is not a bug in the browser, so it gets three tries before
   * the page is called dead - but every attempt is printed, and the checks run
   * against the last one. The tries are back to back: an earlier version
   * yielded twenty thousand times between them, which parked the browser for
   * minutes with its keyboard dead, and looked exactly like a hang. */
  long status = -1;
  char sb[64];
  for (int attempt = 1; attempt <= 3; attempt++) {
    blen = 0;
    why = "not tried";
    snprintf(sb, sizeof sb, "loading %s (%d/3)", cur_host, attempt);
    show_status(sb);
    status = https_get(cur_host, 443, cur_path, page, sizeof page, &why, &blen, ip);
    printf("[net] https://%s%s -> %s attempt %d status=%ld body=%ld %s\n",
           cur_host, cur_path, ip[0] ? ip : "?", attempt, status, blen, why);
    if (status > 0 && blen > 0) break;   /* 200, or a redirect to follow */
    sys3(SYS_YIELD, 0, 0, 0);
  }
  last_status = status; last_blen = blen;
  memcpy(last_ip, ip, sizeof ip);
  status_msg[0] = 0;                  /* the strip goes back to the URL */
  content_h = 0;
  if (status == 200 && blen > 0) {
    err_msg[0] = 0;
    if (web_build(page, (int)blen, measure_w, user_measure) == 0) content_h = web_height();
    else printf("[net] the engine refused the page\n");
  } else if (status > 0) {
    /* A redirect with nowhere to go is the only 3xx that is an error. */
    snprintf(err_msg, sizeof err_msg, "HTTP %ld from %s, nothing to follow", status, cur_host);
  } else {
    /* Said out loud, on screen: a page that will not load and a browser that
     * has stopped working look the same if the failure is only on the serial
     * console, and the one thing a reader must never do is lie about it. */
    snprintf(err_msg, sizeof err_msg, "cannot load %s: %s", cur_host, why);
  }
  return status;
}

static void paint(long win, int off) {
  fill(0, 0, CW, CH, C_BG);
  fill(0, 0, CW, 30, C_PANEL);
  text(MARGIN, 7, "Omi Net", C_TEXT);
  if (editing) {
    /* The field itself, drawn over the URL strip. The caret is a two-pixel bar
     * rather than a blinking cursor: this paints on demand, not on a timer. */
    int fx = MARGIN + 9 * GLYPH_W;
    fill(fx - 4, 4, CW - fx + 2, 22, C_BG);
    int room = (CW - (fx - MARGIN) - 4) / GLYPH_W;
    if (room < 1) room = 1;
    const char *show = edit;
    if (edit_len > room) show = edit + (edit_len - room);   /* keep the caret in view */
    text(fx, 7, show, C_TEXT);
    fill(fx + (int)strlen(show) * GLYPH_W, 7, 2, GLYPH_H, C_ACCENT);
  } else {
    /* The URL is the one thing in the chrome that can be any length. */
    int room = (CW - (MARGIN + 9 * GLYPH_W)) / GLYPH_W;
    char bar[128];
    int n = (int)strlen(cur_url);
    if (room > 126) room = 126;
    if (n <= room) { memcpy(bar, cur_url, n + 1); }
    else { memcpy(bar, cur_url, room - 3); memcpy(bar + room - 3, "...", 4); }
    text(MARGIN + 9 * GLYPH_W, 7, err_msg[0] ? err_msg : status_msg[0] ? status_msg : bar,
         err_msg[0] ? C_ACCENT : status_msg[0] ? C_TEXT : C_DIM);
  }
  fill(0, 30, CW, 1, C_ACCENT);
  fill(0, 31, CW, 3, C_BG);

  /* The display list is in page coordinates, origin at the content's top left.
   * Scrolling is a subtraction - which is the reason to keep a display list
   * instead of re-running layout for every frame. */
  int vp = CH - CONTENT_TOP - 4;
  clip_rect(MARGIN, CONTENT_TOP, MARGIN + measure_w, CONTENT_TOP + vp);
  int n = show_ops ? web_count() : 0;
  const web_op_t *ops = web_ops();
  for (int i = 0; i < n; i++) {
    const web_op_t *o = &ops[i];
    int oy = CONTENT_TOP + o->y - off;
    int oh = o->h > 0 ? o->h : GLYPH_H;
    if (oy > CONTENT_TOP + vp || oy + oh < CONTENT_TOP) continue;
    if (o->kind == 0) {
      if (o->h > 0) fill(MARGIN + o->x, oy, o->w, o->h, o->color);
    } else if (o->kind == 2) {
      fill(MARGIN + o->x, oy, o->w, 1, o->color);
    } else if (o->kind == 3 && o->px) {
      /* No image decoder in the process yet; the engine gave us alt text
       * instead of a bitmap, and an undecoded page is better than a hole. */
    } else if (o->text) {
      text(MARGIN + o->x, oy, o->text, o->color);
      if (o->bold) text(MARGIN + o->x + 1, oy, o->text, o->color);
    }
  }
  clip_none();

  /* A scrollbar, because a page that can move should say so - and because it is
   * the one element whose position an outside check can predict. */
  int vmax = content_h > vp ? content_h - vp : 0;
  if (vmax > 0) {
    int track_y = CONTENT_TOP, track_h = vp;
    int bar_h = track_h * vp / content_h;
    if (bar_h < 12) bar_h = 12;
    int bar_y = track_y + (track_h - bar_h) * off / vmax;
    fill(CW - 5, track_y, 5, track_h, C_PANEL);
    fill(CW - 5, bar_y, 5, bar_h, C_DIM);
  }

  /* the exact-pixel marker: chrome, so it does not move with the page */
  fill(MARGIN, CH - 6, 24, 4, C_MARK);

  sys3(SYS_SURF_PRESENT, win, 0, ((long)CW << 16) | CH);
}

/* Say what is happening, and show it. Every phase of a load goes through here:
 * the point is that the window is never a rectangle of nothing, whatever the
 * network is doing. */
static void show_status(const char *s) {
  int i = 0;
  while (s[i] && i < (int)sizeof status_msg - 1) { status_msg[i] = s[i]; i++; }
  status_msg[i] = 0;
  if (win_id > 0 && pix) {
    int save = show_ops;
    show_ops = 0;          /* status frames carry no page */
    paint(win_id, 0);
    show_ops = save;
  }
}

/* Follow a link. The engine hands back the href as written, so resolve the two
 * shapes a page actually uses: an absolute URL, or a path on this host. A
 * relative link is refused out loud rather than guessed at. */
static void go(const char *href) {
  char host[128], path[256];
  const char *p = href;
  if (!strncmp(p, "https://", 8)) p += 8;
  else if (!strncmp(p, "http://", 7)) p += 7;
  if (*p == '/') {
    memcpy(host, cur_host, sizeof host < sizeof cur_host ? sizeof host : sizeof cur_host);
    host[sizeof host - 1] = 0;
  } else if (*p) {
    int i = 0;
    while (p[i] && p[i] != '/' && i < 127) { host[i] = p[i]; i++; }
    host[i] = 0;
    p += i;
  } else return;

  if (*p != '/') { path[0] = '/'; path[1] = 0; }
  else { int j = 0; while (p[j] && j < 255) { path[j] = p[j]; j++; } path[j] = 0; }

  if (!strcmp(host, cur_host) && !strcmp(path, cur_path)) return;

  /* Typing "iana.org" lands in a 301 to "www.iana.org". A browser that stops at
   * the 301 and shows an error is a browser that cannot be pointed at the web,
   * which is the whole point of typing. Five hops, because that is what every
   * other browser settled on. */
  for (int hops = 0; hops <= 5; hops++) {
    printf("[net] clicked -> https://%s%s\n", host, path);
    memcpy(cur_host, host, sizeof cur_host);
    memcpy(cur_path, path, sizeof cur_path);
    snprintf(cur_url, sizeof cur_url, "https://%s%s", cur_host, cur_path);
    long st = load_page();
    scroll = 0;
    text_glyph_pixels = 0;
    paint(win_id, 0);
    if (st == 200 && content_h > 0) {
      printf("[net] followed: %d ops, content %d, viewport %d\n",
             web_count(), content_h, CH - CONTENT_TOP - 4);
      return;
    }
    const char *loc = https_get_location();
    if (!(st == 301 || st == 302 || st == 303 || st == 307 || st == 308) || !loc[0]) return;
    printf("[net] redirect %ld -> %s\n", st, loc);
    /* Absolute, root-relative or relative: the three forms a server sends. */
    const char *q = loc;
    if (!strncmp(q, "https://", 8)) q += 8;
    else if (!strncmp(q, "http://", 7)) q += 7;
    if (*q == '/') {
      int j = 0;
      while (q[j] && j < 255) { path[j] = q[j]; j++; }
      path[j] = 0;
    } else if (*q) {
      int i = 0;
      while (q[i] && q[i] != '/' && i < 127) { host[i] = q[i]; i++; }
      host[i] = 0;
      q += i;
      if (*q != '/') { path[0] = '/'; path[1] = 0; }
      else { int j = 0; while (q[j] && j < 255) { path[j] = q[j]; j++; } path[j] = 0; }
    } else return;
  }
  snprintf(err_msg, sizeof err_msg, "too many redirects from %s", cur_host);
}

int main(void) {
  win_id = sys3(SYS_WIN_OPEN, (long)"Omi Net", WIN_W, WIN_H);
  check(win_id > 0, "the window opens", win_id, 1);

  int32_t g[4] = {0, 0, 0, 0};
  if (sys3(SYS_WIN_GEOM, win_id, (long)g, 0) == 0)
    printf("[net] client at %d,%d size %dx%d\n",
           g[0] + 1, g[1] + 31, g[2] - 2, g[3] - 32);

  long s = sys3(SYS_SURF_CREATE, win_id, CW, CH);
  check(s > 0, "the surface maps into the process", s, 1);
  if (s <= 0) { printf("[net] 0/%d\n", checks); return 1; }
  pix = (unsigned *)s;
  measure_w = CW - 2 * MARGIN;

  /* First frame, before anything that can fail or take time. The window exists
   * from SYS_WIN_OPEN onward, but its surface holds whatever the compositor last
   * had there - black - until this process presents something. A fetch is
   * seconds at best and unbounded at worst, so the browser used to be an
   * unlit rectangle for that whole time: a reader cannot tell that apart from a
   * process that died. Chrome and a status line go up first, always. */
  show_status("starting");

  /* ---- the fixture page: what the engine has to do with CSS ----
   * A fetched page is whatever it is tomorrow, so the proof is compiled in: a
   * background colour, padding, a stylesheet colour on text, and a list. */
  static const char *const fixture =
    "<!doctype html><html><head><title>fixture</title>"
    "<style>p{color:#00ff00}</style></head><body>"
    "<div style=\"background:#3366cc;padding:12px\">boxed</div>"
    "<p>green paragraph text, long enough to wrap at this width, so the wrap "
    "is exercised as well as the colour</p>"
    "<ul><li>one item</li><li>two item</li></ul></body></html>";
  int fx = web_build(fixture, (int)strlen(fixture), measure_w, user_measure) == 0;
  check(fx, "the engine builds the fixture", fx, 1);
  int ftext = 0, green = 0, blue_h = 0;
  for (int i = 0; i < web_count(); i++) {
    const web_op_t *o = &web_ops()[i];
    if (o->kind == 0 && (o->color & 0xFFFFFF) == 0x3366CC && o->h > blue_h) blue_h = o->h;
    if (o->text) { ftext++; if ((o->color & 0xFFFFFF) == 0x00FF00) green++; }
  }
  printf("[net] fixture %d ops, %d text runs, background box %d px tall, height %d\n",
         web_count(), ftext, blue_h, web_height());
  check(blue_h >= 24, "a padded background is as tall as its padding plus a line",
        blue_h, 24);
  check(green > 0, "a stylesheet colour reaches a text run", green, 1);
  check(ftext >= 3, "the fixture laid its text out", ftext, 3);
  check(web_height() > 0, "the fixture has a height", web_height(), 1);

  /* ---- fetch the real page ---- */
  load_page();
  check(last_ip[0] && strchr(last_ip, '.'), "the kernel resolved the name",
        (long)strlen(last_ip), 7);
  check(last_status == 200, "the page answers 200 over TLS", last_status, 200);
  check(last_blen > 200, "the body arrived, not just the headers", last_blen, 200);

  /* ---- parse, for structure that has to hold whatever the page is ----
   * The engine parsed the same bytes already; this second parse is for the
   * checks, and it is cheap next to the fetch. */
  GumboOutput *out = gumbo_parse(page);
  check(out != NULL, "gumbo parsed the page", out != NULL, 1);
  if (out) {
    int elems = count_elements(out->root);
    char tbuf[256];
    const char *title = find_title(out->root, tbuf, sizeof tbuf);
    printf("[net] dom %d elements, title \"%s\"\n", elems, title ? title : "?");
    check(elems >= 8, "the DOM is a tree, not a stray text node", elems, 8);
    check(title && strlen(title) >= 3, "the parser found a title",
          title ? (long)strlen(title) : 0, 3);
    gumbo_destroy_output(&kGumboDefaultOptions, out);
  }

  check(content_h > 0, "the engine laid the page out", content_h, 1);
  int nops = web_count(), runs = 0, maxr = 0;
  for (int i = 0; i < nops; i++) {
    const web_op_t *o = &web_ops()[i];
    if (!o->text) continue;
    runs++;
    int r = o->x + (int)strlen(o->text) * GLYPH_W;
    if (r > maxr) maxr = r;
  }
  printf("[net] layout %d ops, %d text runs, widest %d px, height %d\n",
         nops, runs, maxr, content_h);
  check(nops > 0, "the page produced a display list", nops, 1);
  check(runs >= 4, "the page has text runs to paint", runs, 4);
  check(maxr <= measure_w, "no text run escapes the measure", maxr, measure_w);
  check(content_h > CH - CONTENT_TOP,
        "the page is taller than the viewport, so it scrolls", content_h, CH);

  /* The click path. A link is a text run that carries an href, so the engine's
   * hit map has to agree with the box it told us to paint - otherwise the link
   * is decoration and the mouse handler is dead code. */
  int links = 0, hits = 0, lost = 0, lx = -1, ly = -1;
  for (int i = 0; i < nops; i++) {
    const web_op_t *o = &web_ops()[i];
    if (o->kind != 1 || !o->href) continue;
    /* <a name="x"> and <a id="x"> are anchors with no box: they paint nothing,
     * so there is nothing for the mouse to land on and nothing to check. */
    if (o->w <= 0 || !o->text || !o->text[0]) { lost++; continue; }
    links++;
    if (web_hit_link(o->x + 1, o->y + 1) == o->href) {
      hits++;
      if (lx < 0) { lx = MARGIN + o->x; ly = CONTENT_TOP + o->y; }
    }
  }
  printf("[net] links %d, hit-tested %d, %d empty anchors, first at client %d,%d\n",
         links, hits, lost, lx, ly);
  check(links > 0, "the page has links", links, 1);
  check(hits == links, "every link's painted box hit-tests to its own URL", hits, links);

  /* ---- paint ---- */
  text_glyph_pixels = 0;
  paint(win_id, 0);
  printf("[net] painted %d glyph pixels\n", text_glyph_pixels);
  check(text_glyph_pixels > 800, "the blitter drew the page", text_glyph_pixels, 800);

  int my = CH - 6;
  printf("[net] marker at %d,%d 24x4\n", MARGIN, my);
  check(pix[my * CW + MARGIN] == C_MARK, "the marker is in the surface",
        pix[my * CW + MARGIN], (long)C_MARK);
  long r = sys3(SYS_SURF_PRESENT, win_id, 0, ((long)CW << 16) | CH);
  check(r == 0, "presenting the page succeeds", r, 0);

  printf("[net] %d/%d\n", checks - fails, checks);
  if (fails) {
    /* A page that did not load is not a reason to take the window away. The
     * reader came for a browser; one that disappears on a bad response leaves
     * them with an empty desktop and no explanation, and one that keeps its
     * chrome can be pointed somewhere else without a relaunch. Drop the page,
     * keep the failure in the strip, and go on listening for a typed URL. */
    static const char *const blank = "<!doctype html><html><body></body></html>";
    web_build(blank, (int)strlen(blank), measure_w, user_measure);
    content_h = 0;
    if (!err_msg[0]) snprintf(err_msg, sizeof err_msg, "the page did not load");
    paint(win_id, 0);
  }

  /* ---- input: arrows scroll, a click follows a link ---- */
  int vp = CH - CONTENT_TOP - 4;
  int vmax = content_h > vp ? content_h - vp : 0;
  int step = web_line_height(2);
  if (step < 8) step = LH_BODY;
  printf("[net] scroll 0/%d (content %d, viewport %d, step %d)\n", vmax, content_h, vp, step);
  for (;;) {
    win_ev_user_t ev;
    long w = sys3(SYS_WIN_WAIT, win_id, (long)&ev, 0);
    if (w < 0) break;
    if (ev.kind == WIN_EV_CLOSE) break;
    if (ev.kind == WIN_EV_KEY && editing) {
      int k = ev.code;
      if (k == '\n' || k == '\r') {                     /* Enter: load it */
        editing = 0;
        if (edit_len) { printf("[net] typed -> %s\n", edit); go(edit); }
        else paint(win_id, scroll);
      } else if (k == 8 || k == 127) {                    /* Backspace */
        if (edit_len > 0) edit[--edit_len] = 0;
        paint(win_id, scroll);
      } else if (k == KEY_ESC) {
        editing = 0;
        paint(win_id, scroll);
      } else if (k >= 32 && k < 127 && edit_len < (int)sizeof(edit) - 1) {
        edit[edit_len++] = (char)k;
        edit[edit_len] = 0;
        paint(win_id, scroll);
      }
    } else if (ev.kind == WIN_EV_KEY) {
      if (ev.code == '/') {                               /* open the address bar */
        editing = 1;
        edit_len = 0;
        edit[0] = 0;
        paint(win_id, scroll);
      } else {
        int before = scroll;
        if (ev.code == KEY_DOWN) scroll += step;
        else if (ev.code == KEY_UP) scroll -= step;
        if (scroll < 0) scroll = 0;
        if (scroll > vmax) scroll = vmax;
        if (scroll != before) {
          printf("[net] scroll %d/%d\n", scroll, vmax);
          text_glyph_pixels = 0;
          paint(win_id, scroll);
        }
      }
    } else if (ev.kind == WIN_EV_DOWN) {
      const char *href = web_hit_link(ev.x - MARGIN, ev.y - CONTENT_TOP + scroll);
      if (href) go(href);
    }
  }
  return 0;
}
