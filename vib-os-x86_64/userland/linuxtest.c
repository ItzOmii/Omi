/* linuxtest - the Linux ABI seen from ring 3, through the `syscall` instruction.
 *
 * The gate's whole risk is in mismatched conventions: a frame built wrong, a
 * number read from the wrong table, a return value that is not quite -errno. So
 * this test drives the instruction the way a libc does - number in rax, args in
 * rdi/rsi/rdx/r10, result in rax - and it also checks that the same mechanisms
 * reachable through int $0x80 are reachable here, because two doors into one
 * kernel are only worth having if they agree.
 */

#define SYS_OMI_EXIT    2
#define SYS_OMI_GETPID  5
#define SYS_OMI_PIPE   34

/* Linux x86-64 */
#define L_READ    0
#define L_WRITE   1
#define L_CLOSE   3
#define L_MMAP    9
#define L_MUNMAP 11
#define L_BRK    12
#define L_PIPE   22
#define L_GETPID 39
#define L_ARCH_PRCTL 158
#define L_CLOCK_GETTIME 228
#define L_PAUSE  34

#define EBADF 9
#define ENOSYS 38
#define EINTR  4

/* Four arguments, because Linux's ABI puts the fourth in r10 rather than rcx -
 * that is the whole reason the syscall stub reads its a3 from a different slot
 * than the int $0x80 stub does. */
static long sysl(long n, long a, long b, long c, long d) {
  long r;
  register long r10 __asm__("r10") = d;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                   : "rcx", "r11", "memory");
  return r;
}

static long siso(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sysl(L_WRITE, 1, (long)s, slen(s), 0); }

static int ok, n;
static void chk(int cond, const char *what) {
  n++;
  if (cond) { ok++; return; }
  out("  FAIL ");
  out(what);
  out("\n");
}

static void num(char *m, int *o, long v) {
  char d[24]; int k = 0;
  unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
  if (v < 0) m[(*o)++] = '-';
  if (!u) d[k++] = '0';
  while (u) { d[k++] = (char)('0' + u % 10); u /= 10; }
  while (k) m[(*o)++] = d[--k];
}

