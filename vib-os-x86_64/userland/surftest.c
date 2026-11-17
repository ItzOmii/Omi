/* surftest - a window whose content is the program's own pixels.
 *
 * Until now a ring-3 window was a retained command list: the process said
 * "text here, rectangle there" and the kernel replayed it. That works for a
 * form and is hopeless for anything that redraws at a frame rate, because every
 * pixel would cross a syscall. A surface inverts the split - the kernel keeps
 * the frames and the placement, the process keeps the pixels - and this program
 * is the smallest thing that can tell whether that works:
 *
 *   - it maps a surface and writes colour bars straight into it
 *   - it presents the whole thing, then changes one square and presents only
 *     that rectangle, which is the damage path
 *   - it asks for a second surface on the same window, which must be refused,
 *     and presents to a window that has none, which must also be refused
 *
 * What it cannot check from the inside is whether the pixels reached the
 * screen: the mapping could be writable and the compositor could still be
 * reading somewhere else. That is checked from outside, by dumping the
 * framebuffer and looking at the coordinates this program prints.
 *
 * The bars are 4 pixels wide, not one, on purpose: a row copy that is off by a
 * pixel, or a 3x downscale of it, leaves a 4-pixel bar intact and destroys a
 * 1-pixel one.
 */
#include <stdint.h>
#include <stdio.h>

#define SYS_YIELD        3
#define SYS_WIN_OPEN     6
#define SYS_WIN_GEOM     14
#define SYS_WIN_CLOSE    10
#define SYS_SURF_CREATE  35
#define SYS_SURF_PRESENT 36
#define SYS_WIN_WAIT     12

#define SURF_W 320
#define SURF_H 200
#define TITLE_H 31            /* the compositor's chrome; only used for reporting */

static int checks, fails;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) { fails++; printf("[surf] FAIL %s (got %ld want %ld)\n", what, got, want); }
  else printf("[surf] ok   %s\n", what);
}

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

static long win_open(const char *title, long w, long h) {
  return sys3(SYS_WIN_OPEN, (long)title, w, h);
}

static long surf_create(long win, long w, long h) {
  return sys3(SYS_SURF_CREATE, win, w, h);
}

/* The ABI packs two 16-bit values per argument: coordinates fit. */
static long surf_present(long win, long x, long y, long w, long h) {
  long xy = ((x & 0xFFFF) << 16) | (y & 0xFFFF);
  long wh = ((w & 0xFFFF) << 16) | (h & 0xFFFF);
  return sys3(SYS_SURF_PRESENT, win, xy, wh);
}

static uint32_t *px;
static int geom[4];

static void bar(int x0, int w, uint32_t c) {
  for (int y = 0; y < SURF_H; y++) {
    uint32_t *row = px + (size_t)y * SURF_W;
    for (int x = x0; x < x0 + w; x++) row[x] = c;
  }
}

static void box(int x0, int y0, int w, int h, uint32_t c) {
  for (int y = y0; y < y0 + h; y++) {
    uint32_t *row = px + (size_t)y * SURF_W;
    for (int x = x0; x < x0 + w; x++) row[x] = c;
  }
}

/* Holding the window open is the point: a framebuffer check from outside the OS
 * can only see what is still on screen. Block instead of spinning - SYS_WIN_WAIT
 * costs nothing until input arrives. */
typedef struct { int kind, x, y, code; } win_ev_user_t;

static void park(long win) {
  win_ev_user_t ev;
  while (sys3(SYS_WIN_WAIT, win, (long)&ev, 0) >= 0)
    sys3(SYS_SURF_PRESENT, win, 0, ((long)SURF_W << 16) | SURF_H);   /* repaint if asked */
}

int main(void) {
  printf("[surf] start\n");

  long win = win_open("surface", SURF_W, SURF_H);
  check(win >= 0, "a window opens", win, 0);
  if (win < 0) { printf("[surf] %d/%d\n", checks - fails, checks); return 1; }

  long addr = surf_create(win, SURF_W, SURF_H);
  check(addr > 0, "a surface is created for it", addr, 0);
  if (addr <= 0) { printf("[surf] %d/%d\n", checks - fails, checks); return 1; }

  /* Where the pixels landed: a user address, page aligned, inside the window
   * the kernel reserved for surfaces (0x8000000000 + 8 MB). */
  check((addr & 0xFFF) == 0 &&
        (unsigned long long)addr >= 0x800F000000ULL &&
        (unsigned long long)addr < 0x800F800000ULL,
        "the pixels are mapped at a user address", addr, 0);
  px = (uint32_t *)addr;

  /* If this were read-only the write would fault the process out. */
  px[0] = 0x11223344u;
  check(px[0] == 0x11223344u, "the mapping is writable", px[0], 0x11223344);

  /* A second surface on the same window has nowhere to go. */
  long again = surf_create(win, 64, 64);
  check(again < 0, "a second surface on one window is refused", again, -17);

  /* Paint: four bars, and a black square that will move to yellow later. */
  bar(0, 80, 0xFF0000u);
  bar(80, 80, 0x00FF00u);
  bar(160, 80, 0x0000FFu);
  bar(240, 80, 0xFFFFFFu);
  box(8, 8, 16, 16, 0x000000u);

  long r = surf_present(win, 0, 0, SURF_W, SURF_H);
  check(r == 0, "the whole surface is presented", r, 0);

  /* Geometry, so the check outside this program knows where to look. */
  if (sys3(SYS_WIN_GEOM, win, (long)geom, 0) == 0) {
    printf("[surf] client at %d,%d size %dx%d\n",
           geom[0] + 1, geom[1] + TITLE_H, geom[2] - 2, geom[3] - TITLE_H - 1);
  } else {
    printf("[surf] no geometry\n");
  }

  /* Let the compositor pick that up before changing anything. */
  for (int i = 0; i < 400; i++) sys3(SYS_YIELD, 0, 0, 0);

  /* The damage path: one square changes, and only that rectangle is offered. */
  box(8, 8, 16, 16, 0xFFFF00u);
  r = surf_present(win, 8, 8, 16, 16);
  check(r == 0, "a single rectangle is presented", r, 0);

  /* Out of bounds is clipped rather than refused: a process painting past its
   * edge gets the part that fits, which is what a resize needs. */
  r = surf_present(win, -10, -10, 20, 20);
  check(r == 0, "a rectangle off the edge is clipped", r, 0);

  /* A window with no surface must reject presents, not silently accept them. */
  long other = win_open("nosurface", 200, 120);
  r = surf_present(other, 0, 0, 10, 10);
  check(r < 0, "presenting to a window without a surface is refused", r, -22);

  /* Take that window away again: it was opened on top of the surface window, and
   * the last frame should show the surface unobstructed - same pixels the
   * compositor copied, nothing left behind. */
  long closed = sys3(SYS_WIN_CLOSE, other, 0, 0);
  check(closed == 0, "the extra window closes", closed, 0);

  printf("[surf] %d/%d\n", checks - fails, checks);
  if (fails) return 1;     /* a failed run must not sit on the screen looking fine */
  park(win);
  return 0;
}