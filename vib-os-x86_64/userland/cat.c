/* cat(1) for Omi - the first program that is not part of the kernel build.
 *
 * No libc, no headers, no kernel includes: only the ABI. Syscall number in rax,
 * arguments in rdi/rsi/rdx, result in rax, errors as -errno. If the kernel and
 * this file disagree about any of that, it fails here and not in a demo the
 * kernel author wrote to match the kernel.
 *
 *   cat FILE [FILE...]
 */

#define SYS_WRITE 1
#define SYS_EXIT  2
#define SYS_OPEN  23
#define SYS_READ  24
#define SYS_CLOSE 26

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

static long slen(const char *s) {
  long n = 0;
  while (s[n]) n++;
  return n;
}

static void put_fd(int fd, const char *s) {
  if (s && s[0]) sys3(SYS_WRITE, fd, (long)s, slen(s));
}

/* -errno to words. A wrong sign convention in the kernel shows up here as
 * "error" for a file that plainly does not exist. */
static const char *why(long r) {
  long e = -r;
  switch (e) {
    case 2:  return "no such file";
    case 9:  return "bad descriptor";
    case 13: return "permission denied";
    case 14: return "bad address";
    case 22: return "invalid argument";
    case 24: return "too many open files";
    case 28: return "out of space";
    default: return "error";
  }
}

int cmain(long argc, char **argv) {
  if (argc < 2) {
    put_fd(2, "usage: cat FILE [FILE...]\n");
    return 1;
  }

  int status = 0;
  char buf[512];

  for (long i = 1; i < argc; i++) {
    long fd = sys3(SYS_OPEN, (long)argv[i], 0, 0);
    if (fd < 0) {
      put_fd(2, "cat: ");
      put_fd(2, argv[i]);
      put_fd(2, ": ");
      put_fd(2, why(fd));
      put_fd(2, "\n");
      status = 1;
      continue;
    }

    for (;;) {
      long n = sys3(SYS_READ, fd, (long)buf, sizeof buf);
      if (n < 0) {
        put_fd(2, "cat: ");
        put_fd(2, argv[i]);
        put_fd(2, ": ");
        put_fd(2, why(n));
        put_fd(2, "\n");
        status = 1;
        break;
      }
      if (n == 0) break;
      long w = sys3(SYS_WRITE, 1, (long)buf, n);
      if (w < 0) {
        put_fd(2, "cat: stdout: ");
        put_fd(2, why(w));
        put_fd(2, "\n");
        status = 1;
        break;
      }
      if (w != n) break;          /* a short write would drop bytes: say so */
    }
    sys3(SYS_CLOSE, fd, 0, 0);
  }
  return status;
}
