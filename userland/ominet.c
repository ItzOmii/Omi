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
#include "../kernel/include/buildstamp.h"   /* which build this is, on screen */
#include "omilayout.h"
#include "imgdec.h"
#include "omifont.h"

#define SYS_YIELD        3
#define SYS_WIN_OPEN     6
#define SYS_WIN_CLOSE    10
#define SYS_WIN_WAIT     12
#define SYS_WIN_GEOM     14
#define SYS_SURF_CREATE  35
#define SYS_SURF_PRESENT 36
#define SYS_SURF_RESIZE  37
#define SYS_TICKS        4
#define SYS_MEMINFO      41

/* The window a browser opens at. 440x360 was a test fixture's size, and it put
 * every page on the wrong side of the desktop breakpoints: at 438 CSS pixels the
 * IANA sheet correctly collapses to a single column, so the browser looked like it
 * had ignored the stylesheet when it had actually honoured the narrow layout. */
#define WIN_W  900
#define WIN_H  620
/* The client area, inside the compositor's chrome. Not a constant any more: the
 * window can be maximised or dragged bigger, and every frame is laid out for the
 * size it actually has. */
static int cw = WIN_W - 2;
/* Two buffers, not one. The compositor copies the mapped surface at any moment
 * it decides to redraw, and paint() calls out to the kernel (status prints, the
 * geometry line) while the frame is half written - so a redraw can catch a
 * surface that is part old page and part new. That is what the stray one-pixel
 * lines were: a row of the previous page left in place until the next repaint.
 * Everything is drawn into `shadow` and copied across in one pass with no
 * syscalls in it, so what the compositor can see is always a whole frame. */
static unsigned *surf;        /* the mapped surface the compositor reads */
static unsigned *shadow;      /* what paint() draws into, when we could get it */
static int ch = WIN_H - 32;
/* omitext.h is written against CW/CH names so any client can supply them; here
 * they are the live size, not a constant. */
#define CW cw
#define CH ch

#define C_BG     0x001E2024u
#define C_PANEL  0x0026292Fu
#define C_TEXT   0x00E6E6E6u
#define C_DIM    0x009AA0A8u
#define C_ACCENT 0x00C4622Du
#define C_MARK   0x0000FF00u

#define LH_BODY  18
#define MARGIN   8

/* A page laid out wider than this stops rendering inside the window: past about
 * 1000 px the display list comes back with its text runs outside the drawable
 * area, which looks exactly like an empty page. Until that is fixed, the column
 * is capped and centred - the same thing a browser does with a max-width
 * article, and legible at any window size. */
/* The column a page is laid out in. Capped and centred rather than stretched to
 * the window: at full-screen widths the table renderer loses its cell text
 * entirely - the rules and the nav survive, the rows do not - and a 640 px column
 * is both what the renderer gets right and what a browser does with a long
 * article. */
#define PAGE_MAX_W 640
static int page_measure(void) {
  int w = cw - 2 * MARGIN;
  if (w > PAGE_MAX_W) w = PAGE_MAX_W;
  if (w < 40) w = 40;
  return w;
}

#define CONTENT_TOP 40
#define BTN_W 20
/* The page buffer used to be a 32 KB static array, so any page larger than that
 * was cut off with nothing said. Big enough now for real articles and documents,
 * and when a page still does not fit, the strip says so. */
/* A quarter of a megabyte. The old 64 KB was chosen because a 128 KB page used to
 * kill the parser: its allocator ran out of mmap slots, malloc returned NULL and
 * Gumbo wrote through it. With the area table raised (kernel/include/task.h), the
 * limit is memory rather than a table, and a 130 KB page now reads whole. */
/* Where Home goes. FrogFind is a search engine that works without JavaScript and
 * fits this browser's idea of a page, which makes it a better home than a blank
 * one. */
#define HOME_URL "https://frogfind.com/"

/* Navigation history: the URLs that actually loaded, and where in them we are. */
#define HIST_MAX 24
#define HIST_URL 200
static char hist[HIST_MAX][HIST_URL];
static int  hist_n, hist_i = -1;
static int  hist_silent;          /* set while moving through history */

#define PAGE_MAX (256 * 1024)
#define PAGE_FLOOR 32768

/* A page with enough text to be worth scrolling: ~11 KB of HTML, about three
 * viewports tall, with links to follow. */
#define HOST0 "www.iana.org"
#define PATH0 "/domains/reserved"
/* Where the page checks get their page. It has to be a real page over real TLS -
 * that is part of what is being tested - and it has to have text, links and
 * height. It does not get to be one host: iana.org is served by Cloudflare, and
 * Cloudflare refuses this machine's SYNs some days while answering the host's
 * curl in the same minute (`connect`, three attempts, then nine red checks that
 * look like a browser bug and are not). So the second one is on a different
 * CDN, and the suite says which one it used. */
static const struct { const char *host, *path; } PAGE_TARGETS[] = {
  { HOST0, PATH0 },
  { "en.wikipedia.org", "/wiki/Chromium_(web_browser)" },
};

static unsigned *pix;
static int checks, fails;

/* ---- form fields -------------------------------------------------------
 * The engine lays a field out and tells us its index; the text in it, the caret
 * and the submit belong to us, because they change without the page changing. */
#define FIELD_MAX  32
#define FIELD_TEXT 64
static char fval[FIELD_MAX][FIELD_TEXT];
static int  focus_field = -1;
/* Where the caret is, and what is selected. A selection is an ordered pair; when
 * they are equal there is nothing selected. */
static int  fcaret[FIELD_MAX];
static int  fsel0[FIELD_MAX], fsel1[FIELD_MAX];
/* A clipboard of our own. The system clipboard would need a channel between
 * processes; that exists now (SCM_RIGHTS, AF_UNIX names) and is the next step,
 * but a browser that can cut and paste inside its own fields is useful first. */
#define CLIP_MAX 1024
static char clip[CLIP_MAX];
static int  clip_n;

#define EV_SHIFT 1
#define EV_CTRL  2
#define KEY_LEFT_K  0x102
#define KEY_RIGHT_K 0x103

static void fields_reset(void) {
  int n = web_field_count();
  if (n > FIELD_MAX) n = FIELD_MAX;
  for (int i = 0; i < n; i++) {
    const char *v = web_field_value(i);
    int k = 0;
    if (v) for (; v[k] && k < FIELD_TEXT - 1; k++) fval[i][k] = v[k];
    fval[i][k] = 0;
    fcaret[i] = k;
    fsel0[i] = fsel1[i] = k;
  }
  focus_field = -1;
}

/* Percent-encode the characters that would end the query or the request. */
static int url_escape(char *out, int cap, const char *in) {
  static const char *hex = "0123456789ABCDEF";
  int o = 0;
  for (; in && *in; in++) {
    unsigned char c = (unsigned char)*in;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '.' || c == '_' || c == '~') {
      if (o + 1 >= cap) break;
      out[o++] = (char)c;
    } else if (c == ' ') {
      if (o + 1 >= cap) break;
      out[o++] = '+';
    } else {
      if (o + 3 >= cap) break;
      out[o++] = '%';
      out[o++] = hex[c >> 4];
      out[o++] = hex[c & 15];
    }
  }
  out[o] = 0;
  return o;
}

/* Which form a field belongs to, or -1. */
static int field_form(int i) { return web_field_form(i); }

static char cur_host[128] = HOST0;
static char cur_path[256] = PATH0;
static char cur_url[192] = "https://" HOST0 PATH0;

/* ---- images -------------------------------------------------------------
 * The engine asks for a bitmap during layout and falls back to the alt text when
 * it gets none, so a failed fetch costs a line of text rather than a hole. Only
 * the first frame is decoded, and only small frames: a browser that crashes on a
 * big image is worse than one that shows the alt text. */
#define IMG_SLOTS   8      /* URLs remembered: a table size, not a policy */
/* The fetch buffer is not a constant either: a gallery thumbnail is commonly
 * 30-100 KB, and a 48 KB buffer made a 48 KB JPEG "truncated" before it was ever
 * decoded. It is sized from the same memory budget in img_cache_reset(), between
 * a floor that fits a thumbnail and a ceiling that keeps one page from taking the
 * machine. */
#define IMG_BUF_MIN (64 * 1024)
#define IMG_BUF_MAX (1024 * 1024)
/* How long the browser will spend fetching a page's images. This is a *time*
 * budget because time is the real cost here (about a second of TLS per image):
 * a page with three images gets all three, and a page with a hundred does not
 * stall for two minutes before it can be read. */
