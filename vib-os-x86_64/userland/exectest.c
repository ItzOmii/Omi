/* execve: the last step between "a program can load a library" and "a program can
 * start another program".
 *
 * Four things are checked, in the order they matter:
 *
 *   1. a name that is not on the medium fails and the caller keeps running - the
 *      refusal has to leave the calling image intact, which is only provable by
 *      still being there to print the next line;
 *   2. the child becomes the target: argv arrives, the descriptor it inherited
 *      still points at the open file, and the exit code is the target's rather
 *      than the 9 the forked child would have returned;
 *   3. a position-independent program can be exec'd too, so the path runs through
 *      the ELF loader and musl's ld.so instead of around them;
 *   4. the parent's own memory is untouched by its child's exec - the fork/COW
 *      question that replacing an address space has to answer.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failures;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) {
    failures++;
    printf("[exectest] FAIL %s (got %ld want %ld)\n", what, got, want);
  } else {
    printf("[exectest] ok   %s\n", what);
  }
}

int main(void) {
  char *bad[] = { "exectarget", 0 };
  errno = 0;
  long r = (long)execve("/apps/not-a-program.elf", bad, 0);
  check(r == -1 && errno == ENOENT, "a name that is not on the medium fails", r, -1);
  check(1, "and the caller is still running afterwards", 1, 1);

  int pfd[2];
  static char payload[8] = "carried";
  check(pipe(pfd) == 0, "a pipe for the descriptor that must survive exec", 0, 0);
  check(write(pfd[1], payload, 7) == 7, "and it carries a message", 7, 7);

  pid_t kid = fork();
  if (kid == 0) {
    char fd[16];
    snprintf(fd, sizeof fd, "%d", pfd[0]);
    char *av[] = { "exectarget", fd, 0 };
    execve("/apps/exectarget.elf", av, 0);
    _exit(9);                         /* reached only when execve itself failed */
  }
  int st = 0;
  int reaped = (int)waitpid(kid, &st, 0);
  int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  check(reaped == (int)kid && code == 42,
        "the child became the target: argv and an inherited descriptor arrived", code, 42);

  unsigned char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int mapped = p != MAP_FAILED;
  if (mapped) p[0] = 0x5A;

  pid_t k2 = fork();
  if (k2 == 0) {
    char *av[] = { "dyntest", 0 };
    execve("/apps/dyntest.elf", av, 0);
    _exit(9);
  }
  int st2 = 0;
  int reaped2 = (int)waitpid(k2, &st2, 0);
  int code2 = WIFEXITED(st2) ? WEXITSTATUS(st2) : -1;
  check(reaped2 == (int)k2 && code2 == 0,
        "a position-independent program can be exec'd as well", code2, 0);
  check(mapped && p[0] == 0x5A,
        "and the parent's own page is untouched by its child's exec", mapped ? (long)p[0] : -1, 0x5A);

  if (failures) {
    printf("[exectest] %d of %d checks failed\n", failures, checks);
    return 1;
  }
  printf("[exectest] OK %d/%d (refusal, argv, inherited descriptor, PIE through the loader, parent intact)\n",
         checks, checks);
  return 0;
}
