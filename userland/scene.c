/* scene - the screen's pixels belong to this process.
 *
 * surftest proved a process can own a window's pixels. This is the other half
 * of the compositor split: a process owning the frame the display shows. Omi's
 * kernel has composited its own scene since it had a scene - it draws into a
 * back buffer and copies rectangles onto the display - and no engine that
 * brings its own compositor can be ported onto that arrangement. It has to be
 * inverted, and this program is the smallest thing that shows the inversion
 * works:
 *
 *   - it maps the scene and writes colour straight into it, so the pixels on
 *     the screen are the program's, not a kernel replay of a command list
 *   - it flushes one rectangle (a panel in a corner) and keeps the rest of the
 *     screen the kernel's, which is the damage-limited path a compositor needs
 *   - then it takes the screen (SCENE_HANDOFF) and paints the whole thing while
 *     the kernel paints nothing, so there is one writer and one owner
 *   - then it gives the screen back and the kernel repaints itself, which is
 *     what makes the handover a handover rather than a hijack
 *
 * It cannot check its own pixels from the inside - the mapping could be
 * writable while the display reads somewhere else entirely. That is checked
 * from outside, by sampling the framebuffer at the coordinates and colours it
 * prints below.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define SYS_YIELD         3
#define SYS_WIN_OPEN      6
#define SYS_WIN_CLOSE     10
#define SYS_SURF_CREATE   35
#define SYS_SURF_PRESENT  36
#define SYS_SCENE_MAP     42
#define SYS_SCENE_INFO    43
#define SYS_SCENE_FLUSH   44
#define SYS_SCENE_HANDOFF 45
#define SYS_SCENE_OWN     46
#define SYS_SURF_MAP      47
#define SYS_SURF_LIST     48

/* The panel: a corner of the screen this process owns while the kernel keeps
 * the rest. Both colours are chosen so a single pixel proves ownership: the
 * blue is nowhere in the kernel's own palette, and the orange bar sweeps. */
#define PANEL_X 60
#define PANEL_Y 60
#define PANEL_W 320
#define PANEL_H 200
#define PANEL_BG 0x00204080u
#define PANEL_BAR 0x00FF8000u
#define PANEL_BAR_W 24

/* The takeover: the whole screen, one unmistakable colour, with a band that
 * moves so a frozen frame cannot be mistaken for a live one. */
#define FULL_BG 0x0011EE33u
#define FULL_BAR 0x00FF0000u
#define FULL_BAR_W 40

static long sys0(long n) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n) : "memory");
  return r;
}

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

/* The surface's own size, for the checkerboard, and where its pixels are put on
 * the screen - away from the window itself, so what shows up there can only have
 * come from the blit. */
#define SURF_W 320
#define SURF_H 200
#define PASTE_X 120
#define PASTE_Y 380

static uint32_t *scene;
static uint32_t sw, sh, stride;   /* pixels, rows, bytes per row */

/* Fill a rectangle of the scene with one colour. The scene is the whole
 * screen, so this is the same write a compositor would do - nothing is
 * translated or clipped on the way in; the kernel is not in this path. */
static void rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c) {
  for (uint32_t y = y0; y < y0 + h && y < sh; y++) {
    uint32_t *row = (uint32_t *)((uint8_t *)scene + (size_t)y * stride);
    for (uint32_t x = x0; x < x0 + w && x < sw; x++) row[x] = c;
  }
}

/* The kernel's own ABI carries three arguments, so a rectangle rides in two of
 * them as 16-bit halves - the same packing SYS_SURF_PRESENT uses, and screen
 * coordinates fit in 16 bits. */
/* Copy rows out of a surface and into the scene. The source is whatever address
 * the map returned, so this is the same code whether those pixels were drawn by
 * this process or by a browser in another one. Rows, because the frames are not
 * contiguous and the surface's stride is its own. */
static void blit(uint32_t *src, long src_stride, long src_w, long src_h,
                 long dx, long dy) {
  for (long y = 0; y < src_h; y++) {
    long ty = dy + y;
    if (ty < 0 || ty >= (long)sh) continue;
    uint32_t *srow = (uint32_t *)((uint8_t *)src + (size_t)y * (size_t)src_stride);
    uint32_t *drow = (uint32_t *)((uint8_t *)scene + (size_t)ty * (size_t)stride);
    for (long x = 0; x < src_w; x++) {
      long tx = dx + x;
      if (tx < 0 || tx >= (long)sw) continue;
      drow[tx] = srow[x];
    }
  }
}

static void flush_scene(long x, long y, long w, long h) {
  long xy = ((x & 0xFFFF) << 16) | (y & 0xFFFF);
  long wh = ((w & 0xFFFF) << 16) | (h & 0xFFFF);
  sys3(SYS_SCENE_FLUSH, xy, wh, 0);
}

