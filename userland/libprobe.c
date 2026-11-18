/* Boot modules as files - the read half of dynamic linking.
 *
 * A dynamic linker does three things before any of our code runs: it opens
 * "/lib/libfoo.so" by name, it stats the descriptor to learn the size it has to
 * map, and it reads or maps the bytes. Two of those three are file operations,
 * and until now nothing on this system could name a file the kernel was not
 * already handed as a path on the command line.
 *
 * So this test is narrow on purpose. It does not try to be a dynamic linker: it
 * proves that a module the bootloader put in memory is reachable as a file under
 * its boot path, that read() returns the module's real bytes right to the last
 * one, and that the size the program is told is the size the file actually is -
 * the three facts ld.so would commit a mapping to before it ever touches a
 * relocation table. File-backed mmap is not built yet; when it is, this test
 * stays the proof that the bytes under it are the right bytes.
 *
 * Every check compares against a value known in advance. Printing whatever the
 * kernel returned would pass when the kernel answered wrongly.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* libgreet.so is ~16 KB: bigger than the 4 KB heap file cap this system used to
 * apply to every file, so a size at or under 4096 means the module path fell
 * back to something that copied it instead of pointing at it. */
#define LIB_PATH   "/lib/libgreet.so"
#define LIB_MIN    8192
#define CHUNK      512

static int failures;
static int checks;

static void check(int cond, const char *what, long got, long want) {
  checks++;
  if (cond) {
    printf("[modtest] ok   %s\n", what);
  } else {
    printf("[modtest] FAIL %s (got %ld, want %ld)\n", what, got, want);
    failures++;
  }
}

/* The module's symbol table carries its own function name as a string. Finding
 * "greet" in the bytes proves the contents arrived, not just a plausible size. */
static int contains(const unsigned char *buf, size_t n, const char *needle) {
  size_t want = strlen(needle);
  if (n < want) return 0;
  for (size_t i = 0; i + want <= n; i++) {
    size_t j = 0;
    while (j < want && buf[i + j] == (unsigned char)needle[j]) j++;
    if (j == want) return 1;
  }
  return 0;
}

int main(void) {
  unsigned char buf[CHUNK];
  struct stat st;

  /* 1. A module is openable by the path Limine was given for it. */
  int fd = open(LIB_PATH, O_RDONLY);
  check(fd >= 0, "open(/lib/libgreet.so)", fd, 0);
  if (fd < 0) {
    printf("[modtest] FAIL 0/%d (cannot continue without the file)\n", checks);
    return 1;
  }

  /* 2. The first four bytes are an ELF header, as the linker expects. The magic
   * is read through the descriptor, so this also proves read() starts at zero. */
  ssize_t n = read(fd, buf, 4);
  unsigned long magic = (n == 4) ? (((unsigned long)buf[0] << 24) |
                                    ((unsigned long)buf[1] << 16) |
                                    ((unsigned long)buf[2] << 8) |
                                    (unsigned long)buf[3]) : 0;
  check(magic == 0x7f454c46UL, "read() returns ELF magic", (long)magic, 0x7f454c46L);

  /* 3. The reported length is the module's, not a buffer's. lseek(END) is what
   * ld.so would use if it distrusted stat(). */
  off_t size = lseek(fd, 0, SEEK_END);
  check(size > LIB_MIN, "file length is the module's (>8192)", (long)size, LIB_MIN + 1);
  check(lseek(fd, 0, SEEK_SET) == 0, "and lseek back to the start works", 0, 0);

  /* 4. stat() on the descriptor agrees with the length, and it is a regular
   * file - a directory or a device would make ld.so give up on the spot. */
  check(fstat(fd, &st) == 0, "fstat on a module file", 0, 0);
  check(S_ISREG(st.st_mode), "a module file is a regular file",
        (long)(st.st_mode & S_IFMT), S_IFREG);
  check((off_t)st.st_size == size, "fstat size matches lseek size",
        (long)st.st_size, (long)size);

  /* 5. Read it all back, in chunks the size of a sector, and count. A short read
   * anywhere loses bytes and the total will say so. */
  long total = 0;
  int found = 0;
  ssize_t got;
  while ((got = read(fd, buf, CHUNK)) > 0) {
    if (contains(buf, (size_t)got, "greet")) found = 1;
    total += got;
    if (total > (long)size + CHUNK) break;   /* runaway read: stop early */
  }
  check(total == (long)size, "read to EOF returns every byte", total, (long)size);
  check(got == 0, "read at EOF returns 0, not -1", (long)got, 0);
  check(found, "the module's own symbols are in the bytes", found, 1);
  close(fd);

  /* 6. A name that is not a module fails as a missing file, not as a panic or an
   * empty descriptor: ld.so has to be able to report "library not found". */
  errno = 0;
  int bad = open("/lib/not-a-module.so", O_RDONLY);
  check(bad < 0, "open of a missing library fails", bad, -1);
  check(errno == ENOENT, "and errno says ENOENT", errno, ENOENT);

  /* 7. Modules are the bootloader's memory. Opening one for writing must be
   * refused rather than truncating a running program's image. */
  errno = 0;
  int wfd = open(LIB_PATH, O_WRONLY);
  check(wfd < 0, "open(O_WRONLY) on a module fails", wfd, -1);
  check(errno == EROFS, "and errno says EROFS", errno, EROFS);
  if (wfd >= 0) close(wfd);

  /* 8. The RAM files still behave: exposing modules must not have shadowed or
   * rewritten the seeded ones. */
  int rfd = open("/hello.txt", O_RDONLY);
  char head[32];
  memset(head, 0, sizeof head);
  ssize_t rn = (rfd >= 0) ? read(rfd, head, sizeof head - 1) : -1;
  if (rfd >= 0) close(rfd);
  check(rn > 0 && strncmp(head, "hello from omi", 14) == 0,
        "/hello.txt is still the seeded RAM file", (long)rn, 1);

  if (failures) {
    printf("[modtest] FAIL %d/%d\n", checks - failures, checks);
    return 1;
  }
  printf("[modtest] OK %d/%d (module %ld bytes, %d chunks of %d)\n",
         checks, checks, (long)size, (int)(size / CHUNK), CHUNK);
  return 0;
}