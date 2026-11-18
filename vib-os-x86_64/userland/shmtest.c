/* shmtest - shared memory the way a multi-process browser uses it.
 *
 * A browser hands memory to a renderer through a file descriptor: a frame
 * buffer, a command buffer, a ring of IPC messages. Two processes map the same
 * memfd with MAP_SHARED and see the same bytes. That path goes through the Linux
 * ABI (memfd_create, ftruncate, mmap), which is the ABI Chromium will arrive on,
 * so testing Omi's private shared-slot syscall here would test the wrong thing.
 *
 * What has to be true for an engine to build on this:
 *
 *   - a memfd can be sized to several megabytes
 *   - two mappings of the same memfd alias, which is the whole point
 *   - two different memfds do not alias
 *   - a browser opens many of these at once, so 16 live regions must work
 *   - a mapping larger than the process's address window is refused instead of
 *     granted and then faulting
 */
#define _GNU_SOURCE 1   /* memfd_create lives behind it in musl */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MB (1024u * 1024u)
#define BIG_MB   8u
#define MANY     16
#define EACH_MB  2u

static int checks, fails;

static void ck(int cond, const char *what, long got) {
  if (cond) {
    printf("[shmtest] ok   %s\n", what);
  } else {
    printf("[shmtest] FAIL %s (got %ld)\n", what, got);
    fails++;
  }
  checks++;
}

static unsigned at_mb(const void *p) { return (unsigned)((uintptr_t)p >> 20); }

int main(void) {
  int fd = memfd_create("region", 0);
  ck(fd >= 0, "a memfd can be created", fd);
  if (fd < 0) { printf("[shmtest] %d/%d\n", checks - fails, checks); return 1; }

  long size = (long)BIG_MB * MB;
  ck(ftruncate(fd, size) == 0, "it can be sized to 8 MB", -1);

  volatile unsigned char *a = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, 0);
  ck(a != MAP_FAILED, "an 8 MB shared mapping is granted", a == MAP_FAILED ? -1 : 0);
  if (a == MAP_FAILED) { printf("[shmtest] %d/%d\n", checks - fails, checks); return 1; }

  volatile unsigned char *b = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, 0);
  ck(b != MAP_FAILED, "the same region maps a second time", b == MAP_FAILED ? -1 : 0);

  if (b != MAP_FAILED) {
    printf("[shmtest] first mapping at %u MB, second at %u MB\n", at_mb((void*)a), at_mb((void*)b));
    a[0] = 0x77;
    a[(size_t)size - 1] = 0x66;              /* the last byte of 8 MB */
    a[(size_t)size / 2] = 0x3c;
    ck(b[0] == 0x77, "a write at the start is visible in the other mapping", b[0]);
    ck(b[(size_t)size - 1] == 0x66, "a write at the end is visible too", b[(size_t)size - 1]);
    ck(b[(size_t)size / 2] == 0x3c, "and so is the middle", b[(size_t)size / 2]);
    b[0] = 0x11;
    ck(a[0] == 0x11, "it is shared in the other direction as well", a[0]);
    munmap((void *)b, (size_t)size);
  }

  /* A second region must not be the first region. */
  int fd2 = memfd_create("other", 0);
  if (fd2 >= 0) {
    ftruncate(fd2, (long)MB);
    volatile unsigned char *c = mmap(NULL, MB, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
    if (c != MAP_FAILED) {
      c[0] = 0x22;
      ck(a[0] == 0x11, "a different region does not alias the first", a[0]);
      munmap((void *)c, MB);
    }
    close(fd2);
  }

  /* How a browser actually holds these: many at once. */
  int fds[MANY];
  void *maps[MANY];
  int made = 0;
  for (int i = 0; i < MANY; i++) {
    fds[i] = memfd_create("frame", 0);
    maps[i] = MAP_FAILED;
    if (fds[i] < 0) continue;
    if (ftruncate(fds[i], (long)EACH_MB * MB) != 0) { close(fds[i]); continue; }
    maps[i] = mmap(NULL, (size_t)EACH_MB * MB, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fds[i], 0);
    if (maps[i] == MAP_FAILED) { close(fds[i]); continue; }
    ((volatile unsigned char *)maps[i])[0] = (unsigned char)(0x40 + i);
    made++;
  }
  ck(made == MANY, "16 regions of 2 MB are live at the same time", made);
  int held = 0;
  for (int i = 0; i < MANY; i++)
    if (maps[i] != MAP_FAILED && ((volatile unsigned char *)maps[i])[0] == (unsigned char)(0x40 + i))
      held++;
  ck(held == made, "every one of them still holds its own byte", held);
  for (int i = 0; i < MANY; i++) {
    if (maps[i] != MAP_FAILED) munmap(maps[i], (size_t)EACH_MB * MB);
    if (fds[i] >= 0) close(fds[i]);
  }

  /* Past the window: refused, not granted and then faulting. */
  void *too_big = mmap(NULL, (size_t)8192 * MB, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  ck(too_big == MAP_FAILED, "a mapping past the window is refused", 0);
  if (too_big != MAP_FAILED) munmap(too_big, (size_t)8192 * MB);

  munmap((void *)a, (size_t)size);
  close(fd);

  printf("[shmtest] %d/%d\n", checks - fails, checks);
  return fails ? 1 : 0;
}
