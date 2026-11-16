/* memfd + MAP_SHARED: one set of bytes with two doors onto it.
 *
 * Chrome's shared regions are this pair - memfd_create for the object, mmap
 * MAP_SHARED over it - and what it assumes is not that the call succeeds but
 * that a write through one mapping is visible through the other, through the
 * descriptor, and in a forked child. So the checks are about sameness, not about
 * return codes: every one of them writes into one door and reads out of another.
 *
 * The refusals are checked as refusals too. A shared mapping of a read-only boot
 * module, a truncation of one, huge pages, and a request that says both PRIVATE
 * and SHARED: each has a right answer, and guessing one would be a program that
 * cannot tell whether it was obeyed.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failures;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) {
    failures++;
    printf("[memfdtest] FAIL %s (got %ld want %ld)\n", what, got, want);
  } else {
    printf("[memfdtest] ok   %s\n", what);
  }
}

static long fd_size(int fd) {
  off_t at = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  return (long)at;
}

int main(void) {
  /* ---- the object itself ---- */
  int fd = (int)syscall(SYS_memfd_create, "region", 0);
  check(fd >= 3, "memfd_create hands back a descriptor", fd, 3);
  if (fd < 0) { printf("[memfdtest] cannot continue\n"); return 1; }

  check(fd_size(fd) == 0, "a new shared file is empty", fd_size(fd), 0);
  check(ftruncate(fd, 8192) == 0, "ftruncate sizes it", 0, 0);
  check(fd_size(fd) == 8192, "and the size sticks", fd_size(fd), 8192);

  long bad = (long)syscall(SYS_memfd_create, "huge", 4);
  check(bad == -1 && errno == EINVAL, "huge pages are refused, not faked", errno, EINVAL);

  /* ---- the file is the memory ---- */
  if (write(fd, "hello", 5) != 5) { printf("[memfdtest] write failed\n"); return 1; }
  char *a = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  check(a != MAP_FAILED, "MAP_SHARED over a memfd maps", (long)(a != MAP_FAILED), 1);
  if (a == MAP_FAILED) { printf("[memfdtest] cannot continue\n"); return 1; }
  check(memcmp(a, "hello", 5) == 0, "the mapping starts as the file's bytes", a[0], 'h');

  memcpy(a + 16, "from the mapping", 16);
  char back[32];
  memset(back, 0, sizeof back);
  lseek(fd, 16, SEEK_SET);
  long n = read(fd, back, 16);
  check(n == 16 && memcmp(back, "from the mapping", 16) == 0,
        "read(2) sees what the mapping wrote", n, 16);

  /* ---- two mappings, one file ---- */
  char *b = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  check(b != MAP_FAILED && b != a, "the same file maps a second time, elsewhere",
        (long)(b != MAP_FAILED && b != a), 1);
  if (b != MAP_FAILED) {
    b[4096] = 0x5A;
    check(a[4096] == 0x5A, "a write through one mapping appears in the other", a[4096], 0x5A);
  }

  /* ---- a mapping outlives the descriptor her before it ---- */
  int fd2 = (int)syscall(SYS_memfd_create, "survivor", 0);
  check(fd2 >= 3, "a second shared file is its own object", fd2, 3);
  if (fd2 >= 3) {
    ftruncate(fd2, 4096);
    char *c2 = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
    close(fd2);
    long ok = 0;
    if (c2 != MAP_FAILED) { c2[0] = 0x11; ok = (c2[0] == 0x11); }
    check(ok, "closing the descriptor leaves the mapping usable", ok, 1);
  }

  /* ---- and across a fork, which is the whole point ---- */
  ftruncate(fd, 8192);
  pid_t kid = fork();
  if (kid == 0) {
    a[2048] = 0x7E;
    _exit(0);
  }
  int st = 0;
  waitpid(kid, &st, 0);
  check(a[2048] == 0x7E, "a child's write lands in the parent's mapping", a[2048], 0x7E);

  /* ---- MAP_PRIVATE is a copy, and starts as one ---- */
  char *priv = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  check(priv != MAP_FAILED, "MAP_PRIVATE over a memfd maps too", (long)(priv != MAP_FAILED), 1);
  if (priv != MAP_FAILED) {
    check(memcmp(priv, a, 8192) == 0, "a private mapping starts as a copy of the file",
          priv[0], a[0]);
    priv[100] = 0x33;
    check(a[100] != 0x33, "a write to the copy does not reach the file", a[100] != 0x33, 1);
  }

  /* ---- anonymous shared, which used to be silently private ---- */
  char *anon = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  check(anon != MAP_FAILED, "an anonymous shared mapping maps", (long)(anon != MAP_FAILED), 1);
  if (anon != MAP_FAILED) {
    anon[0] = 0;
    kid = fork();
    if (kid == 0) { anon[0] = 0x44; _exit(0); }
    waitpid(kid, &st, 0);
    check(anon[0] == 0x44, "and a forked child shares it", anon[0], 0x44);
  }

  /* ---- the refusals ---- */
  int bad2 = (int)syscall(SYS_memfd_create, "both", 0);
  errno = 0;
  char *m = mmap(0, 4096, PROT_READ, MAP_SHARED | MAP_PRIVATE, bad2, 0);
  check(m == MAP_FAILED && errno == EINVAL, "SHARED and PRIVATE together is refused",
        errno, EINVAL);
  errno = 0;
  m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, bad2, 0);
  check(m == MAP_FAILED && errno == EINVAL, "a mapping longer than the file is refused",
        errno, EINVAL);
  close(bad2);

  /* a boot module: read-only memory with no write-back path, so sharing is a
   * refusal rather than a lie */
  int ro = open("/apps/cat.elf", O_RDONLY);
  if (ro >= 3) {
    errno = 0;
    m = mmap(0, 4096, PROT_READ, MAP_SHARED, ro, 0);
    check(m == MAP_FAILED && errno == ENOSYS, "a boot module cannot be shared",
          errno, ENOSYS);
    errno = 0;
    long r = ftruncate(ro, 0);
    check(r == -1 && errno == EROFS, "and cannot be truncated", errno, EROFS);
    close(ro);
  }

  /* ---- and the pages go back ---- */
  check(close(fd) == 0, "the descriptor closes", 0, 0);
  printf("[memfdtest] %d/%d\n", checks - failures, checks);
  if (failures) printf("[memfdtest] %d FAILED\n", failures);
  return failures ? 1 : 0;
}
