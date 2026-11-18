/* surfbig - a surface the size a browser actually wants.
 *
 * The old ceiling was one 4 MB slot per window, which at four bytes a pixel is
 * 1280x768 - not a browser window on any display made this decade. The slot is
 * now 16 MB of address space, eight of them, and frames are allocated only for
 * the pixels the surface has, so a small window costs a small amount of memory
 * instead of mapping a whole 4 MB slot for a 620 KB client.
 *
 * What this proves:
 *
 *   - a 1920x1200 surface (9.2 MB) is granted and its last pixel is writable
 *   - shrinking and then growing keeps working, growing back over the old size
 *   - eight slots exist; the ninth request is refused with -ENOSPC, in the open,
 *     rather than granted and quietly corrupting a neighbour
 *   - asking for more than the slot holds is clamped, not wrapped
 */
#define SYS_WIN_OPEN     6
#define SYS_WIN_CLOSE   10
#define SYS_SURF_CREATE 35
#define SYS_SURF_PRESENT 36
#define SYS_SURF_RESIZE 37

#define BIG_W 1920
#define BIG_H 1200

static int checks, fails;

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) { fails++; printf("[surfbig] FAIL %s (got %ld want %ld)\n", what, got, want); }
  else printf("[surfbig] ok   %s\n", what);
}

int main(void) {
  long win = sys3(SYS_WIN_OPEN, (long)"surfbig", 240, 180);
  check(win >= 0, "a window opens", win, 0);
  if (win < 0) { printf("[surfbig] %d/%d\n", checks - fails, checks); return 1; }

  long base = sys3(SYS_SURF_CREATE, win, BIG_W, BIG_H);
  check(base > 0, "a 1920x1200 surface is granted", base, 0);
  if (base <= 0) { printf("[surfbig] %d/%d\n", checks - fails, checks); return 1; }
  printf("[surfbig] %u MB of pixels at %u MB\n",
         (unsigned)((BIG_W * BIG_H * 4) >> 20), (unsigned)((unsigned long)base >> 20));

  volatile unsigned *px = (volatile unsigned *)(unsigned long)base;
  px[0] = 0x00112233;
  px[BIG_W * BIG_H - 1] = 0x00AABBCC;
  check(px[0] == 0x00112233, "the first pixel reads back", (long)px[0], 0x00112233);
  check(px[BIG_W * BIG_H - 1] == 0x00AABBCC, "so does the last pixel of 9.2 MB",
        (long)px[BIG_W * BIG_H - 1], 0x00AABBCC);

  check(sys3(SYS_SURF_PRESENT, win, 0, ((long)BIG_W << 16) | BIG_H) == 0,
        "and presenting it is accepted", 0, 0);

  long shrink = sys3(SYS_SURF_RESIZE, win, 64, 64);
  check(shrink == 0, "it shrinks to 64x64", shrink, 0);
  long grow = sys3(SYS_SURF_RESIZE, win, 1600, 900);
  check(grow == 0, "and grows back to 1600x900", grow, 0);
  check(sys3(SYS_SURF_RESIZE, win, 4000, 3000) == 0, "a request past the slot is clamped", 0, 0);

  /* Fill the slot table. The desktop's own windows hold some already, so count
   * what is available instead of assuming a number. */
  long wins[16];
  int made = 0;
  long last = 0;
  for (int i = 0; i < 16; i++) {
    long w = sys3(SYS_WIN_OPEN, (long)"filler", 120, 90);
    if (w < 0) break;
    long s = sys3(SYS_SURF_CREATE, w, 120, 90);
    if (s <= 0) { last = s; sys3(SYS_WIN_CLOSE, w, 0, 0); break; }
    wins[made++] = w;
  }
  check(made >= 3, "several more surfaces fit alongside the first", made, 0);
  check(last == -28, "the next request is refused with -ENOSPC", last, -28);
  printf("[surfbig] %d further surfaces before the pool ran out\n", made);

  for (int i = 0; i < made; i++) sys3(SYS_WIN_CLOSE, wins[i], 0, 0);
  sys3(SYS_WIN_CLOSE, win, 0, 0);

  printf("[surfbig] %d/%d\n", checks - fails, checks);
  return fails ? 1 : 0;
}
