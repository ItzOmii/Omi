/* comp - the display server: it owns the frame and composites the windows.
 *
 * scene showed the inversion - a process owning the frame the display shows.
 * This is what a display server does with it. It asks the kernel which windows
 * have pixels (SYS_SURF_LIST), maps each one read-only (SYS_SURF_MAP), asks
 * where each window is (SYS_WIN_GEOM), and puts the pixels there itself, every
 * frame, in a background of its own with a title bar and an edge around each
 * window. The kernel draws none of it.
 *
 * The point is not that this is pretty. It is that the process putting windows
 * on the screen never drew any of their contents: an engine brings its own
 * compositor, and this is the shape it has to plug into. What it still needs is
 * what this program does not have - input, and window furniture that belongs to
 * the server rather than to whoever owns the window.
 *
 * It also takes the raw input: the kernel's shell stops routing keys and clicks
 * and hands the events over, and this process hit-tests them against what it
 * drew last frame, decides who has focus, and delivers each event to a window
 * (SYS_WIN_INPUT). That is the other half of being a display server.
 *
 * It waits for whoever had the screen to give it back (SYS_SCENE_HANDOFF(-1)
 * asks who that is) so it can be a second owner in the same boot instead of
 * fighting scene over the display.
 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "glyphs.h"   /* the kernel's 8x16 face: the server draws its own titles */

#define SYS_GETPID        5
#define SYS_WIN_GEOM      14   /* (id, int32_t out[4]) -> x, y, w, h */
#define SYS_SCENE_MAP     42
#define SYS_SCENE_INFO    43
#define SYS_SCENE_FLUSH   44
#define SYS_SCENE_HANDOFF 45
#define SYS_SURF_MAP      47
#define SYS_SURF_LIST     48
#define SYS_INPUT_TAKE    49   /* (on) -> 1 this process owns the keyboard and pointer */
#define SYS_INPUT_POLL    50   /* (int32_t out[], max) -> count, kind/a/b/c each */
#define SYS_WIN_INPUT     51   /* (win, int32_t ev[4]) -> 0 delivered, -1 no window */
#define SYS_WIN_PLACE     52   /* (id, x, y, (w<<16)|h): the server placing a window */
#define SYS_WIN_CLOSE     10
#define SYS_WIN_TITLE     53   /* (id, char *out, max) -> length */

#define MAXWIN 12
#define BG      0x00101828u   /* the server's own desktop */
#define TITLE   0x00B03050u   /* furniture it draws itself */
#define EDGE    0x00E0E8F0u
#define SWEEP   0x00FFFFFFu
#define FOCUS   0x00E05070u   /* a title bar the server considers focused */
#define CURSOR  0x00FFFFFFu
#define BAR_H   22            /* the title bar, in pixels, drawn above the client */
#define TEXT    0x00F0F4FAu
#define CLOSE   0x00D84868u
#define CLOSEX  0x00FFFFFFu

#define IN_MOVE  1
#define IN_DOWN  2
#define IN_UP    3
#define IN_KEY   4
#define IN_WHEEL 5

#define EV_MOVE   1
#define EV_DOWN   2
#define EV_UP     3
#define EV_KEY    4
#define EV_CLOSE  5
#define EV_WHEEL  6

static long sys1(long n, long a) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a) : "memory");
  return r;
}

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

static void ms(int m) {
  struct timespec ts;
  ts.tv_sec = m / 1000;
  ts.tv_nsec = (long)(m % 1000) * 1000000L;
  nanosleep(&ts, 0);
}

static uint32_t *scene;
static uint32_t sw, sh, stride;

static void rect(long x, long y, long w, long h, uint32_t c) {
  for (long j = 0; j < h; j++) {
    long ty = y + j;
    if (ty < 0 || ty >= (long)sh) continue;
    uint32_t *row = (uint32_t *)((uint8_t *)scene + (size_t)ty * stride);
    for (long i = 0; i < w; i++) {
      long tx = x + i;
      if (tx < 0 || tx >= (long)sw) continue;
      row[tx] = c;
    }
  }
}

