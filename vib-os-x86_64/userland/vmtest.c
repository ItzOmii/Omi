/* vmtest - the address window a process is allowed to occupy.
 *
 * The window used to be 256 MB: segments near the base, the stack at the top,
 * and mmap handing out ranges in between. That is fine for a window manager and
 * hopeless for a browser engine, which reserves a gigabyte or two of address
 * space per renderer and commits a fraction of it - V8 cages, a code range, a
 * garbage-collected heap that only touches what survives a minor GC. Reserving
 * address space costs nothing here because page tables are built on demand, so
 * the only thing in the way was a constant.
 *
 * This is the smallest program that can tell whether the constant moved. It
 * calls mmap, not a private syscall, because mmap is what an engine calls and
 * what the Linux compatibility layer has to translate:
 *
 *   - map 1.5 GB, and a second 512 MB region, both anonymous and private
 *   - write and read back the first page, the last page and the middle of the
 *     1.5 GB region, so the address is real at both ends and not just a number
 *     that was handed back
 *   - touch pages spread across the 512 MB region, which builds page tables
 *     four levels deep instead of only next to the base
 *   - unmap both, then ask for 8 GB, which must fail rather than promise an
 *     address that faults later
 *
 * It deliberately does not touch every page of 1.5 GB: the test machine has
 * 512 MB of RAM, and the claim under test is that address space is cheap and
 * physical memory is not.
 */
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#define MB (1024u * 1024u)
#define BIG_MB    1536u
#define SPREAD_MB 512u

static int checks, fails;

static void ck(int cond, const char *what, long got) {
  if (cond) {
    printf("[vmtest] ok   %s\n", what);
  } else {
    printf("[vmtest] FAIL %s (got %ld)\n", what, got);
    fails++;
  }
  checks++;
}

/* MB offset: prints in 32 bits and says more than an absolute address nobody
 * can read. */
static unsigned at_mb(const void *p) { return (unsigned)((uintptr_t)p >> 20); }

static int poke(volatile unsigned char *p, unsigned char v) {
  *p = v;
  return *p == v;
}

int main(void) {
  volatile unsigned char *big = mmap(NULL, (size_t)BIG_MB * MB,
                                     PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ck(big != MAP_FAILED, "a 1.5 GB mapping is granted", big == MAP_FAILED ? -1 : 0);
  if (big == MAP_FAILED) {
    printf("[vmtest] %d/%d\n", checks - fails, checks);
    return 1;
  }
  printf("[vmtest] big region at %u MB, %u MB long\n", at_mb(big), BIG_MB);

  volatile unsigned char *spread = mmap(NULL, (size_t)SPREAD_MB * MB,
                                        PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ck(spread != MAP_FAILED, "a second 512 MB mapping is granted",
     spread == MAP_FAILED ? -1 : 0);
  if (spread == MAP_FAILED) {
    printf("[vmtest] %d/%d\n", checks - fails, checks);
    return 1;
  }

  /* The claim: both ends of 1.5 GB are real addresses on this machine. */
  ck(poke(big, 0x5a), "the first page of 1.5 GB takes a byte", 0);
  ck(poke(big + (size_t)(BIG_MB - 1) * MB, 0xa5), "the last page takes a byte", 0);
  ck(poke(big + (size_t)(BIG_MB / 2) * MB, 0x3c), "the middle takes a byte", 0);
  ck(big[0] == 0x5a && big[(size_t)(BIG_MB - 1) * MB] == 0xa5 &&
     big[(size_t)(BIG_MB / 2) * MB] == 0x3c,
     "all three markers survive together", 0);

  for (unsigned i = 0; i < 64; i++)
    poke(spread + (size_t)i * (SPREAD_MB / 64) * MB, (unsigned char)(0x11 + i));
  int good = 0;
  for (unsigned i = 0; i < 64; i++)
    if (spread[(size_t)i * (SPREAD_MB / 64) * MB] == (unsigned char)(0x11 + i)) good++;
  ck(good == 64, "64 pages spread across 512 MB are all backed", good);

  ck(munmap((void *)big, (size_t)BIG_MB * MB) == 0, "1.5 GB unmaps", -1);
  ck(munmap((void *)spread, (size_t)SPREAD_MB * MB) == 0, "512 MB unmaps", -1);

  /* Beyond the window: refused, not accepted and then unusable. */
  void *too_big = mmap(NULL, (size_t)8192 * MB, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ck(too_big == MAP_FAILED, "8 GB is refused instead of promised", 0);
  if (too_big != MAP_FAILED) munmap(too_big, (size_t)8192 * MB);

  printf("[vmtest] %d/%d\n", checks - fails, checks);
  return fails ? 1 : 0;
}
