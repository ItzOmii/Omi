/* stat, the way a libc asks for it.
 *
 * A dynamic linker stats every file before it maps it: size to know how much to
 * map, mode to know whether it is a file or a directory, and it expects a
 * descriptor it already holds to answer the same question through a different
 * syscall. musl answers stat() with newfstatat(AT_FDCWD, path, ...) and fstat()
 * with fstat(fd), so both paths are under test here, plus the two facts a
 * program actually branches on: is it a directory, and how big is it.
 *
 * Everything printed is a comparison against a value we already know, not a
 * dump of whatever the kernel returned - a test that prints the kernel's answer
 * passes when the kernel answers wrongly.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BODY "0123456789"

static int failures;

static void check(int cond, const char *what, long got, long want) {
  if (cond) {
    printf("[stat] ok   %s\n", what);
  } else {
    printf("[stat] FAIL %s (got %ld, want %ld)\n", what, got, want);
    failures++;
  }
}

int main(void) {
  struct stat st;

  /* 1. fstat on a descriptor we own. */
  int fd = open("statfile", O_CREAT | O_RDWR | O_TRUNC);
  if (fd < 0) { printf("[stat] FAIL open: %s\n", strerror(errno)); return 1; }
  if (write(fd, BODY, sizeof BODY - 1) != (ssize_t)(sizeof BODY - 1)) {
    printf("[stat] FAIL write\n");
    return 1;
  }
  check(fstat(fd, &st) == 0, "fstat on an open file", (long)st.st_size, 0);
  check(S_ISREG(st.st_mode), "an open file is a regular file", st.st_mode & S_IFMT, S_IFREG);
  check(st.st_size == (off_t)(sizeof BODY - 1), "and its size is what was written",
        (long)st.st_size, sizeof BODY - 1);
  check(st.st_blksize == 4096, "block size", (long)st.st_blksize, 4096);
  check(st.st_blocks == 1, "one block for ten bytes", (long)st.st_blocks, 1);

  /* 2. The same file by name, through newfstatat, which is what stat() is. */
  struct stat named;
  check(stat("statfile", &named) == 0, "stat by name", (long)named.st_size, 0);
  check(named.st_size == st.st_size, "name and descriptor agree on size",
        (long)named.st_size, (long)st.st_size);

  /* 3. A directory: the one answer a linker uses to decide it is not a file. */
  struct stat dir;
  check(stat("/", &dir) == 0, "stat on /", (long)dir.st_size, 0);
  check(S_ISDIR(dir.st_mode), "/ is a directory", dir.st_mode & S_IFMT, S_IFDIR);

  /* 4. A file that exists but was never opened: the kernel seeds this one with
   *    exactly five bytes, so the size is a fact and not just "something". */
  struct stat seeded;
  if (stat("/etc/version", &seeded) == 0) {
    check(S_ISREG(seeded.st_mode), "a file by name is a regular file",
          seeded.st_mode & S_IFMT, S_IFREG);
    check(seeded.st_size == 5, "/etc/version is five bytes", (long)seeded.st_size, 5);
  } else {
    printf("[stat] FAIL stat(/etc/version): %s\n", strerror(errno));
    failures++;
  }

  /* 5. A directory that is not the root. */
  struct stat etc;
  check(stat("/etc", &etc) == 0 && S_ISDIR(etc.st_mode),
        "/etc is a directory", etc.st_mode & S_IFMT, S_IFDIR);

  /* 6. The answer that must be an error: a name nobody has. */
  errno = 0;
  struct stat missing;
  check(stat("/no-such-file", &missing) == -1 && errno == ENOENT,
        "a missing file fails with ENOENT", -(long)errno, -ENOENT);

  /* 7. And an error on a descriptor that is not open. */
  errno = 0;
  check(fstat(99, &missing) == -1 && errno == EBADF,
        "a closed descriptor fails with EBADF", -(long)errno, -EBADF);

  close(fd);

  if (failures) {
    printf("[stat] %d check(s) failed\n", failures);
    return 1;
  }
  printf("[stat] OK stat and fstat agree with the filesystem\n");
  return 0;
}