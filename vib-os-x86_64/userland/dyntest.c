/* The proof that dynamic linking works end to end.
 *
 * This program is not static and not position-dependent: it is a PIE with
 * PT_INTERP /lib/ld-musl-x86_64.so.1 and NEEDED libgreet.so. Nothing in it runs
 * until the kernel has mapped two images and musl's ld.so has relocated both,
 * mapped the library, and filled a GOT entry with greet's address.
 *
 * So the checks are chosen to fail in the interesting ways:
 *
 *   - greet() crossing into a different file proves the PLT/GOT were relocated
 *     by the loader, not folded in by a linker. A statically linked build of this
 *     source would pass it too, which is why the address checks exist: greet must
 *     sit in a page that was mapped by ld.so, above the program itself.
 *   - libc itself is the interpreter, so printf/strlen/malloc succeeding proves
 *     ld.so relocated *itself* before it touched anything else.
 *   - the malloc family exercises libc state that lives in the interpreter's
 *     image: a program that started but had a half-relocated libc would fault
 *     here rather than in the first printf.
 *
 * Printed evidence, in the boot log: where the program is, where the library
 * is, and the value that crossed between them.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Resolved out of libgreet.so at runtime. */
extern int greet(void);
extern int greet_val;
extern void *greet_addr(void);
extern void greet_bump(void);

static int pass, total;

static void check(int cond, const char *what) {
  total++;
  if (cond) {
    pass++;
  } else {
    printf("[dyntest] FAIL %s\n", what);
  }
}

int main(void) {
  /* The library's data symbol: a GLOB_DAT relocation against another file. */
  check(greet_val == 41, "greet_val from libgreet.so");

  /* The library's code: a PLT call into another file. */
  check(greet() == 42, "greet() crosses into libgreet.so");

  /* Where the two images actually are.
   *
   * This is the check that has to be written carefully, because the obvious
   * version proves nothing: linking this program against the library gives
   * greet_val a *copy relocation*, so &greet_val is an address in this
   * executable, filled in once by the loader. Reading it says the loader ran; it
   * says nothing about where the library is. The library itself can say: its own
   * greet_slot() returns the address of the variable its code actually reads,
   * which lives in the mapped file and nowhere else. */
  uintptr_t here = (uintptr_t)(void *)&main;
  uintptr_t lib = (uintptr_t)(void *)greet_addr();
  check(lib > here, "the library is mapped above the program");
  check(lib >= 0x8000400000ULL, "the library came from the mmap window");
  check((lib >> 32) == (here >> 32), "the library is in the same user window");

  /* greet_val is one object, not two: the linker gave this executable a copy
   * relocation and the loader bound the library's references to that copy. So a
   * bump made inside the library has to be visible here, and greet() has to see
   * it too. If the loader had skipped the copy relocation, this program and the
   * library would each have their own 41 and the two would disagree. */
  greet_bump();
  check(greet() == 43, "the library's code sees the bumped value");
  check(greet_val == 42, "one object after the copy relocation");

  /* libc, from the interpreter's own image. */
  check(strlen("omi") == 3, "strlen");
  check(strcmp(strdup("dynamic"), "dynamic") == 0, "strdup + strcmp");
  check(getpid() > 0, "getpid through libc");

  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%s-%d", "omi", 42);
  check(n == 6 && strcmp(buf, "omi-42") == 0, "snprintf");

  /* malloc: state inside the interpreter, and a syscall when the heap grows. */
  char *p = malloc(4096);
  check(p != 0, "malloc");
  if (p) {
    memset(p, 'x', 4096);
    char *q = realloc(p, 65536);
    check(q != 0 && q[0] == 'x' && q[4095] == 'x', "realloc keeps the bytes");
    memset(q, 'y', 65536);
    free(q);
  }
  char *z = calloc(64, 16);
  check(z != 0 && z[0] == 0 && z[1023] == 0, "calloc is zeroed");
  free(z);

  printf("[dyntest] program text 0x%lx, libgreet text 0x%lx, data 0x%lx\n",
         (unsigned long)here, (unsigned long)lib, (unsigned long)(uintptr_t)&greet_val);
  printf("[dyntest] OK %d/%d %s\n", pass, total, pass == total ? "" : "FAILED");
  return pass == total ? 0 : 1;
}