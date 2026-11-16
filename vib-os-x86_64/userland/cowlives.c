/* fork's reference counts, under lifetimes that actually stress them.
 *
 * A COW test that forks once and writes once proves the copy path. It does not
 * prove the accounting, because the accounting is only interesting when spaces
 * die in an order other than the one you tested: the parent first, the child
 * first, three generations deep, several children writing the same inherited
 * page, and a shared segment that must *not* be copied.
 *
 * Every case compares against a value written before the fork, so a case can
 * only pass by the frame being what it should be - not by the kernel answering
 * something well-formed. The kernel prints the mechanism (frames, refs, tallies)
 * alongside; this program only says whether the observable result is right.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* Omi's own syscall numbers, reached through int $0x80. The Linux ABI shares the
 * instruction but not the numbering, and these two live below it. */
#define SYS_SHM_GET    15
#define SYS_SHM_ATTACH 16

static long omi(long nr, long a0, long a1) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "D"(a0), "S"(a1) : "memory");
  return r;
}

/* Its own page, because writing one byte of a page that holds other things makes
 * the kernel's report ambiguous about what was copied. */
static volatile unsigned char page[4096] __attribute__((aligned(4096)));

static int failures, checked;

static void check(int cond, const char *what, long got, long want) {
  checked++;
  if (cond) {
    printf("[lives] ok   %s\n", what);
  } else {
    printf("[lives] FAIL %s (got %ld, want %ld)\n", what, got, want);
    failures++;
  }
}

/* A busy wait: there is no usleep worth trusting here, and the point is only to
 * let another task die before this one runs. */
static void slack(void) {
  for (volatile int i = 0; i < 3000000; i++) { }
}

/* 1. The parent dies first. Two forks deep: A forks B and exits, so B's frames
 *    are held by a space that no longer exists. B then writes. If teardown
 *    freed the frame instead of releasing it, B writes into a freed page. */
static void parent_exits_first(void) {
  int p[2];
  if (pipe(p) != 0) { check(0, "T1 pipe", -1, 0); return; }
  page[0] = 's';
  page[1] = 's';

  pid_t a = fork();
  if (a == 0) {
    pid_t b = fork();
    if (b == 0) {
      close(p[0]);
      slack();                       /* long enough for A to be gone */
      char inherited = (char)page[1];
      page[0] = 'B';
      int ok = (page[0] == 'B' && inherited == 's');
      if (write(p[1], ok ? "1" : "0", 1) != 1) _exit(8);
      _exit(ok ? 0 : 9);
    }
    _exit(0);                        /* A dies while B is still running */
  }
  close(p[1]);
  int st = 0;
  waitpid(a, &st, 0);                /* A is ours. B is A's, and A is dead. */
  char c = 0;
  ssize_t n = read(p[0], &c, 1);
  close(p[0]);
  check(n == 1 && c == '1',
        "T1 a child two forks deep outlived its parent and kept its page", (long)c, '1');
}

/* 2. The child dies first. The parent still holds the page and must be able to
 *    write it: the frame it shares with a dead space is its own again. */
static void child_exits_first(void) {
  page[0] = 's';
  pid_t c = fork();
  if (c == 0) _exit(0);
  int st = 0;
  waitpid(c, &st, 0);
  page[0] = 'P';
  check(page[0] == 'P',
        "T2 the parent keeps its page after the child that shared it dies", page[0], 'P');
}

/* 3. Three generations. D forks E, E forks F, and both ancestors exit before F
 *    writes. The frame that started with three mappings and two dead spaces must
 *    still hold the original byte, and F's write must land somewhere private. */
static void three_generations(void) {
  int p[2];
  if (pipe(p) != 0) { check(0, "T3 pipe", -1, 0); return; }
  page[0] = 'G';
  page[1] = 'G';

  pid_t d = fork();
  if (d == 0) {
    pid_t e = fork();
    if (e == 0) {
      pid_t f = fork();
      if (f == 0) {
        close(p[0]);
        slack();
        char inherited = (char)page[0];
        char second = (char)page[1];
        page[0] = 'F';
        int ok = (page[0] == 'F' && inherited == 'G' && second == 'G');
        if (write(p[1], ok ? "1" : "0", 1) != 1) _exit(8);
        _exit(ok ? 0 : 9);
      }
      _exit(0);                      /* E */
    }
    _exit(0);                        /* D */
  }
  close(p[1]);
  int st = 0;
  waitpid(d, &st, 0);
  char c = 0;
  ssize_t n = read(p[0], &c, 1);
  close(p[0]);
  check(n == 1 && c == '1',
        "T3 the third generation still sees the bytes the first one wrote", (long)c, '1');
}