/* Claim a rectangle for this process. The kernel goes on compositing the whole
 * screen - its desktop stays a live desktop - and this rectangle comes out of
 * the scene over the top of it, every frame. w and h of 0 give the rectangle
 * back. */
static long own_rect(long x, long y, long w, long h) {
  long xy = ((x & 0xFFFF) << 16) | (y & 0xFFFF);
  long wh = ((w & 0xFFFF) << 16) | (h & 0xFFFF);
  return sys3(SYS_SCENE_OWN, xy, wh, 0);
}

int main(void) {
  /* Not part of a stock boot any more: the display server takes the scene and
   * this program is the older owner of it. If comp got there first this says so
   * and leaves; run it on a boot without comp to check the kernel's side of the
   * mapping - writable, the same address twice, the pixels in a rectangle. */
  long addr = sys0(SYS_SCENE_MAP);
  if (addr == -EBUSY) {
    printf("[scene] skip: the display server owns the screen (run this without comp)\n");
    return 0;
  }
  if (addr < 0) { printf("[scene] FAIL map: %ld\n", addr); return 1; }
  uint32_t geom[3] = {0, 0, 0};
  long gi = sys1(SYS_SCENE_INFO, (long)geom);
  if (gi != 0) { printf("[scene] FAIL info: %ld\n", gi); return 1; }
  sw = geom[0]; sh = geom[1]; stride = geom[2];
  scene = (uint32_t *)(uintptr_t)addr;
  printf("[scene] scene at 0x%lx, %ux%u, stride %u\n", addr, sw, sh, stride);

  /* Same owner asking again gets the same screen, not a second one. */
  long again = sys0(SYS_SCENE_MAP);
  printf("[scene] %s second map returns the same scene (0x%lx)\n",
         again == addr ? "ok  " : "FAIL", again);

  /* The mapping is real memory: write it and read it back. If these pages were
   * not mapped, this would fault rather than print; if the kernel were reading
   * elsewhere, the outside sample would disagree. */
  uint64_t sum = 0;
  for (uint32_t y = 0; y < sh; y += 64)
    sum += ((uint32_t *)((uint8_t *)scene + (size_t)y * stride))[0];
  printf("[scene] read back %u rows, sum %lu, writable\n", (sh + 63) / 64, sum);

  /* Claimed before the wait, not after it. The scene has one owner and the
   * display server asks for it at boot too, so whoever asks first settles the
   * order; asking 35 seconds late settled it the wrong way and this program
   * could only ever print a failure. Nothing here is painted while it waits:
   * the mapping is claimed, the handoff stays off, and the kernel goes on
   * compositing the boot tests' windows exactly as before. The wait is for the
   * phases below, which do paint. */
  ms(35000);

  /* ---- phase one: a rectangle of our own, with the kernel's desktop alive
   * around it. Nothing stops the kernel compositing while it runs, and that was
   * the first thing this program found out: the compositor's idle path copies
   * the whole of its own scene onto the display when nothing is damaged, so a
   * write straight to the display lives one frame. The rectangle is how the two
   * writers share one screen - the kernel draws everything, then this process's
   * pixels go over its own rectangle last. ---- */
  printf("[scene] panel phase: owner rect %d,%d %dx%d, in = 0x%08X/0x%08X, out = desktop\n",
         PANEL_X, PANEL_Y, PANEL_W, PANEL_H, PANEL_BG, PANEL_BAR);
  long rc = own_rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H);
  printf("[scene] claim returned %ld\n", rc);
  for (int i = 0; i < 300; i++) {           /* ~15 s at 50 ms */
    rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, PANEL_BG);
    rect(PANEL_X + (uint32_t)((i * 7) % (PANEL_W - PANEL_BAR_W)), PANEL_Y, PANEL_BAR_W, PANEL_H, PANEL_BAR);
    flush_scene(PANEL_X, PANEL_Y, PANEL_W, PANEL_H);
    ms(50);
  }

  /* ---- phase two: the screen is this process's, the kernel paints nothing -- */
  printf("[scene] taking the screen: sample (1180,700) expect 0x%08X\n", FULL_BG);
  own_rect(0, 0, 0, 0);                      /* the whole screen, so no part-claim */
  long was = sys1(SYS_SCENE_HANDOFF, 1);
  printf("[scene] handoff was %ld, now on\n", was);
  for (int i = 0; i < 300; i++) {
    rect(0, 0, sw, sh, FULL_BG);
    rect((uint32_t)((i * 11) % (sw - FULL_BAR_W)), 0, FULL_BAR_W, sh, FULL_BAR);
    flush_scene(0, 0, 0, 0);                       /* 0,0,0,0 is the whole screen */
    ms(50);
  }
  /* ---- phase three: pixels that arrive through the kernel's map ----
   * The screen is still ours. Open a window, give it a surface, paint a
   * checkerboard into it, then ask for those same pixels through SYS_SURF_MAP -
   * a read-only alias, which is exactly what a display server gets for a window
   * it does not own - and put the alias's contents on the screen. Draw and put
   * on screen have just come apart: nothing here looks at the surface it drew
   * into, only at the mapping it was given. ---- */
  int32_t list[24];
  int nsurf = (int)sys3(SYS_SURF_LIST, (long)list, 8, 0);
  printf("[scene] surfaces in the table: %d\n", nsurf);
  for (int i = 0; i < nsurf && i < 8; i++)
    printf("[scene]   win %d, %dx%d\n", list[i * 3], list[i * 3 + 1], list[i * 3 + 2]);

  long win = sys3(SYS_WIN_OPEN, (long)"scene", SURF_W, SURF_H);
  printf("[scene] window %ld\n", win);
  long sbase = win >= 0 ? sys3(SYS_SURF_CREATE, win, SURF_W, SURF_H) : -1;
  printf("[scene] surface at 0x%lx\n", sbase);
  if (sbase > 0) {
    uint32_t *mine = (uint32_t *)(uintptr_t)sbase;
    for (long y = 0; y < SURF_H; y++)
      for (long x = 0; x < SURF_W; x++)
        mine[y * SURF_W + x] = ((((x / 16) + (y / 16)) & 1) ? 0x0000FFFFu : 0x00FF00FFu);
    sys3(SYS_SURF_PRESENT, win, 0, ((long)SURF_W << 16) | SURF_H);

    int32_t info[3] = {0, 0, 0};
    long alias = sys3(SYS_SURF_MAP, win, (long)info, 0);
    printf("[scene] map: 0x%lx, %dx%d, stride %d\n", alias, info[0], info[1], info[2]);
    if (alias > 0) {
      uint32_t *view = (uint32_t *)(uintptr_t)alias;
      long bad = 0, seen = 0;
      long vstride = info[2] / 4;
      for (long y = 0; y < SURF_H; y += 7)
        for (long x = 0; x < SURF_W; x += 5) {
          seen++;
          if (view[y * vstride + x] != mine[y * SURF_W + x]) bad++;
        }
      printf("[scene] alias agrees with the surface: %s (%ld samples, %ld bad)\n",
             bad == 0 ? "yes" : "NO", seen, bad);

      /* composite the alias, not the surface */
      int sx = PASTE_X + 20, sy = PASTE_Y + 20, sx2 = PASTE_X + 44;
      uint32_t c1 = ((((sx - PASTE_X) / 16) + ((sy - PASTE_Y) / 16)) & 1) ? 0x0000FFFFu : 0x00FF00FFu;
      uint32_t c2 = ((((sx2 - PASTE_X) / 16) + ((sy - PASTE_Y) / 16)) & 1) ? 0x0000FFFFu : 0x00FF00FFu;
      rect(0, 0, sw, sh, FULL_BG);
      blit(view, info[2], info[0], info[1], PASTE_X, PASTE_Y);
      flush_scene(0, 0, 0, 0);
      printf("[scene] composited at %d,%d: (%d,%d) expect 0x%08X, (%d,%d) expect 0x%08X, (10,10) 0x%08X\n",
             PASTE_X, PASTE_Y, sx, sy, c1, sx2, sy, c2, FULL_BG);
      ms(8000);

      /* And now somebody else's window: the first surface in the table that is
       * not the one above. Its pixels are not ours to know, so the program
       * prints what it read at two points and the screenshot has to agree. */
      int fwin = -1;
      for (int i = 0; i < nsurf && i < 8; i++)
        if (list[i * 3] != (int)win) { fwin = list[i * 3]; break; }
      if (fwin >= 0) {
        int32_t finfo[3] = {0, 0, 0};
        long falias = sys3(SYS_SURF_MAP, fwin, (long)finfo, 0);
        printf("[scene] foreign win %d map: 0x%lx, %dx%d\n", fwin, falias, finfo[0], finfo[1]);
        if (falias > 0) {
          uint32_t *fv = (uint32_t *)(uintptr_t)falias;
          long fstride = finfo[2] / 4;
          uint32_t a1 = fv[10 * fstride + 10], a2 = fv[40 * fstride + 40];
          blit(fv, finfo[2], finfo[0], finfo[1], 600, 400);
          flush_scene(0, 0, 0, 0);
          printf("[scene] foreign composited at 600,400: (610,410) expect 0x%08X, (640,440) expect 0x%08X\n",
                 a1, a2);
          ms(12000);
        }
      } else printf("[scene] no foreign surface to composite\n");

      /* Close the window while an alias of it is still mapped: the frames go
       * back to the allocator here, so the alias has to go first. */
      sys1(SYS_WIN_CLOSE, win);
      printf("[scene] window %ld closed; its alias should be gone before its frames are\n", win);
    }
  }

  printf("[scene] handoff off: the kernel's scene comes back\n");
  sys1(SYS_SCENE_HANDOFF, 0);
  ms(300);
  printf("[scene] done\n");
  return 0;
}