/* Rows, from whatever address the map handed over. */
static void blit(const uint32_t *px, long src_stride, long src_w, long src_h,
                 long dx, long dy) {
  for (long y = 0; y < src_h; y++) {
    long ty = dy + y;
    if (ty < 0 || ty >= (long)sh) continue;
    const uint32_t *srow = (const uint32_t *)((const uint8_t *)px + (size_t)y * src_stride);
    uint32_t *drow = (uint32_t *)((uint8_t *)scene + (size_t)ty * stride);
    for (long x = 0; x < src_w; x++) {
      long tx = dx + x;
      if (tx < 0 || tx >= (long)sw) continue;
      drow[tx] = srow[x];
    }
  }
}

/* The kernel's 8x16 face, drawn into the frame by the server. Furniture the
 * server renders needs the server to be able to write words, and this is the
 * smallest way to have any. */
static void glyphs_draw(long x, long y, const char *t, uint32_t col) {
  for (; *t; t++) {
    int ch = (unsigned char)*t;
    if (ch < GLYPH_FIRST || ch > GLYPH_LAST) ch = '?';
    const unsigned char *rows = glyphs[ch - GLYPH_FIRST];
    for (int r = 0; r < GLYPH_H; r++) {
      unsigned bits = rows[r];
      for (int c = 0; c < GLYPH_W; c++)
        if (bits & (0x80u >> c)) {
          long ty = y + r, tx = x + c;
          if (ty < 0 || ty >= (long)sh || tx < 0 || tx >= (long)sw) continue;
          *(uint32_t *)((uint8_t *)scene + (size_t)ty * stride + (size_t)tx * 4) = col;
        }
    }
    x += GLYPH_W;
  }
}

/* One window: the pixels the map gave us, and the size they arrived in. A
 * window's contents can change size under us, so the size is re-checked each
 * frame from the list; the stride is not, because a surface never moves. */
struct win {
  int      id;
  uint32_t *px;
  long     w, h, stride;
  long     x, y;          /* where the last frame put it */
  char     title[32];
};

/* Find the record for a window, or make one: map its pixels read-only once and
 * read its title once. The pixels never move for the life of the window; the
 * title can change, so it is re-read now and then. */
/* The server's own idea of the scene, kept from the last frame: input is
 * handled against this before the new frame is painted, the way a compositor
 * with a scene graph hit-tests against what it drew most recently. */
static struct win g_win[MAXWIN];      /* the scene graph, filled by the paint loop */
static int        g_stack[MAXWIN];   /* window ids, bottom of the stack first */
static int        g_nstack;
static int        g_focus;           /* window id, 0 = none */
static int        g_drag;            /* window id being dragged by its title bar */
static long       g_drag_x, g_drag_y;  /* where in the bar the pointer took hold */
static int        g_close_hover;
static long       g_mx = 640, g_my = 400;
static int        g_have_mouse;

static struct win *byid(int id) {
  for (int k = 0; k < MAXWIN; k++) if (g_win[k].id == id) return &g_win[k];
  return 0;
}

int g_mapped;                 /* windows mapped: a number for the closing report */

static struct win *win_map(int id) {
  static int map_calls;
  struct win *w = byid(id);
  if (w && w->px) {
    if ((map_calls++ % 120) == 0) sys3(SYS_WIN_TITLE, id, (long)w->title, sizeof w->title);
    return w;
  }
  if (!w) {
    for (int k = 0; k < MAXWIN; k++) if (!g_win[k].id) { w = &g_win[k]; break; }
    if (!w) return 0;
  }
  int32_t mi[3] = {0, 0, 0};
  long a = sys3(SYS_SURF_MAP, id, (long)mi, 0);
  if (a <= 0) return 0;                   /* closed between list and map */
  w->id = id;
  w->px = (uint32_t *)(uintptr_t)a;
  w->w = mi[0]; w->h = mi[1]; w->stride = mi[2];
  w->title[0] = 0;
  sys3(SYS_WIN_TITLE, id, (long)w->title, sizeof w->title);
  g_mapped++;
  printf("[comp] win %d: %ldx%ld at 0x%lx, stride %ld, \"%s\"\n",
         id, w->w, w->h, a, w->stride, w->title);
  return w;
}


