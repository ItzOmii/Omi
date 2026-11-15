/*
 * MeepOS - symbols required by the prebuilt compiler-rt archive.
 *
 * The soft-float builtins ship compiled with stack protection, so pulling
 * any of them in drags a reference to __stack_chk_fail along with it.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);

void __stack_chk_fail(void) {
  serial_puts("[panic] stack smashing detected\n");
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}