/* 4. Four children, one inherited page. Each writes its own byte; if any of them
 *    wrote the shared frame, at least two would report the same value and the
 *    parent's copy would be wrong too. */
static void four_children(void) {
  enum { KIDS = 4 };
  pid_t kid[KIDS];
  page[0] = 'S';
  page[1] = 'S';

  for (int i = 0; i < KIDS; i++) {
    kid[i] = fork();
    if (kid[i] == 0) {
      char mine = (char)('A' + i);
      page[0] = mine;
      int ok = (page[0] == mine && page[1] == 'S');
      _exit(ok ? 0 : 9);
    }
  }
  int good = 0;
  for (int i = 0; i < KIDS; i++) {
    int st = 0;
    if (waitpid(kid[i], &st, 0) == kid[i] && WIFEXITED(st) && WEXITSTATUS(st) == 0) good++;
  }
  check(good == KIDS, "T4 every child got a private frame of the same page", good, KIDS);
  check(page[0] == 'S' && page[1] == 'S',
        "T4 and the parent's page is untouched by any of them", page[0], 'S');
}

/* 5. A shared segment is the opposite case: it is mapped writable in two spaces
 *    on purpose, so fork must hand over the same frame and a child's write must
 *    be visible to the parent. Then the child dies and the segment is still
 *    there, because its owner is the kernel pool, not the process. */
static void shared_segment(void) {
  if (omi(SYS_SHM_GET, 3, 2) < 0) { check(0, "T5 shm_get", -1, 0); return; }
  long addr = omi(SYS_SHM_ATTACH, 3, 0);
  if (addr <= 0) { check(0, "T5 shm_attach", addr, 1); return; }
  volatile char *shm = (volatile char *)addr;
  shm[0] = 's';

  pid_t k = fork();
  if (k == 0) {
    shm[0] = 'K';                    /* must be the parent's frame too */
    _exit(0);
  }
  int st = 0;
  waitpid(k, &st, 0);
  check(shm[0] == 'K', "T5 a shared segment is not copied at fork", shm[0], 'K');

  shm[1] = 'P';                      /* still ours to write after the death */
  check(shm[1] == 'P',
        "T5 and it is still writable after the child that mapped it is gone", shm[1], 'P');
}

/* 6. munmap of a page a forked child still maps. This is the case that catches a
 *    premature free rather than a leak: if the parent's munmap returns the frame
 *    to the allocator, the child's next write lands in someone else's memory and
 *    the *parent* sees nothing wrong. So the child does the checking. */
static void munmap_while_shared(void) {
  size_t len = 2 * 4096;
  unsigned char *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) { check(0, "T6 mmap", -1, 0); return; }
  m[0] = 'm';
  m[4096] = 'm';

  pid_t k = fork();
  if (k == 0) {
    slack();                         /* the parent munmaps while this runs */
    int ok = (m[0] == 'm' && m[4096] == 'm');
    m[0] = 'c';                      /* into a frame the parent has "freed" */
    ok = ok && (m[0] == 'c') && (m[4096] == 'm');
    _exit(ok ? 0 : 9);
  }
  int r = munmap(m, len);            /* must release, not free, these frames */
  int st = 0;
  waitpid(k, &st, 0);
  int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  check(r == 0 && code == 0,
        "T6 a child survives its parent munmapping the page they shared", code, 0);
}

int main(void) {
  memset((void *)page, 0, sizeof page);
  printf("[lives] lifetimes: parent first, child first, three generations, four children, shared\n");

  parent_exits_first();
  child_exits_first();
  three_generations();
  four_children();
  shared_segment();
  munmap_while_shared();

  if (failures) {
    printf("[lives] FAIL %d of %d\n", failures, checked);
    return 1;
  }
  printf("[lives] OK %d/%d\n", checked, checked);
  return 0;
}