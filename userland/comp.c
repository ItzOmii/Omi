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

#define MAXWIN 8
#define BG      0x00101828u   /* the server's own desktop */
#define TITLE   0x00B03050u   /* furniture it draws itself */
#define EDGE    0x00E0E8F0u
#define SWEEP   0x00FFFFFFu

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
};

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

  static struct win cache[MAXWIN];
  int frames = 0, mapped_total = 0;
  int worst = -1, bad_geom = 0;

  for (int f = 0; f < 700; f++) {            /* ~40 s at 60 ms */
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
      if (y > 24) rect(x - 3, y - 22, g[2] + 6, 22, TITLE);
      rect(x - 3, y - 3, g[2] + 6, g[3] + 6, EDGE);
      blit(w->px, w->stride, w->w, w->h, x, y);
      drawn++;
      if (y > worst) worst = (int)y;
    }

    /* A sweep along the bottom: whatever is on the screen is this frame's. */
    long len = (long)((sw * (double)(f % 200)) / 200.0);
    rect(0, (long)sh - 6, len, 6, SWEEP);
    sys3(SYS_SCENE_FLUSH, 0, 0, 0);
    frames++;
    if (f == 0) printf("[comp] first frame: %d windows composited\n", drawn);
    ms(60);
  }

  printf("[comp] %d frames, %d windows mapped, worst geometry errors %d\n",
         frames, mapped_total, bad_geom);
  sys1(SYS_SCENE_HANDOFF, 0);
  printf("[comp] gave the screen back; the kernel repaints\n");
  return 0;
}