/* Stacking is the server's, not the kernel's. The list the kernel hands over
 * says which windows have pixels; the order they are painted in is this one,
 * and it changes when somebody clicks - a window comes to the front because the
 * server says so. */
static int stack_index(int id) {
  for (int i = 0; i < g_nstack; i++) if (g_stack[i] == id) return i;
  return -1;
}

static void stack_add(int id) {
  if (stack_index(id) < 0 && g_nstack < MAXWIN) g_stack[g_nstack++] = id;
}

static void stack_raise(int id) {
  int i = stack_index(id);
  if (i < 0) return;
  for (; i + 1 < g_nstack; i++) g_stack[i] = g_stack[i + 1];
  g_stack[g_nstack - 1] = id;
}

/* Forget windows that closed. Called with the list the kernel just handed over,
 * so the stack cannot drift away from what still exists. */
static void stack_prune(const int32_t *list, int n) {
  int keep = 0;
  for (int i = 0; i < g_nstack; i++) {
    int found = 0;
    for (int k = 0; k < n; k++) if (list[k * 3 + 0] == g_stack[i]) { found = 1; break; }
    if (found) g_stack[keep++] = g_stack[i];
    else {
      struct win *w = byid(g_stack[i]);
      if (w) w->id = 0;                 /* the record goes with the window */
      if (g_focus == g_stack[i]) g_focus = 0;
      if (g_drag == g_stack[i]) g_drag = 0;
    }
  }
  g_nstack = keep;
}

/* Where in a window's furniture a point lands. The frame is the server's: the
 * bar above the client area, three pixels of edge around it, and a close box at
 * the right end of the bar. */
#define AT_OUTSIDE 0
#define AT_CLIENT  1
#define AT_TITLE   2
#define AT_CLOSE   3

static int frame_hit(struct win *w, long x, long y) {
  /* The frame is the server's: a bar BAR_H tall above the client area, three
   * pixels of edge around both, and a close box inside the right end of the bar.
   * The close box has to be tested before the bar, or the bar swallows it - it
   * did, and clicks on the x went to the title drag. */
  if (x < w->x - 3 || x >= w->x + w->w + 3) return AT_OUTSIDE;
  if (y >= w->y - BAR_H && y < w->y) {
    if (x >= w->x + w->w - 19 && x < w->x + w->w - 3 &&
        y >= w->y - 19 && y < w->y - 3) return AT_CLOSE;
    return AT_TITLE;
  }
  if (y >= w->y && y < w->y + w->h + 3) return AT_CLIENT;
  return AT_OUTSIDE;
}

/* Which window is under a point, topmost first: the stack read backwards. */
static int hit(long x, long y) {
  for (int i = g_nstack - 1; i >= 0; i--) {
    struct win *w = byid(g_stack[i]);
    if (w && frame_hit(w, x, y) != AT_OUTSIDE) return w->id;
  }
  return 0;
}

