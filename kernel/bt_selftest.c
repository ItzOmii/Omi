/* Does the backtrace walk actually walk?
 *
 * A panic path that prints something plausible and wrong is worse than no panic
 * path: it sends the next debugger to the wrong function with confidence. So the
 * three pieces get checked every boot - the symbol table answers with the name of
 * the function the address is in, a known three-deep call chain comes back in
 * order, and a frame pointer that leads nowhere stops the walk instead of
 * inventing frames.
 *
 * The chain is built with noinline functions and a volatile sink after each call:
 * without both, this tests the optimiser rather than the unwinder.
 */
#include "include/bt.h"
#include "include/gui.h"

extern char __text_start[], __text_end[];

static int bt_fails;

static void ok(int cond, const char *what) {
  serial_puts(cond ? "[bttest] ok   " : "[bttest] FAIL ");
  serial_puts(what);
  serial_puts("\n");
  if (!cond) bt_fails++;
}

static int same(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

/* Anything that would let the compiler collapse these into one function would
 * make the test pass without proving anything. */
__attribute__((noinline)) static int bt_depth3(uint64_t *frames, int *count);
__attribute__((noinline)) static int bt_depth2(uint64_t *frames, int *count);
__attribute__((noinline)) static int bt_depth1(uint64_t *frames, int *count);

__attribute__((noinline)) static int bt_depth3(uint64_t *frames, int *count) {
  /* rip = the instruction after the call, i.e. a point inside bt_depth2, and rbp
   * = this frame. The walk should climb from there. */
  *count = bt_capture((uint64_t)(uintptr_t)__builtin_frame_address(0),
                      (uint64_t)(uintptr_t)__builtin_return_address(0),
                      frames, BT_MAX_FRAMES);
  return *count;
}

__attribute__((noinline)) static int bt_depth2(uint64_t *frames, int *count) {
  int n = bt_depth3(frames, count);
  __asm__ volatile("" : "+r"(n));
  return n;
}

__attribute__((noinline)) static int bt_depth1(uint64_t *frames, int *count) {
  int n = bt_depth2(frames, count);
  __asm__ volatile("" : "+r"(n));
  return n;
}

void bt_selftest(void) {
  bt_fails = 0;
  serial_puts("[bttest] backtrace and symbol lookup\n");

  /* 1. The table names a function we can point at. */
  {
    uint64_t off = 0;
    const char *n = bt_lookup((uint64_t)(uintptr_t)&bt_depth3, &off);
    ok(n != 0 && same(n, "bt_depth3") && off == 0,
       "the symbol table names the function an address is in");
  }

  /* 2. A known chain, in order. */
  {
    uint64_t frames[BT_MAX_FRAMES];
    int count = 0;
    bt_depth1(frames, &count);
    uint64_t off = 0;
    const char *n0 = count > 0 ? bt_lookup(frames[0], &off) : 0;
    const char *n1 = count > 1 ? bt_lookup(frames[1], &off) : 0;
    const char *n2 = count > 2 ? bt_lookup(frames[2], &off) : 0;
    ok(count >= 3, "a three-deep call chain walks back to its caller");
    ok(same(n0, "bt_depth2") && same(n1, "bt_depth1") && same(n2, "bt_selftest"),
       "the frames come back in call order, named");
  }

  /* 3. Nonsense stops the walk. A frame pointer from low memory must give the
   *    starting address and nothing else - not a read of address 0x1000. */
  {
    uint64_t frames[BT_MAX_FRAMES];
    int n = bt_capture(0x1000, (uint64_t)(uintptr_t)&bt_depth3, frames, BT_MAX_FRAMES);
    ok(n == 1, "a frame pointer outside the kernel stops the walk");
  }

  /* 4. Only text is ever reported. */
  {
    uint64_t frames[BT_MAX_FRAMES];
    int count = 0;
    int good = 1;
    bt_depth1(frames, &count);
    for (int i = 0; i < count; i++) {
      if (frames[i] < (uint64_t)(uintptr_t)__text_start ||
          frames[i] >= (uint64_t)(uintptr_t)__text_end) good = 0;
    }
    ok(count > 0 && good, "every frame reported is inside kernel text");
  }

  serial_puts("[bttest] ");
  serial_puts(bt_fails ? "FAIL" : "4/4 ok");
  serial_puts("\n");
}