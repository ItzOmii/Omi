/* File-backed mmap: the second thing ld.so needs.
 *
 * ld.so does not read a library into a buffer - it maps each segment and then
 * runs code out of the mapping. So the mapping has to be real: the bytes a
 * program reads through it must be the file's bytes, a write must stay inside
 * the mapping, and MAP_FIXED has to land exactly where it was told, because
 * ld.so maps segments over a reservation it made earlier.
 *
 * The content check is a comparison, not a look: the same byte range is fetched
 * twice, once through read() and once through the mapping, and the two are
 * compared. A mapping that returned zeroes, or the wrong page, or a page of the
 * right length from the wrong offset, fails here.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LIB_PATH "/lib/libgreet.so"
#define PAGE     4096

static int checks, failures;

static void check(int cond, const char *what, long got, long want) {
  checks++;
  if (cond) printf("[maptest] ok   %s\n", what);
  else {
    printf("[maptest] FAIL %s (got %ld, want %ld)\n", what, got, want);
    failures++;
  }
}

int main(void) {
  unsigned char buf[PAGE];

  int fd = open(LIB_PATH, O_RDONLY);
  if (fd < 0) {
    printf("[maptest] FAIL 0/0 no library to map\n");
    return 1;
  }

  /* 1. A read-only mapping of the first page, and it starts with an ELF header. */
  unsigned char *p = mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
  int mapped = (p != MAP_FAILED);
  check(mapped, "mmap(PROT_READ, MAP_PRIVATE, fd, 0)", mapped ? 0 : -1, 0);
  if (!mapped) {
    printf("[maptest] FAIL %d/%d cannot continue\n", checks - failures, checks);
    return 1;
  }
  unsigned long magic = ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
                        ((unsigned long)p[2] << 8) | (unsigned long)p[3];
  check(magic == 0x7f454c46UL, "the mapping starts with ELF magic", (long)magic, 0x7f454c46L);

  /* 2. The same page through read() and through the mapping must be identical.
   * Offset 4096, not 0: page zero can be right by accident, a later page cannot. */
  unsigned char *q = mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE);
  check(q != MAP_FAILED, "mmap at offset 4096", q == MAP_FAILED ? -1 : 0, 0);
  memset(buf, 0, sizeof buf);
  int rn = -1;
  if (q != MAP_FAILED) {
    if (lseek(fd, PAGE, SEEK_SET) == PAGE) {
      rn = 0;
      while (rn < PAGE) {
        ssize_t n = read(fd, buf + rn, PAGE - rn);
        if (n <= 0) break;
        rn += n;
      }
    }
    int same = (rn == PAGE) && memcmp(buf, q, PAGE) == 0;
    check(same, "mapped page 1 equals read() of the same range", same ? 0 : -1, 0);
    /* A page of all zeroes would pass a length check and fail a real comparison;
     * say so by requiring the page to contain something. */
    int nonzero = 0;
    for (int i = 0; i < PAGE; i++) if (q[i]) { nonzero = 1; break; }
    check(nonzero, "and that page is not all zeroes", nonzero, 1);
    munmap(q, PAGE);
  }

  /* 3. A private mapping takes a write without disturbing the file. */
  unsigned char *w = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  check(w != MAP_FAILED, "mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE)", w == MAP_FAILED ? -1 : 0, 0);
  if (w != MAP_FAILED) {
    unsigned char before = w[8];
    w[8] = (unsigned char)(before ^ 0xFF);
    check(w[8] == (unsigned char)(before ^ 0xFF), "a write lands in the mapping", w[8], before ^ 0xFF);
    /* A second mapping of the same file must still show the original byte. */
    unsigned char *w2 = mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    check(w2 != MAP_FAILED && w2[8] == before,
          "a fresh mapping still reads the file's byte", w2 == MAP_FAILED ? -1 : w2[8], before);
    if (w2 != MAP_FAILED) munmap(w2, PAGE);
    munmap(w, PAGE);
  }

  /* 4. A reservation, then a segment mapped over it exactly where asked - the
   * shape ld.so uses for every library it loads. */
  unsigned char *resv = mmap(0, 2 * PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check(resv != MAP_FAILED, "reserve 8 KB anonymously", resv == MAP_FAILED ? -1 : 0, 0);
  if (resv != MAP_FAILED) {
    unsigned char *f = mmap(resv, PAGE, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
    check(f == resv, "MAP_FIXED lands at the reserved address", f == MAP_FAILED ? -1 : (long)f, (long)resv);
    unsigned long m2 = 0;
    if (f == resv)
      m2 = ((unsigned long)f[0] << 24) | ((unsigned long)f[1] << 16) |
           ((unsigned long)f[2] << 8) | (unsigned long)f[3];
    check(m2 == 0x7f454c46UL, "and the file is there, not the reservation", (long)m2, 0x7f454c46L);
    munmap(resv, 2 * PAGE);
  }

  /* 5. The refusals. Each of these would otherwise load a library with nothing
   * behind it, which is a bug that shows up as a crash inside code the linker
   * thinks it mapped. */
  errno = 0;
  void *bad = mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, 123);
  check(bad == MAP_FAILED, "an unaligned offset is refused", bad == MAP_FAILED ? -1 : 0, -1);
  check(errno == EINVAL, "  with EINVAL", errno, EINVAL);

  errno = 0;
  off_t end = lseek(fd, 0, SEEK_END);
  bad = mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, (off_t)((end + PAGE - 1) & ~(off_t)0xFFF));
  check(bad == MAP_FAILED, "an offset at EOF is refused", bad == MAP_FAILED ? -1 : 0, -1);
  check(errno == EINVAL, "  with EINVAL too", errno, EINVAL);

  errno = 0;
  bad = mmap(0, PAGE, PROT_READ, MAP_SHARED, fd, 0);
  check(bad == MAP_FAILED, "a shared file mapping is refused", bad == MAP_FAILED ? -1 : 0, -1);
  check(errno == ENOSYS, "  because nothing can write it back (ENOSYS)", errno, ENOSYS);

  munmap(p, PAGE);

  if (failures) {
    printf("[maptest] FAIL %d/%d\n", checks - failures, checks);
    return 1;
  }
  printf("[maptest] OK %d/%d (file-backed MAP_PRIVATE, MAP_FIXED over a reservation, refusals)\n",
         checks, checks);
  return 0;
}