#define IMG_MS       3000

static unsigned char *img_buf;
static long img_buf_cap;
static struct { char url[120]; web_image_t im; int decoded; } img_slot[IMG_SLOTS];
static int  img_slots;
static long img_budget;      /* bytes this page may spend on bitmaps, from the kernel */
static long img_spent;
static long img_deadline;    /* tick after which no more images are fetched */
static int  img_skipped;     /* turned away by the budget, for the log */

static long sys3(long a, long b, long c, long d);   /* the syscall itself, below */

/* Free memory in bytes, as the kernel reports it. Nothing here carries a memory
 * guess: the budget is a quarter of what is actually free when the page starts. */
static long free_bytes(void) {
  long m = sys3(SYS_MEMINFO, 0, 0, 0);
  return (m & 0xFFFFFFFFL) * 4096L;
}

static void img_cache_reset(void) {
  /* The bitmaps belong to the slots, and the engine's ops point at them, so they
   * are freed here, once the page that used them is gone. */
  for (int i = 0; i < img_slots; i++)
    if (img_slot[i].decoded && img_slot[i].im.px) img_free((uint32_t *)img_slot[i].im.px);
  img_slots = 0;
  img_skipped = 0;
  img_spent = 0;
  img_budget = free_bytes() / 4;      /* a quarter of free: four pages' worth, ever */
  img_deadline = 0;            /* started by the first image, not by the page */

  /* A quarter of the budget for the buffer one image at a time is fetched into. */
  long cap = img_budget / 4;
  if (cap < IMG_BUF_MIN) cap = IMG_BUF_MIN;
  if (cap > IMG_BUF_MAX) cap = IMG_BUF_MAX;
  if (cap != img_buf_cap) {
    unsigned char *nb = (unsigned char *)realloc(img_buf, (size_t)cap);
    if (nb) { img_buf = nb; img_buf_cap = cap; }
  }
}

/* Resolve an <img src> against the page being shown. */
static void img_resolve(const char *src, char *host, int hcap, char *path, int pcap) {
  const char *p = src;
  host[0] = 0; path[0] = 0;
  if (!strncmp(p, "https://", 8) || !strncmp(p, "//", 2)) {
    p += (p[0] == 'h') ? 8 : 2;
    int i = 0;
    while (p[i] && p[i] != '/' && i < hcap - 1) { host[i] = p[i]; i++; }
    host[i] = 0;
    p += i;
    snprintf(path, (size_t)pcap, "%s", *p ? p : "/");
  } else if (p[0] == '/') {
    snprintf(host, (size_t)hcap, "%s", cur_host);
    snprintf(path, (size_t)pcap, "%s", p);
  } else {
    snprintf(host, (size_t)hcap, "%s", cur_host);
    /* relative to the directory of the current path */
    int cut = (int)strlen(cur_path);
    while (cut > 0 && cur_path[cut - 1] != '/') cut--;
    snprintf(path, (size_t)pcap, "%.*s%s", cut, cur_path, p);
  }
}

/* A <link rel=stylesheet> from the page being laid out: resolve, fetch, hand the
 * bytes back to the engine. Same two-buffer discipline as images - one fetch at a
 * time, no reuse of a keep-alive socket that a CDN has already closed. */
/* Sized from the same free-memory budget as images: a theme's stylesheet is
 * routinely 200 KB, and a 48 KB buffer silently cut off most of the page's CSS. */
static unsigned char *css_buf;
static int css_cap;
static int provide_css(const char *href, int len, char *out, int cap) {
  if (!href || len <= 0 || len >= 200) return 0;
  char url[200];
  int n = len < 199 ? len : 199;
  memcpy(url, href, (size_t)n);
  url[n] = 0;
  char host[128], path[256];
  img_resolve(url, host, (int)sizeof host, path, (int)sizeof path);
  if (!host[0]) return 0;
  if (!css_buf) {
    long want = free_bytes() / 8;
    if (want < 64 * 1024) want = 64 * 1024;
    if (want > 1024 * 1024) want = 1024 * 1024;
    css_buf = (unsigned char *)malloc((size_t)want);
    css_cap = css_buf ? (int)want : 0;
  }
  if (!css_buf) return 0;
  const char *why = "not tried";
  long blen = 0;
  char ip[16] = {0};
  https_no_reuse = 1;
  long st = https_get(host, 443, path, (char *)css_buf, (size_t)css_cap, &why, &blen, ip);
  https_no_reuse = 0;
  if (st != 200 || blen <= 0) {
    printf("[net] css %s: status=%ld %s\n", url, st, why ? why : "");
    return 0;
  }
  int take = (int)blen < cap - 1 ? (int)blen : cap - 1;
  memcpy(out, css_buf, (size_t)take);
  printf("[net] css %s: %d bytes\n", url, take);
  return take;
}

static const web_image_t *provide_image(const char *src, int len) {
  if (!src || len <= 0 || len >= 118) return 0;
  char url[120];
  int n = len < 119 ? len : 119;
  memcpy(url, src, (size_t)n);
  url[n] = 0;
  for (int i = 0; i < img_slots; i++)
    if (!strcmp(img_slot[i].url, url))
      return img_slot[i].decoded ? &img_slot[i].im : 0;

  /* The table is full: every slot remembers a URL and what came of it, including
   * refusals, so nothing is fetched twice. Past it, the page keeps its alt text. */
  if (img_slots >= IMG_SLOTS) { img_skipped++; return 0; }
  /* The clock, not a count: an image costs about a second of TLS here, so the
   * thing to bound is the time the reader waits, whatever that buys. The window
   * opens at the first image rather than at the start of the page, because the
   * stylesheet fetches happen inside layout too and used to spend the whole
   * budget before a single thumbnail was asked for. */
  long now = sys3(SYS_TICKS, 0, 0, 0);
  if (!img_deadline) img_deadline = now + IMG_MS / 10;
  if (now > img_deadline) { img_skipped++; return 0; }
  int slot = img_slots++;
  snprintf(img_slot[slot].url, sizeof img_slot[slot].url, "%s", url);
  img_slot[slot].decoded = 0;

  char host[128], path[256];
  img_resolve(url, host, (int)sizeof host, path, (int)sizeof path);
  if (!host[0]) return 0;

  const char *why = "not tried";
  long blen = 0;
  char ip[16] = {0};
  https_no_reuse = 1;                      /* see fetch.h: a stale keep-alive */
  long st = https_get(host, 443, path, (char *)img_buf, (size_t)img_buf_cap, &why, &blen, ip);
  https_no_reuse = 0;
  if (st != 200 || blen <= 0) {
    printf("[net] image %s: status=%ld len=%ld %s\n", url, st, blen, why ? why : "");
    return 0;
  }
  if (fetch_clipped) {
    /* Said differently from a network failure: the file is bigger than the buffer
     * this page's budget allowed, which is a limit, not an error. */
    printf("[net] image %s: body longer than the %ld KB fetch buffer\n", url, img_buf_cap / 1024);
    return 0;
  }

  int w = 0, h = 0;
  uint32_t *px = 0;
  const char *kind = img_kind(img_buf, (int)blen);
  int r = img_decode(img_buf, (int)blen, &w, &h, &px);
  if (r != 0 || !px) {
    printf("[net] image %s: %ld bytes saying %s, no decoder for it (%d)\n",
           url, blen, kind, r);
    return 0;
  }
  long cost = (long)w * (long)h * 4;
  if (img_spent + cost > img_budget) {
    printf("[net] image %s: %dx%d needs %ld KB, %ld of %ld KB left\n",
           url, w, h, cost / 1024, (img_budget - img_spent) / 1024, img_budget / 1024);
    img_free(px);
    img_skipped++;
    return 0;
  }
  img_spent += cost;
  img_slot[slot].im.w = w;
  img_slot[slot].im.h = h;
  img_slot[slot].im.px = px;
  img_slot[slot].decoded = 1;
  printf("[net] image %s: %s %dx%d, %ld bytes decoded, %ld/%ld KB of the image budget\n",
         url, kind, w, h, blen, img_spent / 1024, img_budget / 1024);
  return &img_slot[slot].im;
}

static char *page;            /* PAGE_MAX, or PAGE_FLOOR if the heap said no */
static int page_cap;

static long win_id;

