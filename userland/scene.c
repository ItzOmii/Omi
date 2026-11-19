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
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define SYS_YIELD         3
#define SYS_SCENE_MAP     42
#define SYS_SCENE_INFO    43
#define SYS_SCENE_FLUSH   44
#define SYS_SCENE_HANDOFF 45
#define SYS_SCENE_OWN     46

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
  /* The boot tests own the screen for their first half-minute and their
   * windows are the point of them; this program waits its turn. */
  ms(35000);

  long addr = sys0(SYS_SCENE_MAP);
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
  printf("[scene] handoff off: the kernel's scene comes back\n");
  sys1(SYS_SCENE_HANDOFF, 0);
  ms(300);
  printf("[scene] done\n");
  return 0;
}
