/* fork(2) and wait4(2) through a real libc.
 *
 * The point is not that a process exists - it is that it has its *own* memory
 * and its *parent's* descriptors: a write through the inherited pipe must arrive,
 * a write to a global must not, and the exit code must come back through wait4
 * with the shift a shell expects. Each check compares against a value known
 * before the call, never against whatever the kernel happens to return. */
#include <errno.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;

static void check(int ok, const char *what, long got, long want) {
  if (ok) {
    printf("[fork] ok   %s\n", what);
  } else {
    printf("[fork] FAIL %s (got %ld, want %ld)\n", what, got, want);
    fails++;
  }
}

static volatile int marker = 1;

int main(void) {
  char *heap = malloc(16);
  if (!heap) { printf("[fork] FAIL malloc\n"); return 1; }
  strcpy(heap, "parent");

  int fds[2];
  if (pipe(fds) != 0) { printf("[fork] FAIL pipe: %s\n", strerror(errno)); return 1; }

  /* Printed *before* the fork, so the kernel's [cow] lines can be matched to this
   * buffer by address instead of taken on trust. */
  printf("[fork] the inherited buffer is at %p\n", (void *)heap);

  pid_t pid = fork();
  if (pid == 0) {
    /* The child: change everything the parent can see, then report and exit. */
    marker = 99;
    strcpy(heap, "child");
    /* The private-frame check: write a pattern across the whole page this buffer
     * lives on, read it back, and exit non-zero if the child's own store did not
     * land. A copy that never happened still passes a "parent unchanged" test
     * only if the child's store went nowhere - this catches that case. */
    volatile unsigned char *page = (volatile unsigned char *)((uintptr_t)heap & ~0xFFFULL);
    for (int i = 0; i < 4096; i++) page[i] = (unsigned char)(i & 0xFF);
    int wrong = 0;
    for (int i = 0; i < 4096; i++) if (page[i] != (unsigned char)(i & 0xFF)) wrong++;
    if (wrong) _exit(9);          /* the copy took but the contents are wrong */

    int v = 99;
    ssize_t w = write(fds[1], &v, sizeof v);
    _exit(w == (ssize_t)sizeof v ? 7 : 8);
  }

  check(pid > 0, "fork returns the child's pid to the parent", (long)pid, 1);
  if (pid <= 0) {
    /* Everything below asks about a child. Without one there is nothing to ask,
     * and a comparison like "waitpid returned the pid" would pass on -1 == -1. */
    printf("[fork] FAIL no child was created, so nothing below can be checked\n");
    return 1;
  }

  int status = 0;
  pid_t got = waitpid(pid, &status, 0);
  check(got == pid, "waitpid reports the child that exited", (long)got, (long)pid);
  check(WIFEXITED(status) && WEXITSTATUS(status) == 7,
        "the child's exit code came back as 7",
        WIFEXITED(status) ? WEXITSTATUS(status) : -1, 7);

  int v = 0;
  ssize_t n = read(fds[0], &v, sizeof v);
  check(n == (ssize_t)sizeof v && v == 99,
        "the child wrote through the pipe it inherited", (long)v, 99);

  check(marker == 1, "the child's write to a global stayed in the child", marker, 1);
  check(strcmp(heap, "parent") == 0, "and so did its write to the heap",
        (long)(unsigned char)heap[0], 'p');

  errno = 0;
  long again = waitpid(-1, &status, 0);
  check(again == -1 && errno == ECHILD,
        "waiting twice is ECHILD, not a second report",
        again == -1 ? errno : -again, ECHILD);

  /* A second fork from the same parent, this time reaped with WNOHANG after the
   * child has certainly finished: the parent must not have to block for a child
   * that is already gone. */
  pid = fork();
  if (pid == 0) _exit(3);
  if (pid <= 0) {
    printf("[fork] FAIL the second fork made no child either\n");
    return 1;
  }
  status = 0;
  int spins = 0;
  got = 0;
  /* Yield rather than burn: a bounded busy-spin gives up on a child that simply
   * has not been scheduled yet, which is a property of the load, not of waitpid. */
  while (spins++ < 4000 && (got = waitpid(pid, &status, WNOHANG)) == 0) {
    sched_yield();
    for (volatile int i = 0; i < 200; i++) { }
  }
  check(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 3,
        "a WNOHANG wait still reports exit code 3",
        got == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1, 3);

  printf("[fork] %s\n", fails ? "FAIL something above did not hold" : "OK processes copy, descriptors share");
  return fails ? 1 : 0;
}