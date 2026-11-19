/* sandtest - a process asks to be confined, and the kernel holds it to that.
 *
 * Chromium's renderer runs with a filter over the syscalls it may make: the
 * renderer is the part that parses somebody else's HTML, and the bet is that a
 * process which cannot open a file, start another process or reach the network
 * is worth losing. This is the smallest kernel mechanism that bet needs — a
 * bitmap of allowed syscall numbers per task, checked at the door, narrowing
 * only, inherited by fork and clone.
 *
 * Everything except the calls under test goes through musl, so the parent talks
 * to the kernel the Linux way. The children do not: once a mask is installed,
 * the Linux ABI is itself a violation (it is a second number space the filter
 * was never written against), so a confined child makes native calls and
 * nothing else. That is also what makes the test honest: what it asserts is the
 * door, not a helper's promise to behave.
 *
 * The violation code is 159, which is what an uncaught SIGSYS does on Linux;
 * here it arrives the way every exit code does, in bits 8-15 of the status.
 */
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYS_WRITE    1
#define SYS_EXIT     2
#define SYS_YIELD    3
#define SYS_GETPID   5
#define SYS_SANDBOX 55
#define SYS_FORK    56
#define SYS_WAIT4   57

#define SBX 159      /* what a violation exits with */

static int checks, fails;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) { fails++; printf("[sandtest] FAIL %s (got %ld want %ld)\n", what, got, want); }
  else printf("[sandtest] ok   %s\n", what);
}

/* ---- native doors. int $0x80 is this kernel's own ABI; the syscall
 * instruction is the Linux one, which a confined task may not use. */
static long n0(long n) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n) : "memory");
  return r;
}
static long n1(long n, long a) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a) : "memory");
  return r;
}
static long n2(long n, long a, long b) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "memory");
  return r;
}
static long n3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}
/* The door takes the fourth argument from r8, not rcx (which int $0x80 never
 * carries) - a helper that stops at rdx leaves a3 holding whatever the caller
 * last put there, and wait4 read that as a rusage pointer. */
static long n4(long n, long a, long b, long c, long d) {
  register long a3 __asm__("r8") = d;
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(a3) : "memory");
  return r;
}

/* One bit per syscall number, low bit of word 0 first. */
#define BIT(n) (1ULL << (n))

/* Install a one-word mask. Returns 0, or the negative errno. */
static long confine(uint64_t w0) {
  uint64_t m = w0;
  return n2(SYS_SANDBOX, (long)(uintptr_t)&m, 1);
}

/* Fork, and hand back the child's pid (0 in the child). */
static long spork(void) { return n0(SYS_FORK); }

/* Reap one child natively; -1 if the call refuses. */
static long sreap(long pid, int *status) {
  return n4(SYS_WAIT4, pid, (long)(uintptr_t)status, 0, 0);
}

static int code_of(int status) { return (status >> 8) & 0xFF; }

int main(void) {
  /* 1. The calls the mask allows still work, and the exit code still arrives. */
  {
    long pid = fork();
    if (pid == 0) {
      confine(BIT(SYS_WRITE) | BIT(SYS_EXIT));
      n3(SYS_WRITE, 1, (long)(uintptr_t)"[sandtest]    (a confined child is talking)\n", 46);
      n1(SYS_EXIT, 7);
      for (;;) n0(SYS_YIELD);          /* never reached */
    }
    int st = 0;
    waitpid((pid_t)pid, &st, 0);
    check(WIFEXITED(st) && code_of(st) == 7,
          "a confined child runs what its mask allows, and exits with its code", code_of(st), 7);
  }

  /* 2. A call the mask does not name ends the task before it takes effect. */
  {
    long pid = fork();
    if (pid == 0) {
      confine(BIT(SYS_WRITE) | BIT(SYS_EXIT));
      n0(SYS_GETPID);                  /* not in the mask */
      n1(SYS_EXIT, 3);                 /* must never be reached */
      for (;;) n0(SYS_YIELD);
    }
    int st = 0;
    waitpid((pid_t)pid, &st, 0);
    check(WIFEXITED(st) && code_of(st) == SBX,
          "a call outside the mask ends the task, with SIGSYS's code", code_of(st), SBX);
  }

  /* 3. A mask cannot be widened by installing another one. */
  {
    long pid = fork();
    if (pid == 0) {
      confine(BIT(SYS_WRITE) | BIT(SYS_EXIT) | BIT(SYS_SANDBOX));
      long r = confine(~0ULL);         /* "allow everything", after the fact */
      if (r != 0) n1(SYS_EXIT, 4);
      n0(SYS_GETPID);                  /* must still be outside the filter */
      n1(SYS_EXIT, 3);
      for (;;) n0(SYS_YIELD);
    }
    int st = 0;
    waitpid((pid_t)pid, &st, 0);
    check(WIFEXITED(st) && code_of(st) == SBX,
          "a second mask cannot widen the first one", code_of(st), SBX);
  }

  /* 4. Forking does not hand the child a wider door. */
  {
    long pid = fork();
    if (pid == 0) {
      confine(BIT(SYS_WRITE) | BIT(SYS_EXIT) | BIT(SYS_FORK) | BIT(SYS_WAIT4));
      long g = spork();
      if (g == 0) {                    /* the grandchild: same filter, no escape */
        n0(SYS_GETPID);
        n1(SYS_EXIT, 3);
        for (;;) n0(SYS_YIELD);
      }
      int st2 = 0;
      long r = sreap(g, &st2);
      if (r < 0) n1(SYS_EXIT, 5);
      n1(SYS_EXIT, code_of(st2));      /* pass the grandchild's code up */
      for (;;) n0(SYS_YIELD);
    }
    int st = 0;
    waitpid((pid_t)pid, &st, 0);
    check(WIFEXITED(st) && code_of(st) == SBX,
          "a forked child inherits the filter", code_of(st), SBX);
  }

  /* 5. The mask is a list, not a death sentence: the same call that was a
   * violation in test 2 is ordinary when the mask names it. */
  {
    long pid = fork();
    if (pid == 0) {
      confine(BIT(SYS_WRITE) | BIT(SYS_EXIT) | BIT(SYS_GETPID));
      long me = n0(SYS_GETPID);
      n1(SYS_EXIT, me > 0 ? 0 : 6);
      for (;;) n0(SYS_YIELD);
    }
    int st = 0;
    waitpid((pid_t)pid, &st, 0);
    check(WIFEXITED(st) && code_of(st) == 0,
          "a call inside the mask is allowed - the same one test 2 was killed for",
          code_of(st), 0);
  }

  printf("[sandtest] %d/%d ok\n", checks - fails, checks);
  return fails ? 1 : 0;
}