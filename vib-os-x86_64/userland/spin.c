/* spin - a ring-3 task that makes no syscalls at all.
 *
 * This exists to answer one question: while ring 3 is busy with nothing but its
 * own arithmetic, does the kernel still get to run? Nothing here yields, sleeps
 * or traps, so any kernel progress during the loop is the scheduler's doing and
 * not the program's.
 *
 * The loop is bounded by a count, not by the clock, so it always ends. A test
 * that could hang the machine would be a worse bug than the one it looks for.
 */

#define SYS_EXIT 2

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

#define SPIN_ROUNDS 60000000UL

int cmain(int argc, char **argv) {
  (void)argc; (void)argv;
  volatile unsigned long sink = 0;      /* volatile: the loop must not vanish */
  for (unsigned long i = 0; i < SPIN_ROUNDS; i++) sink = sink + i;
  return (int)(sink & 1);
}