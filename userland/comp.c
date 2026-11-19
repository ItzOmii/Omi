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

#define MAXWIN 8
#define BG      0x00101828u   /* the server's own desktop */
#define TITLE   0x00B03050u   /* furniture it draws itself */
#define EDGE    0x00E0E8F0u
#define SWEEP   0x00FFFFFFu
#define FOCUS   0x00E05070u   /* a title bar the server considers focused */
#define CURSOR  0x00FFFFFFu

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

/* One window: the pixels the map gave us, and the size they arrived in. A
 * window's contents can change size under us, so the size is re-checked each
 * frame from the list; the stride is not, because a surface never moves. */
struct win {
  int      id;
  uint32_t *px;
  long     w, h, stride;
  long     x, y;          /* where the last frame put it */
};

/* The server's own idea of the scene, kept from the last frame: input is
 * handled against this before the new frame is painted, the way a compositor
 * with a scene graph hit-tests against what it drew most recently. */
static struct win g_win[MAXWIN];   /* the scene graph, filled by the paint loop */
static int        g_live[MAXWIN];     /* ids in draw order, topmost last */
static int        g_nlive;
static int        g_focus;            /* window id, 0 = none */
static long       g_mx = 640, g_my = 400;
static int        g_have_mouse;

/* Which window is under a point, topmost first. */
static int hit(long x, long y) {
  for (int i = g_nlive - 1; i >= 0; i--) {
    struct win *w = 0;
    for (int k = 0; k < MAXWIN; k++) if (g_win[k].id == g_live[i]) { w = &g_win[k]; break; }
    if (!w) continue;
    if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) return w->id;
  }
  return 0;
}

static struct win *byid(int id) {
  for (int k = 0; k < MAXWIN; k++) if (g_win[k].id == id) return &g_win[k];
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

  struct win *cache = g_win;   /* the one table: the paint loop fills it, input reads it */
  int frames = 0, mapped_total = 0;
  int worst = -1, bad_geom = 0;

  long got = sys1(SYS_INPUT_TAKE, 1);
  printf("[comp] input: %s\n", got ? "the keyboard and pointer are this process's"
                                    : "refused - somebody else is serving");
  int keys = 0, clicks = 0, fwd = 0, refused = 0, moves = 0;

  for (int f = 0; f < 3000; f++) {           /* ~3 min at 60 ms */
    /* First the events, against the geometry of the frame that is on screen. */
    int32_t ev[4 * 32];
    int ne = (int)sys3(SYS_INPUT_POLL, (long)ev, 32, 0);
    for (int e = 0; e < ne; e++) {
      int32_t kind = ev[e * 4 + 0], a = ev[e * 4 + 1], b = ev[e * 4 + 2], c = ev[e * 4 + 3];
      int32_t out[4];
      if (kind == IN_MOVE) {
        g_mx = a; g_my = b; g_have_mouse = 1;
        if ((moves++ % 25) == 0) printf("[comp] move %ld,%ld\n", g_mx, g_my);
        int id = hit(g_mx, g_my);
        struct win *w = id ? byid(id) : 0;
        if (w) {
          out[0] = EV_MOVE; out[1] = (int32_t)(g_mx - w->x); out[2] = (int32_t)(g_my - w->y); out[3] = 0;
          sys3(SYS_WIN_INPUT, id, (long)out, 0);
        }
      } else if (kind == IN_DOWN) {
        g_mx = a; g_my = b; g_have_mouse = 1;
        int id = hit(g_mx, g_my);
        g_focus = id;
        struct win *w = id ? byid(id) : 0;
        if (w) {
          out[0] = EV_DOWN; out[1] = (int32_t)(g_mx - w->x); out[2] = (int32_t)(g_my - w->y); out[3] = 1;
          long dr = sys3(SYS_WIN_INPUT, id, (long)out, 0);
          if (dr == 0) fwd++;
          clicks++;
          printf("[comp]   delivered: %s%ld\n", dr == 0 ? "yes" : "kernel said ", dr == 0 ? 0 : dr);
        }
        if (id) printf("[comp] click %ld,%ld -> win %d, focus\n", g_mx, g_my, id);
        else    printf("[comp] click %ld,%ld -> no window, focus dropped\n", g_mx, g_my);
      } else if (kind == IN_UP) {
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
    rect(0, 0, sw, sh, BG);

    int drawn = 0;
    for (int i = 0; i < n; i++) {
      int win = list[i * 3 + 0];
      struct win *w = 0;
      for (int k = 0; k < MAXWIN; k++)
        if (cache[k].id == win) { w = &cache[k]; break; }

      if (!w) {                              /* first sight of this window */
        for (int k = 0; k < MAXWIN; k++)
          if (!cache[k].id) { w = &cache[k]; break; }
        if (!w) continue;
        int32_t mi[3] = {0, 0, 0};
        long a = sys3(SYS_SURF_MAP, win, (long)mi, 0);
        if (a <= 0) continue;                /* closed between list and map */
        w->id = win;
        w->px = (uint32_t *)(uintptr_t)a;
        w->w = mi[0]; w->h = mi[1]; w->stride = mi[2];
        printf("[comp] win %d: %ldx%ld at 0x%lx, stride %ld\n", win, w->w, w->h, a, w->stride);
        mapped_total++;
      }

      int32_t g[4] = {0, 0, 0, 0};
      if (sys3(SYS_WIN_GEOM, win, (long)g, 0) != 0) { bad_geom++; continue; }
      long x = g[0], y = g[1];

      /* Furniture first: the server draws the frame, the client draws the
       * contents. That split is what makes the title bar the server's to move. */
      if (y > 24) rect(x - 3, y - 22, g[2] + 6, 22, win == g_focus ? FOCUS : TITLE);
      rect(x - 3, y - 3, g[2] + 6, g[3] + 6, EDGE);
      blit(w->px, w->stride, w->w, w->h, x, y);
      w->x = x; w->y = y;
      g_live[drawn] = win;
      drawn++;
      if (y > worst) worst = (int)y;
    }

    g_nlive = drawn;
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
         frames, mapped_total, bad_geom, clicks, fwd, refused);
  sys1(SYS_INPUT_TAKE, 0);
  sys1(SYS_SCENE_HANDOFF, 0);
  printf("[comp] gave the screen back; the kernel repaints\n");
  return 0;
}
