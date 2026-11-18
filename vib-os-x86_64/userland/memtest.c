/* How much memory a process can actually get.
 *
 * The page cap exists because a parser on a big page used to run its allocator dry
 * and then write through the NULL. Whether that ceiling is the kernel's mapping
 * window, its area table, or the machine's frames is a question with a number for
 * an answer, and this is how it gets asked: allocate in steps, touch every page
 * (an untouched page costs nothing and proves less), and report where it stopped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  size_t step = 64 * 1024;
  size_t total = 0, largest = 0;
  void *blocks[512];
  int n = 0;

  /* Bounded on purpose. Allocating until it fails does not stop at malloc: the
   * kernel's frame allocator gives out before the address space does, and the
   * fault that follows kills the task - so an unbounded probe answers "how much
   * is there" by dying. 64 MB is enough to prove a process is not cramped, and the
   * number printed is what was actually touched. */
  const size_t ceiling = 64u * 1024 * 1024;
  while (total < ceiling && n < (int)(sizeof blocks / sizeof blocks[0])) {
    void *p = malloc(step);
    if (!p) break;
    memset(p, 0xA5, step);            /* make every page real */
    blocks[n++] = p;
    total += step;
    if (step < (1024u * 1024)) { step *= 2; }
  }
  largest = total;

  long pages = 0;
  {
    /* SYS_MEMINFO: (total << 32) | free, in 4 KB pages */
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(41L), "D"(0L), "S"(0L), "d"(0L) : "memory");
    pages = r & 0xFFFFFFFFL;
  }

  printf("[mem] allocated %lu KB in %d blocks before malloc said no\n",
         (unsigned long)(largest / 1024), n);
  printf("[mem] kernel free: %ld KB of physical memory\n", pages * 4);

  int ok = largest >= (32u * 1024 * 1024);
  printf("[mem] %s a process can hold 32 MB of real pages\n", ok ? "ok  " : "FAIL");
  printf("[mem] %d/%d\n", ok ? 2 : 1, 2);
  return ok ? 0 : 1;
}