int main(void) {
  /* The scene has one owner at a time, so a second display server starts by
   * waiting for the first to let go of the mapping. */
  long addr = 0;
  for (int i = 0; i < 480 && addr <= 0; i++) {     /* up to two minutes */
    addr = sys1(SYS_SCENE_MAP, 0);
    if (addr <= 0) ms(250);
  }
  if (addr <= 0) { printf("[comp] FAIL scene map: %ld\n", addr); return 1; }
  uint32_t geom[4];            /* width, height, stride, as the kernel packs them */
  if (sys1(SYS_SCENE_INFO, (long)geom) != 0) { printf("[comp] FAIL scene info\n"); return 1; }
  scene = (uint32_t *)(uintptr_t)addr;
  sw = geom[0]; sh = geom[1]; stride = geom[2];
  printf("[comp] pid %ld: scene 0x%lx, %ux%u, stride %u\n",
         sys1(SYS_GETPID, 0), addr, sw, sh, stride);

  /* Wait for the screen's owner to let go of it. Nothing in the kernel forces
   * that to be orderly; asking who owns it is the polite way. */
  long owner = -1;
  for (int i = 0; i < 480; i++) {           /* up to two minutes */
    owner = sys1(SYS_SCENE_HANDOFF, -1);
    if (owner == 0) break;
    ms(250);
  }
  if (owner != 0) { printf("[comp] screen still owned by pid %ld, giving up\n", owner); return 1; }

  sys1(SYS_SCENE_HANDOFF, 1);
  printf("[comp] the screen is this process's; the kernel paints nothing\n");

  int frames = 0, bad_geom = 0;
  int worst = -1;

  long got = sys1(SYS_INPUT_TAKE, 1);
  printf("[comp] input: %s\n", got ? "the keyboard and pointer are this process's"
                                    : "refused - somebody else is serving");
  int keys = 0, clicks = 0, fwd = 0, refused = 0, moves = 0, drag_said = 0;

  for (int f = 0; f < 3000; f++) {           /* ~3 min at 60 ms */
    /* First the events, against the geometry of the frame that is on screen. */
    int32_t ev[4 * 32];
    int ne = (int)sys3(SYS_INPUT_POLL, (long)ev, 32, 0);
    for (int e = 0; e < ne; e++) {
      int32_t kind = ev[e * 4 + 0], a = ev[e * 4 + 1], b = ev[e * 4 + 2], c = ev[e * 4 + 3];
      int32_t out[4];
      if (kind == IN_MOVE) {
        g_mx = a; g_my = b; g_have_mouse = 1;
        moves++;
        printf("[comp] move %ld,%ld\n", g_mx, g_my);   /* every move: the test harness reads this */
        if (g_drag) {                        /* the title bar is the server's */
          struct win *dw = byid(g_drag);
          int32_t g[4];
          if (!dw || sys3(SYS_WIN_GEOM, g_drag, (long)g, 0) != 0) {
            g_drag = 0;
          } else {
            long nx = g_mx - g_drag_x, ny = g_my - g_drag_y;
            /* x in the low half, y in the high: the same packing the kernel
             * uses for SYS_WIN_RECT, with a syscall that lets the server move a
             * window it did not open. */
            long r = sys3(SYS_WIN_PLACE, g_drag,
                          (nx & 0xFFFFFFFFL) | (ny << 32),
                          ((long)g[2] << 16) | (long)g[3]);
            if (r != 0 && drag_said++ < 3) printf("[comp] place refused: %ld\n", r);
          }
          continue;
        }
        int id = hit(g_mx, g_my);
        struct win *w = id ? byid(id) : 0;
        if (w) {
          out[0] = EV_MOVE; out[1] = (int32_t)(g_mx - w->x); out[2] = (int32_t)(g_my - w->y); out[3] = 0;
          sys3(SYS_WIN_INPUT, id, (long)out, 0);
        }
      } else if (kind == IN_DOWN) {
        g_mx = a; g_my = b; g_have_mouse = 1;
        int id = hit(g_mx, g_my);
        struct win *w = id ? byid(id) : 0;
        if (w) stack_raise(id);              /* clicking a window brings it forward */
        g_focus = id;
        int at = w ? frame_hit(w, g_mx, g_my) : AT_OUTSIDE;
        if (at == AT_CLOSE && w) {
          long r = sys1(SYS_WIN_CLOSE, id);
          printf("[comp] close box on win %d -> %ld\n", id, r);
          clicks++;
        } else if (at == AT_TITLE && w) {
          g_drag = id; g_drag_x = g_mx - w->x; g_drag_y = g_my - w->y;
          printf("[comp] drag win %d by its title bar at %ld,%ld\n", id, g_mx, g_my);
        }
        if (at == AT_CLIENT && w) {
          out[0] = EV_DOWN; out[1] = (int32_t)(g_mx - w->x); out[2] = (int32_t)(g_my - w->y); out[3] = 1;
          long dr = sys3(SYS_WIN_INPUT, id, (long)out, 0);
          if (dr == 0) fwd++;
          clicks++;
          printf("[comp]   delivered: %s%ld\n", dr == 0 ? "yes" : "kernel said ", dr == 0 ? 0 : dr);
        }
        if (id) printf("[comp] click %ld,%ld -> win %d, focus\n", g_mx, g_my, id);
        else    printf("[comp] click %ld,%ld -> no window, focus dropped\n", g_mx, g_my);
      } else if (kind == IN_UP) {
        if (g_drag) {
          struct win *dw = byid(g_drag);
          if (dw) printf("[comp] drag of win %d ended at %ld,%ld\n", g_drag, dw->x, dw->y);
          g_drag = 0;
        }
        struct win *w = g_focus ? byid(g_focus) : 0;
        if (w && sys3(SYS_WIN_INPUT, g_focus, (long)(int32_t[4]){EV_UP,
                (int32_t)(g_mx - w->x), (int32_t)(g_my - w->y), 0}, 0) == 0) fwd++;
      } else if (kind == IN_KEY) {
        struct win *w = g_focus ? byid(g_focus) : 0;
        if (w) {
          out[0] = EV_KEY; out[1] = b; out[2] = 0; out[3] = a;   /* x = mods, code = key,
                                                       * the shape fwin_push uses */
          if (sys3(SYS_WIN_INPUT, g_focus, (long)out, 0) == 0) { fwd++; keys++; }
        } else refused++;
        printf("[comp] key %d -> %s%d\n", a, g_focus ? "win " : "nobody (", g_focus);
      } else if (kind == IN_WHEEL) {
        struct win *w = g_focus ? byid(g_focus) : 0;
        if (w) {
          out[0] = EV_WHEEL; out[1] = (int32_t)(g_mx - w->x); out[2] = (int32_t)(g_my - w->y); out[3] = c;
          sys3(SYS_WIN_INPUT, g_focus, (long)out, 0);
        }
      }
    }

    int32_t list[3 * MAXWIN];
    int n = (int)sys3(SYS_SURF_LIST, (long)list, MAXWIN, 0);
    stack_prune(list, n);
    for (int i = 0; i < n; i++) stack_add(list[i * 3 + 0]);

    rect(0, 0, sw, sh, BG);

    int drawn = 0;
    for (int i = 0; i < g_nstack; i++) {
      struct win *w = win_map(g_stack[i]);
      if (!w) continue;
      int32_t g[4] = {0, 0, 0, 0};
      if (sys3(SYS_WIN_GEOM, w->id, (long)g, 0) != 0) { bad_geom++; continue; }
      long x = g[0], y = g[1], ww = g[2], hh = g[3];

      /* Furniture first, and it is the server's: the edge, the title bar, the
       * title text out of the kernel's own glyph table, a close box. Then the
       * client's pixels, which the server never drew. */
      rect(x - 3, y - 3, ww + 6, hh + 6, EDGE);
      rect(x - 3, y - BAR_H, ww + 6, BAR_H - 2, w->id == g_focus ? FOCUS : TITLE);
      glyphs_draw(x + 4, y - BAR_H + 2, w->title, TEXT);
      rect(x + ww - 19, y - 19, 16, 16, CLOSE);
      glyphs_draw(x + ww - 15, y - 17, "x", CLOSEX);
      blit(w->px, w->stride, w->w, w->h, x, y);
      w->x = x; w->y = y;
      drawn++;
      if (y > worst) worst = (int)y;
    }
    /* The pointer, drawn by the server: the kernel's cursor stood down with the
     * rest of its shell. A crosshair, because a screenshot has to be able to
     * find it without knowing what an arrow looks like. */
    if (g_have_mouse) {
      rect(g_mx - 7, g_my, 15, 2, CURSOR);
      rect(g_mx, g_my - 7, 2, 15, CURSOR);
    }

    /* A sweep along the bottom: whatever is on the screen is this frame's. */
    long len = (long)((sw * (double)(f % 200)) / 200.0);
    rect(0, (long)sh - 6, len, 6, SWEEP);
    sys3(SYS_SCENE_FLUSH, 0, 0, 0);
    frames++;
    if (f == 0) printf("[comp] first frame: %d windows composited\n", drawn);
    ms(60);
  }

  printf("[comp] %d frames, %d windows mapped, geometry errors %d, "
         "clicks %d forwarded, keys %d forwarded, %d with no focus\n",
         frames, g_mapped, bad_geom, clicks, fwd, refused);
  sys1(SYS_INPUT_TAKE, 0);
  sys1(SYS_SCENE_HANDOFF, 0);
  printf("[comp] gave the screen back; the kernel repaints\n");
  return 0;
}