/* The address bar: '/' opens it, Enter loads, Esc drops it. */
static int editing, edit_len;
static int dragging_bar, drag_off;
static char err_msg[96];      /* the last failure, shown in the chrome */
/* What the browser is doing right now: resolving, connecting, reading. A load
 * takes seconds on a good day, and the window used to show nothing at all until
 * it finished - a black rectangle that is indistinguishable from a dead
 * process. This is drawn in the address strip while there is no page yet. */
static char status_msg[64];
/* Defined with the painter, called from the loader above it. */
static void show_status(const char *s);
static char edit[192];
static int measure_w, content_h, scroll, vmax;
/* Which build of the display list is in the frame buffer. A scroll can copy
 * pixels only while both frames were produced from the same display list. */
static unsigned page_gen;

/* Where the page's left edge sits inside the window. paint() centres the column,
 * and the hit tests have to subtract the same thing or every click lands left of
 * where it looks when the window is wider than the page. */
static int page_dx_now(void) {
  int dx = (cw - 2 * MARGIN - measure_w) / 2;
  return dx > 0 ? dx : 0;
}

/* While a load is in flight the engine still holds whatever was built last -
 * the fixture at startup - and painting it would show a page that is not the one
 * being fetched. The status frames suppress the display list entirely. */
static int show_ops = 1;
static int dbg_ink = 0;   /* surface ink probe: flip to 1 to measure a paint */

/* After the surface and its size: the rasteriser works on the includer's pix,
 * cw and ch, which is what makes it shareable by two clients. */
#include "omitext.h"
#include "omittf.h"    /* real outlines, blended through the same rasteriser */

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




#define WIN_EV_MOVE 1
#define WIN_EV_DOWN 2
#define WIN_EV_UP   3
#define WIN_EV_KEY  4
#define WIN_EV_CLOSE 5
#define WIN_EV_WHEEL 6
#define WIN_EV_RESIZE 7
#define KEY_UP   0x100
#define KEY_DOWN 0x101
#define KEY_ESC  0x107
#define KEY_HOME 0x114
#define KEY_END  0x115
#define KEY_PGUP 0x116
#define KEY_PGDN 0x117

/* 16 bytes, matching win_event_t in kernel/include/guiipc.h: kind, x, y, code. */
typedef struct { int kind, x, y, code; } win_ev_user_t;

/* The engine measures through this. A process has one face - 8x16 - so every
 * font index the engine asks about is answered with the same width; the engine
 * keeps its own metrics for placement. */
static long last_status, last_blen;
static char last_ip[16];

/* Width of a run in the face it will be drawn in. It has to agree with
 * web_font_scale(), or lines wrap at a width the renderer does not honour. */
static int omi_faces_ready;   /* set once SYS_FONT has handed over the outlines */

static int user_measure(int font, const char *s) {
  if (omi_faces_ready) return omi_face_measure(font, s);
  return (int)strlen(s) * GLYPH_W * web_font_scale(font);
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
  for (unsigned ti = 0; ti < sizeof PAGE_TARGETS / sizeof PAGE_TARGETS[0]; ti++) {
    if (ti) {                       /* the next host, and say so in the log */
      snprintf(cur_host, sizeof cur_host, "%s", PAGE_TARGETS[ti].host);
      snprintf(cur_path, sizeof cur_path, "%s", PAGE_TARGETS[ti].path);
      snprintf(cur_url, sizeof cur_url, "https://%s%s", cur_host, cur_path);
      printf("[net] %s did not answer, trying %s\n", PAGE_TARGETS[ti - 1].host, cur_host);
    }
    for (int attempt = 1; attempt <= 3; attempt++) {
      blen = 0;
      why = "not tried";
      snprintf(sb, sizeof sb, "loading %s (%d/3)", cur_host, attempt);
      show_status(sb);
      status = https_get(cur_host, 443, cur_path, page, (size_t)page_cap, &why, &blen, ip);
      printf("[net] https://%s%s -> %s attempt %d status=%ld body=%ld %s\n",
             cur_host, cur_path, ip[0] ? ip : "?", attempt, status, blen, why);
      if (status > 0 && blen > 0) break;   /* 200, or a redirect to follow */
      sys3(SYS_YIELD, 0, 0, 0);
    }
    if (status > 0 && blen > 0) break;
  }
  last_status = status; last_blen = blen;
  memcpy(last_ip, ip, sizeof ip);
  status_msg[0] = 0;                  /* the strip goes back to the URL */
  content_h = 0;
  if (status == 200 && blen > 0) {
    err_msg[0] = 0;
    /* A body that filled the buffer is a body that did not fit: the rest was
     * never even read, and the page below is only the beginning of it. Saying so
     * on the strip is the difference between "this page is short" and "this
     * browser is broken". */
    int clipped = fetch_clipped;
    img_cache_reset();
    if (web_build(page, (int)blen, measure_w, user_measure) == 0) {
      content_h = web_height(); page_gen++;
      fields_reset();
      int full = web_ops_full();
      if (clipped && full)
        snprintf(status_msg, sizeof status_msg, "first %d KB only: body and display list both full", page_cap / 1024);
      else if (clipped)
        snprintf(status_msg, sizeof status_msg, "first %d KB only", page_cap / 1024);
      else if (full)
        snprintf(status_msg, sizeof status_msg, "page too long to draw in full");
      if (clipped || full)
        printf("[net] incomplete: clipped=%d display_list_full=%d bytes=%ld of %d\n",
               clipped, full, blen, page_cap);
    } else {
      printf("[net] the engine refused the page\n");
    }
  } else if (status > 0) {
    /* A redirect with nowhere to go is the only 3xx that is an error. */
    snprintf(err_msg, sizeof err_msg, "HTTP %ld from %s, nothing to follow", status, cur_host);
  } else {
    /* Said out loud, on screen: a page that will not load and a browser that
     * has stopped working look the same if the failure is only on the serial
     * console, and the one thing a reader must never do is lie about it. */
    snprintf(err_msg, sizeof err_msg, "cannot load %s: %s", cur_host, why);
    /* And in the log, with the reason: a fetch that fails once in a while is
     * worth being able to name, and "it was flaky" is not a reason. */
    printf("[net] load failed: %s (%s)\n", cur_host, why);
  }
  return status;
}

/* Both are defined further down, and these helpers sit in front of them because
 * paint() is where the widgets are drawn and submit_form() is how a widget
 * navigates. */
static void paint(long win, int off);
static void go(const char *href);

/* Reconcile the client's idea of its size with the window's, before drawing.
 *
 * The size only ever arrived as an event in the queue, so a resize that happened
 * while the client was busy - loading a page, decoding an image - was invisible
 * until the load finished: the window was already bigger while the surface, the
 * page's layout and the scroll limit were all still the old, smaller ones. That
 * stale limit is why a wheel on a maximised page stopped a third of the way down.
 * Asking the window itself costs one syscall per paint and cannot be missed. */
static int sync_geometry(void) {
  if (win_id <= 0) return 0;
  int32_t g[4] = {0, 0, 0, 0};
  if (sys3(SYS_WIN_GEOM, win_id, (long)g, 0) != 0) return 0;
  int nw = g[2] - 2, nh = g[3] - 32;
  if (nw < 80) nw = 80;
  if (nh < 60) nh = 60;
  if (nw == cw && nh == ch) return 0;

  long r = sys3(SYS_SURF_RESIZE, win_id, nw, nh);
  if (r != 0) return 0;                 /* the surface could not follow: keep going */
  cw = nw;
  ch = nh;
  {
    unsigned *ns = (unsigned *)malloc((size_t)cw * (size_t)ch * sizeof(unsigned));
    if (ns) { free(shadow); shadow = ns; }
  }
  measure_w = page_measure();
  if (last_blen > 0 && web_build(page, (int)last_blen, measure_w, user_measure) == 0) {
    content_h = web_height(); page_gen++;
    fields_reset();
  }
  int vp2 = ch - CONTENT_TOP - 4;
  vmax = content_h > vp2 ? content_h - vp2 : 0;
  if (scroll > vmax) scroll = vmax;
  if (scroll < 0) scroll = 0;
  printf("[net] geometry %dx%d, content %d, vmax %d\n", cw, ch, content_h, vmax);
  return 1;
}

/* A field is drawn by us, not by the display list: the box comes from the engine,
 * the text in it changes every keystroke. */
