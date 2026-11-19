/* power: ask the machine to switch off or restart.
 *
 *   power            -> switch off
 *   power reset      -> restart
 *
 * A musl binary that reaches the native door directly, the same way sandtest does:
 * the Linux ABI has no call for this, and the numbers SYS_POWER takes (0, 1) are
 * the kernel's, not anybody else's.
 *
 * It only returns when the machine did not go anywhere, which is the interesting
 * case: a firmware table that names a control block the chipset ignores is a real
 * machine, not a broken one, and the log says what was tried.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SYS_POWER 58

static long door(long n, long a) {
  register long rax __asm__("rax") = n;
  register long rdi __asm__("rdi") = a;
  __asm__ volatile("int $0x80" : "+r"(rax) : "r"(rdi) : "rcx", "r11", "memory");
  return rax;
}

int main(int argc, char **argv) {
  int what = 0;
  if (argc > 1 && strcmp(argv[1], "reset") == 0) what = 1;

  printf("power: asking the kernel to %s\n", what ? "restart" : "switch off");
  fflush(stdout);

  long rc = door(SYS_POWER, what);

  /* Reaching here means the machine is still on. */
  printf("power: refused, rc=%ld (the serial log says what was tried)\n", rc);
  return 1;
}