int cmain(int argc, char **argv) {
  (void)argc; (void)argv;

  /* write through the new door. */
  {
    const char *s = "[linuxtest] hello via syscall\n";
    long r = sysl(L_WRITE, 1, (long)s, slen(s), 0);
    chk(r == slen(s), "write(1) through syscall returns the byte count");
  }

  /* The two doors must agree about the same process. */
  {
    long a = sysl(L_GETPID, 0, 0, 0, 0);
    long b = siso(SYS_OMI_GETPID, 0, 0, 0);
    chk(a > 0, "getpid (linux 39) > 0");
    chk(a == b, "getpid agrees across both ABIs");
  }

  /* A number means nothing without the ABI that delivered it: 34 is a pipe
   * through int $0x80 and `pause` through syscall. pause is implemented and says
   * -EINTR, because nothing in this kernel can ever arrive to interrupt it; the
   * second check uses a number no Linux ever defined, so that "unimplemented
   * returns -ENOSYS" stays a test of that rule rather than of a particular gap. */
  {
    int fds[2];
    chk(siso(SYS_OMI_PIPE, (long)fds, 0, 0) == 0, "int $0x80 34 = pipe: still a pipe");
    chk(sysl(L_PAUSE, 0, 0, 0, 0) == -EINTR, "syscall 34 = pause: nothing can arrive, -EINTR");
    chk(sysl(1000, 0, 0, 0, 0) == -ENOSYS, "syscall 1000 = nothing, -ENOSYS");
    if (fds[0] > 0) { siso(26, fds[0], 0, 0); siso(26, fds[1], 0, 0); }
  }

  /* pipe(2) through the Linux door, then write/read through it. */
  {
    int fds[2];
    chk(sysl(L_PIPE, (long)fds, 0, 0, 0) == 0, "pipe(2) via linux numbers");
    chk(sysl(L_WRITE, fds[1], (long)"seven", 5, 0) == 5, "write into the linux pipe");
    char b[8];
    chk(sysl(L_READ, fds[0], (long)b, 8, 0) == 5, "read back through it");
    chk(b[0] == 's' && b[4] == 'n', "bytes intact");
    chk(sysl(L_CLOSE, fds[0], 0, 0, 0) == 0, "close(3)");
    chk(sysl(L_CLOSE, fds[1], 0, 0, 0) == 0, "close(3) other end");
    chk(sysl(L_CLOSE, 999, 0, 0, 0) == -EBADF, "close(999) -> -EBADF, not a fake 0");
  }

  /* clock_gettime(228): a tick-based clock is coarse but must be monotonic and
   * in range - a wrong tv_nsec would break every timeout in a libc. */
  {
    long ts[2] = { -1, -1 };
    chk(sysl(L_CLOCK_GETTIME, 1, (long)ts, 0, 0) == 0, "clock_gettime(CLOCK_MONOTONIC)");
    chk(ts[0] >= 0, "tv_sec >= 0");
    chk(ts[1] >= 0 && ts[1] < 1000000000L, "tv_nsec in range");
    long ts2[2];
    sysl(L_CLOCK_GETTIME, 1, (long)ts2, 0, 0);
    chk(ts2[0] >= ts[0], "time does not go backwards");
  }

  /* brk(12): the heap a libc's malloc will use. Pages are promised here and
   * appear on first touch, so writing through the whole span is the test. */
  {
    long base = sysl(L_BRK, 0, 0, 0, 0);
    chk(base > 0, "brk(0) reports a break");
    long want = base + 65536;
    chk(sysl(L_BRK, want, 0, 0, 0) == want, "brk grows");
    volatile char *p = (volatile char *)base;
    p[0] = 'A';
    p[4095] = 'B';
    p[65535] = 'C';
    chk(p[0] == 'A' && p[4095] == 'B' && p[65535] == 'C', "the whole heap span is writable");
    chk(sysl(L_BRK, base - 4096, 0, 0, 0) == want, "brk refuses to move below its area");
  }

  /* mmap(9) anonymous, munmap(11). */
  {
    long m = sysl(L_MMAP, 0, 65536, 3, 0x22);   /* addr, len, PROT_RW, MAP_PRIVATE|ANON */
    chk(m > 0, "mmap anonymous");
    if (m > 0) {
      volatile char *p = (volatile char *)m;
      p[0] = 'x';
      p[65535] = 'y';
      chk(p[0] == 'x' && p[65535] == 'y', "mapping is writable end to end");
      chk(sysl(L_MUNMAP, m, 65536, 0, 0) == 0, "munmap");
    }
  }

  /* arch_prctl(SET_FS): the mechanism musl's TLS block stands on. If the MSR did
   * not move, %fs-relative reads are reading the wrong address and every
   * thread-local in a real program is garbage. */
  {
    static long tls_probe[4];
    tls_probe[0] = 0x5EED1234L;
    chk(sysl(L_ARCH_PRCTL, 0x1002, (long)tls_probe, 0, 0) == 0, "arch_prctl(ARCH_SET_FS)");
    long v = 0;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(v));
    chk(v == 0x5EED1234L, "%fs:0 reads what was pinned there");
  }

  /* Unknown number: -ENOSYS, so a libc can fall back instead of dying. */
  {
    long r = sysl(9999, 0, 0, 0, 0);
    chk(r == -ENOSYS, "unimplemented number -> -ENOSYS");
  }

  {
    char m[48]; int o = 0;
    const char *pre = "[linux3] ";
    while (*pre) m[o++] = *pre++;
    num(m, &o, ok);
    m[o++] = '/';
    num(m, &o, n);
    const char *post = " ok\n";
    while (*post) m[o++] = *post++;
    m[o] = 0;
    out(m);
    out(ok == n ? "[linux3] VERDICT: the Linux gate works from ring 3\n"
                : "[linux3] VERDICT: something above failed\n");
  }

  siso(SYS_OMI_EXIT, ok == n ? 0 : 1, 0, 0);
  return 0;
}