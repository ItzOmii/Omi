/* surfexit - a window that dies with its process.
 *
 * The compositor frees a surface when a window closes, but nothing used to close
 * a window when the task behind it ended: SYS_WIN_CLOSE was the only path into
 * surf_release_window. So an app that returned from main, or crashed, left its
 * frames allocated - four of those and no further surface could be created - and
 * its last frame stayed on screen as if it were still running.
 *
 * This program is the proof that the kernel now reaps: it opens a window, fills
 * its surface with a colour nothing else on the desktop uses, presents it, lets
 * the compositor blit it at least once, then returns. If the reaping works, the
 * serial log shows the close (with a blit count above zero, so the frame really
 * was on screen) and the framebuffer keeps no trace of the colour where the
 * window was.
 */
#include <stdint.h>
#include <stdio.h>

#define SYS_YIELD        3
#define SYS_WIN_OPEN     6
#define SYS_WIN_GEOM     14
#define SYS_SURF_CREATE  35
#define SYS_SURF_PRESENT 36

#define CW 198
#define CH 108
#define C_MARKER 0x00FF00FFu     /* magenta: nowhere else in the theme */

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

int main(void) {
  long win = sys3(SYS_WIN_OPEN, (long)"exit", CW + 2, CH + 32);
  if (win <= 0) { printf("[exit] no window\n"); return 1; }

  int32_t g[4] = {0, 0, 0, 0};
  if (sys3(SYS_WIN_GEOM, win, (long)g, 0) == 0)
    printf("[exit] client at %d,%d size %dx%d\n", g[0] + 1, g[1] + 31, CW, CH);

  long s = sys3(SYS_SURF_CREATE, win, CW, CH);
  if (s <= 0) { printf("[exit] no surface\n"); return 1; }
  unsigned *pix = (unsigned *)s;
  for (int i = 0; i < CW * CH; i++) pix[i] = C_MARKER;

  if (sys3(SYS_SURF_PRESENT, win, 0, ((long)CW << 16) | CH) != 0) {
    printf("[exit] present failed\n");
    return 1;
  }

  /* Give the compositor real frames with this surface, so the close line can
   * report a blit count above zero and the reap is not just "nothing was drawn". */
  for (int i = 0; i < 60; i++) sys3(SYS_YIELD, 0, 0, 0);

  printf("[exit] returning with the window still open\n");
  return 0;
}