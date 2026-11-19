/* Backtraces, symbol lookup and the panic path.
 *
 * The kernel is built -fno-omit-frame-pointer so that a walk is a walk and not a
 * stack scan. Anything that reports a fault uses these; anything that finds an
 * impossible state calls panic().
 */

#ifndef _BT_H
#define _BT_H

#include <stdint.h>

/* Enough for a fault under a syscall under a scheduler switch; past that the
 * interesting frames are already printed. */
#define BT_MAX_FRAMES 24

/* Nearest symbol at or below addr, or 0 if the table is empty. *off gets the
 * byte offset from that symbol - the "+0x42" in "ata_read+0x42". */
const char *bt_lookup(uint64_t addr, uint64_t *off);

/* Walk frame pointers from rbp, starting with rip. Returns the frame count. */
int bt_capture(uint64_t rbp, uint64_t rip, uint64_t *out, int max);

/* Print the walk: one line per frame, address and symbol+offset. */
void bt_print(uint64_t rbp, uint64_t rip);

/* Print the running task's name and id. */
void bt_who(void);

/* The body of panic(); use the macro below so the frame pointer is the caller's. */
void panic_from(uint64_t rbp, uint64_t rip, const char *what);

/* Stop the kernel and say why, with the caller's frames. Taking the frame
 * pointer in the caller is the whole point: inside panic_from() the first frame
 * would always be panic_from itself. */
#define panic(what)                                                              \
  panic_from((uint64_t)(uintptr_t)__builtin_frame_address(0),                    \
             (uint64_t)(uintptr_t)__builtin_return_address(0), (what))

/* The whole report for a fault that arrived through the ring3 fault stub: the
 * fifteen saved general registers, then the hardware frame. `saved` points at the
 * *first* of them (r15, the last pushed), and the hardware frame follows at
 * saved+15. Prints the registers, the task, and the backtrace. */
void bt_fault_dump(const uint64_t *saved, uint64_t err, uint64_t rip, uint64_t cs,
                   uint64_t rflags, uint64_t rsp, uint64_t ss);

/* Checks the walk, the table and its own limits; prints [bttest] 4/4 ok. */
void bt_selftest(void);

/* Deliberately faults, three frames deep, to prove the report end to end.
 * Compiled in only with PANIC_DEMO=1 - it stops the machine on purpose. */
void panic_demo(void);

#endif