static void draw_field(int x, int y, int w, int h, int idx, int focused) {
  fill(x, y, w, h, 0x00FFFFFFu);
  unsigned b = focused ? C_ACCENT : 0x00A8A49Au;
  fill(x, y, w, 1, b);
  fill(x, y + h - 1, w, 1, b);
  fill(x, y, 1, h, b);
  fill(x + w - 1, y, 1, h, b);
  const char *t = (idx >= 0 && idx < FIELD_MAX) ? fval[idx] : "";
  int room = (w - 8) / GLYPH_W;
  if (room < 1) room = 1;
  int len = (int)strlen(t);
  int caret = (idx >= 0 && idx < FIELD_MAX) ? fcaret[idx] : len;
  if (caret > len) caret = len;
  if (caret < 0) caret = 0;

  /* Scroll the text so the caret stays visible, and draw from that offset. */
  int off = 0;
  if (caret > room) off = caret - room;
  if (off > len) off = len;
  const char *show = t + off;
  int shown = len - off;
  text(x + 4, y + (h - GLYPH_H) / 2, show, (len || focused) ? 0x001A1B1Du : 0x009A9790u);

  if (idx >= 0 && idx < FIELD_MAX) {
    int a = fsel0[idx] < fsel1[idx] ? fsel0[idx] : fsel1[idx];
    int bb = fsel0[idx] < fsel1[idx] ? fsel1[idx] : fsel0[idx];
    if (bb > a && focused) {
      /* Selected characters are repainted on an accent bar: no compositing here
       * beyond opaque fills, so the text is redrawn white over it. */
      int sx = x + 4 + (a - off) * GLYPH_W;
      int sw = (bb - a) * GLYPH_W;
      if (sx + sw > x + w - 3) sw = x + w - 3 - sx;
      if (sw > 0 && sx < x + w - 3) {
        fill(sx, y + 3, sw, h - 6, C_ACCENT);
        char sel[FIELD_TEXT + 1];
        int n = (int)sizeof sel - 1;
        if (n > bb - a) n = bb - a;
        memcpy(sel, t + a, (size_t)n);
        sel[n] = 0;
        text(sx, y + (h - GLYPH_H) / 2, sel, 0x00FFFFFFu);
      }
    }
    if (focused) {
      int cx = x + 4 + (caret - off) * GLYPH_W;
      if (cx > x + w - 3) cx = x + w - 3;
      if (cx >= x + 2) fill(cx, y + 3, 2, h - 6, C_ACCENT);
    }
  }
  (void)shown;
}

static void draw_button(int x, int y, int w, int h, const char *label) {
  fill(x, y, w, h, 0x00E9E4DAu);
  unsigned b = 0x00A8A49Au;
  fill(x, y, w, 1, b);
  fill(x, y + h - 1, w, 1, b);
  fill(x, y, 1, h, b);
  fill(x + w - 1, y, 1, h, b);
  int tw = (int)strlen(label) * GLYPH_W;
  text(x + (w - tw) / 2, y + (h - GLYPH_H) / 2, label, 0x001A1B1Du);
}

/* The widget under a point, in page coordinates. Returns the field index for a
 * text box, the form index for a button, or -1. */
static int hit_ctl_x, hit_ctl_y, hit_ctl_w;
static int hit_control(int mx, int my, int *is_button) {
  const web_op_t *ops = web_ops();
  int n = web_count();
  for (int i = 0; i < n; i++) {
    const web_op_t *o = &ops[i];
    if (o->kind != 4 && o->kind != 5) continue;
    if (mx >= o->x && mx < o->x + o->w && my >= o->y && my < o->y + o->h) {
      if (is_button) *is_button = (o->kind == 5);
      hit_ctl_x = o->x; hit_ctl_y = o->y; hit_ctl_w = o->w;
      return o->iw;
    }
  }
  return -1;
}

/* Send a form. GET only: the fetch path has no request body, and pretending
 * otherwise would post an empty form to a server that expects fields. */
static void submit_form(int form) {
  if (form < 0) {
    snprintf(err_msg, sizeof err_msg, "nothing to submit: the control is not in a form");
    paint(win_id, scroll);
    return;
  }
  const char *m = web_form_method(form);
  if (m && (m[0] == 'p' || m[0] == 'P')) {
    snprintf(err_msg, sizeof err_msg, "this form posts, which is not supported yet");
    paint(win_id, scroll);
    return;
  }

  char q[1024];
  int o = 0;
  q[0] = 0;
  int n = web_field_count();
  for (int i = 0; i < n && o < (int)sizeof q - 64; i++) {
    if (web_field_form(i) != form) continue;
    const char *nm = web_field_name(i);
    if (!nm || !*nm) continue;              /* nameless: nothing to send */
    if (o) q[o++] = '&';
    o += url_escape(q + o, (int)sizeof q - o - 1, nm);
    if (o < (int)sizeof q - 2) q[o++] = '=';
    o += url_escape(q + o, (int)sizeof q - o - 1,
                    (i < FIELD_MAX) ? fval[i] : web_field_value(i));
    q[o] = 0;
  }

  const char *a = web_form_action(form);
  char action[200];
  if (!a || !*a) snprintf(action, sizeof action, "/");      /* this page */
  else snprintf(action, sizeof action, "%s", a);

  char url[512];
  const char *sep = strchr(action, '?') ? "&" : "?";
  if (action[0] == '/')
    snprintf(url, sizeof url, "%s%s%s", action, o ? sep : "", q);
  else if (!strncmp(action, "http", 4))
    snprintf(url, sizeof url, "%s%s%s", action, o ? sep : "", q);
  else
    snprintf(url, sizeof url, "/%s%s%s", action, o ? sep : "", q);

  printf("[net] submit form %d -> %s\n", form, url);
  err_msg[0] = 0;
  focus_field = -1;
  go(url);
}

/* Which build of the display list is in the frame buffer. A scroll can copy
 * pixels only while both were produced from the same list. */
static unsigned page_gen;

