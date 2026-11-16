/* The program exectest becomes.
 *
 * It answers three questions about how it was started - what argv it got, and
 * whether the descriptor it was handed still reads - and the answer is its exit
 * code, because output from a replaced process is easy to believe and hard to
 * check: 42 when everything arrived, 43 when the descriptor did not. The bytes it
 * is expected to find are the ones its parent put in the pipe before the exec, so
 * a descriptor that survived as a number but lost its open file cannot pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  char buf[16];
  memset(buf, 0, sizeof buf);
  int fd = argc > 1 ? atoi(argv[1]) : -1;
  int n = fd >= 0 ? (int)read(fd, buf, 7) : -1;
  printf("[target] argc=%d argv[0]=%s fd=%d read=%d data=%.7s\n",
         argc, argc > 0 ? argv[0] : "-", fd, n, buf);
  fflush(stdout);
  return (n == 7 && memcmp(buf, "carried", 7) == 0) ? 42 : 43;
}