static void paint(long win, int off) {
  long t_paint = sys3(SYS_TICKS, 0, 0, 0);
  sync_geometry();
  pix = shadow ? shadow : surf;

  /* Scroll by copying, not by rasterising. Everything above the fold that stays
   * on screen after a scroll is the same pixels one step up or down, so the rows
   * that move are a memmove and only the strip that came into view is drawn. The
   * alternative - re-walking the display list and re-blitting every glyph - was
   * most of the 10ms a scroll frame took here. Safe only when the frame being
   * replaced came from the same display list (page_gen) at the same geometry, so
   * a new page or a resize falls back to a full paint. */
  int vp = ch - CONTENT_TOP - 4;
  /* The build stamp, the status line and the exact-pixel marker are chrome drawn
   * over the page. They must not travel with a scroll or the copied frame keeps a
   * ghost of them halfway up the page, so the rows they live in are never moved -
   * they are redrawn every frame as a band of their own. */
  const int strip = 24;
  int svp = vp - strip;                  /* rows a scroll may copy */
  int bt[2], bb[2], nb = 0;              /* the bands that have to be drawn */
  int blit = 0;
  {
    static int bl_off = 0, bl_cw = -1, bl_ch = -1, bl_cont = -1;
    static unsigned bl_gen = 0;
    static const void *bl_ops;
    int step = off - bl_off;
    int d = step > 0 ? step : -step;
#ifdef OMI_NO_BLIT
#warning "blit disabled for verification"
    /* Verification switch: same build, frames always fully drawn, so a frame the
     * copy path produced can be compared pixel for pixel with one the renderer
     * produced. Nothing should set this in a shipped build. */
    const int blit_ok = 0;
#else
    const int blit_ok = 1;
#endif
    if (blit_ok && shadow && !editing && step != 0 && bl_cw == cw && bl_ch == ch &&
        bl_cont == content_h && bl_gen == page_gen &&
        bl_ops == (const void *)web_ops() && d < svp) {
      if (step > 0) {
        /* scrolled down: content moves up */
        memmove(shadow + (size_t)CONTENT_TOP * cw,
                shadow + (size_t)(CONTENT_TOP + d) * cw,
                (size_t)(svp - d) * cw * sizeof *shadow);
        /* what came into view, and the chrome strip under it */
        bt[nb] = CONTENT_TOP + svp - d; bb[nb] = CONTENT_TOP + vp; nb++;
      } else {
        memmove(shadow + (size_t)(CONTENT_TOP + d) * cw,
                shadow + (size_t)CONTENT_TOP * cw,
                (size_t)(svp - d) * cw * sizeof *shadow);
        bt[nb] = CONTENT_TOP; bb[nb] = CONTENT_TOP + d; nb++;
        bt[nb] = CONTENT_TOP + svp; bb[nb] = CONTENT_TOP + vp; nb++;
      }
      for (int i = 0; i < nb; i++)
        fill(0, bt[i], cw, bb[i] - bt[i], show_ops ? web_bg() : C_BG);
      blit = 1;
    }
    bl_off = off; bl_cw = cw; bl_ch = ch; bl_cont = content_h;
    bl_gen = page_gen; bl_ops = (const void *)web_ops();
  }
  if (!blit) { bt[0] = CONTENT_TOP; bb[0] = CONTENT_TOP + vp; nb = 1; fill(0, 0, cw, ch, C_BG); }
  fill(0, 0, cw, 30, C_PANEL);
  /* Four buttons, left of the address: back, forward, home, reload. Drawn as
   * glyphs rather than an icon sheet - the face has '<', '>' and friends. */
  {
    const char *lbl[4] = { "<", ">", "H", "R" };
    int dis[4];
    dis[0] = (hist_i <= 0);
    dis[1] = (hist_i < 0 || hist_i + 1 >= hist_n);
    dis[2] = 0;
    dis[3] = 0;
    for (int i = 0; i < 4; i++) {
      int bx = MARGIN + i * BTN_W;
      text(bx + 5, 7, lbl[i], dis[i] ? C_DIM : C_TEXT);
    }
  }
  if (editing) {
    /* The field itself, drawn over the URL strip. The caret is a two-pixel bar
     * rather than a blinking cursor: this paints on demand, not on a timer. */
    int fx = MARGIN + 4 * BTN_W + 6;
    fill(fx - 4, 4, cw - fx + 2, 22, C_BG);
    int room = (cw - (fx - MARGIN) - 4) / GLYPH_W;
    if (room < 1) room = 1;
    const char *show = edit;
    if (edit_len > room) show = edit + (edit_len - room);   /* keep the caret in view */
    text(fx, 7, show, C_TEXT);
    fill(fx + (int)strlen(show) * GLYPH_W, 7, 2, GLYPH_H, C_ACCENT);
  } else {
    /* The URL is the one thing in the chrome that can be any length. */
    int room = (cw - (MARGIN + 4 * BTN_W + 6)) / GLYPH_W;
    char bar[128];
    int n = (int)strlen(cur_url);
    if (room > 126) room = 126;
    if (n <= room) { memcpy(bar, cur_url, n + 1); }
    else { memcpy(bar, cur_url, room - 3); memcpy(bar + room - 3, "...", 4); }
    /* The strip is the address, always. It used to show the last error instead,
     * which hid where you were and read as a broken window: the one thing an
     * address bar must never stop doing is tell you the address, and the one
     * thing you must always be able to do is type a new one. */
    text(MARGIN + 4 * BTN_W + 6, 7, bar, status_msg[0] ? C_TEXT : C_DIM);
  }
  fill(0, 30, cw, 1, C_ACCENT);
  fill(0, 31, cw, 3, C_BG);

  /* The display list is in page coordinates, origin at the content's top left.
   * Scrolling is a subtraction - which is the reason to keep a display list
   * instead of re-running layout for every frame. */
  int page_dx = (cw - 2 * MARGIN - measure_w) / 2;
  if (page_dx < 0) page_dx = 0;
  int px0 = MARGIN + page_dx;
  /* The document's paper, not the window's: the chrome is dark so the window
   * reads as a window, and the page is whatever colour the page says it is. */
  if (!blit) fill(0, CONTENT_TOP, cw, ch - CONTENT_TOP, show_ops ? web_bg() : C_BG);
  int n = show_ops ? web_count() : 0;
  const web_op_t *ops = web_ops();
  unsigned long np_drawn = 0, np_chars = 0;
  unsigned long np_font[5] = {0, 0, 0, 0, 0};   /* which faces the page asks for */
  unsigned long np_bold = 0, np_runs = 0;
  unsigned long long cyc0 = __builtin_ia32_rdtsc();
  for (int bi = 0; bi < nb; bi++) {
  const int band_top = bt[bi], band_bot = bb[bi];
  clip_rect(px0, band_top, px0 + measure_w, band_bot);
  for (int i = 0; i < n; i++) {
    const web_op_t *o = &ops[i];
    int oy = CONTENT_TOP + o->y - off;
    int oh = o->h > 0 ? o->h : GLYPH_H;
    if (oy > band_bot || oy + oh < band_top) continue;
    np_drawn++;
    if (o->kind == 0) {
      if (o->h > 0) fill(px0 + o->x, oy, o->w, o->h, o->color);
    } else if (o->kind == 2) {
      fill(px0 + o->x, oy, o->w, 1, o->color);
    } else if (o->kind == 4) {
      draw_field(px0 + o->x, oy, o->w, o->h > 0 ? o->h : 20, o->iw, o->iw == focus_field);
    } else if (o->kind == 5) {
      draw_button(px0 + o->x, oy, o->w, o->h > 0 ? o->h : 22, o->text ? o->text : "Submit");
    } else if (o->kind == 3 && o->px && o->iw > 0 && o->ih > 0) {
      /* Nearest neighbour: a page's images are shown at the size the page asked
       * for, and a filter that averages pixels would cost more than it shows. */
      for (int yy = 0; yy < o->h; yy++) {
        int py_ = oy + yy;
        if (py_ < 0 || py_ >= ch) continue;
        const uint32_t *srow = o->px + (size_t)(yy * o->ih / o->h) * (size_t)o->iw;
        for (int xx = 0; xx < o->w; xx++) {
          int px_ = px0 + o->x + xx;
          if (px_ < 0 || px_ >= cw) continue;
          pix[(size_t)py_ * cw + px_] = srow[xx * o->iw / o->w] & 0xFFFFFFu;
        }
      }
    } else if (o->text) {
      np_chars += (unsigned long)strlen(o->text);
      np_font[o->font < 0 ? 0 : o->font > 4 ? 4 : o->font]++;
      np_runs++; if (o->bold) np_bold++;
      if (omi_faces_ready) {
        /* The line box is the face's own height now, so the baseline is where the
         * metrics say it is - not a fixed 12px from the top of a bitmap. */
        int base = oy + web_font_ascent(o->font);
        omi_face_draw(o->font, px0 + o->x, base, o->text, o->color);
        /* <strong> and <b>: the layout marks the run, the face table does not
         * know it, so weight it the same way the bitmap path did. */
        if (o->bold) omi_face_draw(o->font, px0 + o->x + 1, base, o->text, o->color);
      } else {
        int sc = web_font_scale(o->font);
        text_scaled(px0 + o->x, oy, o->text, o->color, sc);
        if (o->bold) text_scaled(px0 + o->x + sc, oy, o->text, o->color, sc);
      }
    }
  }
  }
  clip_none();
  if (dbg_ink) {
    int ink = 0;
    for (int y = CONTENT_TOP; y < ch; y++)
      for (int x = 0; x < cw; x++)
        if ((pix[y * cw + x] & 0xFFFFFF) < 0x404040u) ink++;
    printf("[net] ink=%d of %dx%d\n", ink, cw, ch);
  }

  /* A scrollbar, because a page that can move should say so - and because it is
   * the one element whose position an outside check can predict. */
  vmax = content_h > vp ? content_h - vp : 0;
  {
    /* Where the scrollbar actually is, so input tests can aim instead of guess. */
    static int gc = -1, gh = -1, gm = -1;
    if (cw != gc || ch != gh || measure_w != gm) {
      gc = cw; gh = ch; gm = measure_w;
      printf("[net] geom cw=%d ch=%d vp=%d vmax=%d px0=%d bar_x=%d track_y=%d track_h=%d\n",
             cw, ch, vp, vmax, px0, cw - 13, CONTENT_TOP, vp);
    }
  }
  if (vmax > 0) {
    int track_y = CONTENT_TOP, track_h = vp;
    int bar_h = track_h * vp / content_h;
    if (bar_h < 12) bar_h = 12;
    int bar_y = track_y + (track_h - bar_h) * off / vmax;
    /* Six pixels at cw-13, not five at cw-5: a scrollbar under the window
     * manager's edge zone can never be grabbed, and 3px of border is not a
     * target anyone can hit. */
    fill(cw - 13, track_y, 6, track_h, C_PANEL);
    fill(cw - 13, bar_y, 6, bar_h, C_DIM);
  }

  /* A message gets its own line at the bottom, over the page: visible without
   * taking the address away, and it does not move anything else on screen. */
  if (err_msg[0] || status_msg[0]) {
    const char *msg = err_msg[0] ? err_msg : status_msg;
    int my = ch - GLYPH_H - 6;
    fill(0, my - 2, cw, GLYPH_H + 6, C_PANEL);
    fill(0, my - 2, cw, 1, err_msg[0] ? C_ACCENT : C_DIM);
    int room = (cw - 2 * MARGIN) / GLYPH_W;
    if (room < 4) room = 4;
    char line[96];
    int n = (int)strlen(msg);
    if (n > room - 1) n = room - 1;
    if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
    memcpy(line, msg, (size_t)n);
    line[n] = 0;
    text(MARGIN, my, line, err_msg[0] ? C_ACCENT : C_TEXT);
  }

  /* Which build this is. Small and dim, next to the scrollbar, so a screenshot
   * answers the question "did that fix get into the thing I am looking at". */
  {
    int sw = (int)strlen(OMI_BUILD) * GLYPH_W;
    if (cw > sw + 40) text(cw - 20 - sw, ch - GLYPH_H - 8, OMI_BUILD, C_DIM);
  }

  { static int told;
    if (np_chars && told++ < 3)
      printf("[net] faces used 12px:%lu 14px:%lu mono:%lu 17px:%lu 26px:%lu bold:%lu of %lu\n",
             np_font[3], np_font[0], np_font[2], np_font[4], np_font[1], np_bold, np_runs); }

  /* the exact-pixel marker: chrome, so it does not move with the page */
  fill(MARGIN, ch - 6, 24, 4, C_MARK);

  /* What one frame costs. Printed when the scroll position changes, because that
   * is the moment someone notices a frame, and a number beats an impression of
   * one. ops is the size of the display list walked; drawn is how much of it was
   * on screen; ms is the whole paint, rounded up to the 10ms tick. */
  {
    static int last_off = -1;
    if (off != last_off) {
      long dt = sys3(SYS_TICKS, 0, 0, 0) - t_paint;
      unsigned long long cyc = __builtin_ia32_rdtsc() - cyc0;
      last_off = off;
      printf("[net] paint off=%d ops=%d drawn=%lu chars=%lu ms=%ld %s bands=%d %d..%d cyc=%llu\n",
             off, n, np_drawn, np_chars, (dt + 1) * 10,
             blit ? "blit" : "full", nb, bt[0], bb[0], cyc);
    }
  }

  /* The frame is complete: hand it over in one copy, with nothing in between that
   * could let the compositor draw. */
  if (shadow) {
    memcpy(surf, shadow, (size_t)cw * (size_t)ch * sizeof(unsigned));
    pix = surf;
  }
  sys3(SYS_SURF_PRESENT, win, 0, ((long)cw << 16) | ch);
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
    memcpy(host, cur_host, sizeof host < sizeof cur_host ? sizeof host : sizeof host);
    host[sizeof host - 1] = 0;
  } else if (*p && strchr(p, '.')) {
    /* Something with a dot in it is a hostname. */
    int i = 0;
    while (p[i] && p[i] != '/' && i < 127) { host[i] = p[i]; i++; }
    host[i] = 0;
    p += i;
  } else if (*p) {
    /* No scheme, no leading slash, no dot: a path relative to the page we are on.
     * Treating it as a host asked DNS about a page name - "cannot load document:
     * resolve" was the browser hunting for a website called /document. */
    memcpy(host, cur_host, sizeof host < sizeof cur_host ? sizeof host : sizeof host);
    host[sizeof host - 1] = 0;
    static char rel[256];
    int cut = (int)strlen(cur_path);
    while (cut > 0 && cur_path[cut - 1] != '/') cut--;
    snprintf(rel, sizeof rel, "%.*s%s", cut, cur_path, p);
    snprintf(path, sizeof path, "%s", rel);
    p = path;
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
    /* The fetch is synchronous, so the click has to be acknowledged before it
     * starts or the window looks dead for the whole round trip. */
    snprintf(status_msg, sizeof status_msg, "Loading %s", host);
    paint(win_id, scroll);
    long st = load_page();
    status_msg[0] = 0;
    scroll = 0;
    text_glyph_pixels = 0;
    paint(win_id, 0);
    if (st == 200 && content_h > 0) {
      if (!hist_silent) {
        /* A new page is a new entry; going forward after going back replaces the
         * tail, which is what every browser does. */
        if (hist_n == 0 || strcmp(hist[hist_i < 0 ? 0 : hist_i], cur_url) != 0) {
          if (hist_i + 1 < hist_n) hist_n = hist_i + 1;
          if (hist_n < HIST_MAX) {
            snprintf(hist[hist_n], HIST_URL, "%s", cur_url);
            hist_i = hist_n++;
          } else {
            memmove(hist[0], hist[1], (size_t)(HIST_MAX - 1) * HIST_URL);
            snprintf(hist[HIST_MAX - 1], HIST_URL, "%s", cur_url);
            hist_i = HIST_MAX - 1;
          }
        }
      }
      printf("[net] followed: %d ops, content %d, viewport %d\n",
             web_count(), content_h, ch - CONTENT_TOP - 4);
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
  /* The page buffer first: everything below assumes it exists. */
  page = (char *)malloc(PAGE_MAX);
  page_cap = PAGE_MAX;
  if (!page) {
    page = (char *)malloc(PAGE_FLOOR);
    page_cap = PAGE_FLOOR;
    printf("[net] no room for %d bytes: pages are capped at %d\n", PAGE_MAX, PAGE_FLOOR);
  }
  if (!page) { printf("[net] no page buffer at all\n"); return 1; }

  /* Real outlines, if the kernel has them. Both the wrapper and the painter take
   * their numbers from these, which is what keeps a line inside its box. */
  if (omi_face_init()) {
    omi_faces_ready = 1;
    web_font_metrics(omi_face_line_h, omi_face_ascent);
    printf("[net] faces: display %d bytes, mono %d bytes\n",
           (int)(omi_font_bytes[0] ? 1 : 0), (int)(omi_font_bytes[1] ? 1 : 0));
    printf("[net] line heights %d/%d/%d/%d/%d ascent %d\n",
           omi_face_line_h(0), omi_face_line_h(1), omi_face_line_h(2),
           omi_face_line_h(3), omi_face_line_h(4), omi_face_ascent(0));
  } else {
    printf("[net] no outlines: keeping the 8x16 bitmap face\n");
  }

  win_id = sys3(SYS_WIN_OPEN, (long)"Omi Net", WIN_W, WIN_H);
  check(win_id > 0, "the window opens", win_id, 1);

  int32_t g[4] = {0, 0, 0, 0};
  if (sys3(SYS_WIN_GEOM, win_id, (long)g, 0) == 0)
    printf("[net] client at %d,%d size %dx%d\n",
           g[0] + 1, g[1] + 31, g[2] - 2, g[3] - 32);

  long s = sys3(SYS_SURF_CREATE, win_id, cw, ch);
  check(s > 0, "the surface maps into the process", s, 1);
  if (s <= 0) { printf("[net] 0/%d\n", checks); return 1; }
  surf = (unsigned *)s;
  pix = surf;
  shadow = (unsigned *)malloc((size_t)cw * (size_t)ch * sizeof(unsigned));
  if (shadow) pix = shadow;
  else printf("[net] no shadow frame: painting the surface directly\n");
  measure_w = page_measure();
  web_image_provider(provide_image);
  web_css_provider(provide_css);
  printf("[net] omi net build %s\n", OMI_BUILD);
  /* The faces arrive as boot modules, not as files, so this path is dead - it has
   * always returned -1 and printing it only ever looked like a failure. The real
   * load is omi_face_init() at the top of main. */
  (void)font_load;

  long t_start = sys3(SYS_TICKS, 0, 0, 0);

  /* First frame, before anything that can fail or take time. The window exists
   * from SYS_WIN_OPEN onward, but its surface holds whatever the compositor last
   * had there - black - until this process presents something. A fetch is
   * seconds at best and unbounded at worst, so the browser used to be an
   * unlit rectangle for that whole time: a reader cannot tell that apart from a
   * process that died. Chrome and a status line go up first, always. */
  show_status("starting");

  printf("[net] t=first-fetch %ldms after launch\n",
         (sys3(SYS_TICKS, 0, 0, 0) - t_start) * 10);
  load_page();
  /* The first page is history entry zero, or Back has nowhere to go from it. */
  if (content_h > 0) {
    snprintf(hist[0], HIST_URL, "%s", cur_url);
    hist_n = 1;
    hist_i = 0;
  }
  printf("[net] t=page-painted %ldms after launch\n",
         (sys3(SYS_TICKS, 0, 0, 0) - t_start) * 10);

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
    /* Measured the way it will be painted. This check used to assume the 8x16
     * bitmap - eight pixels a character - which stopped meaning anything the
     * moment the page got real outlines, and reported a 430px run in a 422px
     * column that was in fact 422px wide. */
    int r = o->x + user_measure(o->font, o->text);
    if (r > maxr) maxr = r;
  }
  printf("[net] layout %d ops, %d text runs, widest %d px, height %d\n",
         nops, runs, maxr, content_h);
  printf("[net] css: %d rules, %d custom properties in use\n", web_rule_count(), web_var_count());
  check(nops > 0, "the page produced a display list", nops, 1);
  check(runs >= 4, "the page has text runs to paint", runs, 4);
  check(maxr <= measure_w, "no text run escapes the measure", maxr, measure_w);
  check(content_h > ch - CONTENT_TOP,
        "the page is taller than the viewport, so it scrolls", content_h, ch);

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

  int my = ch - 6;
  printf("[net] marker at %d,%d 24x4\n", MARGIN, my);
  check(pix[my * cw + MARGIN] == C_MARK, "the marker is in the surface",
        pix[my * cw + MARGIN], (long)C_MARK);
  long r = sys3(SYS_SURF_PRESENT, win_id, 0, ((long)cw << 16) | ch);
  check(r == 0, "presenting the page succeeds", r, 0);


  /* ---- the self-checks come after the reader has their page ----
   * They prove the engine's contract with a compiled-in fixture, and they used to
   * run before the fetch: half a second of proof in front of every launch, in
   * front of a window that was already painted and waiting. Now the page is on
   * screen first and the checks rebuild the fixture over it, so the last thing
   * this block does is put the real page back. */
  /* ---- the fixture page: what the engine has to do with CSS ----
   * A fetched page is whatever it is tomorrow, so the proof is compiled in: a
   * background colour, padding, a stylesheet colour on text, and a list. */
  static const char *const fixture =
    "<!doctype html><html><head><title>fixture</title>"
    "<style>p{color:#00ff00}</style></head><body>"
    "<div style=\"background:#3366cc;padding:12px\">boxed</div>"
    "<p>green paragraph text, long enough to wrap at this width, so the wrap "
    "is exercised as well as the colour</p>"
    "<ul><li>one item</li><li>two item</li></ul>"
    /* Latin-1 in UTF-8, an accented word, a copyright sign and an em dash: the
     * font cannot draw any of them, and they used to be dropped silently */
    "<p>Z\xc3\xbcrich \xc2\xa9 2019 \xe2\x80\x94 caf\xc3\xa9</p></body></html>";
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
  /* The fold: an accented word, a copyright sign and an em dash must reach the
   * display list as something the 8x16 ASCII font can actually draw. */
  int f_zurich = 0, f_c = 0, f_dash = 0, f_cafe = 0;
  for (int i = 0; i < web_count(); i++) {
    const web_op_t *o = &web_ops()[i];
    if (!o->text) continue;
    /* runs carry the separating space that preceded them */
    const char *t2 = o->text;
    while (*t2 == ' ') t2++;
    if (!strcmp(t2, "Zurich")) f_zurich = 1;
    else if (!strcmp(t2, "(c)")) f_c = 1;
    else if (!strcmp(t2, "cafe")) f_cafe = 1;
    else if (!strcmp(t2, "-")) f_dash = 1;
  }
  /* named, not counted: the fixture's two list items also draw a "-" marker */
  int folded = f_zurich + f_c + f_cafe + f_dash;
  check(folded == 4, "Latin-1 and typographic text folds to drawable ASCII", folded, 4);
  check(web_height() > 0, "the fixture has a height", web_height(), 1);

  /* put the fetched page back: the fixture proved the engine, it is not what the
   * reader asked for */
  if (last_blen > 0 && web_build(page, (int)last_blen, measure_w, user_measure) == 0) {
    content_h = web_height(); page_gen++;
    paint(win_id, 0);
  }

  printf("[net] %d/%d\n", checks - fails, checks);
  if (fails) {
    /* A page that did not load is not a reason to take the window away. The
     * reader came for a browser; one that disappears on a bad response leaves
     * them with an empty desktop and no explanation, and one that keeps its
     * chrome can be pointed somewhere else without a relaunch. Drop the page,
     * keep the failure in the strip, and go on listening for a typed URL. */
    static const char *const blank = "<!doctype html><html><body></body></html>";
    web_build(blank, (int)strlen(blank), measure_w, user_measure);
    content_h = 0; page_gen++;
    if (!err_msg[0]) snprintf(err_msg, sizeof err_msg, "the page did not load");
    paint(win_id, 0);
  }

  /* ---- input: arrows scroll, a click follows a link ---- */
  int vp = ch - CONTENT_TOP - 4;
  vmax = content_h > vp ? content_h - vp : 0;
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
    } else if (ev.kind == WIN_EV_KEY && focus_field >= 0 && !editing) {
      /* A field has the keyboard: the address bar's shortcuts must not fire while
       * somebody is typing a query. `ev.x` carries the modifiers (see
       * winipc_key), so Ctrl+A is not the letter a. */
      int k = ev.code;
      int mods = ev.x;
      int i = focus_field;
      int len = (int)strlen(fval[i]);
      int a = fsel0[i] < fsel1[i] ? fsel0[i] : fsel1[i];
      int bb = fsel0[i] < fsel1[i] ? fsel1[i] : fsel0[i];
      int has_sel = (bb > a);
      if (i < 0 || i >= FIELD_MAX) { focus_field = -1; paint(win_id, scroll); continue; }

      if ((mods & EV_CTRL) && (k == 'a' || k == 'A')) {
        fsel0[i] = 0; fsel1[i] = len; fcaret[i] = len;
        paint(win_id, scroll);
      } else if ((mods & EV_CTRL) && (k == 'c' || k == 'C')) {
        int from = has_sel ? a : 0, to = has_sel ? bb : len;
        clip_n = to - from;
        if (clip_n > CLIP_MAX - 1) clip_n = CLIP_MAX - 1;
        memcpy(clip, fval[i] + from, (size_t)clip_n);
        clip[clip_n] = 0;
        printf("[net] copied %d chars\n", clip_n);
      } else if ((mods & EV_CTRL) && (k == 'v' || k == 'V')) {
        int at = has_sel ? a : fcaret[i];
        int del = has_sel ? bb - a : 0;
        int room = FIELD_TEXT - 1 - (len - del);
        int n = clip_n < room ? clip_n : room;
        memmove(fval[i] + at + n, fval[i] + at + del, (size_t)(len - at - del) + 1);
        memcpy(fval[i] + at, clip, (size_t)n);
        fcaret[i] = at + n;
        fsel0[i] = fsel1[i] = fcaret[i];
        paint(win_id, scroll);
      } else if (k == '\n' || k == '\r') {
        int f = field_form(i);
        if (f >= 0) submit_form(f);
        else { focus_field = -1; paint(win_id, scroll); }
      } else if (k == 8 || k == 127) {
        if (has_sel) {
          memmove(fval[i] + a, fval[i] + bb, (size_t)(len - bb) + 1);
          fcaret[i] = a;
        } else if (fcaret[i] > 0) {
          memmove(fval[i] + fcaret[i] - 1, fval[i] + fcaret[i], (size_t)(len - fcaret[i]) + 1);
          fcaret[i]--;
        }
        fsel0[i] = fsel1[i] = fcaret[i];
        paint(win_id, scroll);
      } else if (k == KEY_LEFT_K) {
        if (has_sel) fcaret[i] = a;
        else if (fcaret[i] > 0) fcaret[i]--;
        fsel0[i] = fsel1[i] = fcaret[i];
        paint(win_id, scroll);
      } else if (k == KEY_RIGHT_K) {
        if (has_sel) fcaret[i] = bb;
        else if (fcaret[i] < len) fcaret[i]++;
        fsel0[i] = fsel1[i] = fcaret[i];
        paint(win_id, scroll);
      } else if (k == KEY_HOME) {
        fcaret[i] = 0;
        fsel0[i] = fsel1[i] = 0;
        paint(win_id, scroll);
      } else if (k == KEY_END) {
        fcaret[i] = len;
        fsel0[i] = fsel1[i] = len;
        paint(win_id, scroll);
      } else if (k == KEY_ESC) {
        focus_field = -1;
        paint(win_id, scroll);
      } else if (k == '\t') {
        int n = web_field_count();
        for (int j = 1; j <= n; j++) {
          int c = (focus_field + j) % n;
          if (!web_field_hidden(c)) { focus_field = c; break; }
        }
        paint(win_id, scroll);
      } else if (k >= 32 && k < 127) {
        int at = has_sel ? a : fcaret[i];
        int del = has_sel ? bb - a : 0;
        if (len - del < FIELD_TEXT - 1) {
          memmove(fval[i] + at + 1, fval[i] + at + del, (size_t)(len - at - del) + 1);
          fval[i][at] = (char)k;
          fcaret[i] = at + 1;
          fsel0[i] = fsel1[i] = fcaret[i];
          paint(win_id, scroll);
        }
      }
    } else if (ev.kind == WIN_EV_KEY) {
      if (ev.code == '/') {                               /* open the address bar */
        editing = 1;
        edit_len = 0;
        edit[0] = 0;
        paint(win_id, scroll);
      } else {
        int before = scroll;
        int page = ch - CONTENT_TOP - 4 - 2 * step;   /* a screenful less a line at each end */
        if (page < step) page = step;
        if (ev.code == KEY_DOWN) scroll += step;
        else if (ev.code == KEY_UP) scroll -= step;
        else if (ev.code == KEY_PGDN) scroll += page;
        else if (ev.code == KEY_PGUP) scroll -= page;
        else if (ev.code == KEY_HOME) scroll = 0;
        else if (ev.code == KEY_END) scroll = vmax;
        if (scroll < 0) scroll = 0;
        if (scroll > vmax) scroll = vmax;
        if (scroll != before) {
          printf("[net] scroll %d/%d\n", scroll, vmax);
          text_glyph_pixels = 0;
          paint(win_id, scroll);
        }
      }
    } else if (ev.kind == WIN_EV_RESIZE) {
      /* The frame changed size. The pixels are ours, so the surface is resized
       * and the document is laid out again for the new measure - the page is
       * still in `page`, so this is a reflow, not a reload. */
      int nw = (ev.code >> 16) & 0xFFFF, nh = ev.code & 0xFFFF;
      if (nw < 80) nw = 80;
      if (nh < 60) nh = 60;
      if (nw != cw || nh != ch) {
        long r = sys3(SYS_SURF_RESIZE, win_id, nw, nh);
        printf("[net] resize %dx%d -> %ld\n", nw, nh, r);
        if (r == 0) {
          cw = nw; ch = nh;
          {
            unsigned *ns = (unsigned *)malloc((size_t)cw * (size_t)ch * sizeof(unsigned));
            if (ns) { free(shadow); shadow = ns; }
          }
          measure_w = page_measure();
          scroll = 0;
          if (last_blen > 0 && web_build(page, (int)last_blen, measure_w, user_measure) == 0) {
            content_h = web_height(); page_gen++;
            fields_reset();
          }
          int vp = ch - CONTENT_TOP - 4;
          vmax = content_h > vp ? content_h - vp : 0;
          text_glyph_pixels = 0;
          paint(win_id, 0);
        }
      }
    } else if (ev.kind == WIN_EV_WHEEL) {
      /* The wheel is the pointer's scroll: one notch is one line, the same step
       * the arrow keys use, and positive is away from the user - up the page. */
      int before = scroll;
      scroll -= ev.code * step;
      if (scroll < 0) scroll = 0;
      if (scroll > vmax) scroll = vmax;
      if (scroll != before) {
        printf("[net] wheel %d/%d\n", scroll, vmax);
        text_glyph_pixels = 0;
        paint(win_id, scroll);
      }
    } else if (ev.kind == WIN_EV_DOWN) {
      /* The chrome is the window's own: a click in the address strip edits the
       * URL instead of falling through to whatever the page has under it, and a
       * click on the scrollbar moves the page. Both used to do nothing, which
       * read as a dead window. */
      if (ev.y < CONTENT_TOP && ev.x < MARGIN + 4 * BTN_W) {
        int b = (ev.x - MARGIN) / BTN_W;
        if (b == 0 && hist_i > 0) {                      /* back */
          hist_silent = 1;
          hist_i--;
          go(hist[hist_i]);
          hist_silent = 0;
        } else if (b == 1 && hist_i >= 0 && hist_i + 1 < hist_n) {   /* forward */
          hist_silent = 1;
          hist_i++;
          go(hist[hist_i]);
          hist_silent = 0;
        } else if (b == 2) {                             /* home */
          go(HOME_URL);
        } else if (b == 3) {                             /* reload */
          go(cur_url);
        }
      } else if (ev.y < CONTENT_TOP) {
        editing = 1;
        edit_len = 0;
        for (const char *u = cur_url; *u && edit_len < (int)sizeof(edit) - 1; u++) edit[edit_len++] = *u;
        edit[edit_len] = 0;
        paint(win_id, scroll);
      } else if (ev.x >= cw - 16) {
        int vp2 = ch - CONTENT_TOP - 4;
        int vmax2 = content_h > vp2 ? content_h - vp2 : 0;
        if (vmax2 > 0) {
          int bar_h = vp2 * vp2 / content_h;
          if (bar_h < 12) bar_h = 12;
          int bar_y = CONTENT_TOP + (vp2 - bar_h) * scroll / vmax2;
          /* grab the thumb, or page towards the click */
          if (ev.y >= bar_y && ev.y < bar_y + bar_h) {
            drag_off = ev.y - bar_y;
            dragging_bar = 1;
          } else if (ev.y < bar_y) {
            scroll -= vp2 - step;
            if (scroll < 0) scroll = 0;
          } else {
            scroll += vp2 - step;
            if (scroll > vmax2) scroll = vmax2;
          }
          text_glyph_pixels = 0;
          paint(win_id, scroll);
        }
      } else {
        int is_button = 0;
        int pgx = ev.x - MARGIN - page_dx_now();
        int ctl = hit_control(pgx, ev.y - CONTENT_TOP + scroll, &is_button);
        if (ctl >= 0 && is_button) {
          printf("[net] button for form %d pressed\n", ctl);
          submit_form(ctl);
        } else if (ctl >= 0) {
          focus_field = ctl;
          if (ctl < FIELD_MAX) {
            int len = (int)strlen(fval[ctl]);
            int idx = (pgx - (hit_ctl_x + 4) + GLYPH_W / 2) / GLYPH_W;
            if (idx < 0) idx = 0;
            if (idx > len) idx = len;
            fcaret[ctl] = idx;
            fsel0[ctl] = fsel1[ctl] = idx;
          }
          paint(win_id, scroll);
        } else {
          /* Clicking the page puts the keyboard back where it was before a field
           * took it, and only then follows a link. */
          if (focus_field >= 0) focus_field = -1;
          const char *href = web_hit_link(pgx, ev.y - CONTENT_TOP + scroll);
          if (href) go(href);
          else paint(win_id, scroll);
        }
      }
    } else if (ev.kind == WIN_EV_MOVE && dragging_bar) {
      int vp2 = ch - CONTENT_TOP - 4;
      int vmax2 = content_h > vp2 ? content_h - vp2 : 0;
      int bar_h = vp2 * vp2 / content_h;
      if (bar_h < 12) bar_h = 12;
      if (vmax2 > 0 && vp2 > bar_h) {
        int rel = ev.y - CONTENT_TOP - drag_off;
        int ns = rel * vmax2 / (vp2 - bar_h);
        if (ns < 0) ns = 0;
        if (ns > vmax2) ns = vmax2;
        if (ns != scroll) {
          scroll = ns;
          text_glyph_pixels = 0;
          paint(win_id, scroll);
        }
      }
    } else if (ev.kind == WIN_EV_UP) {
      dragging_bar = 0;
    }
  }
  return 0;
}
